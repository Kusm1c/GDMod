// ============================================================
// RE Scanner — passive, continuous reverse-engineering capture of the REAL engine
// ============================================================
// Gated entirely behind the "re-scanner" mod setting (off by default). When on,
// the mod records — while you simply play — everything needed to reverse the game
// into gdsim:
//   * level parameters   -> testlevel/GDMod_levelparams_<id>.txt  (once per level)
//   * move-trigger motion -> testlevel/movetest/GDMod_movetriggers_<id>.txt (per frame)
//   * per-frame physics   -> testlevel/GDMod_physics_<id>.txt      (player state each frame)
//   * mechanics (orbs/pads/portals/toggles) via mech_recorder.cpp, which shares
//     this same rescan::enabled() switch.
//   * hitboxes are already dumped per level to GDMod_hitbox_capture.txt (hooks_play).
//
// Like mech_recorder, this only READS engine state and WRITES text files — it never
// touches gdsim or the solver, so it cannot affect gameplay or solving.

#include "re_scanner.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>

#include <atomic>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <cmath>

using namespace geode::prelude;

// ---- setting cache -------------------------------------------------------

namespace {
    std::atomic<bool> s_enabled{false};
    bool              s_listenerInstalled = false;
}

bool rescan::enabled() {
    if (!s_listenerInstalled) {
        s_listenerInstalled = true;
        s_enabled.store(Mod::get()->getSettingValue<bool>("re-scanner"));
        listenForSettingChanges<bool>("re-scanner", [](bool v) {
            s_enabled.store(v);
            log::info("[REScan] scanner {}", v ? "ENABLED" : "disabled");
        });
    }
    return s_enabled.load();
}

// ---- capture state (single PlayLayer at a time) --------------------------

namespace {

// Per-level capture now lives in the repo's testlevel/ folder, one dedicated file
// per level (named with the level id). Move/other-trigger debug goes into the
// testlevel/movetest/ sub-folder alongside the mechanics database.
const char* kOutDir   = "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/";
const char* kTrigDir  = "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/movetest/";
const char* kFallback = "C:/Users/Kusmic/Documents/";   // used if the repo path is unwritable

int           g_levelId    = 0;
int           g_frame      = 0;
bool          g_needParams = false;   // dump level params + build the track on frame 1
std::string   g_outDir;               // resolved (create+writable-checked) level-capture dir
std::string   g_trigDir;              // resolved trigger-debug dir
std::ofstream g_moveOut;
std::ofstream g_physOut;

// A grouped object we watch for trigger-driven motion. We diff its live pose
// against the previous frame — mechanism-independent, so it captures move,
// follow, rotate and any physics-driven motion regardless of which internal GD
// list holds it. This is the RELIABLE path: reading m_objectsToMove alone came
// out empty (it is cleared before postUpdate runs).
struct Tracked { GameObject* obj; float x, y, rot; };
std::vector<Tracked> g_track;
bool                 g_trackBuilt = false;

// Ensure `preferred` exists and is writable; else fall back to Documents. Returns
// a dir path with a trailing slash. Never throws (std::error_code overloads).
std::string ensureDir(const char* preferred) {
    std::error_code ec;
    std::filesystem::create_directories(preferred, ec);
    // Probe-write a temp file to confirm the dir is actually writable.
    std::string probe = std::string(preferred) + ".rescan_probe";
    { std::ofstream t(probe); if (t.is_open()) { t.close(); std::filesystem::remove(probe, ec); return preferred; } }
    std::filesystem::create_directories(kFallback, ec);
    log::warn("[REScan] '{}' not writable — falling back to '{}'", preferred, kFallback);
    return kFallback;
}

void closeScan() {
    if (g_moveOut.is_open()) g_moveOut.close();
    if (g_physOut.is_open()) g_physOut.close();
    g_track.clear();
    g_trackBuilt = false;
    g_frame = 0;
}

// Snapshot every grouped object's start pose so per-frame motion can be diffed.
// Only grouped objects (m_groupCount>0) can be moved by triggers, so this scans
// the object list once and keeps a compact, hash-free vector for the hot loop.
void buildTrack(PlayLayer* pl) {
    g_track.clear();
    if (pl->m_objects) {
        g_track.reserve(4096);
        for (unsigned i = 0; i < pl->m_objects->count(); ++i) {
            auto* o = typeinfo_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!o || o->m_groupCount <= 0 || !o->m_groups) continue;
            // Skip pure decoration: only collision objects (solids, hazards, portals,
            // pads, orbs, slopes…) have gameplay impact, and moving deco is what blew
            // the capture up to hundreds of MB. Everything that is NOT Decoration is
            // kept.
            if (o->m_objectType == GameObjectType::Decoration) continue;
            g_track.push_back({o, o->getPositionX(), o->getPositionY(), o->getRotation()});
        }
    }
    g_trackBuilt = true;
    log::info("[REScan] tracking {} grouped gameplay objects for trigger motion", g_track.size());
}

