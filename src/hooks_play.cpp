#include "replay_state.hpp"
#include "sim/Level.hpp"
#include "sim/Calib.hpp"
#include "sim/DebugPaths.hpp"
#include <Geode/cocos/support/zip_support/ZipUtils.h>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/Slider.hpp>
#include <fstream>
#include <map>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

using namespace geode::prelude;

// ============================================================
// Debug File Logging
// ============================================================
static const char* DEBUG_LOG_FILE = "GDMod_debug.txt";
static constexpr bool ENABLE_ULTRA_AUDIO_FILE_DEBUG = false;
static constexpr bool ENABLE_RUNTIME_HITBOX_OVERLAY = true;

[[maybe_unused]] static void debugLogToFileImpl(const std::string& msg) {
    try {
        std::ofstream file(DEBUG_LOG_FILE, std::ios::app);
        if (file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            char timestr[64];
            std::tm tmUtc{};
            gmtime_s(&tmUtc, &time);
            std::strftime(timestr, sizeof(timestr), "%H:%M:%S", &tmUtc);
            file << fmt::format("{}.{:03d} {}\n", timestr, ms.count(), msg);
            file.flush();
        }
    } catch (...) {}
}

#define debugLogToFile(msg) do { if constexpr (ENABLE_ULTRA_AUDIO_FILE_DEBUG) debugLogToFileImpl((msg)); } while (0)

template <class... Args>
static void traceDebug(fmt::format_string<Args...> format, Args&&... args) {
    (void)format;
    ((void)args, ...);
}

static bool s_replayInputInjectionActive = false;

// ============================================================
// Frame-exact (per-physics-step) replay input injection
// ============================================================
// GD steps physics at a fixed 240 sub-steps/sec regardless of monitor FPS, doing
// N sub-steps per visual frame. The legacy path injected inputs once per *visual*
// frame (PlayLayer::postUpdate, after the steps already ran), so on any non-240Hz
// / variable display short ship/UFO taps were collapsed and press/release edges
// landed up to N-1 sub-steps late — the ship arc drifts a few px and grazes
// spikes (cube survives; its jumps are discrete). This drives the button per
// physics step (GJBaseGameLayer::processCommands) instead: frame-exact and
// Hz-independent. Flip s_perStepReplay to false to fall back to the old behaviour.
static bool   s_perStepReplay   = true;   // master switch (see report / testing notes)
static double s_replayStepTime  = 0.0;    // physics clock, re-anchored to m_timePlayed each visual frame
static bool   s_replayStepHold  = false;  // last injected hold state (edge tracking)
static bool   s_replayStepActive = false; // true only while a replay plays forward this frame
// TEMP diagnostic: logs every injected handleButton edge (f, dt, hold) to
// GDMod_inject_debug.txt. Used to pin down the short-tap (1-frame press)
// timing bug — cross-reference against a GDMod_truth_*.txt FRAMES section
// to see exactly which physics frame our injection lands the press/release
// on vs. which frame the real Y position actually moves. Flip off once done.
static bool   s_injectDebugLog  = true;
// Diagnostics: prove the per-step path is actually live and frame-exact.
static std::atomic<uint64_t> s_perStepProcessCount{0}; // processCommands ticks seen while replay active
static std::atomic<uint64_t> s_perStepInjectCount{0};  // button edges injected per-step

// During WATCH, show ONLY the input visuals — hide the playback chrome (bottom
// progress bar + time/speed labels + transport buttons). Replay EDIT mode is
// unaffected — it needs its controls. Flip to true to bring the chrome back.
static constexpr bool kWatchShowChrome = false;

// Ground-truth recorder: dumps level + inputs + per-frame real trajectory to
// GDMod_truth_<levelId>.txt so the offline calibrator (test/calibrate.cpp) can
// tune gdsim against real-game data without rebuilding.
static std::ofstream s_calibTruth;

// ============================================================
// Auto-repair: validate & locally fix a gdsim solution in the REAL engine.
// ============================================================
// gdsim solutions can be "almost right" — frame-perfect with no slack — so they
// just barely die in real GD even though they clear in the simulator. Auto-repair
// replays the solution in the actual engine, finds where it dies, and brute-forces
// small input edits (shift a click ±N frames, lengthen/shorten the hold, insert or
// remove a click) near that death until the run gets further. Every trial is a full
// restart validated by the real physics, so the result is guaranteed to complete.
// Simple & robust by design: pure local hill-climb on the death frontier.
// Fast-forward timescale during repair. GD steps physics at a fixed 240 sub-steps/s,
// so a higher value replays each trial in less wall-clock time — important since each
// trial replays from the start to the death (slow on deep deaths in long levels, and
// EVERY trial replays the full prefix from frame 0 — no checkpoints, see
// solver_startpos_bug_and_pipeline_fix memory: GD's loadFromCheckpoint can't
// reproduce a frame-perfect state reliably, so full-runs are the only correct option).
// Raised from a conservative 8x: the (now-removed) window scanner ran the SAME
// speedhacked-trial pattern reliably at 100x for months, PROVEN safe — the 8x here was
// an untested guess, not an empirical limit. Paired with hiding the object render
// layers below (the window scanner's other half — CPU spent drawing thousands of
// sprites was likely the real ceiling, not a substep cap), so the higher number should
// actually reach the CPU/GD, not just request it. On a hard, many-pinch-point level
// this can be the difference between finishing in minutes vs. not finishing in the
// time a user is willing to wait with the game open.
static float s_autoRepairSpeed = 60.0f;

struct AROp {
    enum Type { ShiftPress, Hold, Insert, Remove } type;
    int      idx    = -1;  // target press index (Shift/Hold/Remove)
    int      delta  = 0;   // frames (Shift: move press+release; Hold: release += delta)
    uint64_t insAt  = 0;   // Insert: press frame
    uint64_t insLen = 6;   // Insert: hold length
};

struct AutoRepairState {
    bool                     active = false;   // a repair session is running this PlayLayer
    bool                     done   = false;   // finished (solved or stuck)
    int                      levelId = 0;
    std::string              levelName;         // for the final .gdr2 filename
    double                   framerate = 240.0;
    std::vector<ReplayPress> base;             // best validated input so far
    std::vector<ReplayPress> trial;            // input under test this run
    float                    bestX = 0.f;      // furthest validated X (death frontier)
    float                    trialMaxX = 0.f;  // furthest X this trial
    bool                     trialDied = false;
    uint64_t                 lastDeathFrame = 0;
    std::vector<AROp>        cands;            // candidate edits for current frontier
    size_t                   candIdx = 0;
    int                      attempts = 0;
    int                      maxAttempts = 4000;
    bool                     firstTrial = true;
    // Repair logging — quantify how far off gdsim's solution was.
    std::vector<ReplayPress> original;         // the gdsim solution as-solved (for the final diff)
    AROp                     lastOp{};          // the edit that produced the current trial
    bool                     lastOpValid = false;
    int                      numCorrections = 0;// accepted edits (each = one sim error)
    int                      totalAdjustFrames = 0;
    // Momentum: a level can need the SAME kind of nudge (e.g. "hold click #2 longer")
    // several times in a row as a gradual real-vs-sim timing drift is walked through
    // one small step at a time — proven on 68839068's repair log: 5 consecutive
    // "HOLD click #2 +2 frames" fixes back to back. Track the last accepted op's
    // (type, idx, sign) and a running streak count; arBuildCandidates uses this to
    // ALSO try a bigger jump in the same direction FIRST, so a long drift can be
    // covered in fewer real-engine trials instead of re-discovering the same small
    // step every time.
    AROp::Type               momentumType = AROp::ShiftPress;
    int                      momentumIdx = -1;
    int                      momentumSign = 0;
    int                      momentumStreak = 0;
    int                      momentumLastDelta = 0;
};
static AutoRepairState g_ar;

// Sort by press frame, drop degenerate/overlapping holds so injection stays valid.
static void arNormalize(std::vector<ReplayPress>& v) {
    for (auto& p : v) {
        if (p.player <= 0) p.player = 1;
        if (p.framePress < 1) p.framePress = 1;
        if (p.frameRelease <= p.framePress) p.frameRelease = p.framePress + 1;
    }
    std::sort(v.begin(), v.end(),
        [](const ReplayPress& a, const ReplayPress& b) { return a.framePress < b.framePress; });
    for (size_t i = 0; i + 1 < v.size(); ++i)
        if (v[i].frameRelease >= v[i + 1].framePress)
            v[i].frameRelease = v[i + 1].framePress > v[i].framePress + 1
                ? v[i + 1].framePress - 1 : v[i].framePress + 1;
    v.erase(std::remove_if(v.begin(), v.end(),
        [](const ReplayPress& p) { return p.frameRelease <= p.framePress; }), v.end());
}

static std::vector<ReplayPress> arApply(const std::vector<ReplayPress>& base, const AROp& op) {
    std::vector<ReplayPress> v = base;
    switch (op.type) {
        case AROp::ShiftPress:
            if (op.idx >= 0 && op.idx < (int)v.size()) {
                int64_t np = (int64_t)v[op.idx].framePress + op.delta;
                int64_t nr = (int64_t)v[op.idx].frameRelease + op.delta;
                if (np < 1) { nr += (1 - np); np = 1; }
                v[op.idx].framePress = (uint64_t)np;
                v[op.idx].frameRelease = (uint64_t)nr;
            }
            break;
        case AROp::Hold:
            if (op.idx >= 0 && op.idx < (int)v.size()) {
                int64_t nr = (int64_t)v[op.idx].frameRelease + op.delta;
                if (nr <= (int64_t)v[op.idx].framePress) nr = (int64_t)v[op.idx].framePress + 1;
                v[op.idx].frameRelease = (uint64_t)nr;
            }
            break;
        case AROp::Insert:
            v.push_back({op.insAt, op.insAt + op.insLen, 1});
            break;
        case AROp::Remove:
            if (op.idx >= 0 && op.idx < (int)v.size()) v.erase(v.begin() + op.idx);
            break;
    }
    arNormalize(v);
    return v;
}

// Repair log — records every edit the real engine needed on top of gdsim's solution,
// i.e. exactly where and by how much the simulator was wrong.
static std::string arLogPath() { return gdsim::debugPath("GDMod_repair_log.txt"); }
static void arLog(const std::string& s) {
    log::info("[AR] {}", s);
    std::ofstream f(arLogPath(), std::ios::app);
    if (f.is_open()) f << s << "\n";
}
static std::string arOpStr(const AROp& op) {
    // op.idx is 0-based; display 1-based so "click #N" matches the Nth entry in the
    // gdsim/real solution lists (they were off by one, which read as a bug).
    switch (op.type) {
        case AROp::ShiftPress: return fmt::format("SHIFT click #{} by {:+d} frame(s)", op.idx + 1, op.delta);
        case AROp::Hold:       return fmt::format("HOLD click #{} {:+d} frame(s)", op.idx + 1, op.delta);
        case AROp::Insert:     return fmt::format("INSERT a click at frame {}", op.insAt);
        case AROp::Remove:     return fmt::format("REMOVE click #{}", op.idx + 1);
    }
    return "?";
}
static int arOpAdjustFrames(const AROp& op) {
    switch (op.type) {
        case AROp::ShiftPress: case AROp::Hold: return std::abs(op.delta);
        case AROp::Insert: case AROp::Remove:   return 6;   // a whole click added/removed
    }
    return 0;
}
static std::string arClickList(const std::vector<ReplayPress>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += fmt::format("{}[{}+{}]", i ? " " : "", v[i].framePress, v[i].frameRelease - v[i].framePress);
    return s;
}

