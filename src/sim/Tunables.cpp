#include "Tunables.hpp"
#include "Calib.hpp"
#include "Player.hpp"
#include "Orb.hpp"
#include "Pad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace gdsim {

PhysicsTables g_phys{};

// ─── Tunable accessors ───────────────────────────────────────────────────────

double Tunable::get() const {
    switch (kind) {
        case TunKind::F64:  return *static_cast<double*>(ptr);
        case TunKind::F32:  return (double)*static_cast<float*>(ptr);
        case TunKind::I32:  return (double)*static_cast<int*>(ptr);
        case TunKind::Bool: return *static_cast<bool*>(ptr) ? 1.0 : 0.0;
    }
    return 0.0;
}

void Tunable::set(double v) const {
    switch (kind) {
        case TunKind::F64:  *static_cast<double*>(ptr) = v; break;
        case TunKind::F32:  *static_cast<float*>(ptr) = (float)v; break;
        // std::lround, not a C cast: a value scrubbed to 65.9999 by a float drag
        // should read as 66, not silently truncate to 65 — exactly the off-by-one
        // a frame-count knob like robotChargeFrames is most sensitive to.
        case TunKind::I32:  *static_cast<int*>(ptr) = (int)std::lround(v); break;
        case TunKind::Bool: *static_cast<bool*>(ptr) = (v != 0.0); break;
    }
}

bool Tunable::modified() const {
    // Exact compare on purpose: "modified" must mean "the user touched this",
    // and a reset writes the default back bit-for-bit, so there is no epsilon to
    // chase. A drag that lands 1e-15 away IS a modification worth flagging.
    return get() != defVal;
}

// ─── Registry construction ───────────────────────────────────────────────────

