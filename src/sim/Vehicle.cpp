#include "Vehicle.hpp"
#include "Player.hpp"
#include "Object.hpp"
#include "Level.hpp"
#include "Slope.hpp"
#include "Calib.hpp"
#include "Tunables.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gdsim {

// Moved into PhysicsTables (Tunables.hpp) so it is editable live. This alias
// keeps every existing `velocity_thresholds[p.speed]` use site unchanged.
#define velocity_thresholds (g_phys.velocityThreshold)

// GD's flight velocity clamp, in the order updateJump actually runs it.
//
// Two separate things happen, and they are NOT the same test:
//  1. At the top of the isFlying() branch, m_isAccelerating is cleared whenever
//     the velocity sits inside `-6.4/sizeDiv .. 8.0/sizeDiv` with sizeDiv the
//     FLIGHT divisor (0.85 mini, 1.0 big). Those bounds are exactly the ship's
//     own clamp constants, which is why they are reused for the band here even
//     when the caller's clamp differs (the swing's does: it forces both the size
//     divisor and the asymmetry factor to 1.0, giving a symmetric +-432).
//  2. The clamp itself, guarded by `if (!m_isAccelerating && !m_isDart)`.
//
// Net behaviour: a boost that leaves the band is NOT clamped away; it decays
// under gravity until it re-enters the band, and the clamp resumes from there.
// gdsim used to clamp unconditionally, so every strong flight boost was cut
// down on the frame it landed.
static void flightClamp(Player& p, double lo, double hi) {
    const double bandLo = p.small ? g_phys.shipClampMinSmall : g_phys.shipClampMinBig;
    const double bandHi = p.small ? g_phys.shipClampMaxSmall : g_phys.shipClampMaxBig;
    if (p.velocity > bandLo && p.velocity < bandHi) p.isAccelerating = false;
    if (!p.isAccelerating) p.velocity = std::clamp(p.velocity, lo, hi);
}

static float normalizeRotation(Player const& p, float angle) {
    float playerRotation = (int)p.rotation % 360;
    float diff = std::fmod(playerRotation - angle, 90.0f);
    if (diff < 0) diff += 90.0f;
    if (std::abs(playerRotation - angle) < std::abs(diff)) return angle;
    return playerRotation - diff;
}

static void rotateFly(Player& p, float mult) {
    auto diff = p.pos - p.prevPlayer().pos;
    if (p.dt * 72 <= std::pow(diff.x, 2) + std::pow(diff.y, 2))
        p.rotation = slerp(p.rotation * 0.017453292f, atan2(diff.y, diff.x), (p.dt * 60) * mult) * 57.29578f;
}

// ── Spider helper: scan nearby sections for nearest opposite surface ──────
// (non-static: also used by the Spider orb in Orb.cpp)
float findSpiderTarget(const Player& p) {
    float half_h = p.size.y / 2.f;
    // Fallback to portal floor/ceiling
    float fallback = p.upsideDown
        ? (p.floor   + half_h)
        : (p.ceiling - half_h);

    if (!p.level || p.level->sections.empty())
        return fallback;

    auto& sections = p.level->sections;
    int sectionCount = static_cast<int>(sections.size());
    int si = std::max(0, std::min((int)(p.pos.x / Level::sectionSize), sectionCount - 1));

    float px_l = p.pos.x - p.size.x * 0.45f;
    float px_r = p.pos.x + p.size.x * 0.45f;
    float py   = p.pos.y;

    if (!p.upsideDown) {
        // Right-side-up: find lowest block-bottom that is above the player
        float best = fallback;
        for (int s = std::max(0, si - 1); s <= std::min(sectionCount - 1, si + 1); ++s) {
            for (auto& oc : sections[s]) {
                const Object* obj = oc.operator->();
                if (obj->prio != 1) continue;        // blocks only
                if (obj->rotation != 0.f) continue;  // skip rotated (complex geometry)
                float objBotWorld = obj->pos.y - obj->size.y * 0.5f;
                float objTopWorld = obj->pos.y + obj->size.y * 0.5f;
                if (objBotWorld <= py + half_h) continue; // not above player
                // X-overlap check
                if (obj->pos.x + obj->size.x * 0.5f <= px_l) continue;
                if (obj->pos.x - obj->size.x * 0.5f >= px_r) continue;
                float candidate = objBotWorld - half_h;
                if (candidate < best) best = candidate;
            }
        }
        return best;
    } else {
        // Upside-down: find highest block-top that is below the player
        float best = fallback;
        for (int s = std::max(0, si - 1); s <= std::min(sectionCount - 1, si + 1); ++s) {
            for (auto& oc : sections[s]) {
                const Object* obj = oc.operator->();
                if (obj->prio != 1) continue;
                if (obj->rotation != 0.f) continue;
                float objTopWorld = obj->pos.y + obj->size.y * 0.5f;
                if (objTopWorld >= py - half_h) continue; // not below player
                if (obj->pos.x + obj->size.x * 0.5f <= px_l) continue;
                if (obj->pos.x - obj->size.x * 0.5f >= px_r) continue;
                float candidate = objTopWorld + half_h;
                if (candidate > best) best = candidate;
            }
        }
        return best;
    }
}

