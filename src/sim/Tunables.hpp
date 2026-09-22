#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gdsim LIVE TUNABLES
//
// Every hard physics constant the engine uses, lifted out of the function bodies
// it was hardcoded in and into one mutable, introspectable place, so the debug
// app (and anything else) can edit ANY of them at runtime and re-simulate
// immediately - no rebuild, no recompile, no code edit.
//
// Two layers:
//
//   1. `PhysicsTables g_phys` - plain, directly-readable storage. The sim reads
//      these fields exactly where it used to read a literal, so there is zero
//      behavioural change while the defaults are untouched. Defaults below are
//      byte-for-byte the values that were previously inlined; each keeps the
//      provenance note from wherever it came from.
//
//   2. A REGISTRY (`allTunables()`) that describes each value - where it lives,
//      what it is called, its default, a sane edit range and step, and a one-line
//      explanation. Built lazily on first use (so it is safe from static-init
//      ordering: by the time anything calls it, every table in every translation
//      unit is constructed). This is what the UI enumerates; nothing needs to
//      know the field names.
//
// The registry deliberately also covers values that do NOT live in
// PhysicsTables - CalibParams (Calib.hpp), the speed tables (Player.hpp) and the
// orb/pad velocity maps (Orb.cpp/Pad.cpp) - by pointing straight at their real
// storage. So "everything the simulator uses" really is one flat list.
//
// WHEN A CHANGE TAKES EFFECT: gdsim is a deterministic integrator, so editing a
// constant mid-run only affects frames simulated AFTER the edit. To see the true
// effect of a value the run must be re-simulated from frame 0 - which is what the
// app's "resim on change" does. Object HITBOX sizes (the portal entries) are read
// at Level-construction time, so they need that same fresh Level; they are picked
// up correctly by a resim and NOT by continuing an existing run.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

namespace gdsim {

struct PhysicsTables {
    // ── Flight-group base gravity ─────────────────────────────────────────────
    // VERIFIED 2026-09-10 against the raw binary, not a fit.
    //
    // updateJump reads m_gravity (the per-speed-tier table below), and then, for
    // SIX modes, overwrites it with one flat float constant:
    //     if (m_isBall||m_isShip||m_isBird||m_isDart||m_isSwing||m_isSpider)
    //         usedGravity = <float at .rdata 0x140622bf8>
    // That constant's four bytes in GeometryDash.exe are `88 4c 75 3f`, i.e.
    // exactly 0.958199024f. So ball, ship, UFO, wave, swing and spider do NOT
    // scale their gravity with the speed portal - only the CUBE and the ROBOT do
    // (they are the two modes missing from that OR). 0.958199024 happens to equal
    // the 1x tier, which is why every capture taken at 1x agreed with the
    // per-tier table and hid the difference.
    //
    // 0.958199024 * 2916 = 2794.108354, matching cubeAccel[1] to 1e-6 - the same
    // real->gdsim acceleration factor (2916 = 0.225 dt * 54 vel * 240 fps) that
    // anchors every other constant here.
    double flightGravity = 2794.108353984;