namespace {

// Pick a readable drag step and soft range from the default's own magnitude, so
// a table of ~2700-unit accelerations and a table of ~0.5 scales both feel right
// under the same drag gesture without hand-tuning ~400 entries.
void autoRange(Tunable& t) {
    const double a = std::fabs(t.defVal);
    if (a == 0.0)      { t.lo = -10.0;      t.hi = 10.0;       t.step = 0.01;  }
    else if (a < 1.0)  { t.lo = -a * 4 - 1; t.hi = a * 4 + 1;  t.step = a * 0.002; }
    else               { t.lo = t.defVal - a * 1.5; t.hi = t.defVal + a * 1.5; t.step = a * 0.001; }
    if (t.defVal < 0 && a >= 1.0) std::swap(t.lo, t.hi);
    if (t.step <= 0.0) t.step = 0.001;
}

std::vector<Tunable> g_registry;

void add(const char* group, std::string name, TunKind kind, void* ptr,
         const char* help, double step = 0.0, double lo = 0.0, double hi = 0.0) {
    Tunable t;
    t.group = group;
    t.name  = std::move(name);
    t.help  = help ? help : "";
    t.kind  = kind;
    t.ptr   = ptr;
    t.defVal = t.get();          // whatever the field holds right now IS the default
    if (lo != hi) { t.lo = lo; t.hi = hi; t.step = step > 0 ? step : 0.01; }
    else          { autoRange(t); if (step > 0) t.step = step; }
    g_registry.push_back(std::move(t));
}

void addF64(const char* g, std::string n, double* p, const char* h) { add(g, std::move(n), TunKind::F64, p, h); }
void addF32(const char* g, std::string n, float*  p, const char* h) { add(g, std::move(n), TunKind::F32, p, h); }
void addI32(const char* g, std::string n, int*    p, const char* h, double lo, double hi) {
    add(g, std::move(n), TunKind::I32, p, h, 1.0, lo, hi);
}

// Speed tiers are indexed 0..4 everywhere in the engine; label them the way the
// level editor does so a row is recognisable without counting.
const char* kSpeedName[5] = {"0.5x", "1x", "2x", "3x", "4x"};
// Orb/pad tables only store four tiers (the engine clamps index to <=3).
const char* kOrbSpeedName[4] = {"0.5x", "1x", "2x", "3x"};

const char* orbTypeName(OrbType t) {
    switch (t) {
        case OrbType::Yellow: return "Yellow";
        case OrbType::Blue:   return "Blue";
        case OrbType::Pink:   return "Pink";
        case OrbType::Red:    return "Red";
        case OrbType::Green:  return "Green";
        case OrbType::Black:  return "Black";
        case OrbType::Dash:   return "Dash";
        case OrbType::Spider: return "Spider";
        case OrbType::GravityFlip: return "GravityFlip";
    }
    return "?";
}
const char* padTypeName(PadType t) {
    switch (t) {
        case PadType::Yellow: return "Yellow";
        case PadType::Blue:   return "Blue";
        case PadType::Pink:   return "Pink";
        case PadType::Red:    return "Red";
    }
    return "?";
}
const char* vehName(VehicleType v) {
    switch (v) {
        case VehicleType::Cube:   return "Cube";
        case VehicleType::Ship:   return "Ship";
        case VehicleType::Ball:   return "Ball";
        case VehicleType::Ufo:    return "UFO";
        case VehicleType::Wave:   return "Wave";
        case VehicleType::Robot:  return "Robot";
        case VehicleType::Spider: return "Spider";
        case VehicleType::Swing:  return "Swing";
    }
    return "?";
}

void buildRegistry() {
    auto& P = g_phys;
    auto& C = g_calib;

    // ── Speed ─────────────────────────────────────────────────────────────────
    for (int i = 0; i < 5; i++)
        addF64("Speed", std::string("X rate ") + kSpeedName[i], &player_speeds[i],
               "World units/sec the player advances in X at this speed tier. Also the wave/dart Y rate.");
    for (int i = 0; i < 5; i++)
        add("Speed", std::string("speed multiplier ") + kSpeedName[i], TunKind::F32, &player_speedmults[i],
            "GD's own m_speedMultiplier for this tier (0.7/0.9/1.1/1.3/1.6).");

    // ── Cube ──────────────────────────────────────────────────────────────────
    for (int i = 0; i < 5; i++)
        addF64("Cube", std::string("gravity ") + kSpeedName[i], &P.cubeAccel[i],
               "Downward acceleration (units/sec^2) for a grounded/airborne cube at this tier.");
    for (int i = 0; i < 5; i++)
        addF64("Cube", std::string("jump impulse ") + kSpeedName[i], &P.cubeJumpHeight[i],
               "Velocity set on a ground jump at this tier.");
    addF64("Cube", "upside-down extra gravity", &P.cubeUpsideExtraAccel,
           "Subtracted from acceleration while upside-down (asymmetry present in the real engine).");
    addF64("Cube", "mini jump scale", &P.cubeMiniJumpScale, "Multiplier on the jump impulse when mini.");
    addF64("Cube", "terminal fall speed", &P.maxFallVelocity,
           "Hard clamp on downward velocity. Verified EXACTLY 810 against the decompile.");
    addI32("Cube", "upside-down coyote frames", &P.cubeUpsideCoyoteFrames,
           "Frames after leaving a surface during which an upside-down jump still registers.", 0, 60);

    // ── Ship ──────────────────────────────────────────────────────────────────
    for (int i = 0; i < 5; i++)
        addF64("Ship", std::string("accel threshold ") + kSpeedName[i], &P.velocityThreshold[i],
               "Velocity band edge that switches between the strong and weak accel. Read through grav().");
    addF64("Ship", "accel up strong (mini)",  &P.shipAccelUpStrongSmall,  "Thrust while held and below the threshold, mini.");
    addF64("Ship", "accel up strong (big)",   &P.shipAccelUpStrongBig,    "Thrust while held and below the threshold, full size.");
    addF64("Ship", "accel up weak (mini)",    &P.shipAccelUpWeakSmall,    "Thrust while held and above the threshold, mini.");
    addF64("Ship", "accel up weak (big)",     &P.shipAccelUpWeakBig,      "Thrust while held and above the threshold, full size.");
    addF64("Ship", "accel down strong (mini)",&P.shipAccelDownStrongSmall,"Fall accel while released and above the threshold, mini.");
    addF64("Ship", "accel down strong (big)", &P.shipAccelDownStrongBig,  "Fall accel while released and above the threshold, full size.");
    addF64("Ship", "accel down weak (mini)",  &P.shipAccelDownWeakSmall,  "Fall accel while released and below the threshold, mini.");
    addF64("Ship", "accel down weak (big)",   &P.shipAccelDownWeakBig,    "Fall accel while released and below the threshold, full size.");
    addF64("Ship", "velocity clamp min (mini)", &P.shipClampMinSmall, "Lower bound on ship velocity, mini.");
    addF64("Ship", "velocity clamp min (big)",  &P.shipClampMinBig,   "Lower bound on ship velocity, full size.");
    addF64("Ship", "velocity clamp max (mini)", &P.shipClampMaxSmall, "Upper bound on ship velocity, mini.");
    addF64("Ship", "velocity clamp max (big)",  &P.shipClampMaxBig,   "Upper bound on ship velocity, full size.");
    addF64("Ship", "post-update |v| clamp", &P.shipVelClamp, "Final absolute velocity clamp applied after the ship update.");

    // ── Ball ──────────────────────────────────────────────────────────────────
    addF64("Ball", "gravity", &P.ballAccel, "Ball acceleration (= 0.9582 * 0.6 in the decompile's grouping).");
    for (int i = 0; i < 5; i++)
        addF64("Ball", std::string("flip impulse ") + kSpeedName[i], &P.ballJumpHeight[i],
               "Velocity set when the ball swaps surface at this tier.");
    addI32("Ball", "coyote frames (upside-down)", &P.ballCoyoteUpsideFrames, "Buffered-input window while upside-down.", 0, 60);
    addI32("Ball", "coyote frames (normal)",      &P.ballCoyoteNormalFrames, "Buffered-input window while right-way-up.", 0, 60);

    // ── UFO ───────────────────────────────────────────────────────────────────
    addF64("UFO", "flap floor (mini)", &P.ufoFlapMinSmall, "Minimum POST-flap velocity, mini (a floor, not an impulse).");
    addF64("UFO", "flap floor (big)",  &P.ufoFlapMinBig,   "Minimum POST-flap velocity, full size.");
    addF64("UFO", "accel strong (mini)", &P.ufoAccelStrongSmall, "Accel above the threshold, mini. Threshold is grav()-wrapped.");
    addF64("UFO", "accel strong (big)",  &P.ufoAccelStrongBig,   "Accel above the threshold, full size.");
    addF64("UFO", "accel weak (mini)",   &P.ufoAccelWeakSmall,   "Accel below the threshold, mini.");
    addF64("UFO", "accel weak (big)",    &P.ufoAccelWeakBig,     "Accel below the threshold, full size.");
    addF64("UFO", "velocity clamp min (mini)", &P.ufoClampMinSmall, "Lower velocity bound, mini.");
    addF64("UFO", "velocity clamp min (big)",  &P.ufoClampMinBig,   "Lower velocity bound, full size.");
    addF64("UFO", "velocity clamp max (mini)", &P.ufoClampMaxSmall, "Upper velocity bound, mini.");
    addF64("UFO", "velocity clamp max (big)",  &P.ufoClampMaxBig,   "Upper velocity bound, full size.");

    // ── Wave ──────────────────────────────────────────────────────────────────
    addF64("Wave", "mini rate multiplier", &P.waveMiniMult, "Mini wave climbs/dives this many times faster than full size.");
    addF32("Wave", "half-size (mini)", &P.waveHalfSizeSmall, "Wave hitbox half-extent when mini.");
    addF32("Wave", "half-size (big)",  &P.waveHalfSizeBig,   "Wave hitbox half-extent at full size.");
    addF32("Wave", "rotation rate (mini)", &P.waveRotSmall, "Cosmetic sprite rotation rate, mini.");
    addF32("Wave", "rotation rate (big)",  &P.waveRotBig,   "Cosmetic sprite rotation rate, full size.");

    // ── Robot ─────────────────────────────────────────────────────────────────
    addI32("Robot", "charge frames (mini)", &P.robotChargeFramesSmall, "Hold-to-charge sustain cap, mini.", 0, 200);
    addI32("Robot", "charge frames (big)",  &P.robotChargeFramesBig,   "Hold-to-charge sustain cap, full size. 66 measured from a real capture.", 0, 200);
    for (int i = 0; i < 5; i++)
        addF64("Robot", std::string("jump max ") + kSpeedName[i], &P.robotJumpMax[i], "Peak jump velocity at this tier.");
    for (int i = 0; i < 5; i++)
        addF64("Robot", std::string("gravity ") + kSpeedName[i], &P.robotAccel[i], "Base gravity at this tier (scaled by the fall scale below).");
    addF64("Robot", "ballistic fall scale", &P.robotFallScale, "Multiplier on gravity once the charge ends.");
    addF64("Robot", "upside-down extra gravity", &P.robotUpsideExtraAccel, "Extra gravity while upside-down.");
    addI32("Robot", "upside-down coyote frames", &P.robotUpsideCoyoteFrames, "Buffered-input window while upside-down.", 0, 60);

    // ── Spider ────────────────────────────────────────────────────────────────
    addF64("Spider", "gravity", &P.spiderAccel, "Spider acceleration - same gravity group as the Ball.");
    addI32("Spider", "coyote frames (upside-down)", &P.spiderCoyoteUpsideFrames, "Buffered-input window while upside-down.", 0, 60);
    addI32("Spider", "coyote frames (normal)",      &P.spiderCoyoteNormalFrames, "Buffered-input window while right-way-up.", 0, 60);

    // ── Swing ─────────────────────────────────────────────────────────────────
    addF64("Swing", "velocity clamp min (mini)", &P.swingClampMinSmall, "Lower velocity bound, mini.");
    addF64("Swing", "velocity clamp min (big)",  &P.swingClampMinBig,   "Lower velocity bound, full size.");
    addF64("Swing", "velocity clamp max (mini)", &P.swingClampMaxSmall, "Upper velocity bound, mini.");
    addF64("Swing", "velocity clamp max (big)",  &P.swingClampMaxBig,   "Upper velocity bound, full size.");

    // ── Portal hitboxes ───────────────────────────────────────────────────────
    // These are read when a Level is CONSTRUCTED, so the UI must resim (not just
    // continue) for a change here to be visible. Engine-measured 2026-08-23.
    struct { const char* n; float* w; float* h; const char* help; } portals[] = {
        {"vehicle 12/13/47/111/660/745/1331/1933/2751", &P.portalVehicleW,  &P.portalVehicleH,  "Gamemode portal. Measured 34x86."},
        {"teleport 747",   &P.portalTeleportW,  &P.portalTeleportH,  "Teleport portal. Measured 25x90."},
        {"gravity 10/11",  &P.portalGravityW,   &P.portalGravityH,   "Gravity portal. Measured 25x75."},
        {"size 99/101",    &P.portalSizeW,      &P.portalSizeH,      "Mini/big portal. Measured 31x90."},
        {"dual 286/287",   &P.portalDualW,      &P.portalDualH,      "Dual portal. Measured 41x91."},
        {"speed 200",      &P.portalSpeed200W,  &P.portalSpeed200H,  "0.5x speed portal. Measured 35x44."},
        {"speed 201",      &P.portalSpeed201W,  &P.portalSpeed201H,  "1x speed portal. Measured 33x56."},
        {"speed 202",      &P.portalSpeed202W,  &P.portalSpeed202H,  "2x speed portal. Measured 51x56."},
        {"speed 203",      &P.portalSpeed203W,  &P.portalSpeed203H,  "3x speed portal (65 x 56, GameObject::setupSpriteSize)."},
        {"speed 1334",     &P.portalSpeed1334W, &P.portalSpeed1334H, "4x speed portal. Measured 69x56."},
    };
    for (auto& pt : portals) {
        add("Portals", std::string(pt.n) + " - width",  TunKind::F32, pt.w, pt.help, 0.5, 1.0, 120.0);
        add("Portals", std::string(pt.n) + " - height", TunKind::F32, pt.h, pt.help, 0.5, 1.0, 200.0);
    }

    // ── Calibration (Calib.hpp) ───────────────────────────────────────────────
    static const char* pcatName[PCAT_COUNT] =
        {"none", "speed", "size", "vehicle", "gravity", "dual", "teleport"};
    for (int i = 0; i < PCAT_COUNT; i++)
        add("Calibration", std::string("portal reach - ") + pcatName[i], TunKind::F32, &C.portalReach[i],
            "Extra half-extent added to this portal category's hitbox: + fires earlier, - later.",
            0.05, -20.0, 20.0);
    add("Calibration", "wave entry reach adjust", TunKind::F32, &C.waveEntryReachAdjust,
        "Wave-only extra reach at a Cube->Wave portal, separate from the shared vehicle reach.", 0.05, -20.0, 20.0);
    add("Calibration", "gravity-up reach adjust", TunKind::F32, &C.gravityUpReachAdjust,
        "Extra reach applied ONLY to the id=11 (gravity-up) portal, on top of the gravity category.", 0.05, -20.0, 20.0);
    add("Calibration", "speed X lag frames", TunKind::I32, &C.speedXLagFrames,
        "Frames a speed change is delayed before it reaches the X integrator.", 1.0, 0.0, 10.0);
    add("Calibration", "hazard hitbox inflate", TunKind::F32, &C.hazardHitboxInflate,
        "Margin added to the player hitbox for HAZARD tests only.", 0.05, -10.0, 10.0);
    add("Calibration", "move trigger scale", TunKind::F32, &C.moveScale,
        "Scale on move-trigger offsets (level fields 28/29).", 0.005, 0.0, 4.0);
    add("Calibration", "slope exit velocity scale", TunKind::F32, &C.slopeExitVelScale,
        "Overall scale on the slope launch velocity. 0.873 from a real DeCode capture.", 0.002, 0.0, 2.0);
    add("Calibration", "slope jump velocity scale", TunKind::F32, &C.slopeJumpVelScale,
        "Overall scale on the click-while-on-a-slope boost. 0.588 from a real capture.", 0.002, 0.0, 2.0);
    add("Calibration", "gravity portal flip scale", TunKind::F32, &C.gravityPortalFlipScale,
        "Magnitude factor on velocity at a gravity flip. The decompile says a flat 0.5.", 0.002, 0.0, 1.5);

    // ── Solver margins ────────────────────────────────────────────────────────
    // Not physics - deliberate robustness padding the solver adds. Exposed so the
    // effect of shrinking them (the Phase-B endgame) can be tried without a rebuild.
    add("Solver margins", "ship block clearance", TunKind::F32, &g_solveShipBlockClearance,
        "SOLVER-ONLY vertical padding on the flight death box vs solid blocks. MUST be 0 for replay/validation.",
        0.1, 0.0, 20.0);
    add("Solver margins", "robot block clearance", TunKind::F32, &g_solveRobotBlockClearance,
        "SOLVER-ONLY padding on the grounded death box vs solid blocks. MUST be 0 for replay/validation.",
        0.1, 0.0, 20.0);

    // ── Orb / Pad velocity tables ─────────────────────────────────────────────
    // One row per (type, vehicle, mini) key, one tunable per speed tier inside it.
    // Pointers are into the map's stored std::vector<double>; the map is never
    // rehashed after construction, so they stay valid for the process lifetime.
    {
        // Deterministic ordering: iterating an unordered_map directly would shuffle
        // the UI between runs, which makes a long list unusable.
        std::vector<std::pair<std::string, std::vector<double>*>> rows;
        for (auto& kv : orb_velocities) {
            auto [ot, vt, mini] = kv.first;
            std::string label = std::string(orbTypeName(ot)) + " - " + vehName(vt) + (mini ? " (mini)" : " (big)");
            rows.emplace_back(std::move(label), &kv.second);
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [label, vec] : rows)
            for (size_t s = 0; s < vec->size() && s < 4; s++)
                addF64("Orbs", label + "  " + kOrbSpeedName[s], &(*vec)[s],
                       "Velocity this orb sets, for this vehicle/size at this speed tier.");
    }
    {
        std::vector<std::pair<std::string, std::vector<double>*>> rows;
        for (auto& kv : pad_velocities) {
            auto [pt, vt, mini] = kv.first;
            std::string label = std::string(padTypeName(pt)) + " - " + vehName(vt) + (mini ? " (mini)" : " (big)");
            rows.emplace_back(std::move(label), &kv.second);
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [label, vec] : rows)
            for (size_t s = 0; s < vec->size() && s < 4; s++)
                addF64("Pads", label + "  " + kOrbSpeedName[s], &(*vec)[s],
                       "Velocity this pad sets, for this vehicle/size at this speed tier.");
    }
}

} // namespace