// ── Cube ──────────────────────────────────────────────────────────────────
static Vehicle cube() {
    Vehicle v;
    v.type = VehicleType::Cube;

    v.enter = +[](Player& p) {
        if (p.prevPlayer().vehicle.type != VehicleType::Ball)
            p.velocity = p.velocity / 2;
        if (p.prevPlayer().vehicle.type == VehicleType::Wave)
            p.velocity = p.velocity / 2;
        if (p.prevPlayer().vehicle.type == VehicleType::Ship && p.input)
            p.buffer = true;
    };

    v.clamp = +[](Player& p) {
        // Terminal velocity: real code (src/gdp-2.2/PlayerObject/PlayerObject_updateJump.cpp
        // line ~459, cube/robot fall branch) clamps to the literal constant 15 (in
        // GD's internal small-unit velocity scale). 15 * 54 (the confirmed real->gdsim
        // scale, cross-validated via the ship clamp constants 8.0*54=432, 6.4*54=345.6)
        // = EXACTLY 810 — i.e. this was already exactly correct before a prior pass
        // this session "corrected" it to -805.18 from a Desmos-based measurement with
        // ~0.6% error. Reverted to the exact value now that the real source confirms it.
        if (p.velocity < -g_phys.maxFallVelocity) p.velocity = -g_phys.maxFallVelocity;
        if (p.gravTop(p.innerHitbox()) >= p.gravCeiling()) { p.dead = true; p.deathCause = "ceiling"; }
    };

    v.update = +[](Player& p) {
        // REVERTED to the original per-speed values. The Desmos-derived uniform
        // constant (2837.18) was calculated assuming a simple real-seconds dt and
        // the velocity-only scale factor (x54, confirmed via terminal-velocity
        // matches). But src/gdp-2.2/GJBaseGameLayer/GJBaseGameLayer_update.cpp
        // reveals real GD's physics functions run on an internal dt convention of
        // a FIXED 0.25 per substep (see `physicsSecond = delta*60/stepCount` where
        // stepCount is defined as `max(1, delta*240)` — algebraically always 0.25
        // at any framerate/timewarp), not real elapsed seconds. Any ACCELERATION
        // constant (unlike a velocity/terminal-velocity constant, which isn't
        // dt-multiplied) needs an extra x60 factor on top of x54 to convert
        // real->gdsim correctly. Neither this session's Desmos value nor a naive
        // x54-only port accounts for that, so reverting to the original values
        // (presumably already correctly reverse-engineered/tuned before this
        // session) rather than keep an unverified "fix" — needs redoing with the
        // x54*60 = x3240 factor before touching this again.
        double* accelerations = g_phys.cubeAccel;
        p.acceleration = accelerations[p.speed];

        if (p.gravityPortal && p.grav(p.velocity) > 350 && p.speed > 1)
            p.acceleration -= g_phys.cubeUpsideExtraAccel;

        p.rotation = 0;
        bool jump = false;

        if (p.grounded) {
            // Real GD (updateJump.cpp): a grounded cube jumps whenever the button is
            // buffered — INCLUDING a held button, which re-jumps every grounded frame.
            // GD avoids jumping on the landing frame only because m_isOnGround isn't set
            // yet during updateJump; gdsim's floor-snap sets grounded before this runs,
            // so we exclude the landing frame explicitly via prevGrounded.
            //   • fresh buffered press:  buffer rising edge
            //   • held re-jump:          button held AND already grounded last frame
            //                            (never on the landing frame itself)
            // J block (SpecialBlock.hpp, id 1813 "Stop Jump Buffer"): a press
            // queued BEFORE landing must NOT auto-fire on touchdown while
            // touching a J block over a horizontal block (real GD excludes the
            // ground/slopes from this — approximated here via the slope check
            // only, since a J block always co-locates with an actual Block
            // object, naturally excluding the bare-floor case in practice).
            bool bufferedJump = p.buffer && !p.prevPlayer().buffer;
            if (bufferedJump && p.touchingJBlock && !p.slopeData.slope) bufferedJump = false;
            // FOUND 2026-08-17 (real Watch capture, level 2997354 "DeCode", fresh
            // 251-click solve): a click landing on the EXACT frame a slope grip is
            // first acquired fired the slope-boosted jump formula below (vel=632
            // vs the ~126 real Y at that instant tracking smoothly) — real GD shows
            // no jump at all on that frame, position continues the ordinary
            // diagonal ride untouched. A held re-jump is already excluded on a
            // fresh (flat-ground) landing frame via the prevPlayer().grounded
            // check above (see the comment there — real GD's own updateJump
            // doesn't see this frame's landing yet); a freshly-gripped SLOPE
            // apparently isn't jump-eligible on its own first frame either, so
            // exclude a bufferedJump the same way: only when the slope grip
            // already existed last frame too.
            bool freshSlopeGrip = p.slopeData.slope && !p.prevPlayer().slopeData.slope;
            if (bufferedJump && freshSlopeGrip) bufferedJump = false;
            if (bufferedJump || (p.input && p.prevPlayer().grounded))
                jump = true;
            else p.setVelocity(0, true);
            p.buffer = false;
        }

        if (p.upsideDown && p.input && p.coyoteFrames < (unsigned)g_phys.cubeUpsideCoyoteFrames) {
            jump = true;
            p.buffer = false;
        }

        if (jump) {
            double* jumpHeights = g_phys.cubeJumpHeight;
            if (p.slopeData.slope && p.slopeData.slope->orientation == 0) {
                auto time = std::clamp(10*(p.timeElapsed - p.slopeData.elapsed), 0.4, 1.0);
                // FOUND 2026-08-17 (real DeCode capture): this shares the exact
                // core term of Slope.cpp's natural-exit formula (min(1.12/angle,
                // 1.54) * size.y*player_speeds/size.x) but had its OWN separate,
                // never-recalibrated hardcoded 0.9 scale — the natural-exit path
                // USED TO have this same 0.9 too (see that formula's own comment:
                // "gdsim had an unexplained extra 0.9 multiplier... dropped") but
                // this mid-ride manual-jump path was apparently never revisited
                // when that one was replaced. Real yVel at the jump frame pinned
                // this formula's own scale empirically at 0.588 — see
                // g_calib.slopeJumpVelScale's own comment for why this couldn't be
                // solved algebraically (the `+ jumpHeights[speed]` term below is
                // scale-independent, so the naive "match the ratio" approach that
                // worked for a moment doesn't actually isolate this constant).
                double vel = g_calib.slopeJumpVelScale * std::min(1.12 / p.slopeData.slope->angle(), 1.54)
                           * (p.slopeData.slope->size.y * player_speeds[p.speed] / p.slopeData.slope->size.x);
                if (getenv("GDSIM_JUMP_DEBUG"))
                    std::fprintf(stderr, "JUMP-SLOPE f=%d speed=%d angle=%.4f slopeSize=(%.2f,%.2f) time=%.4f vel=%.4f result=%.4f\n",
                                 p.frame, p.speed, p.slopeData.slope->angle(), p.slopeData.slope->size.x, p.slopeData.slope->size.y,
                                 time, vel, 0.25*time*vel + jumpHeights[p.speed]);
                // FOUND 2026-08-17 (real Watch capture, level 2997354 "DeCode",
                // 246-click solve): a click-jump fired mid-ride (not the fresh-grip
                // frame excluded above) matched real GD's velocity almost exactly
                // (11.89 vs 11.813) but jumped position by an extra ~1.3u on this
                // SAME frame. Two compounding causes, both needed:
                // (1) the postCollision "semi-implicit resync" (Player.cpp, fires
                //     whenever !velocityOverride) applied the brand-new jump
                //     velocity to THIS frame's position immediately — same class
                //     of bug as the natural slope-exit fix above. The old
                //     p.prevPlayer().input override flag was incidental here
                //     (false on a fresh press) rather than a deliberate same-vs-
                //     next-frame choice for this specific impulse.
                // (2) Slope::calc() ALSO still runs THIS frame (slopeData.slope
                //     isn't cleared until the natural-exit path's queued action,
                //     which for THIS jump-off path never even runs) and its
                //     ride-tracking clamp (`pos.y = max(pos.y, expectedY(p))`)
                //     independently re-pulled Y toward the still-live diagonal,
                //     re-introducing almost the same offset fix (1) had just
                //     removed. Clearing the grip synchronously, right here, stops
                //     calc() from re-engaging this same frame — mirroring what the
                //     natural exit's queued action does, just not deferred, since
                //     this frame's ride is already over the instant the player
                //     jumps off.
                // This residual compounded over the rest of the level into a
                // fatal ~22u drift by the time it reached the first ship section.
                p.setVelocity(0.25*time*vel + jumpHeights[p.speed], true);
                p.grounded = false;
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0;
                p.slopeData.snapDown = false;
            } else {
                if (getenv("GDSIM_JUMP_DEBUG"))
                    std::fprintf(stderr, "JUMP-FLAT f=%d speed=%d jumpHeight=%.4f small=%d result=%.4f\n",
                                 p.frame, p.speed, jumpHeights[p.speed], p.small, jumpHeights[p.speed] * (p.small ? g_phys.cubeMiniJumpScale : 1.0));
                p.setVelocity(jumpHeights[p.speed], p.prevPlayer().input);
                p.grounded = false;
            }
            p.resyncPosition = true;
        }
    };

    v.bounds = FLT_MAX;
    return v;
}