    // ── Cube ──────────────────────────────────────────────────────────────────
    // Per-speed-tier gravity. Was `static double accelerations[]` inside cube().
    //
    // Gravity really IS speed-dependent in GD (it looks wrong, it isn't):
    // PlayerObject::updateTimeMod(speed) sets m_gravity per tier, and updateJump
    // then applies `-m_gravity * m_gravityMod * dt * flipMod * float_b`. The
    // decompiled table (src/gdp-2.2/PlayerObject/PlayerObject_updateTimeMod.cpp):
    //     0.7 -> 0.940199      1.1 -> 0.957199
    //     0.9 -> 0.958199024   1.3 or 1.6 -> 0.961199   (3x and 4x really do share)
    //
    // CORRECTED 2026-08-30 from those values. Only the 1x tier had been right;
    // the other four were empirically-fitted numbers that missed by 0.12-0.22%,
    // and in OPPOSITE directions (0.5x too strong, 2x/3x/4x too weak) - the
    // signature of per-tier curve-fitting rather than derivation. The
    // real->gdsim acceleration factor is 2916, anchored twice and independently:
    // the 1x tier (0.958199024*2916 = 2794.1083, matching the one value that was
    // already correct) and the ball (0.9582*0.6*2916 = 1676.46672, matching
    // ballAccel exactly). Sibling table cubeJumpHeight was already exact on all
    // five tiers against the same file's m_yStart, which is what made the gravity
    // mismatch stand out.
    //
    // CLINCHER: velocityThreshold[] below is EXACTLY m_gravity[i] * 108 on all
    // five tiers (zero difference to 12 decimals). So the correct per-tier
    // m_gravity values were ALREADY in this codebase, encoded inside the ship/UFO
    // threshold table - the cube gravity table just never received them. Two
    // tables in the same struct derived from the same source constant, one right
    // and one fitted by hand.
    // TIGHTENED 2026-09-10: the five m_gravity doubles are now transcribed bit-for
    // -bit out of PlayerObject::updateTimeMod (0x1403a0d50), which stores them as
    // raw 8-byte immediates. Only the 1x entry moved (last two digits).
    //   0.5x 0x3fee161c36976bc2   1x   0x3feea99100000000
    //   2x   0x3feea15fcc1871e7   3x/4x 0x3feec22467be553b
    double cubeAccel[5]      = {-2741.620284, -2794.1083545685, -2791.192284, -2802.856284, -2802.856284};
    // Per-speed-tier jump impulse. Was `static double jumpHeights[]` inside cube().
    // m_yStart * 54, same source function and same bit-exact transcription.
    //   0.5x 0x40253d74d594f26b   1x   0x40265c2d20000000
    //   2x   0x4026d70e6f2e8c05   3x/4x 0x402675c6c11a1123
    double cubeJumpHeight[5] = {573.481728, 603.7217159271, 616.681728, 606.421728, 606.421728};
    // Extra gravity applied only while upside-down (`p.acceleration -= 6.48`).
    double cubeUpsideExtraAccel = 6.48;
    // Mini multiplier on the jump impulse (currently only on the debug-log path;
    // kept tunable so a mini-jump investigation has a knob to turn).
    double cubeMiniJumpScale = 0.8;
    // Terminal fall speed clamp. Verified EXACTLY 810 against the decompile.
    double maxFallVelocity = 810.0;
    // Coyote-time window (frames) for an upside-down cube's buffered jump.
    int cubeUpsideCoyoteFrames = 10;

    // ── Ship ──────────────────────────────────────────────────────────────────
    // Per-tier velocity band edge that switches ship/UFO between the strong and
    // the weak acceleration. MUST be read through grav() at the use site.
    // = m_gravity[tier] * 108, i.e. the `2 * m_gravity` that
    // PlayerObject::playerIsFallingBugged (0x14039a430) tests m_yVelocity against,
    // times the x54 velocity scale. Confirmed by reading that function directly.
    // NOTE the asymmetry it encodes: for every mode EXCEPT swing/platformer/
    // sideways, the upside-down branch compares against +2*m_gravity rather than
    // -2*m_gravity — that is the actual bug the function is named after, and
    // gdsim reproduces it by wrapping this in Player::grav() at the use site.
    double velocityThreshold[5] = {101.541492, 103.4854946136, 103.377492, 103.809492, 103.809492};
    // DERIVED 2026-09-10 from the disassembled formula, replacing four values
    // that were right to ~4e-6 and four that were fitted. updateJump's ship arm:
    //     dv = -(flipMod * dt * usedGravity * m20 * m24) / sizeDiv
    // with (all four constants read straight out of .rdata, addresses in the
    // ship comment block in Vehicle.cpp):
    //     m20 = -1.0   holding                     (thrust)
    //     m20 =  1.2   released and NOT falling    (0x140622c84 = 1.2f)
    //     m20 =  0.8   released and falling        (0x140622ba4 = 0.8f)
    //     m24 =  0.5   holding and falling         (0x140622b08 = 0.5f)
    //     m24 =  0.4   otherwise                   (0x140622ad0 = 0.4f)
    //     sizeDiv = 0.85 when mini (flight modes), else 1.0
    // "falling" is PlayerObject::playerIsFallingBugged() - see velocityThreshold.
    // So the four bands are flightGravity * {0.5, 0.4, 0.48, 0.32}, each / 0.85
    // for mini. Nothing here is fitted any more.
    double shipAccelUpStrongSmall   =  1643.5931494024, shipAccelUpStrongBig   =  1397.0541769920;
    double shipAccelUpWeakSmall     =  1314.8745195219, shipAccelUpWeakBig     =  1117.6433415936;
    double shipAccelDownStrongSmall = -1577.8494234262, shipAccelDownStrongBig = -1341.1720099123;
    double shipAccelDownWeakSmall   = -1051.8996156175, shipAccelDownWeakBig   =  -894.1146732749;
    // Flight velocity clamp, shared by ship and UFO (skipped for the wave, and
    // skipped entirely while m_isAccelerating - see the note in Vehicle.cpp):
    //     normal gravity : clamp(v, (0.8 * -8.0)/sizeDiv,  8.0/sizeDiv)
    //     upside-down    : clamp(v,       -8.0 /sizeDiv, (0.8 * 8.0)/sizeDiv)
    // -8.0 is 0x1406237fc, 0.8 is 0x140622ba4, sizeDiv is 1.0 or 0.85. Times the
    // x54 velocity scale: +-345.6 / 432.0 big, +-406.588 / 508.235 mini.
    double shipClampMinSmall = -406.5882352941, shipClampMinBig = -345.6;
    double shipClampMaxSmall =  508.2352941176, shipClampMaxBig =  432.0;
    double shipVelClamp = 810.0;   // hard |v| clamp applied after the ship update

