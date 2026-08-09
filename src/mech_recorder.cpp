// ============================================================
// Mechanic recorder — build a perfect physics database from the REAL engine
// ============================================================
// Instead of reverse-engineering every Geometry Dash mechanic into gdsim by hand,
// we let the user just PLAY with the mod running in the background. The mod hooks
// the exact engine methods that fire each mechanic (ringJump = orbs, bumpPlayer =
// pads, flipGravity / toggle*Mode = portals, togglePlayerScale = mini, propell/push
// = direct velocity) and records the player's REAL state immediately before and
// after each one. The captured deltas — keyed by (mechanic, object id) — are the
// engine's own exact parameters, persisted to a GLOBAL database that grows every
// session. A later increment feeds this database into gdsim so the simulator
// reproduces GD exactly for everything you've ever encountered.
//
// This file is intentionally decoupled: it only READS engine state and WRITES a
// data file. It does not touch gdsim or the solver, so it cannot break them.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <map>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iterator>
#include <cmath>
#include <cstdint>

#include "re_scanner.hpp"

using namespace geode::prelude;

static const char* kMechDBPath = "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/movetest/GDMod_mechanics.txt";

// Make sure the movetest/ folder exists before we read/write the DB there, so the
// capture never silently no-ops just because the target directory is missing.
static void ensureMechDir() {
    std::error_code ec;
    std::filesystem::create_directories(
        "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/movetest/", ec);
}

enum class Mech : int {
    Ring, Bump, Propell, Push, FlipGravity,
    ToggleFly, ToggleBird, ToggleDart, ToggleRobot, ToggleSpider, ToggleSwing, ToggleRoll,
    Scale, SwitchMode, HitGround
};
static const char* kMechName[] = {
    "RING", "BUMP", "PROPELL", "PUSH", "FLIPGRAV",
    "FLY", "BIRD", "DART", "ROBOT", "SPIDER", "SWING", "ROLL",
    "SCALE", "SWITCHMODE", "HITGROUND"
};

// Snapshot of the physics state we care about, in the engine's own terms.
struct MechState {
    int   veh = 0;      // 0..7 Cube/Ship/Ball/Ufo/Wave/Robot/Spider/Swing
    int   up = 0;       // upside-down (gravity) flag
    float size = 1.f;   // vehicleSize (mini ~0.6)
    float yVel = 0.f;   // m_yVelocity (true engine Y velocity)
    float speed = 0.f;  // m_playerSpeed
};

struct MechEntry {
    Mech      mech{};
    int       objectId = 0;
    int       objectType = 0;
    MechState before{};
    MechState after{};
    float     param = 0.f;   // method argument (bumpMod / propell yVel)
    int       count = 0;
};

// Global, cross-level database. The key includes the activation CONTEXT, not just
// (mechanic, object id): the method's enable/flip arg plus the player's vehicle,
// gravity and mini state at activation. Without context, GD's internal cleanup
// calls (toggleXMode(false) on every other vehicle when entering one) overwrite the
// real portal-enter events, and an orb's mini-vs-big impulse collapse into one row.
// Context-keyed, every distinct case becomes its own exact learned entry.
static std::map<std::string, MechEntry> g_mechDB;
static bool g_mechDBLoaded = false;
static bool g_mechDBDirty = false;

static std::string mechKey(const MechEntry& e) {
    int pb = (int)std::lround(e.param * 100.f);
    int mini = (e.before.size < 0.8f) ? 1 : 0;
    return fmt::format("{}|{}|{}|{}|{}|{}",
                       kMechName[(int)e.mech], e.objectId, pb,
                       e.before.veh, e.before.up, mini);
}

static int encodeVehicle(PlayerObject* p) {
    if (p->m_isShip)   return 1;
    if (p->m_isBall)   return 2;
    if (p->m_isBird)   return 3;
    if (p->m_isDart)   return 4;
    if (p->m_isRobot)  return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing)  return 7;
    return 0;
}