// ── Ship ──────────────────────────────────────────────────────────────────
static Vehicle ship() {
    Vehicle v;
    v.type = VehicleType::Ship;

    v.enter = +[](Player& p) {
        // Real GD switches vehicle in checkCollisions — AFTER the LEAVING vehicle's
        // gravity step (updateJump) and position integration — and the ship's own
        // thrust/gravity doesn't run until the next frame. gdsim runs enter in the
        // effects phase, on the PRE-gravity velocity, and then postCollision applies
        // the NEW ship's acceleration. That halved the wrong (pre-gravity) velocity
        // AND added a spurious ship thrust step, leaving a constant +0.19 dV at every
        // flight-mode entry (proven on truth 174 f332: real V→3.00, gdsim →3.19).
        // Apply the leaving vehicle's gravity step BEFORE halving, and suppress this
        // frame's ship acceleration via velocityOverride. Exact: (6.21-0.216)/2=3.00.
        p.velocity += p.prevPlayer().acceleration * p.dt;
        if (p.prevPlayer().vehicle.type == VehicleType::Ufo || p.prevPlayer().vehicle.type == VehicleType::Wave)
            p.velocity = p.velocity / 4.0;
        else
            p.velocity = p.velocity / 2.0;
        p.velocityOverride = true;
    };

    v.clamp = +[](Player& p) {
        p.buffer = false;
        // Unconditional clamp (same bounds regardless of upsideDown) — matches
        // camila314/pathfinder's gd-sim exactly (the same proven-accurate
        // reference ship() was ported from, confirmed working against real
        // captured triple-bounce/corridor data this session). A later pass this
        // session tried to derive an upsideDown-asymmetric version directly from
        // the raw decompiled binary (widening the not-upside-down lower bound to
        // -432.0) and that derivation was WRONG: it caused a real, reproducible
        // false death (gdsim dying on a spike shortly after a Cube->Ship portal
        // where the real captured playthrough survives) that didn't exist before.
        // Reverted to the reference's exact values.
        flightClamp(p, p.small ? g_phys.shipClampMinSmall : g_phys.shipClampMinBig,
                       p.small ? g_phys.shipClampMaxSmall : g_phys.shipClampMaxBig);
        if (p.gravTop(p) > p.gravCeiling()) {
            if (p.velocity > 0) p.setVelocity(0, false);
            p.pos.y = p.grav(p.gravCeiling()) - p.grav(p.size.y / 2);
        }
    };

    v.update = +[](Player& p) {
        p.buffer = false;
        if (p.grounded) p.setVelocity(0, !p.input);

        // Stateless per-frame threshold compare — matches camila314/pathfinder's
        // gd-sim (a verified-accurate reference implementation), which uses this
        // exact model with these exact constants and has NO persistent
        // "isAccelerating" flag. This session previously replaced this with a
        // persistent-flag state machine based on a misreading of the decompiled
        // binary's m_isAccelerating; reverted after cross-checking the reference.
        //
        // velocity_thresholds IS wrapped in grav(): confirmed (not guessed) by a
        // full trace of updateJump's helper FUN_14039a430, the real function this
        // threshold compare stands in for. Its two branches are NOT a textual
        // mirror (one applies an extra sign flip via a rare, unidentified flag the
        // other doesn't), but with that rare flag assumed off, substituting
        // m_yVelocity == grav(velocity) makes the not-upside-down branch reduce to
        // `velocity < 2*base` and the upside-down branch reduce to
        // `velocity < grav(2*base)` — i.e. exactly the grav()-wrapped form used
        // here. (By contrast, ship's OTHER real velocity-sign check — whether
        // m_yVelocity is negative — reduces to the SAME condition in both
        // orientations once substituted, i.e. no wrap; gdsim's simplified 2-branch
        // model doesn't have a separate place for that check, so there was nowhere
        // to wrongly wrap it.)
        // Size at the START of this frame. Real GD runs updateJump (which picks the
        // mini vs full-size acceleration) BEFORE checkCollisions (where a size portal
        // actually changes m_vehicleSize), so on the portal frame the OLD size's accel
        // still applies — the mini accel only bites the frame AFTER. gdsim fires the
        // SizePortal in the effects phase before this update, so p.small is already the
        // new size here; using it would apply the mini accel one frame early, injecting
        // a constant velocity offset of exactly (1643.5872-1397.0491)/240/54 = 0.019
        // internal units that never corrects → a ~2u drift over a long mini section
        // (level 98414841, mini straight-fly at x2183-3082). Use prevPlayer().small so
        // the accel switches one frame later, matching real GD. (Identical to p.small on
        // every non-transition frame.)
        const bool sm = p.prevPlayer().small;
        if (p.input) {
            if (p.velocity <= p.grav(velocity_thresholds[p.speed]))
                p.acceleration = sm ? g_phys.shipAccelUpStrongSmall : g_phys.shipAccelUpStrongBig;
            else
                p.acceleration = sm ? g_phys.shipAccelUpWeakSmall : g_phys.shipAccelUpWeakBig;
        } else {
            if (p.velocity >= p.grav(velocity_thresholds[p.speed]))
                p.acceleration = sm ? g_phys.shipAccelDownStrongSmall : g_phys.shipAccelDownStrongBig;
            else
                p.acceleration = sm ? g_phys.shipAccelDownWeakSmall : g_phys.shipAccelDownWeakBig;
        }
        if (p.grav(p.pos.y) >= p.gravCeiling()) p.setVelocity(0, false);
        rotateFly(p, 0.15f);
    };

    v.bounds = 300;
    return v;
}

