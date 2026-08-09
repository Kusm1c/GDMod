#include "replay_state.hpp"
#include "sim/Level.hpp"
#include "sim/Solver.hpp"
#include "sim/Slope.hpp"
#include "sim/DebugPaths.hpp"
#include <Geode/cocos/support/zip_support/ZipUtils.h>
#include "solver_overlay.hpp"
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/utils/string.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <random>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

using namespace geode::prelude;

// MenuLayer Hook
// ============================================================

static bool isDevMode() {
    auto c = Mod::get()->getSettingValue<ccColor3B>("accent-color");
    return c.r == 21 && c.g == 3 && c.b == 20;
}

class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        if (!isDevMode()) return true;

        // Create button with a sprite
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto label = CCLabelBMFont::create("GDR2\n->JSON", "bigFont.fnt");
        label->setScale(0.25f);
        label->setPosition(btnSprite->getContentSize() / 2);
        btnSprite->addChild(label);

        auto myButton = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(MyMenuLayer::onConvertGDR2)
        );

        auto menu = this->getChildByID("bottom-menu");
        menu->addChild(myButton);
        myButton->setID("gdr2-to-json-btn"_spr);
        menu->updateLayout();

        return true;
    }

    void onConvertGDR2(CCObject*) {
#ifdef GEODE_IS_WINDOWS
        // Use Win32 native file dialog (synchronous, no arc dependency)
        OPENFILENAMEW ofn{};
        wchar_t szFile[MAX_PATH] = {0};

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"GDR Replay Files (*.gdr2;*.gdr)\0*.gdr2;*.gdr\0GDR2 (*.gdr2)\0*.gdr2\0GDR (*.gdr)\0*.gdr\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        // Set default directory to Eclipse replays folder
        auto defaultDir = dirs::getGameDir() / "geode" / "config" / "prevter.eclipsemenu" / "replays";
        if (!std::filesystem::exists(defaultDir)) {
            defaultDir = dirs::getGameDir();
        }
        auto defaultDirStr = defaultDir.wstring();
        ofn.lpstrInitialDir = defaultDirStr.c_str();

        if (GetOpenFileNameW(&ofn)) {
            auto path = std::filesystem::path(szFile);
            convertGdr2File(path);
        }
#else
        FLAlertLayer::create("Error", "File picker is only available on Windows.", "OK")->show();
#endif
    }
};

// ============================================================
// GD level string decompression (base64url + zlib)
// ============================================================
static std::string decompressGDLevelString(const std::string& str) {
    if (str.empty()) return {};
    if (str.find(';') != std::string::npos) return str; // already raw

    // Some exported levels use a raw kS prefix with |/_ separators rather than
    // the compressed base64 form. Normalize those first.
    auto normalized = gdsim::normalizeLevelString(str);
    if (!normalized.empty() && normalized.find(';') != std::string::npos) {
        return normalized;
    }

    // Otherwise this should be a compressed/base64url form.
    return std::string(cocos2d::ZipUtils::decompressString(gd::string(str), false, 0));
}

// Stable per-level key for replay files. Online levels use their real ID. LOCAL /
// created levels report m_levelID == 0, so keying by ID alone would make every one
// of them share a single replay file. Key those by a hash of the level name instead
// (made negative so it can never collide with a real online ID). This is what lets
// the mod's Simulate/Watch work on levels you build yourself in the editor.
static int effectiveLevelKey(GJGameLevel* level) {
    if (!level) return 0;
    int id = level->m_levelID;
    if (id > 0) return id;
    std::string name = level->m_levelName;
    if (name.empty()) return 0;
    uint32_t h = 2166136261u;                       // FNV-1a over the name
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    int key = -static_cast<int>(h & 0x7fffffffu);
    return key == 0 ? -1 : key;
}