// Build the ordered candidate-edit list for the current death frontier. Small,
// localized edits first (most likely the frame-perfect fix); insert/remove last.
static void arBuildCandidates() {
    g_ar.cands.clear();
    g_ar.candIdx = 0;
    const auto& b = g_ar.base;
    uint64_t df = g_ar.lastDeathFrame;
    int focus = -1;
    for (int i = 0; i < (int)b.size(); ++i) {
        if (b[i].framePress <= df) focus = i; else break;
    }
    // Momentum: 2+ consecutive accepted fixes of the same (type, idx, sign) mean we're
    // walking through a gradual drift one small step at a time. Try jumping straight to
    // 3x and 6x the last delta FIRST — if the drift continues at a similar rate, this
    // can skip several round-trip trials. Falls through to the normal small deltas if
    // the big jump overshoots (candidates are tried in order; the first that actually
    // improves wins, so a failed big jump costs nothing but one trial).
    if (g_ar.momentumStreak >= 2 && g_ar.momentumIdx >= 0 && g_ar.momentumIdx < (int)b.size()) {
        int base = g_ar.momentumSign * std::abs(g_ar.momentumLastDelta);
        for (int mult : {3, 6, 10}) {
            int d = base * mult;
            if (g_ar.momentumType == AROp::Hold)
                g_ar.cands.push_back({AROp::Hold, g_ar.momentumIdx, d, 0, 6});
            else if (g_ar.momentumType == AROp::ShiftPress)
                g_ar.cands.push_back({AROp::ShiftPress, g_ar.momentumIdx, d, 0, 6});
        }
    }
    const int shifts[] = {-1, 1, -2, 2, -3, 3, -4, 4, -6, 6, -8, 8};
    const int holds[]  = {1, -1, 2, -2, 3, -3, 4};
    if (focus >= 0) {
        // Edit the click AT the wall and the few before it — a deep death is often
        // caused by an earlier click that set up a bad approach, not the last one.
        // Nearest-first so the cheapest likely fix is tried before the rest.
        for (int j = 0; j <= 3; ++j) {
            int fi = focus - j;
            if (fi < 0) break;
            for (int d : shifts) g_ar.cands.push_back({AROp::ShiftPress, fi, d, 0, 6});
            for (int d : holds)  g_ar.cands.push_back({AROp::Hold,       fi, d, 0, 6});
        }
        if (focus + 1 < (int)b.size())
            for (int d : shifts) g_ar.cands.push_back({AROp::ShiftPress, focus + 1, d, 0, 6});
    }
    // A click may simply be missing right at the wall — try inserting one nearby.
    const int insOff[] = {0, -6, 6, -12, 12, -3, 3, -18, 18};
    for (int o : insOff) {
        int64_t at = (int64_t)df + o;
        if (at < 1) at = 1;
        g_ar.cands.push_back({AROp::Insert, -1, 0, (uint64_t)at, 6});
    }
    if (focus >= 0) g_ar.cands.push_back({AROp::Remove, focus, 0, 0, 6});
}

// Is the jump held at physics frame `f`? presses are sorted by framePress and
// non-overlapping, so the only candidate covering f is the last press starting
// at or before f.
static bool replayHeldAtFrame(uint64_t f) {
    if (!g_replayPlayer.replay.has_value()) return false;
    const auto& presses = g_replayPlayer.replay->presses;
    if (presses.empty()) return false;
    int lo = 0, hi = (int)presses.size();          // first index with framePress > f
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (presses[mid].framePress <= f) lo = mid + 1; else hi = mid;
    }
    if (lo - 1 < 0) return false;
    const auto& p = presses[lo - 1];
    return p.framePress <= f && f < p.frameRelease;
}


class $modify(MyGJBGL, GJBaseGameLayer) {
    void handleButton(bool push, int button, bool isPlayer1) {
        bool replayMode = g_replayPlayer.isActive && g_replayPlayer.replay.has_value();

        if (s_replayInputInjectionActive) {
            GJBaseGameLayer::handleButton(push, button, isPlayer1);
            return;
        }

        if (replayMode) {
            return;
        }

        GJBaseGameLayer::handleButton(push, button, isPlayer1);
    }

    // Anchor the per-step replay clock to the simulated time BEFORE this visual
    // frame's physics sub-steps run, so processCommands below can map each step to
    // an exact physics frame (drift-free: re-anchored every frame).
    void update(float dt) {
        if (s_perStepReplay) {
            auto* pl = PlayLayer::get();
            bool active = pl && g_replayPlayer.isActive && g_replayPlayer.replay.has_value()
                          && g_replayExternalCommands.liveReplayActive.load()
                          && !g_replayExternalCommands.livePaused.load();
            s_replayStepActive = active;
            if (active) s_replayStepTime = static_cast<double>(pl->m_timePlayed);
        }
        GJBaseGameLayer::update(dt);
    }