std::string groupsOf(GameObject* obj) {
    if (!obj->m_groups || obj->m_groupCount <= 0) return "-";
    std::string s;
    int n = obj->m_groupCount;
    if (n > 10) n = 10;   // m_groups is a fixed array of 10 (property 57)
    for (int i = 0; i < n; ++i) {
        if (i) s += '.';
        s += std::to_string((*obj->m_groups)[i]);
    }
    return s.empty() ? "-" : s;
}

int encodeVehicle(PlayerObject* p) {
    if (p->m_isShip)   return 1;   // ship
    if (p->m_isBall)   return 2;   // ball
    if (p->m_isBird)   return 3;   // ufo
    if (p->m_isDart)   return 4;   // wave
    if (p->m_isRobot)  return 5;   // robot
    if (p->m_isSpider) return 6;   // spider
    if (p->m_isSwing)  return 7;   // swing
    return 0;                      // cube
}

void dumpLevelParams(PlayLayer* pl) {
    auto* ls  = pl->m_levelSettings;
    auto* lvl = pl->m_level;
    if (!ls || !lvl) return;
    // Written on the first PLAYED frame (not init): at init the object array is not
    // yet populated, so objectCount would read 0.
    std::ofstream f(fmt::format("{}GDMod_levelparams_{}.txt", g_outDir, g_levelId), std::ios::trunc);
    if (!f.is_open()) return;
    f << "# GDMod level parameters (captured from the REAL engine) — level " << g_levelId << "\n";
    f << "name "        << lvl->m_levelName        << "\n";
    f << "startMode "   << ls->m_startMode         << "   # 0 cube 1 ship 2 ball 3 ufo 4 wave 5 robot 6 spider 7 swing\n";
    f << "startSpeed "  << (int)ls->m_startSpeed   << "   # engine Speed enum (0 slow .. 4 fastest)\n";
    f << "startMini "   << (ls->m_startMini  ? 1 : 0) << "\n";
    f << "startDual "   << (ls->m_startDual  ? 1 : 0) << "\n";
    f << "mirror "      << (ls->m_mirrorMode ? 1 : 0) << "\n";
    f << "rotateGameplay " << (ls->m_rotateGameplay ? 1 : 0) << "\n";
    f << "twoPlayer "   << (ls->m_twoPlayerMode ? 1 : 0) << "\n";
    f << "platformer "  << (ls->m_platformerMode ? 1 : 0) << "\n";
    f << "flipped "     << (ls->m_isFlipped ? 1 : 0) << "\n";
    f << "reverse "     << (ls->m_reverseGameplay ? 1 : 0) << "\n";
    f << "songOffset "  << ls->m_songOffset << "\n";
    f << "objectCount " << (pl->m_objects ? pl->m_objects->count() : 0) << "\n";
    log::info("[REScan] level params dumped for {}", g_levelId);
}

} // namespace

// ---- hook ----------------------------------------------------------------