static MechState snapPlayer(PlayerObject* p) {
    MechState s;
    s.veh   = encodeVehicle(p);
    s.up    = p->m_isUpsideDown ? 1 : 0;
    s.size  = p->m_vehicleSize;
    s.yVel  = static_cast<float>(p->m_yVelocity);
    s.speed = p->m_playerSpeed;
    return s;
}

static void loadMechDB() {
    g_mechDBLoaded = true;
    std::ifstream in(kMechDBPath);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name;
        MechEntry e;
        ss >> name;
        int midx = -1;
        for (int i = 0; i < (int)std::size(kMechName); ++i)
            if (name == kMechName[i]) { midx = i; break; }
        if (midx < 0) continue;
        e.mech = (Mech)midx;
        // The save format puts a literal '|' between `count` and the state block.
        // It MUST be consumed explicitly: `>>` into an int hits '|', sets failbit,
        // and every field after it silently keeps its MechEntry default (size=1,
        // rest 0). Since mechKey() is built from param/before.veh/before.up/mini,
        // every loaded row then collapsed onto one key per (mech,objectId) and the
        // per-vehicle/per-gravity variants — the whole point of the DB — were lost
        // on each save. Rows that don't parse cleanly are skipped, never inserted
        // as zeroed garbage.
        std::string sep;
        if (!(ss >> e.objectId >> e.objectType >> e.count >> sep)) continue;
        if (sep != "|") continue;
        if (!(ss >> e.before.veh >> e.after.veh >> e.before.up >> e.after.up
                 >> e.before.size >> e.after.size >> e.before.yVel >> e.after.yVel
                 >> e.before.speed >> e.after.speed >> e.param)) continue;
        g_mechDB[mechKey(e)] = e;
    }
    log::info("[MechDB] loaded {} entries from {}", g_mechDB.size(), kMechDBPath);
}

static void saveMechDB() {
    ensureMechDir();
    std::ofstream out(kMechDBPath, std::ios::trunc);
    if (!out.is_open()) { log::error("[MechDB] cannot write {}", kMechDBPath); return; }
    out << "# GDMod mechanics database - auto-captured from the real GD engine.\n";
    out << "# mech objId objType count | bVeh aVeh bUp aUp bSize aSize bYVel aYVel bSpeed aSpeed param\n";
    for (auto& [k, e] : g_mechDB) {
        out << kMechName[(int)e.mech] << ' ' << e.objectId << ' ' << e.objectType << ' ' << e.count
            << " | " << e.before.veh << ' ' << e.after.veh << ' ' << e.before.up << ' ' << e.after.up
            << ' ' << e.before.size << ' ' << e.after.size << ' ' << e.before.yVel << ' ' << e.after.yVel
            << ' ' << e.before.speed << ' ' << e.after.speed << ' ' << e.param << '\n';
    }
    g_mechDBDirty = false;
}

// Materially different effect than what we already have? (so we only log/rewrite on
// genuinely new information, not every repeat of a known orb).
static bool effectChanged(const MechEntry& a, const MechEntry& b) {
    auto df = [](float x, float y) { return std::abs(x - y) > 0.05f; };
    return a.after.veh != b.after.veh || a.after.up != b.after.up
        || df(a.after.size, b.after.size) || df(a.after.yVel, b.after.yVel)
        || df(a.after.speed, b.after.speed) || df(a.param, b.param);
}