    // Per-physics-step input injection. GD::update loops this once per sub-step;
    // we set the held state for the exact physics frame this step represents, then
    // advance the clock by the step delta. Half-ticks map to the same frame index
    // (round) so the hold stays consistent across them.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (s_perStepReplay && s_replayStepActive && g_replayPlayer.replay.has_value()) {
            double fr = g_replayPlayer.replay->framerate;
            if (fr > 0.0) {
                s_perStepProcessCount.fetch_add(1, std::memory_order_relaxed);
                // Advance the clock to the frame THIS step produces BEFORE reading the
                // index. The anchor (s_replayStepTime = m_timePlayed) is the previous
                // frame's end time, so reading f first labelled each frame's first
                // sub-step with the PREVIOUS frame's index — every input landed one
                // physics frame late (harmless for cube's discrete jumps, fatal for
                // hold-duration vehicles: ship arcs drift, robot jumps mis-charge).
                // Adding dt first also collapses both half-ticks of a frame onto the
                // same rounded index instead of splitting them across N and N+1.
                s_replayStepTime += static_cast<double>(dt);
                uint64_t f = static_cast<uint64_t>(std::llround(s_replayStepTime * fr));
                bool hold = replayHeldAtFrame(f);
                if (hold != s_replayStepHold) {
                    if (s_injectDebugLog) {
                        std::ofstream lg(gdsim::debugPath("GDMod_inject_debug.txt"), std::ios::app);
                        if (lg.is_open()) {
                            lg << fmt::format(
                                "f={} hold={} dt={:.6f} halfTick={} lastTick={} stepTime={:.6f} fr={:.1f}\n",
                                f, hold, dt, isHalfTick, isLastTick, s_replayStepTime, fr);
                        }
                    }
                    s_replayInputInjectionActive = true;
                    this->handleButton(hold, 1, true);
                    s_replayInputInjectionActive = false;
                    s_replayStepHold = hold;
                    s_perStepInjectCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        // ========== Input bars (normal play, "Show Inputs In-Game" setting) ==========
        std::vector<ReplayPress> m_presses;
        CCNode* m_circleLayer = nullptr;
        CCNode* m_indicator = nullptr; // hollow square following the player
        double m_framerate = 240.0;
        bool m_active = false;
        bool m_dotsCreated = false;
        int m_barHeight = 10;
        int m_barY = 15;
        ccColor4B m_p1Color = ccc4(50, 255, 80, 200);
        ccColor4B m_p2Color = ccc4(80, 130, 255, 200);
        ccColor4B m_indicatorColor = ccc4(255, 255, 255, 220);
        int m_indicatorStyle = 1; // 0 = line, 1 = square
        ccColor4B m_bgColor = ccc4(0, 0, 0, 120);
        CCLayerColor* m_bgStrip = nullptr;
        // Position history: (time, playerWorldX) samples for accurate interpolation
        // across speed changes (portals)
        std::vector<std::pair<float, float>> m_posHistory;
        // Dot nodes (one per press): (pressIndex, dotNode)
        std::vector<std::pair<size_t, CCNode*>> m_dots;

        // ========== Runtime hitbox capture (offline RE workflow) ==========
        CCDrawNode* m_runtimeHitboxOverlay = nullptr;
        bool m_runtimeHitboxCaptured = false;

        // ========== Auto-repair: validate & locally fix a gdsim solution in REAL GD ==========
        bool m_autoRepairActive = false;

        // ========== Confirm-only: ONE non-blocking real-game pass, no repair loop =====
        // Default post-Solve path (see doSimulate) — the .gdr2 is already written by
        // the time this runs; this just confirms + captures telemetry, never retries.
        bool m_confirmOnlyActive = false;
        bool m_confirmDone       = false;

        // ========== Sim-vs-game divergence detector (runs during auto-repair trials) ==========
        // Runs gdsim in lockstep with the live replay and logs the first frame
        // where the predicted player position diverges from the real one.
        std::shared_ptr<gdsim::Level> m_divSim;
        std::vector<bool> m_divInput;
        uint64_t m_divFrame = 0;
        bool  m_divInit = false;
        bool  m_divReported = false;
        bool  m_divDeathReported = false;   // separate from drift so a death MISMATCH always logs
        bool  m_deathLogged = false;        // death-debug report written this attempt
        // Real player state from the last ALIVE frame — the clean "approach" state, so
        // the death report isn't corrupted by a same-frame pad/portal flip on the kill frame.
        bool  m_prevRealValid = false, m_prevRealUp = false;
        float m_prevRealVel = 0.f, m_prevRealX = 0.f, m_prevRealY = 0.f;
        int   m_prevRealVeh = 0;
        int   m_divDriftCount = 0;          // log the first few drifts, not just one
        bool  m_divBaselined = false;
        float m_divOffX = 0.f, m_divOffY = 0.f;
        // Continuous drift tracking for the death report: WHERE real GD first parted
        // from gdsim (onset), and the worst gap reached. Distinguishes a clean
        // sim-predicted death (no drift) from a desync (real diverged then died).
        uint64_t m_divFirstDriftFrame = 0;  // first frame |drift| exceeded 2u (0 = never)
        float m_divFirstDx = 0.f, m_divFirstDy = 0.f;
        float m_divMaxDx = 0.f, m_divMaxDy = 0.f;  // peak drift (signed, by magnitude)
        uint64_t m_divMaxDriftFrame = 0;
        float m_divRealMaxX = 0.f;          // furthest X the real player reached
        bool  m_truthDeadLogged = false;    // stop appending the (frozen) death frame 60x
    };

    void rebuildIndicatorVisual() {
        auto fields = m_fields.self();
        if (!fields->m_indicator) return;

        fields->m_indicator->removeAllChildrenWithCleanup(true);

        auto ic = fields->m_indicatorColor;
        float barH = static_cast<float>(fields->m_barHeight);
        float indSize = barH + 6.0f;
        float border = 2.0f;
        fields->m_indicator->setContentSize({indSize, indSize});

        if (fields->m_indicatorStyle == 0) {
            // Line style: a crisp vertical playhead with a small bright cap dot on top.
            float lineHeight = std::max(6.0f, barH + 4.0f);
            auto line = CCLayerColor::create(ic, border, lineHeight);
            line->setPosition({-border * 0.5f, -lineHeight * 0.5f});
            fields->m_indicator->addChild(line);
            float cap = border + 2.0f;
            auto capNode = CCLayerColor::create(ccc4(255, 255, 255, ic.a),
                                                cap, cap);
            capNode->setPosition({-cap * 0.5f, lineHeight * 0.5f - 1.0f});
            fields->m_indicator->addChild(capNode);
        } else {
            // Square style: hollow square border + a small filled core, so the
            // playhead reads clearly against busy segments underneath.
            float half = indSize * 0.5f;

            auto top = CCLayerColor::create(ic, indSize, border);
            top->setPosition({-half, half - border});
            fields->m_indicator->addChild(top);

            auto bottom = CCLayerColor::create(ic, indSize, border);
            bottom->setPosition({-half, -half});
            fields->m_indicator->addChild(bottom);

            auto left = CCLayerColor::create(ic, border, indSize);
            left->setPosition({-half, -half});
            fields->m_indicator->addChild(left);

            auto right = CCLayerColor::create(ic, border, indSize);
            right->setPosition({half - border, -half});
            fields->m_indicator->addChild(right);

            float core = indSize * 0.34f;
            auto center = CCLayerColor::create(ccc4(ic.r, ic.g, ic.b, 150),
                                               core, core);
            center->setPosition({-core * 0.5f, -core * 0.5f});
            fields->m_indicator->addChild(center);
        }
    }

    bool captureRuntimeHitboxesForLevel(GJGameLevel* level) {
        if (!level || level->m_levelID <= 0) return false;

        auto isHazardType = [](GameObjectType t) {
            return t == GameObjectType::Hazard || t == GameObjectType::AnimatedHazard;
        };
        auto isSolidType = [](GameObjectType t) {
            return t == GameObjectType::Solid || t == GameObjectType::Slope || t == GameObjectType::CollisionObject;
        };

        ReplayRuntimeHitboxSnapshot snapshot;
        snapshot.levelId = level->m_levelID;
        snapshot.captureMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        snapshot.minX = std::numeric_limits<float>::max();
        snapshot.minY = std::numeric_limits<float>::max();
        snapshot.maxX = -std::numeric_limits<float>::max();
        snapshot.maxY = -std::numeric_limits<float>::max();

        std::unordered_set<GameObject*> seen;
        size_t reserveGuess = static_cast<size_t>(m_solidCollisionObjects.size() + m_hazardCollisionObjects.size());
        if (m_objects) reserveGuess += static_cast<size_t>(m_objects->count());
        if (m_collisionBlocks) reserveGuess += static_cast<size_t>(m_collisionBlocks->count());
        seen.reserve(std::max<size_t>(64, reserveGuess));

        auto collectObject = [&](GameObject* obj, bool sourceSolid, bool sourceHazard) {
            if (!obj) return;

                bool isHazard = sourceHazard || isHazardType(obj->m_objectType);
                bool isSolid = sourceSolid || isSolidType(obj->m_objectType);
                if (obj->m_isPassable) {
                    isSolid = false;
                }
                if (!isSolid && !isHazard) {
                    return;
                }

                auto [_, inserted] = seen.insert(obj);
                if (!inserted) {
                    if (isSolid) snapshot.solidCount++;
                    if (isHazard) snapshot.hazardCount++;
                    return;
                }

                auto rect = obj->getObjectRect();
                if (!std::isfinite(rect.size.width) || !std::isfinite(rect.size.height)) return;
                if (!std::isfinite(rect.origin.x) || !std::isfinite(rect.origin.y)) return;
                if (rect.size.width <= 0.01f || rect.size.height <= 0.01f) return;

                ReplayRuntimeHitboxRect hb;
                hb.objectId = obj->m_objectID;
                hb.objectType = static_cast<int>(obj->m_objectType);
                hb.isSolid = isSolid;
                hb.isHazard = isHazard;

                if (auto* obb = obj->getOrientedBox(); obb) {
                    hb.shape = ReplayRuntimeHitboxRect::Shape::OrientedQuad;
                    hb.corners = obb->m_corners;

                    float minX = hb.corners[0].x;
                    float minY = hb.corners[0].y;
                    float maxX = hb.corners[0].x;
                    float maxY = hb.corners[0].y;
                    for (auto const& c : hb.corners) {
                        minX = std::min(minX, c.x);
                        minY = std::min(minY, c.y);
                        maxX = std::max(maxX, c.x);
                        maxY = std::max(maxY, c.y);
                    }
                    hb.x = (minX + maxX) * 0.5f;
                    hb.y = (minY + maxY) * 0.5f;
                    hb.width = std::max(1.0f, maxX - minX);
                    hb.height = std::max(1.0f, maxY - minY);
                } else {
                    float radius = std::max(obj->m_scaleX, obj->m_scaleY) * obj->m_objectRadius;
                    bool likelyCircle = radius > 0.2f && (hb.objectType == static_cast<int>(GameObjectType::Hazard) ||
                        hb.objectType == static_cast<int>(GameObjectType::AnimatedHazard));

                    if (likelyCircle) {
                        hb.shape = ReplayRuntimeHitboxRect::Shape::Circle;
                        hb.x = obj->getPositionX();
                        hb.y = obj->getPositionY();
                        hb.radius = radius;
                        hb.width = radius * 2.0f;
                        hb.height = radius * 2.0f;
                    } else {
                        hb.shape = ReplayRuntimeHitboxRect::Shape::Rectangle;
                        hb.x = rect.origin.x + rect.size.width * 0.5f;
                        hb.y = rect.origin.y + rect.size.height * 0.5f;
                        hb.width = rect.size.width;
                        hb.height = rect.size.height;
                    }
                }

                snapshot.hitboxes.push_back(hb);

                float left = hb.x - hb.width * 0.5f;
                float right = hb.x + hb.width * 0.5f;
                float bottom = hb.y - hb.height * 0.5f;
                float top = hb.y + hb.height * 0.5f;
                snapshot.minX = std::min(snapshot.minX, left);
                snapshot.maxX = std::max(snapshot.maxX, right);
                snapshot.minY = std::min(snapshot.minY, bottom);
                snapshot.maxY = std::max(snapshot.maxY, top);

                if (isSolid) snapshot.solidCount++;
                if (isHazard) snapshot.hazardCount++;
        };

        auto collectFromVector = [&](gd::vector<GameObject*>& objects, bool sourceSolid, bool sourceHazard) {
            for (auto* obj : objects) {
                collectObject(obj, sourceSolid, sourceHazard);
            }
        };

        auto collectFromArray = [&](cocos2d::CCArray* objects, bool sourceSolid, bool sourceHazard) {
            if (!objects) return;
            auto count = objects->count();
            for (unsigned i = 0; i < count; ++i) {
                auto* obj = typeinfo_cast<GameObject*>(objects->objectAtIndex(i));
                collectObject(obj, sourceSolid, sourceHazard);
            }
        };

        // Broad pass first for full-level coverage, then collision vectors for correctness parity.
        collectFromArray(m_objects, false, false);
        collectFromArray(m_collisionBlocks, true, false);
        collectFromVector(m_solidCollisionObjects, true, false);
        collectFromVector(m_hazardCollisionObjects, false, true);

        if (snapshot.hitboxes.empty()) return false;
        if (!std::isfinite(snapshot.minX) || !std::isfinite(snapshot.maxX)) return false;

        // Dump id -> real hitbox to a plain-text file for offline reference (e.g. the
        // hitbox_test.spwn level, which places every GD object id once so a single
        // playthrough captures the whole table). Decorations never appear here since
        // collectObject() already skips anything that's neither Solid/Slope/
        // CollisionObject nor Hazard/AnimatedHazard by real m_objectType — exactly the
        // triage gdsim's Object::create() factory needs, read from the engine itself
        // instead of guessed from manual play.
        {
            auto sorted = snapshot.hitboxes;
            std::sort(sorted.begin(), sorted.end(),
                [](auto const& a, auto const& b) { return a.objectId < b.objectId; });
            // Write the same table to (a) the legacy global path some offline tools
            // read, and (b) a per-level file in testlevel/movetest/ named by id, so
            // the RE tool keeps a permanent per-level hitbox record. Directory is
            // created first so the write never silently fails.
            std::error_code ec;
            std::filesystem::create_directories(
                "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/movetest/", ec);
            const std::string perLevel = fmt::format(
                "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/movetest/GDMod_hitbox_{}.txt",
                level->m_levelID);
            const std::string latest = gdsim::capturePath("GDMod_hitbox_capture.txt");
            for (const std::string& path : {latest, perLevel}) {
                std::ofstream f(path, std::ios::trunc);
                if (!f.is_open()) continue;
                f << "# GDMod hitbox capture -- level " << level->m_levelID
                  << "  objects=" << sorted.size() << "\n";
                f << "# id type shape x y width height radius gdObjectType\n";
                for (auto const& hb : sorted) {
                    const char* shapeName = hb.shape == ReplayRuntimeHitboxRect::Shape::Circle ? "circle"
                        : hb.shape == ReplayRuntimeHitboxRect::Shape::OrientedQuad ? "obb" : "rect";
                    f << hb.objectId << " "
                      << (hb.isHazard ? "hazard" : (hb.isSolid ? "solid" : "other")) << " "
                      << shapeName << " "
                      << hb.x << " " << hb.y << " " << hb.width << " " << hb.height << " "
                      << hb.radius << " " << hb.objectType << "\n";
                }
            }
        }

        storeRuntimeHitboxSnapshot(std::move(snapshot));
        log::info(
            "[RuntimeHitbox] captured level={} unique={} solidRefs={} hazardRefs={}",
            level->m_levelID,
            seen.size(),
            m_solidCollisionObjects.size(),
            m_hazardCollisionObjects.size()
        );

        if constexpr (ENABLE_RUNTIME_HITBOX_OVERLAY) {
            auto fields = m_fields.self();
            if (!fields->m_runtimeHitboxOverlay && m_debugDrawNode && m_debugDrawNode->getParent()) {
                auto* dn = CCDrawNode::create();
                dn->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
                dn->setID("runtime-hitbox-overlay"_spr);
                dn->m_bUseArea = false;
                m_debugDrawNode->getParent()->addChild(dn, 1408);
                fields->m_runtimeHitboxOverlay = dn;
            }

            if (auto* dn = fields->m_runtimeHitboxOverlay) {
                dn->clear();
                for (auto const& hb : snapshot.hitboxes) {
                    cocos2d::ccColor4F border = hb.isHazard
                        ? cocos2d::ccColor4F{1.0f, 0.30f, 0.30f, 0.95f}
                        : (hb.isSolid ? cocos2d::ccColor4F{0.45f, 0.70f, 1.0f, 0.95f} : cocos2d::ccColor4F{0.85f, 0.85f, 0.85f, 0.80f});
                    cocos2d::ccColor4F fill = hb.isHazard
                        ? cocos2d::ccColor4F{1.0f, 0.30f, 0.30f, 0.18f}
                        : (hb.isSolid ? cocos2d::ccColor4F{0.45f, 0.70f, 1.0f, 0.16f} : cocos2d::ccColor4F{0.85f, 0.85f, 0.85f, 0.10f});

                    if (hb.shape == ReplayRuntimeHitboxRect::Shape::OrientedQuad) {
                        std::array<cocos2d::CCPoint, 4> poly = hb.corners;
                        dn->drawPolygon(poly.data(), 4, fill, 0.35f, border);
                        continue;
                    }

                    if (hb.shape == ReplayRuntimeHitboxRect::Shape::Circle && hb.radius > 0.01f) {
                        dn->drawCircle({hb.x, hb.y}, hb.radius, fill, 0.35f, border, 20);
                        continue;
                    }

                    float halfW = std::max(1.0f, hb.width) * 0.5f;
                    float halfH = std::max(1.0f, hb.height) * 0.5f;
                    std::array<cocos2d::CCPoint, 4> rectPts {
                        cocos2d::CCPoint{hb.x - halfW, hb.y - halfH},
                        cocos2d::CCPoint{hb.x - halfW, hb.y + halfH},
                        cocos2d::CCPoint{hb.x + halfW, hb.y + halfH},
                        cocos2d::CCPoint{hb.x + halfW, hb.y - halfH}
                    };
                    dn->drawPolygon(rectPts.data(), 4, fill, 0.35f, border);
                }
            }
        }

        return true;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        debugLogToFile("=== PlayLayer::init START ===");
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            debugLogToFile("PlayLayer::init FAILED");
            return false;
        }

        auto fields = m_fields.self();
        fields->m_runtimeHitboxCaptured = captureRuntimeHitboxesForLevel(level);
        debugLogToFile("PlayLayer::init succeeded");

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // ========== Replay Player Mode Setup ==========
        if (g_replayPlayer.isActive && g_replayPlayer.replay.has_value()) {
            if (g_replayPlayer.replay->presses.empty() || g_replayPlayer.replay->framerate <= 0.0) {
                log::warn("[ReplaySafety] Ignoring invalid replay payload during PlayLayer init");
                g_replayPlayer.isActive = false;
                g_replayPlayer.replay.reset();
            } else if (g_autoRepairRequested.exchange(false)) {
                // ===== Auto-repair mode: refine the gdsim solution in real GD =====
                fields->m_autoRepairActive = true;
                g_ar = AutoRepairState{};
                g_ar.active     = true;
                g_ar.levelId    = g_replayPlayer.levelId;
                g_ar.levelName  = level ? std::string(level->m_levelName) : std::string();
                g_ar.framerate  = g_replayPlayer.replay->framerate;
                g_ar.base       = g_replayPlayer.replay->presses;
                g_ar.trial      = g_ar.base;
                arNormalize(g_ar.base);
                arNormalize(g_ar.trial);
                g_ar.original   = g_ar.base;   // gdsim's solution, for the final diff
                g_replayPlayer.replay->presses = g_ar.trial;
                // Fresh repair log — records how far off the sim solution was.
                std::ofstream(arLogPath(), std::ios::trunc)
                    << "=== repair log  level=" << g_ar.levelId << "  fps=" << g_ar.framerate << "\n"
                    << "gdsim solution: " << g_ar.base.size() << " clicks  " << arClickList(g_ar.base) << "\n"
                    << "(each line below is an edit the REAL engine needed = where gdsim was wrong)\n";
                s_replayStepHold   = false;
                s_replayStepActive = false;
                s_replayStepTime   = 0.0;
                g_replayExternalCommands.liveReplayActive.store(true);
                g_replayExternalCommands.livePaused.store(false);
                this->togglePracticeMode(false);
                // Headless-ish, same as the (now-removed) window scanner: hide the
                // object render layers so the CPU spends its frame budget on physics
                // substeps, not drawing — lets the raised speedhack above actually land.
                // Collision/hitboxes live in update(), independent of rendering.
                if (this->m_objectLayer)            this->m_objectLayer->setVisible(false);
                if (this->m_inShaderObjectLayer)    this->m_inShaderObjectLayer->setVisible(false);
                if (this->m_aboveShaderObjectLayer) this->m_aboveShaderObjectLayer->setVisible(false);
                CCDirector::sharedDirector()->getScheduler()->setTimeScale(s_autoRepairSpeed);
                log::info("[AR] start: {} seed clicks, level {}, speed {}x",
                          g_ar.base.size(), g_ar.levelId, s_autoRepairSpeed);
            } else if (g_confirmRequested.exchange(false)) {
                // ===== Confirm-only mode: ONE real-game pass, no repair loop =====
                // The .gdr2 was already written before play() was called (doSimulate).
                // This just runs the solve once for real, at speed, so a divergence
                // from gdsim gets logged/captured (updateDivergenceDetector already
                // writes a full truth capture for ANY active replay) instead of going
                // undetected — but nothing here can withhold or retry the .gdr2.
                fields->m_confirmOnlyActive = true;
                fields->m_confirmDone       = false;
                s_replayStepHold   = false;
                s_replayStepActive = false;
                s_replayStepTime   = 0.0;
                g_replayExternalCommands.liveReplayActive.store(true);
                g_replayExternalCommands.livePaused.store(false);
                this->togglePracticeMode(false);
                if (this->m_objectLayer)            this->m_objectLayer->setVisible(false);
                if (this->m_inShaderObjectLayer)    this->m_inShaderObjectLayer->setVisible(false);
                if (this->m_aboveShaderObjectLayer) this->m_aboveShaderObjectLayer->setVisible(false);
                CCDirector::sharedDirector()->getScheduler()->setTimeScale(s_autoRepairSpeed);
                log::info("[Confirm] start: level {}, speed {}x", g_replayPlayer.levelId, s_autoRepairSpeed);
            }
        }

        log::info("[IsMyModUpdated] 1"); // increment this when making changes to force users to update their local replay JSON files (if needed)

        bool setting = Mod::get()->getSettingValue<bool>("show-inputs-ingame");
        log::info("[InputCircles] Setting show-inputs-ingame = {}", setting);
        if (!setting) return true;

        int levelId = level->m_levelID;
        log::info("[InputCircles] Level ID = {}", levelId);

        // Load replay for visualization (input circles mode)
        {
            auto loadResult = loadMatchingReplay(levelId);
            if (!loadResult.has_value()) {
                log::info("[InputCircles] No matching replay found for level {}", levelId);
                return true;
            }

            log::info("[InputCircles] Loaded {} presses, framerate={}", loadResult->presses.size(), loadResult->framerate);

            fields->m_presses = std::move(loadResult->presses);
            fields->m_framerate = loadResult->framerate;
            fields->m_active = true;
        }

        // Read settings
        fields->m_barHeight = Mod::get()->getSettingValue<int64_t>("bar-height");
        fields->m_barY = Mod::get()->getSettingValue<int64_t>("bar-y-position");
        auto p1c = Mod::get()->getSettingValue<ccColor4B>("player1-color");
        auto p2c = Mod::get()->getSettingValue<ccColor4B>("player2-color");
        auto ic = Mod::get()->getSettingValue<ccColor4B>("indicator-color");
        bool indicatorAsLine = Mod::get()->getSettingValue<bool>("indicator-style-line");
        auto bgc = Mod::get()->getSettingValue<ccColor4B>("background-color");
        fields->m_p1Color = p1c;
        fields->m_p2Color = p2c;
        fields->m_indicatorColor = ic;
        fields->m_indicatorStyle = indicatorAsLine ? 0 : 1;
        fields->m_bgColor = bgc;

        // Create background strip spanning the full screen width at bar height
        float initBarH = static_cast<float>(fields->m_barHeight);
        float initBottomY = static_cast<float>(fields->m_barY);
        auto gameplayWinSize = CCDirector::sharedDirector()->getWinSize();
        auto bgStrip = CCLayerColor::create(bgc, gameplayWinSize.width, initBarH);
        bgStrip->setPosition({0.0f, initBottomY - initBarH / 2.0f});
        bgStrip->setID("input-bg-strip"_spr);
        this->addChild(bgStrip, 9999);
        fields->m_bgStrip = bgStrip;
        // Subtle top/bottom hairlines so the track reads as a clean, defined bar
        // (instead of a flat translucent rectangle bleeding into the level).
        {
            auto topLine = CCLayerColor::create(ccc4(255, 255, 255, 40),
                                                gameplayWinSize.width, 1.0f);
            topLine->setPosition({0.0f, initBarH - 1.0f});
            bgStrip->addChild(topLine, 1);
            auto botLine = CCLayerColor::create(ccc4(0, 0, 0, 90),
                                                gameplayWinSize.width, 1.0f);
            botLine->setPosition({0.0f, 0.0f});
            bgStrip->addChild(botLine, 1);
        }

        // Create a layer for dots, added to PlayLayer directly (screen space, high Z)
        auto circleLayer = CCNode::create();
        circleLayer->setID("input-circles-layer"_spr);
        this->addChild(circleLayer, 10000);
        fields->m_circleLayer = circleLayer;

        // Create indicator container. Visual style is built by rebuildIndicatorVisual().
        float indSize = static_cast<float>(fields->m_barHeight) + 6.0f;
        auto indicator = CCNode::create();
        indicator->setContentSize({indSize, indSize});
        indicator->setAnchorPoint({0.5f, 0.5f});

        this->addChild(indicator, 10001);
        fields->m_indicator = indicator;
        rebuildIndicatorVisual();

        return true;
    }