    // ── Ball ──────────────────────────────────────────────────────────────────
    // flightGravity * 0.6. The 0.6 is a literal in updateJump's non-flying arm
    // (`if (!ball) { if (!spider) { if (robot) m = 0.9 } else m = 0.6 } else m = 0.6`,
    // cube falling through as 1.0), and the base is the flat 0.958199024f, not
    // the per-tier table. Was -1676.46672, from rounding the base to 0.9582.
    double ballAccel = -1676.4650123904;
    // = cubeJumpHeight[tier] * 0.3, and the 0.3 is now derived rather than fitted:
    // updateJump gives the ball the same `flipMod * m_yStart * sizeDiv` impulse as
    // the cube, then takes the ball-only branch `flipGravity(...); m_yVelocity *=
    // 0.6`, and the gravity flip itself halves the velocity. 0.5 * 0.6 = 0.3.
    double ballJumpHeight[5] = {-172.0445184, -181.1165147781, -185.0045184, -181.9265184, -181.9265184};
    int ballCoyoteUpsideFrames = 16, ballCoyoteNormalFrames = 1;

    // ── UFO ───────────────────────────────────────────────────────────────────
    // Floor applied to velocity on a flap (the POST-flap velocity, not an impulse).
    double ufoFlapMinSmall = 358.992, ufoFlapMinBig = 371.034;
    // CORRECTED 2026-09-10. The UFO arm of updateJump is
    //     dv = -(flipMod * dt * usedGravity * m3 * 0.5) / sizeDiv
    //     m3 = playerIsFallingBugged() ? 0.8 : 1.2
    // so the two bands are flightGravity * {0.6, 0.4} (/0.85 for mini) — the same
    // 0.8/1.2/0.5 constants the ship uses, just without the input term.
    //
    // The four values here were previously -1671.84 / -1114.56 / -1969.92 /
    // -1308.96, i.e. 0.28%-0.45% low, and that error had a specific cause worth
    // remembering: -1114.56/12960 = 0.086 EXACTLY, and -1671.84/12960 = 0.129
    // EXACTLY. Those are the true per-step dv (0.0862379.., 0.1293569..) after
    // GD's own 3-decimal velocity quantisation. They were read off a capture as
    // if they were the rate, so the quantisation got baked into the constant and
    // then re-applied every step. Any constant here that lands on a round
    // 3-decimal dv should be suspected of the same thing.
    double ufoAccelStrongSmall = -1972.3117792828, ufoAccelStrongBig = -1676.4650123904;
    double ufoAccelWeakSmall   = -1314.8745195219, ufoAccelWeakBig   = -1117.6433415936;
    // Same clamp code as the ship (one shared basic block at 0x14038ca9f), so the
    // bounds are identical. ufoClampMaxBig was 520.0, which the disassembly does
    // not support at any size; the real ceiling is 8.0 * 54 = 432.
    double ufoClampMinSmall = -406.5882352941, ufoClampMinBig = -345.6;
    double ufoClampMaxSmall  =  508.2352941176, ufoClampMaxBig  =  432.0;