std::vector<Tunable>& allTunables() {
    // Lazy, so every static table in every TU is guaranteed constructed by the
    // time we take pointers into it (static-init order across TUs is undefined).
    if (g_registry.empty()) buildRegistry();
    return g_registry;
}

int modifiedTunableCount() {
    int n = 0;
    for (auto& t : allTunables()) if (t.modified()) n++;
    return n;
}

void resetAllTunables() {
    for (auto& t : allTunables()) t.reset();
}

bool saveTunablePreset(const std::string& path) {
    std::ofstream o(path, std::ios::trunc);
    if (!o) return false;
    o << "# gdsim tunable preset - one 'group/name<TAB>value' per line.\n";
    o << "# Only values that differ from the built-in default are written, so a\n";
    o << "# preset stays readable and keeps tracking future default changes.\n";
    // 17 significant digits round-trips an IEEE double exactly, so loading a
    // preset restores the bit-identical value and 'modified' stays honest.
    o.precision(17);
    for (auto& t : allTunables()) {
        if (!t.modified()) continue;
        o << t.key() << "\t" << t.get() << "\n";
    }
    return (bool)o;
}

bool loadTunablePreset(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;

    std::unordered_map<std::string, double> want;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = line.substr(0, tab);
        double v = 0.0;
        std::istringstream(line.substr(tab + 1)) >> v;
        want[key] = v;
    }

    // A preset is a full state, not a patch: anything it does NOT mention goes
    // back to its default. Otherwise loading preset B after preset A would leave
    // A's untouched-by-B edits silently in effect.
    for (auto& t : allTunables()) {
        auto it = want.find(t.key());
        if (it != want.end()) t.set(it->second);
        else                  t.reset();
    }
    return true;
}

} // namespace gdsim