    // Steps a parallel gdsim Level alongside the live replay and reports the
    // first frame where the simulated player position diverges from the real
    // one — pinpoints exactly which physics feature the simulator gets wrong.
    static std::string divLogPath() { return gdsim::debugPath("GDMod_divergence.txt"); }
    void divLog(const std::string& s) {
        log::warn("[DIV] {}", s);
        std::ofstream f(divLogPath(), std::ios::app);
        if (f.is_open()) f << s << "\n";
    }

    static std::string deathLogPath() { return gdsim::debugPath("GDMod_death_debug.txt"); }
    static void deathLog(const std::string& s) {
        log::warn("[DEATH] {}", s);
        std::ofstream f(deathLogPath(), std::ios::app);
        if (f.is_open()) f << s << "\n";
    }
    // Human-readable name for the common object IDs that kill you.
    static const char* objName(int id) {
        switch (id) {
            case 8: case 39: case 88: case 89: case 98: case 216: case 217: case 218: return "spike";
            case 9: case 61: case 243: case 244: return "spike/saw";
            case 135: case 1734: case 1705: case 1706: case 1707: return "sawblade";
            case 1: case 2: case 3: case 4: case 5: case 6: case 7: return "block";
            case 35: return "yellow pad"; case 67: return "blue pad";
            case 140: return "pink pad"; case 1332: return "red pad";
            case 36: return "yellow orb"; case 84: return "blue orb"; case 141: return "pink orb";
            case 1333: return "red orb"; case 1022: return "green orb"; case 1330: return "black orb";
            case 10: case 11: return "gravity portal";
            case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1933: return "vehicle portal";
            case 200: case 201: case 202: case 203: case 1334: return "speed portal";
            default: return "?";
        }
    }