// ── Ball ──────────────────────────────────────────────────────────────────
static Vehicle ball() {
    Vehicle v;
    v.type = VehicleType::Ball;

    v.clamp = +[](Player& p) {
        if (p.velocity >=  g_phys.shipVelClamp) p.velocity =  g_phys.shipVelClamp;
        if (p.velocity <= -g_phys.shipVelClamp) p.velocity = -g_phys.shipVelClamp;
        if (p.grav(p.pos.y) >= p.gravCeiling() && p.velocity > 0) {
            p.setVelocity(0, true);
            if (p.input) p.upsideDown = !p.upsideDown;
        }
    };

    v.enter = +[](Player& p) {
        if (p.input) p.vehicleBuffer = true;
        switch (p.prevPlayer().vehicle.type) {
            case VehicleType::Ship:
            case VehicleType::Ufo:
                p.velocity = p.velocity / 2;
                break;
            default: break;
        }
    };

    v.update = +[](Player& p) {
        if (!p.prevPlayer().velocityOverride || p.prevPlayer().slopeData.slope)
            p.acceleration = g_phys.ballAccel;

        if (!p.input) p.vehicleBuffer = false;

        bool jump = false;
        if (p.grounded) {
            if (p.input && (p.prevPlayer().buffer || !p.prevPlayer().input || p.vehicleBuffer)) {
                jump = true;
            } else {
                p.setVelocity(0, true);
            }
            p.buffer = false;
        } else if (p.buffer && p.coyoteFrames < (p.upsideDown ? (unsigned)g_phys.ballCoyoteUpsideFrames : (unsigned)g_phys.ballCoyoteNormalFrames)) {
            jump = true;
        }

        if (jump) {
            double* jumpHeights = g_phys.ballJumpHeight;
            double newVel = jumpHeights[p.speed];
            if (p.slopeData.slope && p.slopeData.slope->orientation == 0) {
                auto slope = p.slopeData.slope;
                newVel -= p.grav(0.300000001 * roundVel(0.16875 * std::min(1.12/slope->angle(), 1.54)
                    * (slope->size.y * player_speeds[p.speed] / slope->size.x), p.upsideDown));
            }
            p.upsideDown = !p.upsideDown;
            p.setVelocity(newVel, p.prevPlayer().buffer || p.vehicleBuffer);
            p.vehicleBuffer = false;
            p.buffer = false;
            p.input = false;
            p.roundVelocity = false;
        }
    };

    v.bounds = 240;
    return v;
}

