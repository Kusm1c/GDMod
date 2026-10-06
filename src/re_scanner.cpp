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
#include "sim/Gmb.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>

#include <atomic>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <cmath>

using namespace geode::prelude;

// ---- setting cache -------------------------------------------------------

namespace {
    std::atomic<bool> s_enabled{false};
    bool              s_listenerInstalled = false;
    std::atomic<bool> s_jumpProbe{false};
    bool              s_jumpListenerInstalled = false;
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

bool rescan::jumpProbeEnabled() {
    if (!s_jumpListenerInstalled) {
        s_jumpListenerInstalled = true;
        s_jumpProbe.store(Mod::get()->getSettingValue<bool>("jump-probe"));
        listenForSettingChanges<bool>("jump-probe", [](bool v) {
            s_jumpProbe.store(v);
            log::info("[REScan] jump probe {}", v ? "ENABLED" : "disabled");
        });
    }
    return s_jumpProbe.load();
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
// Move-trigger capture is BINARY (see sim/Gmb.hpp): as text it reached 17 GB for
// a single level. The physics/jump captures stay text — they have many readers
// (the app, a dozen tools) and are nowhere near the same size.
gdsim::gmb::Writer g_moveBin;
std::unordered_set<long long> g_dedup;              // per-frame (group,delta) key set
std::unordered_map<int,int>   g_groupRep;           // group -> its STABLE representative m_uniqueID
std::ofstream g_physOut;

// Sub-pixel per-physics-step capture (see the JUMP PROBE section at the bottom).
// Its own stream and its own step counter: g_frame counts RENDERED frames, this
// counts updateJump calls, and conflating the two is exactly the mistake the
// probe exists to remove.
std::ofstream g_jumpOut;
long long     g_jumpStep = 0;

// ── velocity write trace ─────────────────────────────────────────────────────
// WINDOWS 2.2081: THE TAG IS NOT USABLE. Measured on a real capture it takes
// 1210 distinct values in the range 0..1212 - a counter or an uninitialised
// register, not the decompiled source's small call-site id set (2,3,4,7..16,
// 61..66). The `int type` parameter is evidently unused in the release build, so
// callers leave garbage in it. The WRITTEN VALUE is still exact and useful (it
// tells you a velocity write happened this step and what it wrote); only the tag
// must be ignored. Left in the file rather than removed so nobody re-derives the
// same dead end.
//
// Originally added because setYVelocity's SECOND parameter looked like the
// decompiled source's own call-site tag — `setYVelocity(v46, 14)`, `setYVelocity(v37, 7)` and so on. It
// is not data the game uses; it is a label identifying WHICH branch of
// updateJump wrote the velocity. Capturing it turns "guess which branch gdsim
// should mirror" into "read the branch the engine actually took".
//
// Its sibling addToYVelocity is `win inline`, so it cannot be hooked — but its
// contribution is still recoverable by arithmetic:
//     dv_addTo = (vAfter - vBefore) - sum(setYVelocity writes this step)
// so no information is lost, it just arrives as a residual instead of an event.
struct YWrite { double value; int tag; };
std::vector<YWrite> g_yWrites;      // writes seen during the current updateJump
bool                g_inUpdateJump = false;
PlayerObject*       g_probeTarget  = nullptr;   // the player being captured

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
    g_moveBin.close();
    if (g_physOut.is_open()) g_physOut.close();
    if (g_jumpOut.is_open()) g_jumpOut.close();
    g_jumpStep = 0;
    g_track.clear();
    g_groupRep.clear();
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

// First group id, or -1. The binary capture stores one number instead of the old
// "1.7.12" string: a move trigger targets a single group, and the full membership
// list is recoverable from the level itself if it is ever needed.
int firstGroupOf(GameObject* obj) {
    if (!obj->m_groups || obj->m_groupCount <= 0) return -1;
    return (*obj->m_groups)[0];
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

    // SPAWN POSITION — added 2026-09-23 to settle an open gdsim bug.
    //
    // gdsim hardcodes the spawn at simY=15 (Player::Player()'s `floor(0), grounded(true)`),
    // i.e. real Y=105. Measured across the 73 GDMod_physics_* captures, 24 of them (33%)
    // really spawn somewhere else — 195, 225, …, 975, …, 2085 — every single value exactly
    // 105 + 30k. On those levels gdsim simulates a different part of the map from frame 1,
    // so every replay and every solve there is meaningless (proven on ALLOY 123617195:
    // forcing the real simY=885 as a free-fall start matches the real capture to ±0.005u
    // over 60 frames, where the stock spawn diverges instantly).
    //
    // The MECHANISM is still unknown, and the decompile ruled out the obvious candidates:
    //   * GJBaseGameLayer::setupLevelStart (0x140212220) sets gravity/reverse/dual/size/
    //     gamemode and NO position at all.
    //   * PlayLayer::resetLevel (0x1403b8eb0) only READS the player's position and copies
    //     it into player+0xa90 / player+0x4d0 — the spawn is already established before it.
    //   * loadStartPosObject (0x140235760) needs m_startPosObject, and the object factory's
    //     `case 0x1f` (=31) is the ONLY id that constructs a StartPosObject. ALLOY has no
    //     id 31 anywhere (46372 objects checked), so that path cannot be it.
    //   * getGroundHeightForMode (0x140211d50) returns per-GAMEMODE constants only
    //     (240 / 270 / 300), never a per-level value.
    // So it has to come from state this dump can observe directly. These four lines are
    // what distinguishes an offset level from a normal one; capture one of each and diff.
    if (auto* p1 = pl->m_player1) {
        auto sp = p1->getPosition();
        f << "spawnX " << sp.x << "\n";
        f << "spawnY " << sp.y << "   # real-engine Y; gdsim assumes 105 (simY 15) for every level\n";
    }
    f << "startPosObject " << (pl->m_startPosObject ? 1 : 0)
      << "   # 1 = level carries an id-31 StartPosObject (the only id that makes one)\n";
    if (auto* spo = pl->m_startPosObject) {
        auto p = spo->getPosition();
        f << "startPosObjectX " << p.x << "\n";
        f << "startPosObjectY " << p.y << "\n";
    }
    f << "startsWithStartPos " << (ls->m_startsWithStartPos ? 1 : 0) << "\n";
    f << "disableStartPos "    << (ls->m_disableStartPos    ? 1 : 0) << "\n";

    log::info("[REScan] level params dumped for {}", g_levelId);
}

} // namespace

// ---- hook ----------------------------------------------------------------

class $modify(ReScanPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        closeScan();
        // Either capture can run alone: the jump probe is a focused, high-volume
        // tool you switch on for one run, while the scanner is meant to sit in the
        // background — requiring both to be on would make the probe awkward to use.
        if ((rescan::enabled() || rescan::jumpProbeEnabled()) && level) {
            g_levelId = level->m_levelID;
            g_needParams = true;
            g_outDir  = ensureDir(kOutDir);    // resolves + creates; falls back to Documents
            g_trigDir = ensureDir(kTrigDir);
          if (rescan::enabled()) {
            // APPEND, not truncate: a hard level gets many attempts, and truncating on
            // every retry would leave only the last (often aborted, empty) run. Each
            // attempt is delimited by an ATTEMPT marker; frame counter resets per attempt.
            // Binary (.gmb) — the TEXT form of this one capture reached 17 GB on a
            // single level, and 36 GB across the folder. Positions are stored to
            // 1/1024 of a unit, finer than the 3-decimal quantisation the engine
            // itself applies, so nothing measurable is lost. dx/dy/drot are no
            // longer stored at all: they are exactly the difference between
            // consecutive rows for the same object, so the reader recomputes them.
            using gdsim::gmb::Column;
            // Column 2 is "uid" (GameObject::m_uniqueID), NOT m_objectID. It used
            // to be m_objectID, which is a TYPE id shared by every instance of the
            // same block — so "the group's representative object" matched several
            // physically distinct objects at once and their rows interleaved. A
            // reader following one representative then saw it teleporting between
            // two real objects: on level 123617195 group 61 that produced a fake
            // 47-unit travel for an object whose real motion is 12, and it was
            // nearly mistaken for a gdsim trigger-engine bug. The header carries
            // the column name, so an old capture is self-identifying.
            const std::vector<Column> moveCols = {
                {"frame", 1.0}, {"uid", 1.0}, {"group", 1.0},
                {"x", 1024.0}, {"y", 1024.0}, {"rot", 64.0},
                {"dx", 1024.0}, {"dy", 1024.0}, {"drot", 64.0},
            };
            if (g_moveBin.open(fmt::format("{}GDMod_movetriggers_{}.gmb", g_trigDir, g_levelId),
                               gdsim::gmb::KIND_MOVETRIGGERS, (uint32_t)g_levelId, moveCols)) {
                g_moveBin.attempt();
            } else {
                log::error("[REScan] could not open movetriggers file in '{}'", g_trigDir);
            }
            const bool physNew = !std::ifstream(fmt::format("{}GDMod_physics_{}.txt", g_outDir, g_levelId)).good();
            g_physOut = std::ofstream(fmt::format("{}GDMod_physics_{}.txt", g_outDir, g_levelId), std::ios::app);
            if (g_physOut.is_open()) {
                // Default ostream precision is 6 SIGNIFICANT digits, which at
                // x=22625 leaves 0.1-unit resolution and at x>100000 loses the
                // fractional part entirely — fatal for sub-unit comparison, and
                // the reason position deltas read as suspiciously quantised.
                // 17 digits round-trips a double exactly.
                g_physOut.precision(17);
                if (physNew) g_physOut << "# frame x y yVel speed veh up mini grounded  — real per-frame player physics\n";
                g_physOut << "# --- ATTEMPT ---\n";
            } else {
                log::error("[REScan] could not open physics file in '{}'", g_outDir);
            }
          } // rescan::enabled()

            // Sub-pixel per-physics-step capture. Opened only when its own setting
            // is on, because it writes ~10 MB per minute of play.
            if (rescan::jumpProbeEnabled()) {
                const bool jumpNew = !std::ifstream(fmt::format("{}GDMod_jump_{}.txt", g_outDir, g_levelId)).good();
                g_jumpOut = std::ofstream(fmt::format("{}GDMod_jump_{}.txt", g_outDir, g_levelId), std::ios::app);
                if (g_jumpOut.is_open()) {
                    g_jumpOut.precision(17);   // exact double round-trip
                    if (jumpNew)
                        g_jumpOut << "# ONE ROW PER 240Hz PHYSICS STEP, captured around PlayerObject::updateJump.\n"
                                     "# (vAfter-vBefore)/dt is the acceleration the real engine applied on that\n"
                                     "# step - no averaging, nothing inferred. 17 digits = exact double.\n"
                                     "# step dt x0 x1 y0 y1 vBefore vAfter gravity gravityMod yStart speedMult\n"
                                     "#   playerSpeed vehicleSize veh upsideDown onGround onSlope slopeAngle\n"
                                     "#   slopeVelocity slopeAngleRad dashing sliding jumpBuffered sideways | tag:value ...\n"
                                     "# After the | is the setYVelocity BRANCH TRACE: the decompiled source's\n"
                                     "# own call-site tags (setYVelocity(v,14) -> tag 14), in call order. An\n"
                                     "# empty trace means the whole dv came from the inlined addToYVelocity,\n"
                                     "# i.e. plain gravity. Recover that part as dv - sum(trace values).\n"
                                     "# Lines starting with Y are velocity writes OUTSIDE updateJump (orbs,\n"
                                     "# pads, portals):  Y step tag writtenValue resultingYVel\n";
                    g_jumpOut << "# --- ATTEMPT ---\n";
                    log::info("[REScan] jump probe capturing to GDMod_jump_{}.txt", g_levelId);
                } else {
                    log::error("[REScan] could not open jump-probe file in '{}'", g_outDir);
                }
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
        if (g_moveBin.isOpen() && g_trackBuilt) {
            // Only log objects near the player: gdsim only poses objects within ~220u
            // of the player anyway, so off-screen motion is noise for RE and unbounded
            // in size. Baselines still advance every frame so a re-entering object
            // reports its true per-frame delta, not an accumulated jump.
            const float px = m_player1 ? m_player1->getPositionX() : 0.f;
            const float kWindow = 1500.f;      // ~2 screens of context around the player

            // Per-frame dedup. Objects sharing a group are moved by one trigger and
            // therefore by the SAME delta, so a 40-object group wrote 40
            // identical-motion rows every frame — that redundancy, not the per-row
            // cost, is what made these files reach 17 GB.
            //
            // The representative is STABLE across frames (the first object ever
            // seen for a group), not "whichever object happened to be logged
            // first this frame". An unstable representative silently breaks the
            // reader: positions are stored absolutely and per-frame deltas are
            // reconstructed as the change since that OBJECT's previous row, so a
            // representative that changes frame to frame produces deltas spanning
            // dozens of frames. That bug made a 44-unit platform read as 128869
            // units of travel before this was fixed.
            //
            // Objects that move DIFFERENTLY from their group's representative — a
            // rotate about a centre moves each member differently — still have a
            // different delta and are still logged, so no motion is lost.
            struct Moved { GameObject* obj; int g; float x, y, r, dx, dy, dr; };
            static std::vector<Moved> moved;   // reused; this runs every frame
            moved.clear();
            for (auto& t : g_track) {
                if (!t.obj) continue;
                float x = t.obj->getPositionX(), y = t.obj->getPositionY(), r = t.obj->getRotation();
                float dx = x - t.x, dy = y - t.y, dr = r - t.rot;
                t.x = x; t.y = y; t.rot = r;   // per-frame baseline (always advance)
                if (std::abs(dx) < 1e-4f && std::abs(dy) < 1e-4f && std::abs(dr) < 1e-4f)
                    continue;                  // only rows with real motion
                if (std::abs(x - px) > kWindow) continue;   // off-screen → skip logging
                moved.push_back({t.obj, firstGroupOf(t.obj), x, y, r, dx, dy, dr});
            }

            // Claim a stable representative for any group seen for the first time.
            // Keyed on m_uniqueID (per-instance), never m_objectID (per-type).
            for (auto& m : moved)
                if (m.g >= 0) g_groupRep.emplace(m.g, m.obj->m_uniqueID);

            // Quantised so float noise cannot defeat the "same motion" match.
            auto deltaKey = [](const Moved& m) {
                return ((long long)std::llround(m.dx * 64.0) << 42)
                     ^ ((long long)std::llround(m.dy * 64.0) << 21)
                     ^  (long long)std::llround(m.dr * 4.0);
            };
            g_dedup.clear();
            for (auto& m : moved) {
                auto rep = g_groupRep.find(m.g);
                const bool isRep = (rep != g_groupRep.end() && rep->second == m.obj->m_uniqueID);
                if (isRep) g_dedup.insert(((long long)m.g << 21) ^ deltaKey(m));
            }
            for (auto& m : moved) {
                auto rep = g_groupRep.find(m.g);
                const bool isRep = (rep != g_groupRep.end() && rep->second == m.obj->m_uniqueID);
                if (!isRep && m.g >= 0 &&
                    g_dedup.count(((long long)m.g << 21) ^ deltaKey(m)))
                    continue;                  // moves exactly like its representative
                const double row[9] = { (double)g_frame, (double)m.obj->m_uniqueID,
                                        (double)m.g, m.x, m.y, m.r, m.dx, m.dy, m.dr };
                g_moveBin.row(row);
            }
        }

        // Per-frame physics of whatever you're playing (not gated on a replay).
        // NOTE: this is a per-VISUAL-frame sample, so it necessarily misses physics
        // steps (GD runs 240 of those a second regardless of the render rate) —
        // that is the long-known "dropped frames" property of this file. For
        // per-STEP ground truth use the jump probe below instead.
        if (g_physOut.is_open() && m_player1) {
            auto* p = m_player1;
            g_physOut << g_frame << ' ' << p->getPositionX() << ' ' << p->getPositionY() << ' '
                      << (float)p->m_yVelocity << ' ' << p->m_playerSpeed << ' '
                      << encodeVehicle(p) << ' ' << (p->m_isUpsideDown ? 1 : 0) << ' '
                      << (p->m_vehicleSize < 0.8f ? 1 : 0) << ' ' << (p->m_isOnGround ? 1 : 0) << '\n';
        }

        if ((g_frame % 30) == 0) {
            g_moveBin.flush();
            if (g_physOut.is_open()) g_physOut.flush();
        }
    }
};

// ============================================================
// JUMP PROBE — sub-pixel, per-physics-step capture
// ============================================================
// The per-frame file above samples once per RENDERED frame, so at 60 fps it sees
// one row for every four 240Hz physics steps. Any constant derived from it is
// therefore averaged over an unknown number of steps, which is exactly the wrong
// tool for questions like "is this acceleration 0.958199024 or 0.96?".
//
// This hooks PlayerObject::updateJump — the function the decompiled source calls
// once per physics step — and records the engine's own variables immediately
// BEFORE and AFTER it runs, at 17 significant digits (an exact double
// round-trip). That turns a derivation into arithmetic: one row IS one
// integration step, so
//     (vAfter - vBefore) / dt
// is the acceleration the real engine actually applied on that step, with no
// averaging and nothing inferred.
//
// It logs the same quantities updateJump reads, so the model can be checked term
// by term rather than only at the position it eventually produces:
//   m_gravity, m_gravityMod  -> `float_c = usedGravity * m_gravityMod`
//   m_yStart                 -> the jump impulse for this speed tier
//   m_speedMultiplier        -> the wave/dart rate term
//   size / ground / slope / dash / upside-down -> which branch was taken
//
// Gated behind its OWN setting because it writes ~10 MB per minute of play.
class $modify(JumpProbePlayer, PlayerObject) {
    void updateJump(float dt) {
        // Only the level's real player 1, and only while a capture is active.
        // Menus, icon-kit previews and the dual mirror all run updateJump too and
        // would interleave unrelated rows into the same file.
        PlayLayer* pl = PlayLayer::get();
        const bool capture = rescan::jumpProbeEnabled() && g_jumpOut.is_open()
                          && pl && pl->m_player1 == this;
        if (!capture) { PlayerObject::updateJump(dt); return; }

        const double y0 = this->getPositionY();
        const double x0 = this->getPositionX();
        const double v0 = this->m_yVelocity;

        // Collect the branch tags this step's updateJump writes (see YWrite).
        g_probeTarget = this;
        g_yWrites.clear();
        g_inUpdateJump = true;
        PlayerObject::updateJump(dt);
        g_inUpdateJump = false;

        ++g_jumpStep;
        g_jumpOut
            << g_jumpStep << ' ' << dt << ' '
            << x0 << ' ' << this->getPositionX() << ' '
            << y0 << ' ' << this->getPositionY() << ' '
            << v0 << ' ' << this->m_yVelocity << ' '
            << this->m_gravity << ' ' << this->m_gravityMod << ' '
            << this->m_yStart << ' ' << this->m_speedMultiplier << ' '
            << this->m_playerSpeed << ' ' << this->m_vehicleSize << ' '
            << encodeVehicle(this) << ' '
            << (this->m_isUpsideDown ? 1 : 0) << ' '
            << (this->m_isOnGround   ? 1 : 0) << ' '
            << (this->m_isOnSlope    ? 1 : 0) << ' '
            << this->m_slopeAngle    << ' '
            << this->m_slopeVelocity << ' '
            << this->m_slopeAngleRadians << ' '
            << (this->m_isDashing    ? 1 : 0) << ' '
            << (this->m_isSliding    ? 1 : 0) << ' '
            << (this->m_jumpBuffered ? 1 : 0) << ' '
            << (this->m_isSideways   ? 1 : 0);

        // Branch trace: every setYVelocity this step, as tag:value, in call order.
        // An empty list means the step's whole velocity change came from the
        // inlined addToYVelocity — i.e. plain gravity.
        g_jumpOut << " |";
        for (auto& w : g_yWrites) g_jumpOut << ' ' << w.tag << ':' << w.value;
        g_jumpOut << '\n';

        // Flushed rarely: this runs 240x a second and a flush per step would cost
        // more than the capture itself.
        if ((g_jumpStep % 240) == 0) g_jumpOut.flush();
    }

    // Records the call-site tag of every velocity write. Writes that happen
    // OUTSIDE updateJump (orbs, pads, portals, the collision phase) are just as
    // interesting — they are the impulse tables — so they get their own `Y` line
    // rather than being dropped.
    void setYVelocity(double velocity, int type) {
        const bool capture = rescan::jumpProbeEnabled() && g_jumpOut.is_open()
                          && g_probeTarget == this;
        if (capture) {
            if (g_inUpdateJump) g_yWrites.push_back({velocity, type});
            else g_jumpOut << "Y " << g_jumpStep << ' ' << type << ' ' << velocity
                           << ' ' << this->m_yVelocity << '\n';
        }
        PlayerObject::setYVelocity(velocity, type);
    }
};