static void recordMech(Mech m, int objectId, int objectType,
                       const MechState& before, const MechState& after, float param) {
    if (!rescan::enabled()) return;   // gated by the shared "re-scanner" master switch

    // Skip level-load / preview noise: these mechanic methods fire during init and
    // resets with the player not actually playing (m_playerSpeed == 0). Those carry
    // no real physics, only pollute the database.
    if (before.speed == 0.f && after.speed == 0.f) return;

    // Skip pure no-ops: GD internally calls toggleXMode(false) on every other
    // vehicle when entering one, and the toggles fire each frame with no change.
    // If nothing the player cares about changed, there is nothing to learn.
    bool stateChanged = before.veh != after.veh || before.up != after.up
        || std::abs(before.size - after.size) > 0.01f
        || std::abs(before.yVel - after.yVel) > 0.01f
        || std::abs(before.speed - after.speed) > 0.01f;
    if (!stateChanged) return;

    if (!g_mechDBLoaded) loadMechDB();

    MechEntry e;
    e.mech = m; e.objectId = objectId; e.objectType = objectType;
    e.before = before; e.after = after; e.param = param; e.count = 1;

    std::string key = mechKey(e);
    auto it = g_mechDB.find(key);
    bool isNew = (it == g_mechDB.end());
    bool changed = isNew || effectChanged(e, it->second);
    if (!isNew) e.count = it->second.count + 1;
    g_mechDB[key] = e;

    if (changed) {
        g_mechDBDirty = true;
        saveMechDB();
        log::info("[MechDB] {} id={} type={}  veh {}->{} up {}->{} size {:.2f}->{:.2f} "
                  "yVel {:.2f}->{:.2f} speed {:.1f}->{:.1f} param={:.3f}{}",
                  kMechName[(int)m], objectId, objectType,
                  before.veh, after.veh, before.up, after.up, before.size, after.size,
                  before.yVel, after.yVel, before.speed, after.speed, param,
                  isNew ? "  (NEW)" : "  (changed)");
    }
}

class $modify(MecRecPlayer, PlayerObject) {
    void ringJump(RingObject* object, bool skipCheck) {
        MechState before = snapPlayer(this);
        PlayerObject::ringJump(object, skipCheck);
        MechState after = snapPlayer(this);
        int id = object ? object->m_objectID : 0;
        int ty = object ? (int)object->m_objectType : -1;
        recordMech(Mech::Ring, id, ty, before, after, 0.f);
    }

    void bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object) {
        MechState before = snapPlayer(this);
        PlayerObject::bumpPlayer(bumpMod, objectType, noEffects, object);
        MechState after = snapPlayer(this);
        int id = object ? object->m_objectID : 0;
        recordMech(Mech::Bump, id, objectType, before, after, bumpMod);
    }

    void propellPlayer(float yVelocity, bool noEffects, int objectType) {
        MechState before = snapPlayer(this);
        PlayerObject::propellPlayer(yVelocity, noEffects, objectType);
        MechState after = snapPlayer(this);
        recordMech(Mech::Propell, 0, objectType, before, after, yVelocity);
    }

    void flipGravity(bool flip, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::flipGravity(flip, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::FlipGravity, 0, -1, before, after, flip ? 1.f : 0.f);
    }

    void toggleFlyMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleFlyMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleFly, 0, -1, before, after, enable ? 1.f : 0.f);
    }
    void toggleBirdMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleBirdMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleBird, 0, -1, before, after, enable ? 1.f : 0.f);
    }
    void toggleDartMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleDartMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleDart, 0, -1, before, after, enable ? 1.f : 0.f);
    }
    void toggleRobotMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleRobotMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleRobot, 0, -1, before, after, enable ? 1.f : 0.f);
    }
    void toggleSpiderMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleSpiderMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleSpider, 0, -1, before, after, enable ? 1.f : 0.f);
    }
    void toggleSwingMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleSwingMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleSwing, 0, -1, before, after, enable ? 1.f : 0.f);
    }
    void toggleRollMode(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::toggleRollMode(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::ToggleRoll, 0, -1, before, after, enable ? 1.f : 0.f);
    }

    void togglePlayerScale(bool enable, bool noEffects) {
        MechState before = snapPlayer(this);
        PlayerObject::togglePlayerScale(enable, noEffects);
        MechState after = snapPlayer(this);
        recordMech(Mech::Scale, 0, -1, before, after, enable ? 1.f : 0.f);
    }
};