    void updateDivergenceDetector(uint64_t liveFrame) {
        auto fields = m_fields.self();
        if (!m_player1 || !g_replayPlayer.replay.has_value()) return;
        auto& replay = g_replayPlayer.replay.value();

        if (!fields->m_divInit) {
            fields->m_divInit = true; // attempt once per level load
            if (!m_level) return;
            // Apply calibrated physics so the detector's sim reflects the tuned
            // model (lets us verify a calibration pass in-game).
            gdsim::loadCalibFromFile(gdsim::configPath("GDMod_calib.txt").c_str());

            std::string raw = m_level->m_levelString;
            std::string lvl = raw.find(';') != std::string::npos
                ? raw
                : std::string(cocos2d::ZipUtils::decompressString(gd::string(raw), false, 0));

            std::ofstream(divLogPath(), std::ios::trunc)
                << "=== divergence log  level=" << (m_level ? m_level->m_levelID : 0)
                << "  presses=" << replay.presses.size()
                << "  fps=" << replay.framerate
                << "  rawLen=" << raw.size() << "  lvlLen=" << lvl.size() << "\n";

            // Fresh death-debug log for this watch attempt ("Test" feature).
            fields->m_deathLogged = false;
            std::ofstream(deathLogPath(), std::ios::trunc)
                << "=== death-debug log  level=" << (m_level ? m_level->m_levelID : 0)
                << "  presses=" << replay.presses.size() << "  fps=" << replay.framerate << "\n"
                << "(one report is written the first time the player dies during this watch)\n";

            if (lvl.empty() || lvl.find(';') == std::string::npos) {
                divLog("ABORT: could not decompress level string");
                return;
            }
            try {
                fields->m_divSim = std::make_shared<gdsim::Level>(lvl);
            } catch (...) { divLog("ABORT: Level ctor threw"); fields->m_divSim = nullptr; return; }

            uint64_t maxF = 16;
            for (auto& p : replay.presses) maxF = std::max(maxF, p.frameRelease);
            fields->m_divInput.assign(maxF + 4, false);
            for (auto& p : replay.presses)
                if (p.player == 1)
                    for (uint64_t f = p.framePress; f < p.frameRelease && f < fields->m_divInput.size(); ++f)
                        fields->m_divInput[f] = true;

            fields->m_divFrame = 0;
            fields->m_divReported = false;
            fields->m_divDeathReported = false;
            fields->m_divDriftCount = 0;
            fields->m_divBaselined = false;
            fields->m_divFirstDriftFrame = 0;
            fields->m_divFirstDx = fields->m_divFirstDy = 0.f;
            fields->m_divMaxDx = fields->m_divMaxDy = 0.f;
            fields->m_divMaxDriftFrame = 0;
            fields->m_divRealMaxX = 0.f;
            fields->m_truthDeadLogged = false;
            divLog(fmt::format("armed: sections={} maxInputFrame={}",
                               fields->m_divSim->sections.size(), maxF));

            // Open the calibration ground-truth file and write its header (level
            // + inputs). Per-frame real states are appended below each tick.
            int lid = m_level ? m_level->m_levelID : 0;
            s_calibTruth = std::ofstream(
                gdsim::capturePath(fmt::format("GDMod_truth_{}.txt", lid)), std::ios::trunc);
            if (s_calibTruth.is_open()) {
                s_calibTruth << "LEVELID " << lid << "\n";
                s_calibTruth << "FPS " << (int)replay.framerate << "\n";
                s_calibTruth << "NPRESS " << replay.presses.size() << "\n";
                for (auto& p : replay.presses)
                    s_calibTruth << p.framePress << ' ' << p.frameRelease << ' ' << p.player << "\n";
                s_calibTruth << "LVL " << lvl << "\n";
                // Per-frame REAL state. Columns: frame x y dead vehId mini yVel speed
                // (yVel = PlayerObject::m_yVelocity, the engine's true Y velocity BEFORE
                // position rounding/clamping; speed = m_playerSpeed). Capturing velocity
                // — not just position — lets analysis pin a divergence to its source one
                // frame before it shows up as a position gap. Trailing columns are
                // ignored by the older 6-field parsers, so this stays backward-compatible.
                s_calibTruth << "FRAMES\n";
                s_calibTruth.flush();
            }
        }
        if (!fields->m_divSim) return;

        // Seek/restart: replay time jumped backward — resync the sim.
        if (liveFrame < fields->m_divFrame) {
            fields->m_divSim->rollback(0);
            fields->m_divFrame = 0;
            fields->m_divReported = false;
            fields->m_divDeathReported = false;
            fields->m_divDriftCount = 0;
            fields->m_divBaselined = false;
            fields->m_divFirstDriftFrame = 0;
            fields->m_divFirstDx = fields->m_divFirstDy = 0.f;
            fields->m_divMaxDx = fields->m_divMaxDy = 0.f;
            fields->m_divMaxDriftFrame = 0;
            fields->m_divRealMaxX = 0.f;
            fields->m_truthDeadLogged = false;
        }

        const float dt = 1.f / (float)(replay.framerate > 0.0 ? replay.framerate : 240.0);
        uint64_t target = std::min(liveFrame, fields->m_divFrame + 4000); // cap catch-up
        while (fields->m_divFrame < target) {
            uint64_t f = fields->m_divFrame + 1;
            bool pressed = f < fields->m_divInput.size() && fields->m_divInput[f];
            fields->m_divSim->runFrame(pressed, dt);
            fields->m_divFrame = f;
        }
        if (fields->m_divFrame == 0 || fields->m_divSim->gameStates.empty()) return;

        const auto& simP = fields->m_divSim->gameStates.back();
        float simX = simP.pos.x, simY = simP.pos.y;
        bool  simDead = simP.dead;
        float realX = m_player1->getPositionX();
        float realY = m_player1->getPositionY();
        bool  realDead = m_player1->m_isDead;

        // Remember the last ALIVE real state — the death report uses this as the clean
        // "approach" so a same-frame pad/portal flip on the kill frame can't corrupt it.
        if (!realDead) {
            fields->m_prevRealValid = true;
            fields->m_prevRealUp  = m_player1->m_isUpsideDown;
            fields->m_prevRealVel = m_player1->m_yVelocity;
            fields->m_prevRealX = realX; fields->m_prevRealY = realY;
            fields->m_prevRealVeh = m_player1->m_isShip?1:m_player1->m_isBall?2:m_player1->m_isBird?3
                                  :m_player1->m_isDart?4:m_player1->m_isRobot?5:m_player1->m_isSpider?6
                                  :m_player1->m_isSwing?7:0;
        }

        // Establish the constant coordinate offset on the first compared frame.
        if (!fields->m_divBaselined) {
            fields->m_divOffX = realX - simX;
            fields->m_divOffY = realY - simY;
            fields->m_divBaselined = true;
            divLog(fmt::format("baseline frame={} sim=({:.1f},{:.1f}) real=({:.1f},{:.1f}) offset=({:.1f},{:.1f})",
                               liveFrame, simX, simY, realX, realY, fields->m_divOffX, fields->m_divOffY));
        }
        float dx = (realX - fields->m_divOffX) - simX;
        float dy = (realY - fields->m_divOffY) - simY;

        // Continuous drift tracking (feeds the death report's divergence summary).
        fields->m_divRealMaxX = std::max(fields->m_divRealMaxX, realX);
        {
            float adx = std::fabs(dx), ady = std::fabs(dy);
            if (std::max(adx, ady) > 2.0f && fields->m_divFirstDriftFrame == 0) {
                fields->m_divFirstDriftFrame = liveFrame;
                fields->m_divFirstDx = dx; fields->m_divFirstDy = dy;
            }
            float mag  = std::max(adx, ady);
            float peak = std::max(std::fabs(fields->m_divMaxDx), std::fabs(fields->m_divMaxDy));
            if (mag > peak) {
                fields->m_divMaxDx = dx; fields->m_divMaxDy = dy;
                fields->m_divMaxDriftFrame = liveFrame;
            }
        }

        static const char* kVeh[] = {"Cube","Ship","Ball","Ufo","Wave","Robot","Spider","Swing"};
        int svi = (int)simP.vehicle.type;
        const char* simVeh = (svi >= 0 && svi < 8) ? kVeh[svi] : "?";
        // Order/ids must match the sim's kVeh above. m_isBall MUST be tested — without
        // it a real Ball falls through to "Cube" (id 0), so every ball section showed a
        // bogus "simVeh=Ball realVeh=Cube" divergence and the truth file mislabelled balls.
        const char* realVeh = m_player1->m_isShip ? "Ship" : m_player1->m_isBall ? "Ball"
                            : m_player1->m_isBird ? "Ufo" : m_player1->m_isDart ? "Wave"
                            : m_player1->m_isRobot ? "Robot" : m_player1->m_isSpider ? "Spider"
                            : m_player1->m_isSwing ? "Swing" : "Cube";

        // Pinpoint the exact frame either side changes vehicle or mini-state, so a
        // few-frame portal-timing lag between sim and real is visible.
        {
            static std::string s_pVeh, s_pRVeh;
            static int s_pSmall = -1, s_pRMini = -1;
            int curRealMini = (m_player1->m_vehicleSize < 0.8f) ? 1 : 0;
            int curSmall = simP.small ? 1 : 0;
            if (simVeh != s_pVeh || realVeh != s_pRVeh || curSmall != s_pSmall || curRealMini != s_pRMini) {
                divLog(fmt::format("  [trans] f={} realX={:.0f} simVeh={} realVeh={} simSmall={} realMini={} dy={:.1f} simVel={:.1f} realVel={:.1f}",
                                   liveFrame, realX, simVeh, realVeh, curSmall, curRealMini, dy,
                                   simP.velocity, m_player1->m_yVelocity));
                s_pVeh = simVeh; s_pRVeh = realVeh; s_pSmall = curSmall; s_pRMini = curRealMini;
            }
        }

        // Append this frame's REAL state to the calibration ground-truth file. Once the
        // player is dead the position freezes, so log the first dead frame then stop —
        // otherwise the same death frame is appended every tick (bloated / truncated file).
        if (s_calibTruth.is_open() && !fields->m_truthDeadLogged) {
            int rVehId = m_player1->m_isShip ? 1 : m_player1->m_isBall ? 2
                       : m_player1->m_isBird ? 3 : m_player1->m_isDart ? 4
                       : m_player1->m_isRobot ? 5 : m_player1->m_isSpider ? 6
                       : m_player1->m_isSwing ? 7 : 0;
            int rMini = (m_player1->m_vehicleSize < 0.8f) ? 1 : 0;
            s_calibTruth << liveFrame << ' ' << realX << ' ' << realY << ' '
                         << (realDead ? 1 : 0) << ' ' << rVehId << ' ' << rMini << ' '
                         << m_player1->m_yVelocity << ' ' << m_player1->m_playerSpeed << "\n";
            if (realDead) { fields->m_truthDeadLogged = true; s_calibTruth.flush(); }
        }

        // Heartbeat (every ~0.25s) — vehicle + velocity + proof the per-step
        // injection path is live (procN ticks, injN button edges).
        if (liveFrame % 60 == 0)
            divLog(fmt::format("f={} realX={:.0f} dy={:.1f} dx={:.1f} simVeh={} realVeh={} simVel={:.1f} realVel={:.1f} simSmall={} realSz={:.2f} procN={} injN={} simDead={} realDead={}",
                               liveFrame, realX, dy, dx, simVeh, realVeh,
                               simP.velocity, m_player1->m_yVelocity,
                               simP.small ? 1 : 0, m_player1->m_vehicleSize,
                               s_perStepProcessCount.load(), s_perStepInjectCount.load(),
                               simDead, realDead));

        // Death mismatch — the player died in-game but the sim thinks it survived
        // (or vice-versa). This catches hitbox/collision divergences with no drift.
        if (realDead != simDead && !fields->m_divDeathReported) {
            fields->m_divDeathReported = true;
            divLog(fmt::format("*** DEATH MISMATCH ({}) frame={} realX={:.0f} simVeh={} realVeh={}  realDead={} simDead={}  sim=({:.1f},{:.1f}) real=({:.1f},{:.1f}) diff=({:.1f},{:.1f})",
                               realDead ? "REAL died, gdsim SURVIVED (gdsim hazard too small/missing)" : "gdsim died, REAL survived (gdsim hazard too big)",
                               liveFrame, realX, simVeh, realVeh, realDead, simDead, simX, simY,
                               realX - fields->m_divOffX, realY - fields->m_divOffY, dx, dy));
            // Dump the sim's player hitbox + every nearby hazard so we can tell a
            // MISSING hazard from a too-small one (sim survived where real died).
            if (fields->m_divSim) {
                divLog(fmt::format("  sim player box L={:.1f} R={:.1f} B={:.1f} T={:.1f}  size=({:.1f},{:.1f})",
                                   simP.getLeft(), simP.getRight(), simP.getBottom(), simP.getTop(),
                                   simP.size.x, simP.size.y));
                auto& secs = fields->m_divSim->sections;
                int si = std::clamp((int)(simP.pos.x / (float)gdsim::Level::sectionSize), 0, (int)secs.size() - 1);
                for (int s = std::max(0, si - 1); s <= std::min((int)secs.size() - 1, si + 1); ++s) {
                    for (auto& oc : secs[s]) {
                        const gdsim::Object* o = oc.operator->();
                        if (o->prio != 2) continue;                     // hazards only
                        if (std::abs(o->pos.x - simP.pos.x) > 60.f) continue;
                        float gapX = std::max({0.f, o->getLeft() - simP.getRight(), simP.getLeft() - o->getRight()});
                        float gapY = std::max({0.f, o->getBottom() - simP.getTop(), simP.getBottom() - o->getTop()});
                        divLog(fmt::format("  hazard type={} pos=({:.1f},{:.1f}) size=({:.1f},{:.1f}) rot={:.0f} box[L={:.1f} R={:.1f} B={:.1f} T={:.1f}] gap=({:.2f},{:.2f})",
                                           o->typeId, o->pos.x, o->pos.y, o->size.x, o->size.y, o->rotation,
                                           o->getLeft(), o->getRight(), o->getBottom(), o->getTop(), gapX, gapY));
                    }
                }
            }
        }

        if (fields->m_divDriftCount < 5 && (std::abs(dx) > 8.f || std::abs(dy) > 8.f)) {
            fields->m_divDriftCount++;
            fields->m_divReported = true;
            divLog(fmt::format("*** DRIFT #{} frame={} realX={:.0f} simVeh={} realVeh={}  sim=({:.1f},{:.1f}) real=({:.1f},{:.1f}) diff=({:.1f},{:.1f})  simVel={:.1f} realVel={:.1f} procN={} injN={}",
                               fields->m_divDriftCount,
                               liveFrame, realX, simVeh, realVeh, simX, simY,
                               realX - fields->m_divOffX, realY - fields->m_divOffY, dx, dy,
                               simP.velocity, m_player1->m_yVelocity,
                               s_perStepProcessCount.load(), s_perStepInjectCount.load()));
        }
    }

    // Load the current trial's inputs and restart the level for a fresh attempt.
    void arBeginTrial() {
        if (g_replayPlayer.replay.has_value())
            g_replayPlayer.replay->presses = g_ar.trial;
        g_ar.trialMaxX = 0.f;
        g_ar.trialDied = false;
        s_replayStepHold   = false;
        s_replayStepActive = false;
        // Force the divergence detector to re-arm against THIS trial's clicks — its
        // init latch (m_divInit) otherwise only fires once per level load, so every
        // retry after the first would keep comparing against trial 1's stale input
        // (and never write a fresh GDMod_truth_<id>.txt for the trial actually running).
        m_fields->m_divInit = false;
        this->resetLevel();
    }