    // ── Wave ──────────────────────────────────────────────────────────────────
    // Mini wave climbs/dives at this multiple of the full-size rate.
    double waveMiniMult = 2.0;
    float  waveHalfSizeSmall = 6.f, waveHalfSizeBig = 10.f;
    // Spider hitbox. PlayerObject::toggleSpiderMode (0x14039ba70) writes 27.0
    // (0x41d80000) into BOTH m_width and m_height, and the effective rect is
    // `m_width * m_scaleX * m_spriteWidthScale` with the player's sprite scale
    // being m_vehicleSize — so 27 big, 16.2 mini. Every other mode leaves the
    // 30x30 that PlayerObject's constructor and resetPlayerIcon set; only the
    // wave (10) and the spider (27) override it. gdsim gave the spider the
    // generic 30 that VehiclePortal::collide installs, i.e. a hitbox 11% too
    // large on both axes.
    float  spiderSizeSmall = 16.2f, spiderSizeBig = 27.f;
    float  waveRotSmall = 0.4f, waveRotBig = 0.25f;

    // ── Robot ─────────────────────────────────────────────────────────────────
    // Hold-to-charge sustain cap. 66 was measured against a real capture (35 was
    // the long-standing wrong value); mini gets one extra frame.
    int robotChargeFramesSmall = 67, robotChargeFramesBig = 66;
    double robotJumpMax[5] = {573.481728, 603.7217172, 616.681728, 606.421728, 606.421728};
    // Same m_gravity table as the cube - the robot is not in the decompile's
    // ball/flying/spider group, so it reads m_gravity too; its only difference is
    // float_b = 0.9 (robotFallScale below). Corrected alongside cubeAccel.
    double robotAccel[5]   = {-2741.620284, -2794.108354, -2791.192284, -2802.856284, -2802.856284};
    // Ballistic-fall gravity is the cube's, scaled by this.
    double robotFallScale = 0.9;
    double robotUpsideExtraAccel = 6.48;
    int robotUpsideCoyoteFrames = 10;

    // ── Spider ────────────────────────────────────────────────────────────────
    double spiderAccel = -1676.4650123904;   // flightGravity * 0.6, same as Ball
    int spiderCoyoteUpsideFrames = 16, spiderCoyoteNormalFrames = 1;

    // ── Swing ─────────────────────────────────────────────────────────────────
    // SETTLED 2026-09-10 against the disassembly. The capture-derived model below
    // is CORRECT and the "FLAGGED WRONG" note that briefly sat here was not — it
    // came from reading Ghidra's C output, where updateJump's swing arm ends in a
    // `setYVelocity()` call whose arguments the decompiler failed to recover, and
    // I attributed the neighbouring SHIP arm's formula to it. Disassembling the
    // function directly (llvm-objdump on GeometryDash.exe, 0x14038c8e4 onward)
    // shows the swing's own basic block at 0x14038c959:
    //
    //     movaps %xmm8,%xmm9        ; sizeDiv  = 1.0   (swing ignores mini scaling)
    //     movaps %xmm8,%xmm7        ; clampAsym= 1.0   (swing widens its own clamp)
    //     movss  0x140622ad0,%xmm1  ; m = 0.4f
    //     movss  0x9f0(%rdi),%xmm0  ; m_vehicleSize
    //     ucomiss %xmm8,%xmm0       ; == 1.0 ?
    //     je     .keep
    //     movss  0x140622b38,%xmm1  ; m = 0.6f   (mini)
    //  .keep:
    //     ...  sign * dt * usedGravity * m ,  negate, add m_yVelocity
    //
    // No input term, no threshold, no playerIsFallingBugged call, and no divide by
    // the size divisor — a flat gravity whose only input is size. That is exactly
    // what the 2026-08-31 capture measured, so both sources now agree:
    //     mini 0.6, big 0.4, zero thrust in any input state.
    // The two multipliers are the literal floats 0.4f (0x140622ad0, shared with
    // the ship's m24 default) and 0.6f (0x140622b38).
    //
    // ONE REAL FIX came out of this: the base. Vehicle.cpp was scaling
    // cubeAccel[speed], but the swing is one of the six modes that take the flat
    // 0.958199024f instead of the per-tier table, so the base must be
    // flightGravity. Identical at 1x (which is all the capture covered, and why
    // this went unnoticed), off by up to 0.19% at 0.5x/3x/4x.
    //
    // Historical note, superseded (kept because it explains the wrong turn):
    // the Ghidra C output attributes the ship arm's graduated
    // `-(sign*dt*grav*m20*m24)/sizeDiv` formula to the swing, because the swing's
    // own call lost its arguments in decompilation. It does not apply here.
    //
    // ORIGINAL finding, now CONFIRMED by the disassembly, kept for its evidence:
    // MEASURED 2026-08-31 (Input Lab capture, 1458 swing steps): the swing is a
    // CONSTANT-GRAVITY mode, not a thrust mode.
    //   * of 1730 captured swing steps, ZERO accelerated upward. It never
    //     thrusts, under any input. (The player was holding the button.)
    //   * the applied acceleration is a flat multiple of gravity depending only
    //     on SIZE:  mini 0.598345 -> 0.6 (810 samples)
    //               big  0.398896 -> 0.4 (648 samples)
    //     The 0.28% shortfall from exactly 0.6/0.4 is GD's 3-decimal velocity
    //     quantisation showing up in the measurement, the same artefact that had
    //     been baked into the UFO constants above; the true multipliers are the
    //     literal 0.6f/0.4f now read straight out of .rdata.
    double swingGravScaleSmall = 0.6, swingGravScaleBig = 0.4;
    // The swing forces BOTH the size divisor and the clamp asymmetry factor to
    // 1.0 before reaching the shared clamp block, so its bounds are a symmetric
    // +-8.0 * 54 at either size — it is the only flight mode whose clamp does not
    // narrow to -6.4 on one side, and the only one where mini does not widen it.
    // Mini was previously given the ship's -406.566/508.248 by analogy.
    double swingClampMinSmall = -432.0, swingClampMinBig = -432.0;
    double swingClampMaxSmall =  432.0, swingClampMaxBig =  432.0;