// ── UFO ───────────────────────────────────────────────────────────────────
static Vehicle ufo() {
    Vehicle v;
    v.type = VehicleType::Ufo;

    v.enter = +[](Player& p) {
        VehicleType pv = p.prevPlayer().vehicle.type;
        if ((pv == VehicleType::Ship || pv == VehicleType::Wave) && p.input)
            p.buffer = true;
        p.velocity = p.velocity / (p.prevPlayer().vehicle.type == VehicleType::Ship ? 4 : 2);
    };

    v.clamp = +[](Player& p) {
        // Upper bound raised 432->520 so a RED pad (id 1332, table 518.4) can push a
        // UFO past its flap terminal velocity (432): a real-GD capture (physlab, no
        // input) shows a red pad raises the UFO 80.8u vs a yellow pad's 58u, but the
        // old 432 cap clamped BOTH to 432 → identical 56.3u bounces. Flaps (371) and
        // yellow/pink pads (now at their real table values, <432) are unaffected;
        // only above-flap pad boosts (red) change. See [[physlab_spwn_pipeline]].
        flightClamp(p, p.small ? g_phys.ufoClampMinSmall : g_phys.ufoClampMinBig,
                       p.small ? g_phys.ufoClampMaxSmall : g_phys.ufoClampMaxBig);
        p.input = p.button;
        if (p.gravTop(p) > p.gravCeiling()) {
            if (p.velocity > 0) p.setVelocity(0, false);
            p.pos.y = p.grav(p.gravCeiling()) - p.grav(p.size.y / 2);
        }
    };

    v.update = +[](Player& p) {
        if (p.buffer) {
            p.velocity = std::max(p.velocity, p.small ? g_phys.ufoFlapMinSmall : g_phys.ufoFlapMinBig);
            p.velocityOverride = true;
            p.buffer = false;
            p.grounded = false;
            // FOUND 2026-08-23 (DeCode UFO section, user-reported, proven against the
            // raw capture): a UFO flap is the VEHICLE'S OWN jump, so real GD applies it
            // inside updateJump — BEFORE that same function integrates position. The
            // flap therefore moves this frame's Y with the NEW velocity, unlike an
            // orb/pad/portal impulse (checkCollisions, i.e. after the integration),
            // which only affects the NEXT frame. Measured at the flap on x=4141:
            // real dY = -1.5460 = -371.034/240 exactly (the POST-flap velocity), while
            // gdsim moved -0.4140 = -106.326/240 (the pre-flap one) — a 1.13u error
            // injected at EVERY flap, which is why the UFO section's deviation grew in
            // clean steps (-0.78 -> -4.36 -> -7.36 -> -8.41 -> -9.34) rather than
            // drifting smoothly. cube() and robot() already set this flag for exactly
            // the same reason; ufo() was simply missing it.
            p.resyncPosition = true;
        } else {
            // FIXED 2026-08-22 (DeCode UFO section, x=4003-5024, proven against the raw
            // capture): this threshold MUST be wrapped in grav(), exactly as ship() does
            // 120 lines above — the old comment here claimed the opposite ("must NOT be
            // wrapped"), but ship()'s own code (p.grav(velocity_thresholds[...])) already
            // contradicted it, and real capture settles it.
            // Evidence: real GD switches the UFO from the strong accel (6.966/frame) to
            // the weak one (4.644/frame) exactly when its WORLD velocity crosses
            // +103.377 (= velocity_thresholds[2] at this level's 1.1x tier) going up.
            // gdsim compared the PLAYER-RELATIVE velocity to a bare +103.377, which for
            // an UPSIDE-DOWN ufo is the opposite sign — so the switch fired at world
            // -99.36 instead of +109.62, a 209 unit/sec early switch. Since both engines
            // otherwise agree exactly (same jump impulse -371.034, same two accel
            // constants), that mistimed switch was the ENTIRE source of a ~36u Y drift
            // accumulating in steps across this level's upside-down UFO section.
            // grav() leaves normal-gravity ufos completely unchanged (grav(T)==T when
            // not upsideDown), so this only affects the previously-broken flipped case.
            if (p.velocity > p.grav(velocity_thresholds[p.speed]))
                p.acceleration = p.small ? g_phys.ufoAccelStrongSmall : g_phys.ufoAccelStrongBig;
            else
                p.acceleration = p.small ? g_phys.ufoAccelWeakSmall : g_phys.ufoAccelWeakBig;
            if (p.grounded) p.setVelocity(0, true);
            if (p.button) p.input = false;
        }
    };

    v.bounds = 300;
    return v;
}