// User-toggleable solver post-processing (set via the Optimize / Center toggles).
static bool s_solverOptimize = true;
static bool s_solverCenter   = true;
static bool s_solverHumanLimits = false; // cap to ~14 clicks/s (replay-robust, no superhuman bursts)
// Default OFF: a solve that passes gdsim's own zero-margin re-check writes the
// .gdr2 immediately and only runs ONE non-blocking real-game confirmation pass
// afterward (see doSimulate) — correctness comes from gdsim, not a real-game gate.
// Toggling this ON opts back into the old behavior: withhold the .gdr2 and
// brute-force real-engine fixes (arBuildCandidates/arOnTrialEnd in hooks_play.cpp)
// until it clears for real, for the cases where you want that guarantee badly
// enough to wait for it.
static bool s_solverAutoRepair = false;
// Physics step rate. GD steps PHYSICS at a fixed 240 sub-steps/sec regardless of
// monitor FPS (see hooks_play.cpp) — it is NOT variable, so this is HARD-CODED to
// 240. cfg.dt (=1/240), the generated replay framerate and the human click-cap all
// derive from it, keeping the sim/solver locked to the real 240-substep game and to
// every recorded truth file. (This used to be a user-cyclable 60→960 knob; since the
// physics isn't actually variable, moving it off 240 only ever caused desync.)
static constexpr double s_solverFps = 240.0;
static std::string solverFpsLabel() { return "Sim " + std::to_string((int)s_solverFps) + " FPS"; }

// ============================================================
// LevelInfoLayer Hook – Add Watch Replay Button
// ============================================================

// Opens the consolidated Pathfinder Game menu (popup defined just below the class).
// Takes the level + a launch callback so it works from any host page (online
// LevelInfoLayer and local EditLevelLayer).
static void showPathfinderMenu(GJGameLevel* level, std::function<void()> play);

class $modify(MyLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool selectLevel) {
        if (!LevelInfoLayer::init(level, selectLevel)) return false;

        // Single entry point: one tidy button opens the Pathfinder Game popup,
        // which gathers Watch / Edit / View / Simulate / Export and the solver
        // options. Replaces the old row of 5 buttons + 3 loose toggles.
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        if (auto noteIcon = CCSprite::createWithSpriteFrameName("GJ_musicOnBtn_001.png")) {
            noteIcon->setScale(0.7f);
            noteIcon->setPosition(btnSprite->getContentSize() / 2);
            btnSprite->addChild(noteIcon);
        } else {
            auto label = CCLabelBMFont::create("Pathfinder", "bigFont.fnt");
            label->setScale(0.3f);
            label->setPosition(btnSprite->getContentSize() / 2);
            btnSprite->addChild(label);
        }

        auto menuButton = CCMenuItemSpriteExtra::create(
            btnSprite, this, menu_selector(MyLevelInfoLayer::onOpenPathfinderMenu));
        menuButton->setID("pathfinder-menu-btn"_spr);

        // Place on the LEFT, grouped with GD's own action buttons (copy, etc.).
        // node-ids exposes the left column as "left-side-menu"; if present, drop
        // the button in and let its layout space it. Otherwise fall back to a
        // manual position on the left edge.
        if (auto leftMenu = typeinfo_cast<CCMenu*>(this->getChildByID("left-side-menu"))) {
            btnSprite->setScale(0.85f);
            leftMenu->addChild(menuButton);
            leftMenu->updateLayout();
        } else {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            menuButton->setPosition({28.0f, winSize.height / 2.0f - 40.0f});
            auto pathfinderMenu = CCMenu::create();
            pathfinderMenu->setID("pathfinder-menu"_spr);
            pathfinderMenu->setPosition(CCPointZero);
            pathfinderMenu->addChild(menuButton);
            this->addChild(pathfinderMenu, 200);
        }

        return true;
    }