    // ── Portal hitbox sizes ───────────────────────────────────────────────────
    // Engine-measured (2026-08-23) from GDMod_hitbox_<id>.txt captures - these
    // replaced a flat guessed 30x90 that every portalReach constant had been
    // silently absorbing. Read at Level construction, so a change needs a resim.
    float portalVehicleW  = 34.f, portalVehicleH  = 86.f;
    float portalTeleportW = 25.f, portalTeleportH = 90.f;
    float portalGravityW  = 25.f, portalGravityH  = 75.f;
    float portalSizeW     = 31.f, portalSizeH     = 90.f;
    float portalDualW     = 41.f, portalDualH     = 91.f;
    float portalSpeed200W = 35.f, portalSpeed200H = 44.f;
    float portalSpeed201W = 33.f, portalSpeed201H = 56.f;
    float portalSpeed202W = 51.f, portalSpeed202H = 56.f;
    // STILL UNVERIFIED - no upright instance captured yet, this is a guess.
    float portalSpeed203W = 30.f, portalSpeed203H = 90.f;
    float portalSpeed1334W = 69.f, portalSpeed1334H = 56.f;
};

// The single live instance the whole sim reads from.
extern PhysicsTables g_phys;

// ─────────────────────────────────────────────────────────────────────────────
// Registry
// ─────────────────────────────────────────────────────────────────────────────

enum class TunKind { F64, F32, I32, Bool };

struct Tunable {
    std::string group;   // UI section, e.g. "Ship", "Orbs - Yellow"
    std::string name;    // row label, unique within the group
    std::string help;    // one-line explanation shown on hover
    TunKind     kind = TunKind::F64;
    void*       ptr  = nullptr;
    double      defVal = 0.0;
    // Soft range for the drag/slider. NOT a clamp on typed input - a typed value
    // is accepted as-is, since exploring outside the "sane" range is the entire
    // point of a calibration tool.
    double      lo = 0.0, hi = 0.0;
    double      step = 0.01;         // base drag/wheel increment

    double get() const;
    void   set(double v) const;
    bool   modified() const;
    void   reset() const { set(defVal); }
    // Stable identity for preset files: "group/name".
    std::string key() const { return group + "/" + name; }
};

// Flat list of every tunable in the engine. Built once, then returned by
// reference; the pointers inside stay valid for the process lifetime.
std::vector<Tunable>& allTunables();

// Number of registered values whose current setting differs from its default.
int  modifiedTunableCount();
void resetAllTunables();

// Preset I/O - a plain text file, one `group/name<TAB>value` per line, so it can
// be diffed, hand-edited and committed. Unknown keys are ignored (forward
// compatible with a preset written by a newer build).
bool saveTunablePreset(const std::string& path);
bool loadTunablePreset(const std::string& path);

} // namespace gdsim