    // A trial finished (died or completed). Update the frontier, pick the next edit,
    // and either restart, finish (solved), or give up (stuck) — saving the best run.
    void arOnTrialEnd(bool success) {
        auto fields = m_fields.self();
        if (g_ar.done) return;

        if (success) {
            ReplayLoadResult out;
            out.framerate = g_ar.framerate;
            out.presses   = g_ar.trial;
            saveReplayToLocalJson(g_ar.levelId, out);
            // This is the ONLY point that ever writes a .gdr2 for a solved level: the
            // solution has now actually completed in the real engine (possibly after
            // local edits above), not just in gdsim's headless model.
            std::string gdr2Path = writeSolvedGdr2(g_ar.levelId, g_ar.levelName, out);
            g_ar.done = true;
            fields->m_autoRepairActive = false;
            g_replayExternalCommands.liveReplayActive.store(false);
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
            if (this->m_objectLayer)            this->m_objectLayer->setVisible(true);
            if (this->m_inShaderObjectLayer)    this->m_inShaderObjectLayer->setVisible(true);
            if (this->m_aboveShaderObjectLayer) this->m_aboveShaderObjectLayer->setVisible(true);
            log::info("[AR] SOLVED in {} trials, {} clicks", g_ar.attempts, g_ar.trial.size());
            // Log the FINAL, level-clearing edit too. Without this the winning
            // correction was invisible: the last "fix #" line stopped short of the end
            // and the correction/frame totals under-counted by one, so they disagreed
            // with the saved "real solution". Count it before the summary prints.
            if (g_ar.lastOpValid) {
                g_ar.numCorrections++;
                g_ar.totalAdjustFrames += arOpAdjustFrames(g_ar.lastOp);
                arLog(fmt::format("fix #{}: {}  -> got past x={:.0f}, now CLEARS the level (x={:.0f})",
                                  g_ar.numCorrections, arOpStr(g_ar.lastOp), g_ar.bestX, g_ar.trialMaxX));
                g_ar.lastOpValid = false;
            }
            arLog("");
            arLog(fmt::format("=== SOLVED in {} trials.  gdsim needed {} correction(s), "
                              "{} frames of total adjustment (~{:.1f} ms @ {}fps) ===",
                              g_ar.attempts, g_ar.numCorrections, g_ar.totalAdjustFrames,
                              1000.0 * g_ar.totalAdjustFrames / (g_ar.framerate > 0 ? g_ar.framerate : 240.0),
                              (int)g_ar.framerate));
            arLog(fmt::format("gdsim solution : {}", arClickList(g_ar.original)));
            arLog(fmt::format("real solution  : {}", arClickList(g_ar.trial)));
            arLog(g_ar.numCorrections == 0
                  ? "=> gdsim was PERFECT here (no edits needed)."
                  : "=> the edits above are exactly where/by how much the simulator diverged from real GD.");

            // ── Click alignment: per-click net shift (real press - gdsim press).
            // A CONSISTENT nonzero shift across clicks = a systematic solver/replay
            // timing offset (the solver's clicks all land a fixed number of frames
            // early/late vs real GD), which is fixable at the source — not per-obstacle
            // noise. Clicks that didn't move just had enough slack to survive the offset.
            if (g_ar.original.size() == g_ar.trial.size() && !g_ar.original.empty()) {
                arLog("");
                arLog("=== click alignment (real press frame - gdsim press frame) ===");
                std::vector<int> deltas;
                std::string ls;
                for (size_t i = 0; i < g_ar.trial.size(); ++i) {
                    int d = (int)g_ar.trial[i].framePress - (int)g_ar.original[i].framePress;
                    deltas.push_back(d);
                    ls += fmt::format("{}{:+d}", i ? "," : "", d);
                    arLog(fmt::format("  click #{}: gdsim f{} -> real f{}   ({:+d})",
                                      i + 1, g_ar.original[i].framePress, g_ar.trial[i].framePress, d));
                }
                std::vector<int> nz;
                for (int d : deltas) if (d != 0) nz.push_back(d);
                arLog(fmt::format("net shifts: [{}]", ls));
                if (nz.empty()) {
                    arLog("=> every click matched gdsim exactly — no timing offset on this level.");
                } else {
                    int mn = *std::min_element(nz.begin(), nz.end());
                    int mx = *std::max_element(nz.begin(), nz.end());
                    if (mn == mx)
                        arLog(fmt::format("=> SYSTEMATIC OFFSET: every adjusted click moved exactly {:+d} frame(s). "
                                          "The solver's clicks run {} frame(s) too {} vs real GD — a fixable timing "
                                          "bias at the source, not per-obstacle noise. The unmoved clicks just had "
                                          "enough slack to survive the same offset.",
                                          mn, std::abs(mn), mn > 0 ? "early" : "late"));
                    else
                        arLog(fmt::format("=> shifts range {:+d}..{:+d} — partly systematic, partly per-obstacle. "
                                          "Most common nonzero shift is the likely constant offset; make another "
                                          "level to confirm the constant part.", mn, mx));
                }
            }
            size_t n = g_ar.trial.size();
            int a = g_ar.attempts;
            Loader::get()->queueInMainThread([n, a, gdr2Path]() {
                FLAlertLayer::create("Solve",
                    fmt::format(
                        "<cg>Validated in the real engine!</c>\n{} clicks, {} trials.\n{}",
                        n, a,
                        gdr2Path.empty()
                            ? std::string("<cr>Failed to write .gdr2.</c>")
                            : fmt::format("<cy>.gdr2 saved:</c>\n{}", gdr2Path)
                    ).c_str(), "OK")->show();
            });
            return;
        }

        // Died this trial.
        g_ar.attempts++;
        bool progressed = g_ar.trialMaxX > g_ar.bestX + 3.f;
        if (g_ar.firstTrial) {
            arLog(fmt::format("trial 1 (gdsim as-is): reached x={:.0f}, DIED at frame {} "
                              "-- this is where the sim was wrong (it thought this survived)",
                              g_ar.trialMaxX, g_ar.lastDeathFrame));
            g_ar.base = g_ar.trial; g_ar.bestX = g_ar.trialMaxX; g_ar.firstTrial = false;
            arBuildCandidates();
        } else if (progressed) {
            if (g_ar.lastOpValid) {
                g_ar.numCorrections++;
                g_ar.totalAdjustFrames += arOpAdjustFrames(g_ar.lastOp);
                arLog(fmt::format("fix #{}: {}  -> got past x={:.0f}, now reaches x={:.0f}",
                                  g_ar.numCorrections, arOpStr(g_ar.lastOp), g_ar.bestX, g_ar.trialMaxX));
                // Update momentum tracking (see AutoRepairState::momentum* comment).
                const auto& op = g_ar.lastOp;
                if ((op.type == AROp::Hold || op.type == AROp::ShiftPress) && op.delta != 0) {
                    int sign = op.delta > 0 ? 1 : -1;
                    if (op.type == g_ar.momentumType && op.idx == g_ar.momentumIdx && sign == g_ar.momentumSign) {
                        g_ar.momentumStreak++;
                    } else {
                        g_ar.momentumType = op.type; g_ar.momentumIdx = op.idx;
                        g_ar.momentumSign = sign; g_ar.momentumStreak = 1;
                    }
                    g_ar.momentumLastDelta = op.delta;
                } else {
                    g_ar.momentumStreak = 0; g_ar.momentumIdx = -1;
                }
            }
            g_ar.base  = g_ar.trial;
            g_ar.bestX = std::max(g_ar.bestX, g_ar.trialMaxX);
            arBuildCandidates();
        }

        if (g_ar.candIdx >= g_ar.cands.size() || g_ar.attempts >= g_ar.maxAttempts) {
            // Exhausted local edits without breaking through — save the best run.
            ReplayLoadResult out;
            out.framerate = g_ar.framerate;
            out.presses   = g_ar.base;
            saveReplayToLocalJson(g_ar.levelId, out);
            g_ar.done = true;
            fields->m_autoRepairActive = false;
            g_replayExternalCommands.liveReplayActive.store(false);
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
            if (this->m_objectLayer)            this->m_objectLayer->setVisible(true);
            if (this->m_inShaderObjectLayer)    this->m_inShaderObjectLayer->setVisible(true);
            if (this->m_aboveShaderObjectLayer) this->m_aboveShaderObjectLayer->setVisible(true);
            log::info("[AR] STUCK at x={:.0f} after {} trials", g_ar.bestX, g_ar.attempts);
            arLog("");
            arLog(fmt::format("=== STUCK at x={:.0f} after {} trials.  {} correction(s) applied so far, "
                              "{} frames adjusted.  Real GD couldn't get past frame {} with local edits ===",
                              g_ar.bestX, g_ar.attempts, g_ar.numCorrections, g_ar.totalAdjustFrames,
                              g_ar.lastDeathFrame));
            arLog(fmt::format("gdsim solution : {}", arClickList(g_ar.original)));
            arLog(fmt::format("best real run  : {}", arClickList(g_ar.base)));
            float x = g_ar.bestX;
            int a = g_ar.attempts;
            Loader::get()->queueInMainThread([x, a]() {
                FLAlertLayer::create("Solve",
                    fmt::format("<cr>Stuck at x={:.0f} after {} trials.</c>\n"
                                "No .gdr2 written — best partial run saved locally.\n"
                                "See GDMod_repair_log.txt for exactly where gdsim's\n"
                                "solution parted ways with the real engine.",
                                x, a).c_str(), "OK")->show();
            });
            return;
        }

        AROp op = g_ar.cands[g_ar.candIdx++];
        g_ar.lastOp = op; g_ar.lastOpValid = true;
        g_ar.trial = arApply(g_ar.base, op);
        this->arBeginTrial();
    }

    // Confirm-only mode's single completion point (death or level-complete) — the
    // non-blocking counterpart to arOnTrialEnd. The .gdr2 was already written before
    // this pass started (doSimulate); this only reports the outcome and restores
    // normal playback. On failure it does NOT retry — updateDivergenceDetector has
    // already captured a full truth file (GDMod_truth_<levelId>.txt under
    // testlevel/captures/) for this run, which is exactly the input test/regress.sh
    // --hunt needs to turn "it didn't clear" into a concrete, fixable divergence.
    void confirmOnlyFinish(bool success) {
        auto fields = m_fields.self();
        if (fields->m_confirmDone) return;
        fields->m_confirmDone       = true;
        fields->m_confirmOnlyActive = false;
        g_replayExternalCommands.liveReplayActive.store(false);
        CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
        if (this->m_objectLayer)            this->m_objectLayer->setVisible(true);
        if (this->m_inShaderObjectLayer)    this->m_inShaderObjectLayer->setVisible(true);
        if (this->m_aboveShaderObjectLayer) this->m_aboveShaderObjectLayer->setVisible(true);

        float x = m_player1 ? m_player1->getPositionX() : 0.f;
        log::info("[Confirm] {}", success ? "CLEARED for real" : fmt::format("diverged near x={:.0f}", x));
        std::string msg = success
            ? "Confirmed: clears in real GD."
            : fmt::format("Diverged from real GD near x={:.0f} — .gdr2 already saved; "
                          "captured to testlevel/captures/ for a future fidelity fix "
                          "(or toggle Repair and re-solve to force a real clear now).", x);
        auto icon = success ? NotificationIcon::Success : NotificationIcon::Warning;
        Loader::get()->queueInMainThread([msg, icon]() {
            Notification::create(msg, icon, 4.f)->show();
        });
    }