// ── Wave ──────────────────────────────────────────────────────────────────
static Vehicle wave() {
    Vehicle v;
    v.type = VehicleType::Wave;

    v.enter = +[](Player& p) {
        p.actions.push_back(+[](Player& p) {
            p.size = p.small ? Vec2D(g_phys.waveHalfSizeSmall, g_phys.waveHalfSizeSmall) : Vec2D(g_phys.waveHalfSizeBig, g_phys.waveHalfSizeBig);
        });
    };

    v.clamp = +[](Player& p) {
        float waveTop    = p.grav(p.pos.y + p.grav(p.size.y));
        float waveBottom = p.grav(p.pos.y - p.grav(p.size.y));
        if (!p.velocityOverride)
            p.velocity = (p.input * 2 - 1) * player_speeds[p.speed] * (p.small ? (float)g_phys.waveMiniMult : 1.f);
        if (waveBottom <= p.gravFloor()) {
            p.pos.y = p.grav(p.gravFloor() + p.size.y);
            if (waveBottom == p.gravFloor() && !p.input) p.velocity = 0;
        } else if (waveTop >= p.gravCeiling()) {
            p.pos.y = p.grav(p.gravCeiling() - p.size.y);
            if (waveTop == p.gravCeiling() && p.input) p.velocity = 0;
        }
    };

    v.update = +[](Player& p) {
        p.acceleration = 0;
        p.buffer = false;
        rotateFly(p, p.small ? g_phys.waveRotSmall : g_phys.waveRotBig);
    };

    v.bounds = 300;
    return v;
}