public:
    // Opens the consolidated Pathfinder Game menu popup (defined after this class).
    void onOpenPathfinderMenu(CCObject*) {
        // Solve now launches straight into the level for real in-game verification once
        // the (possibly long, backgrounded) headless solve finishes — so `play` can fire
        // well after this call returns. Retain `this` for the callback's whole lifetime
        // so navigating away from this page mid-solve can't leave it calling into a
        // freed LevelInfoLayer; released exactly once, whether play() ever fires or not,
        // when the last copy of the callback (and this guard) is destroyed.
        this->retain();
        std::shared_ptr<void> guard(this, [](void* p) { static_cast<CCObject*>(p)->release(); });
        showPathfinderMenu(m_level, [this, guard]{ this->onPlay(nullptr); });
    }

    // Solver/replay actions are STATIC and take (level, play) so they work from ANY
    // host page — the online LevelInfoLayer and the local EditLevelLayer (created
    // levels) — without needing a LevelInfoLayer instance. `play` launches the level
    // (each host passes its own onPlay). Keyed by effectiveLevelKey so local levels
    // (m_levelID==0) get a stable, non-colliding replay file.
    static void doSimulate(GJGameLevel* level, std::function<void()> play, bool cubeSolver = false) {
        if (!level) return;

        std::string levelStr = decompressGDLevelString(level->m_levelString);
        if (levelStr.empty() || levelStr.find(';') == std::string::npos) {
            FLAlertLayer::create("Simulate", "Impossible de lire les donnees du niveau.", "OK")->show();
            return;
        }

        int levelId = effectiveLevelKey(level);
        std::string levelName = level->m_levelName;   // copy: `level` isn't safe across the worker thread

        // Dump the raw level string so it can be replayed in the standalone
        // diagnostic harness (test/diag.cpp) to debug simulator physics bugs.
        {
            std::ofstream dump(gdsim::debugPath("last_level.txt"),
                               std::ios::out | std::ios::trunc | std::ios::binary);
            if (dump.is_open()) dump << levelStr;
        }

        // In-game Cocos overlay (cross-platform) — replaces the old native windows.
        auto progress  = std::make_shared<gdsim::SolverProgressReport>();
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        showSolverOverlay(levelId, progress, cancelled);

        std::thread([levelStr, levelId, levelName, progress, cancelled, cubeSolver, play]() {
            gdsim::SolverConfig cfg;
            cfg.progress   = progress.get();
            cfg.doOptimize = s_solverOptimize;
            cfg.doCenter   = s_solverCenter;
            // Human click-rate cap is in FRAMES, so it MUST scale with the target fps.
            // A fixed 17 was ~14 clicks/s at 240fps but only 3.5 clicks/s at 60fps —
            // which made frame-perfect 60fps waves the user CAN clear report as "not
            // humanly possible". Keep a constant ~14 clicks/s across all framerates.
            cfg.minClickGap = s_solverHumanLimits
                ? std::max(1, (int)std::lround(s_solverFps / 14.0)) : 0;
            cfg.dt = (float)(1.0 / s_solverFps);            // 1/240 — GD's fixed physics step

            // "Sim Cube" routes to the decision-point cube solver (long/frame-perfect
            // cube levels); plain "Simulate" uses the general pipeline.
            auto result = cubeSolver
                ? gdsim::solveLevelCube(levelStr, cfg, cancelled.get())
                : gdsim::solveLevel(levelStr, cfg, cancelled.get());

            // Independent zero-margin re-verification: replay the found clicks through
            // a FRESH Level with no safety margin (Level's hazardInflate defaults to 0)
            // and confirm it actually reaches the end without dying. solveLevel()'s own
            // pipeline already does this internally (see finalizeSolved's "ZERO-MARGIN
            // CHECK" in Solver.cpp, logged to solver_debug.txt) — but solveLevelCube has
            // no such check, so this is the ONLY zero-margin proof for "Solve Cube". It
            // also writes a debug trace file so a bad solve is diagnosable without a GUI.
            bool trajDied = false;
            uint64_t trajDeathFrame = 0;
            std::pair<float, float> trajDeath{0.f, 0.f};
            if (!result.clicks.empty()) {
                // Dump the exact click list this attempt is about to re-verify — a
                // trajDied result previously vanished with no way to replay it outside
                // the game (no .gdr2 is written on failure). Same frame-pair format
                // test/gdrcheck.cpp's NPRESS section already parses, so a failed solve
                // can go straight into that tool with last_level.txt.
                {
                    std::ofstream ck(gdsim::debugPath(fmt::format("GDMod_solve_clicks_{}.txt", levelId)),
                                     std::ios::trunc);
                    if (ck.is_open()) {
                        ck << "NPRESS " << result.clicks.size() << "\n";
                        for (auto& c : result.clicks)
                            ck << c.pressFrame << ' ' << c.releaseFrame << " 1\n";
                    }
                }
                gdsim::Level trajSim(levelStr);
                size_t inputSz = static_cast<size_t>(
                    result.framesTotal > 0 ? result.framesTotal + 2 : cfg.maxFrames + 2);
                std::vector<bool> inputAt(inputSz, false);
                for (auto& c : result.clicks) {
                    for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
                        inputAt[f] = true;
                }

                uint64_t maxF = result.framesTotal > 0
                    ? static_cast<uint64_t>(result.framesTotal)
                    : cfg.maxFrames;
                float levelEnd = result.levelEndEstimate;
                // Sparse trace for the debug file — every 8th frame is plenty to see the
                // shape of the path without a multi-MB dump on long levels.
                std::ofstream trace(gdsim::debugPath(fmt::format("GDMod_solve_trace_{}.txt", levelId)),
                                    std::ios::trunc);
                if (trace.is_open())
                    trace << "# frame x y dead  (every 8th frame; solve via "
                          << (cubeSolver ? "SolveCube" : "Solve") << ")\n";
                for (uint64_t f = 1; f <= maxF; ++f) {
                    bool p  = f < inputAt.size() && inputAt[f];
                    auto& s = trajSim.runFrame(p, cfg.dt);
                    if (trace.is_open() && (f % 8 == 0))
                        trace << f << ' ' << s.pos.x << ' ' << s.pos.y << ' ' << (s.dead ? 1 : 0) << '\n';
                    if (levelEnd > 0.f && s.pos.x >= levelEnd) break; // stop at finish
                    if (s.dead) {
                        trajDied      = true;
                        trajDeathFrame = f;
                        trajDeath     = {s.pos.x, s.pos.y};
                        if (trace.is_open())
                            trace << "DIED f=" << f << " x=" << s.pos.x << " y=" << s.pos.y << '\n';
                        break;
                    }
                }
            }

            Loader::get()->queueInMainThread([result, levelId, levelName, trajDied, trajDeathFrame, trajDeath, play]() {
                if (!result.solved) {
                    FLAlertLayer::create(
                        "Solve",
                        fmt::format("No solution found.\n{}", result.message).c_str(),
                        "OK"
                    )->show();
                    return;
                }
                if (trajDied) {
                    // gdsim's own headless re-check already dies with no margin at all —
                    // real GD would only do worse. Not worth burning a real trial on it;
                    // report it as a diagnosable failure instead of a false "solved".
                    FLAlertLayer::create(
                        "Solve",
                        fmt::format(
                            "<cr>Solver said solved, but the zero-margin re-check DIES</c>\n"
                            "at frame {} x={:.0f} y={:.0f}.\n"
                            "See GDMod_solve_trace_{}.txt",
                            trajDeathFrame, trajDeath.first, trajDeath.second, levelId
                        ).c_str(),
                        "OK"
                    )->show();
                    return;
                }

                ReplayLoadResult replay;
                replay.framerate = s_solverFps;   // clicks are in 1/fps frames
                for (auto& c : result.clicks) {
                    replay.presses.push_back({c.pressFrame, c.releaseFrame, 1});
                }
                saveReplayToLocalJson(levelId, replay);

                // gdsim has ALREADY re-verified this solution at zero margin (both the
                // fresh re-check above and solveLevel()'s own internal gate — see
                // Solver.cpp finalizeSolved) — that's now the sole authority for
                // correctness, so the .gdr2 is written right here instead of waiting on
                // a real-game gate that could otherwise discard a valid solve entirely
                // (see solve_realverify_autorepair_flow / this session's plan).
                std::string gdr2Path = writeSolvedGdr2(levelId, levelName, replay);

                g_replayPlayer.isActive = true;
                g_replayPlayer.replay   = replay;
                g_replayPlayer.levelId  = levelId;
                if (s_solverAutoRepair) {
                    // Opt-in: withhold nothing new (the .gdr2 above already exists, and
                    // stays even if this fails/gets stuck) but ALSO brute-force real-
                    // engine fixes until it clears, overwriting the .gdr2 with the
                    // repaired result on success (see arOnTrialEnd).
                    g_autoRepairRequested.store(true);
                    Notification::create(
                        fmt::format("Solved ({} clicks) — repairing in-game...", result.clicks.size()),
                        NotificationIcon::Loading, 2.f)->show();
                } else {
                    // Default: one non-blocking real-game pass for confirmation +
                    // telemetry only (see confirmOnlyFinish in hooks_play.cpp) — never
                    // retries, never blocks on the .gdr2 already written above.
                    g_confirmRequested.store(true);
                    Notification::create(
                        fmt::format("Solved ({} clicks) — {}", result.clicks.size(),
                            gdr2Path.empty() ? "FAILED to write .gdr2!" : ".gdr2 saved, confirming in-game..."),
                        gdr2Path.empty() ? NotificationIcon::Error : NotificationIcon::Success, 3.f)->show();
                }
                play();
            });
        }).detach();
    }

};