    void postUpdate(float dt) {
        traceDebug("[POSTDBG-ENTRY] postUpdate called dt={}", dt);
        PlayLayer::postUpdate(dt);

        // ===== Auto-repair driver: fast-forward, watch for death/finish =====
        if (m_fields->m_autoRepairActive && !g_ar.done) {
            auto sched = CCDirector::sharedDirector()->getScheduler();
            sched->setTimeScale(s_autoRepairSpeed);
            g_replayExternalCommands.liveReplayActive.store(true);
            g_replayExternalCommands.livePaused.store(false);
            // Compare gdsim's prediction against this trial's REAL trajectory frame by
            // frame — a physics-fidelity check that comes for free with every trial
            // (see arBeginTrial, which resets m_divInit so each trial gets its own
            // fresh comparison instead of reusing trial 1's).
            if (g_ar.framerate > 0.0) {
                uint64_t liveFrame = (uint64_t)std::llround((double)m_timePlayed * g_ar.framerate);
                updateDivergenceDetector(liveFrame);
            }
            if (m_player1) {
                float x = m_player1->getPositionX();
                if (x > g_ar.trialMaxX) g_ar.trialMaxX = x;
                if (m_player1->m_isDead && !g_ar.trialDied) {
                    g_ar.trialDied = true;
                    g_ar.lastDeathFrame = (uint64_t)std::llround(
                        (double)m_timePlayed * g_ar.framerate);
                    this->arOnTrialEnd(false);
                }
            }
            return;
        }

        // ===== Confirm-only driver: ONE pass, report death/finish, never retry =====
        if (m_fields->m_confirmOnlyActive && !m_fields->m_confirmDone) {
            auto sched = CCDirector::sharedDirector()->getScheduler();
            sched->setTimeScale(s_autoRepairSpeed);
            g_replayExternalCommands.liveReplayActive.store(true);
            g_replayExternalCommands.livePaused.store(false);
            if (g_replayPlayer.replay.has_value() && g_replayPlayer.replay->framerate > 0.0) {
                uint64_t liveFrame = (uint64_t)std::llround(
                    (double)m_timePlayed * g_replayPlayer.replay->framerate);
                updateDivergenceDetector(liveFrame);
            }
            if (m_player1 && m_player1->m_isDead) {
                this->confirmOnlyFinish(false);
            }
            return;
        }

        auto fields = m_fields.self();
        if (!fields->m_runtimeHitboxCaptured && m_level && m_objects && m_objects->count() > 0) {
            fields->m_runtimeHitboxCaptured = captureRuntimeHitboxesForLevel(m_level);
        }
        g_replayExternalCommands.liveReplayActive.store(false);


        // Debug counter – log every ~120 frames. BEFORE any early return!
        static int s_dbg = 0;
        s_dbg++;
        bool dbg = (s_dbg % 120 == 1);

        if (dbg) log::info("[DBG] === postUpdate CALLED === frame={}", s_dbg);

        if (!fields->m_active || !fields->m_circleLayer) {
            if (dbg) log::info("[DBG] EARLY EXIT: active={} circleLayer={}",
                fields->m_active, fields->m_circleLayer ? "yes" : "NULL");
            return;
        }

        auto player = m_player1;
        if (!player) {
            if (dbg) log::info("[DBG] EARLY EXIT: player is NULL");
            return;
        }

        if (dbg) log::info("[DBG] postUpdate ACTIVE frame={}", s_dbg);

        float playerX = player->getPositionX();
        float currentTime = static_cast<float>(m_timePlayed);

        if (dbg) log::info("[DBG] playerX={:.2f} currentTime={:.4f} historySize={}", playerX, currentTime, fields->m_posHistory.size());

        // Record player position history for accurate world-X interpolation
        // This correctly handles speed changes from speed portals
        if (currentTime > 0.0f) {
            if (!fields->m_posHistory.empty() && currentTime < fields->m_posHistory.back().first) {
                // Time went backwards (reset/respawn), trim future entries
                auto it = std::lower_bound(fields->m_posHistory.begin(), fields->m_posHistory.end(), currentTime,
                    [](const std::pair<float,float>& p, float t) { return p.first < t; });
                fields->m_posHistory.erase(it, fields->m_posHistory.end());
            }
            if (fields->m_posHistory.empty() || currentTime > fields->m_posHistory.back().first + 0.001f) {
                fields->m_posHistory.push_back({currentTime, playerX});
            }
        }

        float bottomY = static_cast<float>(fields->m_barY);
        float barH = static_cast<float>(fields->m_barHeight);

        // Height of the input segment: inset inside the track so the hairlines
        // frame it (a cleaner "piano-roll" look than a full-height flat block).
        float segH = std::max(3.0f, barH - 4.0f);

        // Create dot nodes once (positions are computed below each frame). Each
        // input = a coloured HOLD segment + a brighter CAP pinned to its leading
        // edge, so the exact press instant pops instead of the hold reading as one
        // flat bar. The cap is a child at local x≈0 (the segment's left = press
        // moment for normal left→right motion), so it needs no per-frame upkeep.
        if (!fields->m_dotsCreated && !fields->m_presses.empty()) {
            for (size_t i = 0; i < fields->m_presses.size(); i++) {
                auto& press = fields->m_presses[i];
                ccColor4B color = (press.player == 1) ? fields->m_p1Color : fields->m_p2Color;
                auto dot = CCLayerColor::create(color, 1.0f, segH);
                dot->setAnchorPoint({0.0f, 0.5f});
                dot->ignoreAnchorPointForPosition(false);
                dot->setOpacity(255);
                dot->setVisible(false);

                // Bright press cap (a lighter tint of the player colour) at the
                // leading edge. Fixed 3px wide; the hold body stretches behind it.
                auto cap = CCLayerColor::create(
                    ccc4(std::min(255, color.r + 90), std::min(255, color.g + 90),
                         std::min(255, color.b + 90), 255),
                    3.0f, segH);
                cap->setAnchorPoint({0.0f, 0.0f});
                cap->setPosition({0.0f, 0.0f});
                cap->setID("press-cap"_spr);
                dot->addChild(cap, 2);

                fields->m_circleLayer->addChild(dot);
                fields->m_dots.push_back({i, dot});
            }
            fields->m_dotsCreated = true;
            log::info("[InputCircles] Created {} dot nodes", fields->m_presses.size());
        }

        // Interpolation helper: map a time value to a world X using recorded history.
        // Uses binary search + linear interpolation between samples, and extrapolates
        // at the edges using the local speed derivative.
        auto& history = fields->m_posHistory;
        auto interpolateWorldX = [&history](float time) -> std::optional<float> {
            if (history.empty()) return std::nullopt;
            if (time <= history.front().first) {
                // Extrapolate backward from earliest data
                if (history.size() >= 2) {
                    auto& a = history[0];
                    auto& b = history[1];
                    float dt = b.first - a.first;
                    if (dt > 0.0001f) {
                        float speed = (b.second - a.second) / dt;
                        return a.second + speed * (time - a.first);
                    }
                }
                return history.front().second;
            }
            if (time >= history.back().first) {
                // Extrapolate forward from latest data
                if (history.size() >= 2) {
                    auto& a = history[history.size() - 2];
                    auto& b = history[history.size() - 1];
                    float dt = b.first - a.first;
                    if (dt > 0.0001f) {
                        float speed = (b.second - a.second) / dt;
                        return b.second + speed * (time - b.first);
                    }
                }
                return history.back().second;
            }
            // Binary search for the right interval
            auto it = std::lower_bound(history.begin(), history.end(), time,
                [](const std::pair<float,float>& p, float t) { return p.first < t; });
            if (it == history.begin()) return it->second;
            auto prev = std::prev(it);
            float t0 = prev->first, x0 = prev->second;
            float t1 = it->first, x1 = it->second;
            float frac = (time - t0) / (t1 - t0);
            return x0 + frac * (x1 - x0);
        };

        // Update all dot positions using interpolated world coordinates.
        // Use the player's actual screen position as the anchor point,
        // and compute a pixels-per-world-unit ratio from the object layer.
        // This correctly handles mirror portals because convertToWorldSpace
        // goes through the full transform chain (including negative scaleX).
        double framerate = fields->m_framerate;
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Get player's ACTUAL screen position (we know this is always correct)
        auto playerParent = player->getParent();
        if (dbg) {
            log::info("[DBG] player parent={} objectLayer={}",
                playerParent ? "yes" : "NULL",
                m_objectLayer ? "yes" : "NULL");
            if (playerParent) {
                log::info("[DBG] playerParent pos=({:.2f},{:.2f}) scale=({:.4f},{:.4f})",
                    playerParent->getPositionX(), playerParent->getPositionY(),
                    playerParent->getScaleX(), playerParent->getScaleY());
            }
            if (m_objectLayer) {
                log::info("[DBG] objectLayer pos=({:.2f},{:.2f}) scale=({:.4f},{:.4f})",
                    m_objectLayer->getPositionX(), m_objectLayer->getPositionY(),
                    m_objectLayer->getScaleX(), m_objectLayer->getScaleY());
                // Walk up parent chain
                auto p = m_objectLayer->getParent();
                int depth = 0;
                while (p && depth < 5) {
                    log::info("[DBG]   objectLayer parent[{}] pos=({:.2f},{:.2f}) scale=({:.4f},{:.4f})",
                        depth, p->getPositionX(), p->getPositionY(),
                        p->getScaleX(), p->getScaleY());
                    p = p->getParent();
                    depth++;
                }
            }
        }

        CCPoint playerScreenPos = playerParent ? playerParent->convertToWorldSpace(player->getPosition()) : ccp(playerX, player->getPositionY());
        float playerScreenX = playerScreenPos.x;

        // Compute pixels-per-world-unit from two reference points on the object layer.
        // After a mirror portal, this value becomes NEGATIVE, which correctly flips dot positions.
        CCPoint ref0 = m_objectLayer->convertToWorldSpace(ccp(0.0f, 0.0f));
        CCPoint ref1 = m_objectLayer->convertToWorldSpace(ccp(1000.0f, 0.0f));
        float pixelsPerUnit = (ref1.x - ref0.x) / 1000.0f;

        if (dbg) {
            log::info("[DBG] playerScreenX={:.2f} ref0=({:.2f},{:.2f}) ref1=({:.2f},{:.2f}) ppu={:.6f}",
                playerScreenX, ref0.x, ref0.y, ref1.x, ref1.y, pixelsPerUnit);
        }

        // Fallback: if scale is zero somehow, skip rendering
        if (std::abs(pixelsPerUnit) < 0.0001f) {
            if (dbg) log::info("[DBG] pixelsPerUnit too small, SKIPPING");
            return;
        }

        for (auto& [pressIdx, dot] : fields->m_dots) {
            auto& press = fields->m_presses[pressIdx];
            float timeStart = static_cast<float>(press.framePress) / static_cast<float>(framerate);
            float timeEnd = static_cast<float>(press.frameRelease) / static_cast<float>(framerate);

            auto wxStart = interpolateWorldX(timeStart);
            auto wxEnd = interpolateWorldX(timeEnd);
            if (!wxStart.has_value() || !wxEnd.has_value()) {
                dot->setVisible(false);
                continue;
            }

            // Find the TRUE min/max world X across the entire press duration.
            // This handles cases where the player reverses direction mid-press.
            float minWX = std::min(wxStart.value(), wxEnd.value());
            float maxWX = std::max(wxStart.value(), wxEnd.value());

            float tMin = std::min(timeStart, timeEnd);
            float tMax = std::max(timeStart, timeEnd);
            auto itBegin = std::lower_bound(history.begin(), history.end(), tMin,
                [](const std::pair<float,float>& p, float t) { return p.first < t; });
            auto itEnd = std::upper_bound(history.begin(), history.end(), tMax,
                [](float t, const std::pair<float,float>& p) { return t < p.first; });
            for (auto it = itBegin; it != itEnd; ++it) {
                minWX = std::min(minWX, it->second);
                maxWX = std::max(maxWX, it->second);
            }

            // Convert to screen space relative to the player's known screen position
            float sA = playerScreenX + (minWX - playerX) * pixelsPerUnit;
            float sB = playerScreenX + (maxWX - playerX) * pixelsPerUnit;
            float screenLeft = std::min(sA, sB);
            float screenRight = std::max(sA, sB);

            // Skip dots that are off screen
            if (screenRight < -50.0f || screenLeft > winSize.width + 50.0f) {
                dot->setVisible(false);
                continue;
            }

            float screenWidth = std::max(screenRight - screenLeft, 4.0f);
            dot->setPosition({screenLeft, bottomY});
            dot->setContentSize({screenWidth, segH});
            dot->setVisible(true);

            // Log first 3 visible dots
            if (dbg && pressIdx < 3) {
                log::info("[DBG] dot[{}] minWX={:.2f} maxWX={:.2f} sA={:.2f} sB={:.2f} screenLeft={:.2f} screenW={:.2f}",
                    pressIdx, minWX, maxWX, sA, sB, screenLeft, screenWidth);
            }
        }

        // Update indicator position to follow the player's screen X
        if (fields->m_indicator) {
            fields->m_indicator->setPosition({playerScreenX, bottomY});
            if (dbg) log::info("[DBG] indicator pos=({:.2f},{:.2f})", playerScreenX, bottomY);
        }

    }

    void levelComplete() {
        // Auto-repair: the current trial actually finished the level in real GD.
        if (m_fields->m_autoRepairActive && !g_ar.done) {
            this->arOnTrialEnd(true);
        }
        // Confirm-only: the solve cleared for real (the .gdr2 was already written).
        if (m_fields->m_confirmOnlyActive && !m_fields->m_confirmDone) {
            this->confirmOnlyFinish(true);
        }

        PlayLayer::levelComplete();

        auto fields = m_fields.self();
        if (!fields->m_active) return;
    }