// ── Robot (variable-height jump) ─────────────────────────────────────────
//  Press  → launches at full height
//  Release early (< 8 frames of air) → velocity is cut to a shorter jump
static Vehicle robot() {
    Vehicle v;
    v.type = VehicleType::Robot;

    v.enter = +[](Player& p) {
        if (p.prevPlayer().vehicle.type != VehicleType::Ball)
            p.velocity = p.velocity / 2;
        if (p.prevPlayer().vehicle.type == VehicleType::Wave)
            p.velocity = p.velocity / 2;
        if (p.prevPlayer().vehicle.type == VehicleType::Ship && p.input)
            p.buffer = true;
    };

    v.clamp = +[](Player& p) {
        if (p.velocity < -g_phys.maxFallVelocity) p.velocity = -g_phys.maxFallVelocity;
        if (p.gravTop(p.innerHitbox()) >= p.gravCeiling()) { p.dead = true; p.deathCause = "ceiling"; }
    };

    v.update = +[](Player& p) {
        // Robot jump = gdp CHARGE model, confirmed by real truth data:
        //   launch at HALF cube velocity, then SUSTAIN that velocity (gravity
        //   cancelled, accel=0 -> constant-velocity linear rise) while the button is
        //   held, up to a max-charge cap; then fall ballistically at robot gravity
        //   (cube accel * 0.9). Real: dy=+1.263/frame flat for ~35 frames, then
        //   -0.044/frame. The old "launch full + cut on release" was a parabola (wrong).
        double* jumpMax = g_phys.robotJumpMax;
        double* accelerations = g_phys.robotAccel;
        // Full-jump sustain length. Confirmed 66 by a full-hold truth (144641895:
        // yVel flat 5.59 from the jump frame f68 through f134 = 66 charge frames, then
        // ballistic). The old 35 was from a truth where the button was RELEASED early,
        // which capped the rise before the real limit — it made the robot jump ~half
        // height and false-die on tall spike rows the real robot clears in one jump.
        // Real robot full-charge (button held past the cap) lasts a fixed frame count
        // that differs by size: a non-mini robot sustains 67 rise frames (66 charge +
        // the jump frame; truth 144641895), a MINI robot sustains 68 (67 charge; truth
        // 13711278 — two independent full holds f430-497 and f851-918 both 68 frames).
        // A flat 66 left the mini robot 1 frame short EVERY jump, and on a long-hold
        // robot level that per-jump ~1u error accumulated to ~36u, desyncing solutions
        // enough to false-survive in gdsim / die on real-GD replay.
        const int ROBOT_CHARGE_FRAMES = p.small ? g_phys.robotChargeFramesSmall : g_phys.robotChargeFramesBig;
        p.rotation = 0;

        bool jump = false;
        if (p.grounded) {
            // The robot jumps on a DISCRETE press, not a continuous hold. Unlike the
            // cube (which auto-repeats jumps while the button is held), real GD's robot
            // needs a fresh press or a buffered press to launch again — holding the
            // button on the ground does NOTHING after the first jump. gdsim was jumping
            // whenever the button was down and it was grounded last frame, so a held
            // button auto-re-launched the robot off every landing: it flew over
            // obstacles the real grounded robot walks into (13971403: a solver hold
            // f237-450 re-jumped off the f388 landing and cleared a ground spike at
            // x=554 that the real robot dies on — the solution "died at the first
            // click"). Match the cube's press discriminator (rising edge OR buffered
            // press); the prevGrounded guard keeps the landing-frame rule (a tap
            // starting on the exact landing frame doesn't jump). See [[jump_on_landing_frame]].
            // J block (see cube()'s identical guard above): a press buffered from
            // before landing must not count as the trigger while touching a J
            // block over a horizontal block.
            bool bufferedFromBeforeLanding = p.prevPlayer().buffer;
            if (bufferedFromBeforeLanding && p.touchingJBlock && !p.slopeData.slope)
                bufferedFromBeforeLanding = false;
            if (p.input && (bufferedFromBeforeLanding || !p.prevPlayer().input)
                && p.prevPlayer().grounded) jump = true;
            else p.setVelocity(0, true);
            p.buffer = false;
        }
        if (p.upsideDown && p.input && p.coyoteFrames < (unsigned)g_phys.robotUpsideCoyoteFrames) {
            jump = true;
            p.buffer = false;
        }

        // Charge only continues on a CONTINUOUS hold from the jump. Requiring the
        // button to have been down last frame too (prevPlayer().button) means a FRESH
        // press while airborne can't resume the charge — real GD's robot doesn't
        // re-charge mid-air (truth 144641895: a 3rd press at f250, mid-arc, must be a
        // no-op; without this gdsim froze the velocity and floated the player up,
        // clearing a spike the real run fell short of → solver solutions desynced).
        // Charge continues while still moving in the LAUNCH direction. A robot jump
        // always sets velocity POSITIVE (setVelocity(jumpMax*0.5)) — for an upside-down
        // robot the launch is world-DOWNward (grav() flips it) but the raw velocity is
        // still +, and it decays through 0 into the ballistic fall exactly as upright.
        // So the direction test is simply velocity>0 in BOTH orientations. The old
        // `velocity*(upsideDown?-1:1)>0` was BACKWARDS when flipped: after a valid
        // upside-down launch (velocity=+242) it read -242>0 = false, so the sustain
        // never engaged and every upside-down robot jump fell ballistically at half
        // height (truth 13971403 f5682: real holds a flat -4.492/-1.011u-per-frame
        // descent off a ceiling-floor for ~18 frames; gdsim decayed it to 0 and floated
        // ~15u too high into a spike → FALSE-DEATH the real run clears).
        bool charging = !p.grounded && p.button && p.prevPlayer().button
                        && p.velocity > 0
                        && p.robotHoldFrames < ROBOT_CHARGE_FRAMES;

        if (jump) {
            p.setVelocity(jumpMax[p.speed] * 0.5, p.prevPlayer().input);
            p.robotHoldFrames = 0;
            p.grounded = false;
            p.acceleration = 0;            // sustain (no gravity this frame)
            p.resyncPosition = true;
        } else if (charging) {
            p.robotHoldFrames++;
            p.acceleration = 0;            // hold -> constant velocity (linear rise)
        } else {
            // Ballistic fall. If we're airborne and NOT charging, this jump's charge
            // is SPENT — lock robotHoldFrames at the cap so a later mid-air press can't
            // resume it (only a fresh ground jump resets it to 0). Without this, the
            // charge was rechargeable as long as rHF < cap, so any airborne press
            // re-froze the velocity (truth 144641895 f250).
            if (!p.grounded) p.robotHoldFrames = ROBOT_CHARGE_FRAMES;
            p.acceleration = accelerations[p.speed] * g_phys.robotFallScale;   // ballistic fall
            if (p.gravityPortal && p.grav(p.velocity) > 350 && p.speed > 1)
                p.acceleration -= g_phys.robotUpsideExtraAccel;
        }
    };

    v.bounds = FLT_MAX;
    return v;
}