// ------------------------------------------------------------
// In-game menu popup: the whole mod. One button on the level page opens this;
// it solves the level (beam / cube solver) and writes the solution as a .gdr2,
// with the solver options (Optimize / Center / Human).
// ------------------------------------------------------------
class PathfinderMenuPopup : public geode::Popup {
protected:
    GJGameLevel* m_glevel = nullptr;        // target level (online or local/created)
    std::function<void()> m_play;           // host page's launch action
    ButtonSprite* m_fpsBtn = nullptr;   // shows/cycles the target sim framerate

    bool init(GJGameLevel* level, std::function<void()> play) {
        m_glevel = level;
        m_play   = std::move(play);
        if (!Popup::init(320.f, 415.f)) return false;
        this->setTitle("Kusmic's Pathfinder");

        auto size = m_mainLayer->getContentSize();
        const float cx = size.width / 2.f;

        auto menu = CCMenu::create();
        menu->setPosition(0, 0);
        m_mainLayer->addChild(menu);

        auto mkBtn = [&](const char* txt, const char* tex, cocos2d::SEL_MenuHandler sel,
                         float x, float y) {
            auto bs = ButtonSprite::create(txt, "bigFont.fnt", tex, 0.7f);
            auto b = CCMenuItemSpriteExtra::create(bs, this, sel);
            b->setPosition(x, y);
            menu->addChild(b);
        };
        auto mkLabel = [&](const char* txt, float x, float y, float scale) {
            auto l = CCLabelBMFont::create(txt, "goldFont.fnt");
            l->setScale(scale);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(x, y);
            m_mainLayer->addChild(l);
        };

        // --- Solve section: the whole mod. Solve the level → write a .gdr2. ---
        mkLabel("Solve", 26.f, size.height - 52.f, 0.5f);
        const float ry = size.height - 92.f;
        mkBtn("Solve",      "GJ_button_01.png", menu_selector(PathfinderMenuPopup::onSimulate),     cx - 70.f, ry);
        mkBtn("Solve Cube", "GJ_button_02.png", menu_selector(PathfinderMenuPopup::onSimulateCube), cx + 70.f, ry);

        // --- Solver options ---
        mkLabel("Options", 26.f, ry - 52.f, 0.5f);
        auto addTog = [&](const char* txt, bool initial, cocos2d::SEL_MenuHandler sel, float x) {
            auto t = CCMenuItemToggler::createWithStandardSprites(this, sel, 0.6f);
            t->toggle(initial);
            t->setPosition(x, ry - 92.f);
            menu->addChild(t);
            auto l = CCLabelBMFont::create(txt, "bigFont.fnt");
            l->setScale(0.32f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(x + 14.f, ry - 92.f);
            m_mainLayer->addChild(l);
        };
        addTog("Optimize", s_solverOptimize,    menu_selector(PathfinderMenuPopup::onTogOptimize), 36.f);
        addTog("Center",   s_solverCenter,      menu_selector(PathfinderMenuPopup::onTogCenter),   145.f);
        addTog("Human",    s_solverHumanLimits, menu_selector(PathfinderMenuPopup::onTogHuman),    235.f);

        // Repair: OFF by default (write .gdr2 immediately, confirm in-game once,
        // non-blocking). ON opts back into withholding the .gdr2 until a real-engine
        // brute-force repair pass actually clears the level (see s_solverAutoRepair).
        {
            auto t = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(PathfinderMenuPopup::onTogAutoRepair), 0.6f);
            t->toggle(s_solverAutoRepair);
            t->setPosition(36.f, ry - 132.f);
            menu->addChild(t);
            auto l = CCLabelBMFont::create("Repair (blocking)", "bigFont.fnt");
            l->setScale(0.32f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(50.f, ry - 132.f);
            m_mainLayer->addChild(l);
        }

        // Physics rate indicator, HARD-CODED to 240 (GD's fixed sub-step rate).
        m_fpsBtn = ButtonSprite::create(solverFpsLabel().c_str(), "bigFont.fnt", "GJ_button_04.png", 0.55f);
        {
            auto b = CCMenuItemSpriteExtra::create(m_fpsBtn, this, menu_selector(PathfinderMenuPopup::onCycleFps));
            b->setPosition(cx, ry - 180.f);
            menu->addChild(b);
        }

        return true;
    }

    // Physics rate is HARD-CODED to 240 (GD's fixed sub-step rate) — a static indicator.
    void onCycleFps(CCObject*) {}

    // Solve → .gdr2. Close the popup first (so the solver overlay / result dialog isn't
    // stacked behind it), then run the solver on our level.
    void onSimulate(CCObject*)     { auto l = m_glevel; auto p = m_play; this->onClose(nullptr); MyLevelInfoLayer::doSimulate(l, p); }
    void onSimulateCube(CCObject*) { auto l = m_glevel; auto p = m_play; this->onClose(nullptr); MyLevelInfoLayer::doSimulate(l, p, true); }

    void onTogOptimize(CCObject*)    { s_solverOptimize    = !s_solverOptimize; }
    void onTogCenter(CCObject*)      { s_solverCenter      = !s_solverCenter; }
    void onTogHuman(CCObject*)       { s_solverHumanLimits = !s_solverHumanLimits; }
    void onTogAutoRepair(CCObject*)  { s_solverAutoRepair  = !s_solverAutoRepair; }

public:
    static PathfinderMenuPopup* create(GJGameLevel* level, std::function<void()> play) {
        auto ret = new PathfinderMenuPopup();
        if (ret->init(level, std::move(play))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

static void showPathfinderMenu(GJGameLevel* level, std::function<void()> play) {
    if (auto p = PathfinderMenuPopup::create(level, std::move(play)))
        p->show();
}

// ============================================================
// EditLevelLayer Hook – Pathfinder button on LOCAL / created levels
// ------------------------------------------------------------
// The Pathfinder button only existed on LevelInfoLayer (online levels), so the mod
// couldn't be used on levels you build yourself. EditLevelLayer is the info page
// for a local level; add the same button here, opening the same popup on this
// level with EditLevelLayer's own onPlay as the launch action. Simulate/Watch key
// their replay by effectiveLevelKey(), so id-0 local levels don't collide.
class $modify(MyEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;

        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        if (auto noteIcon = CCSprite::createWithSpriteFrameName("GJ_musicOnBtn_001.png")) {
            noteIcon->setScale(0.7f);
            noteIcon->setPosition(btnSprite->getContentSize() / 2);
            btnSprite->addChild(noteIcon);
        } else {
            auto label = CCLabelBMFont::create("Pathfinder", "bigFont.fnt");
            label->setScale(0.3f);
            label->setPosition(btnSprite->getContentSize() / 2);
            btnSprite->addChild(label);
        }

        auto menuButton = CCMenuItemSpriteExtra::create(
            btnSprite, this, menu_selector(MyEditLevelLayer::onOpenPathfinderMenu));
        menuButton->setID("pathfinder-menu-btn"_spr);

        // Drop it into the level's own button row (Edit / Play / Share…) and let
        // the existing layout space it; fall back to a manual left-edge position.
        if (m_buttonMenu) {
            btnSprite->setScale(0.85f);
            m_buttonMenu->addChild(menuButton);
            m_buttonMenu->updateLayout();
        } else {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            menuButton->setPosition({28.0f, winSize.height / 2.0f - 40.0f});
            auto pathfinderMenu = CCMenu::create();
            pathfinderMenu->setID("pathfinder-menu"_spr);
            pathfinderMenu->setPosition(CCPointZero);
            pathfinderMenu->addChild(menuButton);
            this->addChild(pathfinderMenu, 200);
        }

        return true;
    }

    void onOpenPathfinderMenu(CCObject*) {
        // See MyLevelInfoLayer::onOpenPathfinderMenu — same async-play retain guard.
        this->retain();
        std::shared_ptr<void> guard(this, [](void* p) { static_cast<CCObject*>(p)->release(); });
        showPathfinderMenu(m_level, [this, guard]{ this->onPlay(nullptr); });
    }
};

// ============================================================