    // Death-debug: when the player dies during an auto-repair trial, write a full
    // report of WHAT killed it and WHY (killing object, player state, and the gdsim
    // comparison — did the sim predict this death?). One report per attempt, to
    // GDMod_death_debug.txt.
    void destroyPlayer(PlayerObject* p0, GameObject* p1) {
        auto fields = m_fields.self();
        PlayerObject* pl = p0 ? p0 : m_player1;
        // GD calls destroyPlayer once during level setup (frame ~1, player still at
        // spawn, with a placeholder object) — skip it so it doesn't consume the
        // one-report flag and mask the REAL death. Nothing kills you this early.
        if (fields->m_autoRepairActive && !fields->m_deathLogged && pl && m_timePlayed > 0.06f) {
            fields->m_deathLogged = true;
            uint64_t frame = (uint64_t)std::llround((double)m_timePlayed * fields->m_framerate);
            float px = pl->getPositionX(), py = pl->getPositionY();
            const char* veh = pl->m_isShip ? "Ship" : pl->m_isBird ? "UFO"
                            : pl->m_isBall ? "Ball" : pl->m_isDart ? "Wave"
                            : pl->m_isRobot ? "Robot" : pl->m_isSpider ? "Spider"
                            : pl->m_isSwing ? "Swing" : "Cube";
            deathLog(fmt::format("\n===== DEATH  frame={}  time={:.3f}s  progress={:.2f}%  x={:.1f} =====",
                                 frame, m_timePlayed, this->getCurrentPercent(), px));
            deathLog(fmt::format("player @death: pos=({:.1f},{:.1f})  yVel={:.3f}  veh={}  mini={:.2f}  upsideDown={} (death-frame flags may include a same-frame pad/portal flip)",
                                 px, py, pl->m_yVelocity, veh, pl->m_vehicleSize, pl->m_isUpsideDown ? 1 : 0));
            if (fields->m_prevRealValid) {
                static const char* kVeh[] = {"Cube","Ship","Ball","UFO","Wave","Robot","Spider","Swing"};
                deathLog(fmt::format("approach (last alive frame): pos=({:.1f},{:.1f})  yVel={:.3f}  veh={}  upsideDown={}  <-- the real state going IN",
                                     fields->m_prevRealX, fields->m_prevRealY, fields->m_prevRealVel,
                                     kVeh[fields->m_prevRealVeh & 7], fields->m_prevRealUp ? 1 : 0));
            }
            if (p1)
                deathLog(fmt::format("KILLED BY: objID={} ({})  pos=({:.1f},{:.1f})  delta=({:.1f},{:.1f})",
                                     p1->m_objectID, objName(p1->m_objectID),
                                     p1->getPositionX(), p1->getPositionY(),
                                     p1->getPositionX() - px, p1->getPositionY() - py));
            else
                deathLog("KILLED BY: (no object — out of bounds / forced)");

            // Input context: was a click held at the death frame, and where are the
            // nearest presses/releases? Tells you if the solution *intended* an input
            // right here (so a mis-timed click is suspect) or if the death is between
            // clicks (pure physics/geometry).
            if (g_replayPlayer.replay.has_value()) {
                const auto& presses = g_replayPlayer.replay->presses;
                bool held = false; uint64_t heldPress = 0, heldRel = 0;
                uint64_t prevRel = 0, nextPress = 0;   // nearest edges around `frame`
                int idxHeld = -1;
                for (size_t i = 0; i < presses.size(); ++i) {
                    const auto& pr = presses[i];
                    if (pr.framePress <= frame && frame < pr.frameRelease) {
                        held = true; heldPress = pr.framePress; heldRel = pr.frameRelease; idxHeld = (int)i;
                    }
                    if (pr.frameRelease <= frame) prevRel = std::max(prevRel, pr.frameRelease);
                    if (pr.framePress   >  frame && (nextPress == 0 || pr.framePress < nextPress))
                        nextPress = pr.framePress;
                }
                if (held)
                    deathLog(fmt::format("input @death: HOLDING click #{} (press f{} -> release f{}), {} frame(s) into the hold",
                                         idxHeld, heldPress, heldRel, frame - heldPress));
                else
                    deathLog(fmt::format("input @death: no click held. prev release f{} ({} frames ago), next press f{} (in {} frames)",
                                         prevRel, prevRel ? frame - prevRel : 0,
                                         nextPress, nextPress ? nextPress - frame : 0));
                deathLog(fmt::format("total clicks in solution: {}", presses.size()));
            }

            // Divergence summary: did real GD track gdsim up to the death, or drift off?
            // This is the crux of "solves in sim, dies in the watch" — a large drift
            // before death means the real run desynced from the path the solver found.
            {
                float peak = std::max(std::fabs(fields->m_divMaxDx), std::fabs(fields->m_divMaxDy));
                if (fields->m_divFirstDriftFrame == 0)
                    deathLog("divergence: real GD tracked gdsim to <2u the whole way (no drift before death).");
                else
                    deathLog(fmt::format("divergence: first drifted >2u at frame {} (Δ={:.1f},{:.1f}); peak Δ=({:.1f},{:.1f}) at frame {}. realMaxX={:.0f}",
                                         fields->m_divFirstDriftFrame, fields->m_divFirstDx, fields->m_divFirstDy,
                                         fields->m_divMaxDx, fields->m_divMaxDy, fields->m_divMaxDriftFrame,
                                         fields->m_divRealMaxX));
                (void)peak;
            }

            // gdsim comparison: did the sim predict this death, and where?
            // velMismatch: sim and real vertical velocity point OPPOSITE ways at the
            // death — the tell for a gravity/jump-phase divergence (the click lands on
            // a different arc phase / flip side), which no hazardInflate can fix but a
            // 1-2 frame click shift (Auto-fix) can. Set here, used by the VERDICT below.
            bool  velMismatch = false;
            bool  simSurvived = false;
            if (fields->m_divSim && !fields->m_divSim->gameStates.empty()) {
                auto& sp = fields->m_divSim->gameStates.back();
                simSurvived = !sp.dead;
                const float kUnit = 54.f;            // gdsim stores velocity = real yVel * 54
                float simVelReal = sp.velocity / kUnit;
                velMismatch = (simVelReal * pl->m_yVelocity < -0.5f);
                deathLog(fmt::format("gdsim: dead={}  cause={}  killer=id{} @({:.0f},{:.0f})  simPos=({:.1f},{:.1f})  simVel={:.1f}  yOffset={:.1f}",
                                     sp.dead ? 1 : 0, sp.deathCause ? sp.deathCause : "-",
                                     sp.deathObjType, sp.deathObjPos.x, sp.deathObjPos.y,
                                     sp.pos.x, sp.pos.y, sp.velocity, fields->m_divOffY));
                deathLog(fmt::format("  gdsim gravity state: upsideDown={} grounded={} small={}  velReal={:.2f}   vs REAL upsideDown={} yVel={:.2f}",
                                     sp.upsideDown ? 1 : 0, sp.grounded ? 1 : 0, sp.small ? 1 : 0,
                                     simVelReal, pl->m_isUpsideDown ? 1 : 0, pl->m_yVelocity));
                if (velMismatch)
                    deathLog("  >>> VERTICAL DIRECTION MISMATCH: gdsim velocity is the OPPOSITE sign to real. "
                             "The jump/flip lands on a different arc phase — a gravity/timing divergence, "
                             "NOT a hitbox-size gap. A 1-2 frame click shift (Auto-fix) fixes it.");
                else if (!sp.dead)
                    deathLog("  >>> gdsim SURVIVED here: the sim is MISSING this death "
                             "(hitbox too small or the ~2u jump phase error). Fidelity gap.");
                deathLog(fmt::format("  sim player box L={:.1f} R={:.1f} B={:.1f} T={:.1f}",
                                     sp.getLeft(), sp.getRight(), sp.getBottom(), sp.getTop()));
                auto& secs = fields->m_divSim->sections;
                int si = std::clamp((int)(sp.pos.x / (float)gdsim::Level::sectionSize), 0, (int)secs.size() - 1);
                for (int s = std::max(0, si - 1); s <= std::min((int)secs.size() - 1, si + 1); ++s)
                    for (auto& oc : secs[s]) {
                        const gdsim::Object* o = oc.operator->();
                        if (std::abs(o->pos.x - sp.pos.x) > 50.f) continue;
                        float gapX = std::max({0.f, o->getLeft() - sp.getRight(), sp.getLeft() - o->getRight()});
                        float gapY = std::max({0.f, o->getBottom() - sp.getTop(), sp.getBottom() - o->getTop()});
                        deathLog(fmt::format("  near {}: id={} pos=({:.1f},{:.1f}) box[L={:.1f} R={:.1f} B={:.1f} T={:.1f}] gap=({:.2f},{:.2f})",
                                             o->prio == 2 ? "HAZARD" : "solid", o->typeId, o->pos.x, o->pos.y,
                                             o->getLeft(), o->getRight(), o->getBottom(), o->getTop(), gapX, gapY));
                    }
            }

            // ── VERDICT ──────────────────────────────────────────────────────────
            // One-line root-cause classification so you don't have to read the whole
            // report: did the solver hand over a losing path, or did the real run
            // desync from a winning one (and by how much)?
            {
                float peak = std::max(std::fabs(fields->m_divMaxDx), std::fabs(fields->m_divMaxDy));
                std::string v;
                if (!simSurvived)
                    v = "gdsim ALSO dies at ~this point -> the solved path is genuinely losing "
                        "(not a fidelity gap). The solver produced a bad solution; re-Solve "
                        "(try Center off) or check for an unmodeled mechanic near this X.";
                else if (velMismatch)
                    v = fmt::format("TIMING / GRAVITY-PHASE gap: gdsim SURVIVES but its vertical velocity is "
                                    "OPPOSITE real's here (sim on one side of the arc/flip, real on the other, "
                                    "peak drift only {:.1f}u). Not a hazard-size issue — the click lands a frame "
                                    "or two off the real arc. Auto-fix (shift clicks) recovers it; raising "
                                    "hazardInflate will NOT.", peak);
                else if (peak > 6.f)
                    v = fmt::format("DESYNC: gdsim SURVIVES but the real run drifted {:.1f}u off the "
                                    "simulated path (onset frame {}). The click timing that works in "
                                    "the sim doesn't line up with real GD here. Auto-fix should recover it.",
                                    peak, fields->m_divFirstDriftFrame);
                else
                    v = fmt::format("FIDELITY GAP: gdsim SURVIVES with only {:.1f}u drift and same velocity "
                                    "direction -> the sim's hazard/hitbox is slightly smaller than real GD "
                                    "(a graze the sim misses). Raise hazardInflate or run Auto-fix.", peak);
                deathLog("VERDICT: " + v);
            }
            deathLog("(written to " + deathLogPath() + ")");
        }
        PlayLayer::destroyPlayer(p0, p1);
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        auto fields = m_fields.self();
        if (fields->m_autoRepairActive) {
            // Re-anchor the per-step injection clock to the (reset) play time and
            // keep fast-forward + the injection gate live for the next trial.
            s_replayStepHold   = false;
            s_replayStepTime   = static_cast<double>(m_timePlayed);
            s_replayStepActive = false;
            g_replayExternalCommands.liveReplayActive.store(true);
            g_replayExternalCommands.livePaused.store(false);
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(s_autoRepairSpeed);
        }
        // Confirm-only is a single pass, not a retry loop — a manual reset mid-pass
        // (practice mode, user hit retry) just drops out of it quietly instead of
        // trying to keep going, so speed/render state can't get stuck.
        if (fields->m_confirmOnlyActive && !fields->m_confirmDone) {
            fields->m_confirmOnlyActive = false;
            fields->m_confirmDone       = true;
            g_replayExternalCommands.liveReplayActive.store(false);
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
            if (this->m_objectLayer)            this->m_objectLayer->setVisible(true);
            if (this->m_inShaderObjectLayer)    this->m_inShaderObjectLayer->setVisible(true);
            if (this->m_aboveShaderObjectLayer) this->m_aboveShaderObjectLayer->setVisible(true);
        }

        if (fields->m_active && !fields->m_posHistory.empty()) {
            // Trim history entries past the respawn time so we
            // re-record accurate positions from the checkpoint onward
            float respawnTime = static_cast<float>(m_timePlayed);
            auto it = std::lower_bound(fields->m_posHistory.begin(), fields->m_posHistory.end(), respawnTime,
                [](const std::pair<float,float>& p, float t) { return p.first < t; });
            fields->m_posHistory.erase(it, fields->m_posHistory.end());
        }

        if (fields->m_active) {
            s_replayInputInjectionActive = false;
        }
    }

    void onQuit() {
        auto fields = m_fields.self();
        g_replayExternalCommands.liveReplayActive.store(false);
        g_replayExternalCommands.livePaused.store(false);
        g_replayExternalCommands.liveFrame.store(0);
        if (fields->m_autoRepairActive) {
            fields->m_autoRepairActive = false;
            g_ar.done = true;
            g_ar.active = false;
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
            g_replayPlayer.isActive = false;
            g_replayPlayer.replay.reset();
        }
        if (fields->m_confirmOnlyActive) {
            fields->m_confirmOnlyActive = false;
            fields->m_confirmDone       = true;
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
            g_replayPlayer.isActive = false;
            g_replayPlayer.replay.reset();
        }

        s_replayInputInjectionActive = false;
        PlayLayer::onQuit();
    }

    void onExit() {
        PlayLayer::onExit();
    }
};