// ── Spider (instant surface teleport) ────────────────────────────────────
//  Click → scan nearby blocks for nearest opposite surface, teleport there.
static Vehicle spider() {
    Vehicle v;
    v.type = VehicleType::Spider;

    v.enter = +[](Player& p) {
        if (p.prevPlayer().vehicle.type != VehicleType::Ball)
            p.velocity = p.velocity / 2;
        // toggleSpiderMode sets m_width = m_height = 27; the generic 30 that
        // VehiclePortal::collide installs is the CUBE's. Deferred exactly the
        // way the wave defers its own 10/6 (see wave()'s enter) so the size
        // lands on the same frame boundary as every other mode switch.
        p.actions.push_back(+[](Player& p) {
            const float s = p.small ? g_phys.spiderSizeSmall : g_phys.spiderSizeBig;
            p.size = Vec2D(s, s);
        });
    };

    v.clamp = +[](Player& p) {
        if (p.velocity >=  g_phys.shipVelClamp) p.velocity =  g_phys.shipVelClamp;
        if (p.velocity <= -g_phys.shipVelClamp) p.velocity = -g_phys.shipVelClamp;
    };

    v.update = +[](Player& p) {
        // gdp updateJump: spider is in the 0.9582-gravity group with float_b=0.6 —
        // identical to Ball. The fork wrongly used cube accel (~1.67x too fast).
        p.acceleration = g_phys.spiderAccel;  // = ball gravity (0.9582 * 0.6)
        p.rotation = 0;

        bool jump = false;
        if (p.grounded) {
            if (p.input && (p.prevPlayer().buffer || !p.prevPlayer().input || p.vehicleBuffer))
                jump = true;
            else
                p.setVelocity(0, true);
            p.buffer = false;
        } else if (p.buffer && p.coyoteFrames < (p.upsideDown ? (unsigned)g_phys.spiderCoyoteUpsideFrames : (unsigned)g_phys.spiderCoyoteNormalFrames)) {
            jump = true;
        }

        if (jump) {
            // Scan for opposite surface BEFORE flipping gravity
            float targetY = findSpiderTarget(p);
            p.upsideDown = !p.upsideDown;
            p.pos.y      = targetY;
            p.velocity   = 0;
            p.grounded   = true;
            p.vehicleBuffer = false;
            p.buffer     = false;
            p.input      = false;
        }
    };

    v.bounds = FLT_MAX;
    return v;
}

// ── Swing (ship-like, gravity flip on click) ──────────────────────────────
//  Physics identical to Ship; portal ID differs for level parsing.
static Vehicle swing() {
    Vehicle v;
    v.type = VehicleType::Swing;

    v.enter = +[](Player& p) {
        if (p.prevPlayer().vehicle.type == VehicleType::Ufo ||
            p.prevPlayer().vehicle.type == VehicleType::Wave)
            p.velocity = p.velocity / 4.0;
        else
            p.velocity = p.velocity / 2.0;
    };

    v.clamp = +[](Player& p) {
        p.buffer = false;
        flightClamp(p, p.small ? g_phys.swingClampMinSmall : g_phys.swingClampMinBig,
                       p.small ? g_phys.swingClampMaxSmall : g_phys.swingClampMaxBig);
        if (p.gravTop(p) > p.gravCeiling()) {
            if (p.velocity > 0) p.setVelocity(0, false);
            p.pos.y = p.grav(p.gravCeiling()) - p.grav(p.size.y / 2);
        }
    };

    v.update = +[](Player& p) {
        p.buffer = false;
        if (p.grounded) p.setVelocity(0, !p.input);

        // Flat gravity, size-selected, no thrust term — verified against the
        // disassembly of updateJump's swing block at 0x14038c959 (see the Swing
        // comment in Tunables.hpp for the instruction listing).
        //
        // BASE CORRECTED 2026-09-10: was cubeAccel[p.speed]. The swing is one of
        // the six modes updateJump overwrites m_gravity for with the flat
        // 0.958199024f, so it must NOT track the speed portal. Same value at 1x,
        // which is why a 1x-only capture could not tell the two apart.
        p.acceleration = -g_phys.flightGravity *
                         (p.small ? g_phys.swingGravScaleSmall : g_phys.swingGravScaleBig);

        // Gravity flip on a NEW press (not a hold): the same discrete-press rule
        // the robot needs, so holding cannot flip every frame.
        // NOT INDEPENDENTLY MEASURED — the capture proves the swing has no
        // thrust and shows it in both gravity orientations, but says nothing
        // about what the flip does to velocity. Velocity is carried through
        // unchanged here, which is the simplest reading; if a swing level
        // desyncs at a flip, this is the first thing to measure.
        if (p.input && !p.prevPlayer().input) {
            p.upsideDown = !p.upsideDown;
            p.gravityPortal = false;
        }

        if (p.grav(p.pos.y) >= p.gravCeiling()) p.setVelocity(0, false);
        rotateFly(p, 0.15f);
    };

    v.bounds = 300;
    return v;
}

Vehicle Vehicle::from(VehicleType v) {
    switch (v) {
        case VehicleType::Cube:   return cube();
        case VehicleType::Ship:   return ship();
        case VehicleType::Ball:   return ball();
        case VehicleType::Ufo:    return ufo();
        case VehicleType::Wave:   return wave();
        case VehicleType::Robot:  return robot();
        case VehicleType::Spider: return spider();
        case VehicleType::Swing:  return swing();
    }
    return cube();
}

} // namespace gdsim
