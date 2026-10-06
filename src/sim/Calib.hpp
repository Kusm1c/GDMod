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
    // Re-tuned 2026-08-18 (auto-calibrator, test/calibrate.cpp, full 13-level
    // truth bank) after Object.cpp's portal hitboxes were corrected to real
    // measured in-game values (every portal 30x90 except gravity's 30x75 —
    // see Object.cpp's 2026-08-18 comment). The old reach values were tuned
    // against the PREVIOUS, mostly-guessed base sizes (e.g. SpeedPortal varied
    // 35x44 to 69x56 per sub-id); with correct base sizes the old reach
    // over/under-shot at several portal touches (0.85u/812u drift on 3
    // truth-bank levels, one previously-clean level regressing to ~1.8u).
    // SPEED -5.5, VEHICLE +2, GRAVITY -2.5; SIZE/DUAL unchanged (already 0
    // residual). Still respects the VEHICLE-never-negative rule below.
    // KNOWN TRADE-OFF: fixed 2 truth-bank regressions this same change caused
    // (174, 308) and cut level 85701165's divergence from 812u to 1.3u, at
    // the cost of a small NEW one on 62687326 (Wave, ground bounce near
    // spawn, sev=1.81 — was clean before). Root cause there looks like
    // reach.speed=-5.5 being slightly too aggressive for a speed portal very
    // close to that level's spawn; not re-tuned further given the net win
    // elsewhere and the 157-level real-macro batch staying at 1/157 clear
    // (no regression there). Revisit if 62687326 (or another wave-near-spawn
    // level) becomes load-bearing.
    // TRIED 2026-08-21 (DeCode wave section): found gdsim's Cube->Wave vehicle-portal
    // touch registers 2 frames EARLIER than a click-based reference frame (see
    // wave_velocity_scale_trap_and_decode_status.md memory) — produces a small,
    // constant ~5.35u Y offset for the rest of DeCode's wave section. Tried lowering
    // portalReach[PCAT_VEHICLE] 2.0->-1.2 (~-3.2, the X-distance 2 frames covers here)
    // to delay the trigger — REGRESSED level 174 (f402->f334). This constant is
    // GLOBAL (all vehicle-portal touches, all levels, also used by the solver's own
    // reach calc), so a value that fixes ONE wave-entry instance broke a different
    // level's portal timing. Reverted. If revisited: needs a WAVE-specific (or even
    // this-transition-specific) adjustment, not a change to the shared PCAT_VEHICLE
    // constant — e.g. in VehiclePortal::collide/touching, gated on `type ==
    // VehicleType::Wave` specifically, not a global reach retune.
    float portalReach[PCAT_COUNT] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 5.f};

    // FOUND+FIXED 2026-08-21 (DeCode, level 2997354): a Wave-specific reach
    // override, separate from the shared portalReach[PCAT_VEHICLE] (proven
    // unsafe to retune globally — regresses truth-bank level 174's own
    // Cube->Ship transition). Targets a precisely-characterized ~1-frame
    // alignment gap at the Cube->Wave vehicle-portal touch specifically (see
    // wave_velocity_scale_trap_and_decode_status.md memory for the full
    // derivation) — narrow to Wave so it can't touch any other vehicle's
    // transitions. -4.0 delays the touch by exactly one 240Hz frame (confirmed:
    // touch moves from f4276 to f4277 at DeCode's wave entry, x~6291) and
    // collapses the ~5.35u wave-section Y drift that was causing a sawblade
    // death at x=7810 (31% of the level) — DeCode's real-.gdr2 replay now
    // reaches x=22019.5 (~87%, the SAME point reached when sawblades are
    // entirely bypassed via GDSIM_DIAG_NOSAWBLADE — i.e. this is the natural
    // next blocker, not an artifact of an imperfect fix). Swept -3.0 (too
    // small, stays f4276, no effect) through -10.0 (overshoots to a 3-frame
    // shift, f4279, worse overall reach) — -4.0 gave the best overall reach of
    // every value tried; -4.5 was one frame's worth too aggressive for a
    // SECOND wave-entry later in the level (x~19518, shares this same
    // constant) even though it looked fine at the first one alone. Passed the
    // full regression bank clean (only 108166595's unrelated pre-existing
    // issue). See VehiclePortal::touching().
    float waveEntryReachAdjust = 0.f;

    // EXTRA reach applied ONLY to the gravity portal that flips gravity UP
    // (object id 11, i.e. the one that makes the player upside-down), on top of
    // portalReach[PCAT_GRAVITY]. User-reported 2026-08-22 ("the portal that sets
    // gravity up looks wrong, the one that restores normal gravity looks ok"), and
    // the capture agrees: measuring each of DeCode's gravity flips against the real
    // run, the id=11 portals imply an effective reach around -3.1..-3.6 while the
    // id=10 portal at x=1905 needs -1.2. Modelled as a separate offset instead of
    // retuning the shared constant, which is already over-constrained. 0 = the two
    // portal types behave identically (old behaviour). See GravityPortal::touching().
    float gravityUpReachAdjust = 0.f;

    // Frames a speed-portal change is delayed before it reaches the X-position
    // integrator (GD applies it later than the wave Y-rate etc.). 1 = current.
    int speedXLagFrames = 0;

    // Margin (units) added to the player's hitbox for HAZARD tests, to match GD's
    // slightly larger effective hazard hitboxes. This is the ACCURACY value (auto
    // applied everywhere); the solver may add its own extra safety on top.
    float hazardHitboxInflate = 0.f;

    // Scale applied to move-trigger offsets (level fields 28/29). GD's stored unit
    // convention is ambiguous; calibrate it against a recorded moving-object level.
    float moveScale = 1.0f;

    // Overall scale on the slope EXIT LAUNCH velocity (Slope.cpp's
    // `min(1.12/angle,1.54) * height*speed/width` formula, case-0/gravOrient-0
    // "climb and launch off a rising slope" path). FOUND 2026-08-14 via a real
    // RE-scanner capture (testlevel/GDMod_physics_2997354.txt, DeCode, cube
    // climbing the level's first slope at 0.9x speed, 45°): real launch velocity,
    // recovered from the captured position deltas right after leaving the ramp
    // (dy/frame ÷ captured raw m_yVelocity, averaged over the first 3 post-launch
    // frames where the real-yVel→world-units-per-second conversion factor is
    // cleanest — ≈52.4 units/sec per raw m_yVelocity unit — then launch_yVel(7.405)
    // × that factor), came out to ≈388 units/sec, while gdsim's own formula for
    // the identical angle/speed computes 444.3 — a consistent ~14.5% overshoot.
    // This directly matches a long-reported, repeatedly-flagged real playtest
    // complaint ("the cube goes further in gdsim than in real GD" near this exact
    // slope) — an over-strong launch carries the cube measurably higher/further
    // per arc (peak height scales with the SQUARE of launch velocity), letting it
    // clear obstacles a correctly-launched cube wouldn't. Applied as a single
    // overall scale (not a rederivation of which specific sub-term — 1.12, the
    // player-speed term, or the angle term — is responsible) because one capture
    // at one angle/speed can't disentangle that; if a different angle/speed later
    // shows a DIFFERENT overshoot ratio, that's the signal to dig into a specific
    // sub-term instead of this flat scale.
    float slopeExitVelScale = 0.873f;

    // Overall scale on the slope MANUAL-JUMP boost (Vehicle.cpp's cube jump
    // handler, the "click while riding a slope" path — same core term as
    // slopeExitVelScale above, min(1.12/angle,1.54)*height*speed/width, but a
    // structurally separate formula (its own scale, its own additive
    // jumpHeights[speed] term on top) for a separate scenario, so its own scale.
    // FOUND 2026-08-17 via a real Watch capture on level 2997354 "DeCode"
    // (jump landing on frame 668): recovered real yVel at the jump frame
    // (11.735) vs this formula's raw output. FIRST PASS wrongly treated the
    // formula's full output as directly proportional to this scale and got
    // ~0.852 — invalid, since `result = 0.25*time*(scale*coreterm) +
    // jumpHeights[speed]` has a scale-INDEPENDENT additive term, so that ratio
    // doesn't isolate the scale at all. Redone properly: rebuilt+retested at
    // several candidate scales against the SAME real frame (empirical, not
    // algebraic, since the additive term and `time` ramp aren't cleanly
    // separable from a single sample) — 0.588 reproduces the real yVel to
    // within 0.06 (633.636 vs target 633.69). This path used to share the OLD,
    // never-recalibrated 0.9 constant (see slopeExitVelScale's own history).
    // Single real sample — if a future capture at a different speed/angle
    // shows a different ratio, that's the signal the additive term itself
    // (jumpHeights[speed], or the `time` ramp) needs its own look, not just
    // this scale.
    float slopeJumpVelScale = 0.588f;

    // Magnitude factor GravityPortal.cpp applies to velocity on flip
    // (`velocity = -velocity * scale`) — was a flat 0.5 (simple negate-and-
    // halve). FOUND 2026-08-17 (level 2997354 "DeCode", re-scanner capture):
    // the SAME gravity portal, crossed 9 separate times across repeated
    // practice-mode attempts (each with a different pre-flip yVel from
    // slightly different click timing), gave a post/pre yVel ratio of
    // 0.5098-0.5124 EVERY time — tight, repeatable agreement across 9
    // independent real samples, clearly not measurement noise, and clearly
    // not exactly 0.5. Average: 0.5107.
    //
    // REVISED 2026-08-17 (Watch capture, same portal): 0.5107 alone still left
    // gdsim ~5% short of the real post-flip yVel. Cause: GravityPortal::collide()
    // fires in the EFFECTS pass, before postCollision()'s own per-frame gravity
    // integration (`if (!velocityOverride) velocity += acceleration*dt`) — since
    // this portal doesn't set velocityOverride, that step subtracts one extra
    // frame of gravity ON TOP of the flip, every time. The 9-sample ratio
    // measures real's fully-processed result (whatever real GD's own pipeline
    // does, gravity included), so gdsim's ISOLATED collide()-time factor must be
    // higher to land on the same final answer after its own later gravity
    // subtraction. Solved empirically against a Watch capture (real post-flip
    // yVel 4.454, gdsim target 240.516 raw): 0.537 reproduces it to within 0.11
    // (240.408). Effective end-to-end ratio (240.408/469.368=0.5122) lands right
    // back in the 9-sample range once this extra step is accounted for — the two
    // measurements agree, this just isolates the right constant for where
    // gdsim's pipeline actually applies it.
    //
    // REVISED AGAIN 2026-08-19 (DeCode's own gravity-portal corridor, x~1900-2505,
    // 5 consecutive real flips 30-90 frames apart): 0.537 reproduced the FIRST,
    // isolated flip of the corridor almost exactly (posY diff -0.07 vs real), but
    // each subsequent flip's error grew — +1.30, +1.68, +3.50, +4.67 by the 5th —
    // a clean monotonic drift the single-isolated-flip validation above couldn't
    // have caught. So 0.537's "extra factor to cover one later gravity subtraction"
    // reasoning holds for a flip with a normal free-fall run-up, but overcorrects
    // when flips come in quick succession (each inheriting velocity from a flip
    // just resolved a few dozen frames earlier, not a full free-fall). Rather than
    // model that per-flip-spacing effect, reverted toward the original flat 9-sample
    // measurement (0.5107) — 0.513 closes the corridor's drift sharply (5th-flip
    // diff +4.67 -> +0.78) while leaving the first, isolated flip effectively
    // unchanged (-0.07 -> -0.12). Validated: truth bank 13/13 unaffected; local
    // macro batch (157 real levels) net positive, 7 improved (up to +14.4% further
    // reached) vs 4 minor regressions (worst -4.5%), no clears gained or lost.
    // REVISED AGAIN 2026-08-21: found the real decompiled source
    // (src/gdp-2.2/PlayerObject/PlayerObject_flipGravity.cpp, FUN_14039a1d0) —
    // `m_yVelocity *= 0.5` on any flip, a flat half, no negation (m_yVelocity is
    // world-space; gdsim's player-relative `-v*scale` is the SAME operation once
    // converted through grav(), for BOTH flip directions — worked out by hand,
    // matches the code's own existing -v*scale structure exactly). The empirical
    // 0.513-0.537 climb above was chasing a real secondary effect (gdsim's own
    // postCollision applying an extra frame of gravity after the flip, which the
    // decompiled source gives no indication real GD does at this same instant)
    // with the base constant, instead of fixing that pipeline-ordering gap
    // directly. See GravityPortal.cpp's own use of this constant for the fix.
    float gravityPortalFlipScale = 0.5f;

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
