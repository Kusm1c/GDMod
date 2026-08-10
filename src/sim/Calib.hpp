#pragma once

// Runtime-tunable physics constants for gdsim ACCURACY calibration.
//
// The solver only matches the real game if gdsim reproduces GD's physics exactly.
// Rather than guess each constant and rebuild, these knobs are read live by the
// sim, so the offline calibrator (test/calibrate.cpp) can sweep them against
// recorded real-game trajectories until divergence ~= 0. Once converged, the
// values are baked back in as the defaults below.
//
// IMPORTANT: defaults here must reproduce the *current* behaviour exactly, so the
// sim is unchanged until the calibrator finds better values.

namespace gdsim {

// Portal trigger categories (which CalibParams.portalReach entry applies).
enum PortalCat : int {
    PCAT_NONE     = 0,
    PCAT_SPEED    = 1,
    PCAT_SIZE     = 2,
    PCAT_VEHICLE  = 3,
    PCAT_GRAVITY  = 4,
    PCAT_DUAL     = 5,
    PCAT_TELEPORT = 6,
    PCAT_COUNT    = 7,
};

struct CalibParams {
    // Extra half-extent (world units) added to a portal's hitbox per category, so
    // it fires earlier (+) / later (-) to land on GD's exact trigger frame.
    // PCAT order: NONE, SPEED, SIZE, VEHICLE, GRAVITY, DUAL, TELEPORT. SIZE gets a
    // +1u forgiveness: when a mini portal is STACKED behind a wider wave portal at
    // the same X, the wave shrinks the player before its tiny hitbox reaches the
    // narrower mini portal, so gdsim ran a 1× wave where the real game runs a 2×
    // mini wave. NEVER make VEHICLE reach negative — it delays the gamemode change
    // a frame and desyncs fast sections (the calibrator's old −2 did exactly that).
    // TELEPORT gets +5u: TeleportPortal never had a triggerCat at all until found
    // 2026-08-10 via level 82172844 ("Cobwebs") — its very first teleport portal
    // sits at x=-29 (behind spawn) and was geometrically unreachable by a MINI
    // player's hitbox (needs the player's raw `size` — currently the "cube's
    // 30/18" convention, i.e. half-width 9 when mini — to reach 3+ units further
    // back than it can) even though a full-size player's half-width (15) would
    // have reached it fine. +5 closes that specific gap with a small margin; not
    // independently re-derived from a real capture, so treat it as a placeholder
    // pending real evidence, same caveat as the untouched categories below.
    float portalReach[PCAT_COUNT] = {0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 5.f};

    // Frames a speed-portal change is delayed before it reaches the X-position
    // integrator (GD applies it later than the wave Y-rate etc.). 1 = current.
    int speedXLagFrames = 1;

    // Margin (units) added to the player's hitbox for HAZARD tests, to match GD's
    // slightly larger effective hazard hitboxes. This is the ACCURACY value (auto
    // applied everywhere); the solver may add its own extra safety on top.
    float hazardHitboxInflate = 0.f;

    // Scale applied to move-trigger offsets (level fields 28/29). GD's stored unit
    // convention is ambiguous; calibrate it against a recorded moving-object level.
    float moveScale = 1.0f;

    // Ship's m_isAccelerating auto-clears (per the decompiled PlayerObject::updateJump)
    // whenever m_yVelocity re-enters a small band around zero: (-6.4, 8.0) in real
    // units — asymmetric, not a plain abs()<threshold. gdsim's velocity isn't in the
    // same units as real m_yVelocity (real positions/velocities go through an
    // internal scale before reaching world coordinates), so these are gdsim's own-unit
    // equivalents of that band's two edges, calibrated against truth rather than
    // hand-derived — the exact real->gdsim velocity scale isn't otherwise known.
    // Defaults to the old (uncalibrated, symmetric) guess on both edges.
};

// Single live instance the whole sim reads from. The calibrator mutates it
// between runs; the mod leaves it at the baked-in defaults.
extern CalibParams g_calib;

// SOLVER-ONLY vertical robustness margin (world units) for the flight death-box
// against SOLID BLOCKS. This is NOT a physics-accuracy value — gdsim's ship physics
// are exact (measured). It exists because gdsim tracks the real ship only within
// ~6-7u of accumulated click-phase drift over long flight sections, with the real
// ship sitting consistently on the grav-up (ceiling) side of the sim. The solver
// raises this so it keeps clearance below block ceilings instead of threading gaps
// frame-perfect (those "solve" here but die in real GD once the ship drifts up).
// Player::blockDeathHitbox extends the flight box toward the ship's ceiling by this
// much. MUST stay 0 for View Level / replay / divergence / validation (those model
// the true physics). Set live by solveLevel(), reset to 0 when it returns.
inline float g_solveShipBlockClearance = 0.f;

// Solver-only clearance margin (world units) for the GROUNDED-mode death box
// (Cube/Robot/Spider) against SOLID BLOCKS. gdsim's block death box is calibrated
// small (7×7) to match GD's corner-clip leniency, but that leaves the solver free
// to thread a path that grazes a block inside-corner by <1u — survivable in-sim yet
// fatal in real GD once ~a-few-u of click-phase/apex residual is added (mini Robot
// on 13711278 dies at x=1468 wedged into an id-468 bar+ledge junction the 4.2-wide
// mini death box never reaches). Player::blockDeathHitbox widens the box on the
// grav-up (top) + both horizontal sides — never the bottom, so a normal platform
// landing can't false-trigger. Like the ship margin, MUST stay 0 for View Level /
// replay / divergence / validation; set live by solveLevel() and reset to 0 on exit.
inline float g_solveRobotBlockClearance = 0.f;

// Load/save g_calib to a plain key-value file so the calibrator's output can be
// applied in-game WITHOUT rebuilding (the mod reads it at level load). Missing
// file / keys leave the defaults untouched. Returns true if the file was read.
bool loadCalibFromFile(const char* path);
void saveCalibToFile(const char* path);

} // namespace gdsim