class $modify(ReScanPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        closeScan();
        if (rescan::enabled() && level) {
            g_levelId = level->m_levelID;
            g_needParams = true;
            g_outDir  = ensureDir(kOutDir);    // resolves + creates; falls back to Documents
            g_trigDir = ensureDir(kTrigDir);
            // APPEND, not truncate: a hard level gets many attempts, and truncating on
            // every retry would leave only the last (often aborted, empty) run. Each
            // attempt is delimited by an ATTEMPT marker; frame counter resets per attempt.
            const bool moveNew = !std::ifstream(fmt::format("{}GDMod_movetriggers_{}.txt", g_trigDir, g_levelId)).good();
            g_moveOut = std::ofstream(fmt::format("{}GDMod_movetriggers_{}.txt", g_trigDir, g_levelId), std::ios::app);
            if (g_moveOut.is_open()) {
                if (moveNew) g_moveOut << "# frame objId groups x y dx dy rot drot  — objects moved this frame (move/rotate/follow/physics), pose-diffed\n";
                g_moveOut << "# --- ATTEMPT ---\n";
            } else {
                log::error("[REScan] could not open movetriggers file in '{}'", g_trigDir);
            }
            const bool physNew = !std::ifstream(fmt::format("{}GDMod_physics_{}.txt", g_outDir, g_levelId)).good();
            g_physOut = std::ofstream(fmt::format("{}GDMod_physics_{}.txt", g_outDir, g_levelId), std::ios::app);
            if (g_physOut.is_open()) {
                if (physNew) g_physOut << "# frame x y yVel speed veh up mini grounded  — real per-frame player physics\n";
                g_physOut << "# --- ATTEMPT ---\n";
            } else {
                log::error("[REScan] could not open physics file in '{}'", g_outDir);
            }
        }
        return true;
    }

    // PlayLayer does NOT declare update() (that lives on GJBaseGameLayer); it DOES
    // override postUpdate(), which runs every frame after the physics step — the
    // right place to read final per-frame positions. Hooking update() here silently
    // did nothing (Geode couldn't resolve it on PlayLayer), which left the capture
    // files empty despite init() firing.
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!rescan::enabled()) return;
        ++g_frame;

        // Level params + object track on the first played frame — the object array
        // is populated now (at init() it is still empty).
        if (g_needParams) { g_needParams = false; dumpLevelParams(this); buildTrack(this); }

        // Move/rotate/follow effects: diff every grouped object's live pose against
        // last frame. Independent of m_objectsToMove (which is empty here), so it
        // reliably captures the exact motion any trigger — or physics — produces.
        if (g_moveOut.is_open() && g_trackBuilt) {
            // Only log objects near the player: gdsim only poses objects within ~220u
            // of the player anyway, so off-screen motion is noise for RE and unbounded
            // in size. Baselines still advance every frame so a re-entering object
            // reports its true per-frame delta, not an accumulated jump.
            const float px = m_player1 ? m_player1->getPositionX() : 0.f;
            const float kWindow = 1500.f;      // ~2 screens of context around the player
            for (auto& t : g_track) {
                if (!t.obj) continue;
                float x = t.obj->getPositionX(), y = t.obj->getPositionY(), r = t.obj->getRotation();
                float dx = x - t.x, dy = y - t.y, dr = r - t.rot;
                t.x = x; t.y = y; t.rot = r;   // per-frame baseline (always advance)
                if (std::abs(dx) < 1e-4f && std::abs(dy) < 1e-4f && std::abs(dr) < 1e-4f)
                    continue;                  // only rows with real motion
                if (std::abs(x - px) > kWindow) continue;   // off-screen → skip logging
                g_moveOut << g_frame << ' ' << t.obj->m_objectID << ' ' << groupsOf(t.obj) << ' '
                          << x << ' ' << y << ' ' << dx << ' ' << dy << ' ' << r << ' ' << dr << '\n';
            }
        }

        // Per-frame physics of whatever you're playing (not gated on a replay).
        if (g_physOut.is_open() && m_player1) {
            auto* p = m_player1;
            g_physOut << g_frame << ' ' << p->getPositionX() << ' ' << p->getPositionY() << ' '
                      << (float)p->m_yVelocity << ' ' << p->m_playerSpeed << ' '
                      << encodeVehicle(p) << ' ' << (p->m_isUpsideDown ? 1 : 0) << ' '
                      << (p->m_vehicleSize < 0.8f ? 1 : 0) << ' ' << (p->m_isOnGround ? 1 : 0) << '\n';
        }

        if ((g_frame % 30) == 0) {
            if (g_moveOut.is_open()) g_moveOut.flush();
            if (g_physOut.is_open()) g_physOut.flush();
        }
    }
};
