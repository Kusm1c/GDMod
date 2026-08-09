#include "Vehicle.hpp"
#include "Player.hpp"
#include "Object.hpp"
#include "Level.hpp"
#include "Slope.hpp"
#include "Calib.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace gdsim {

constexpr double velocity_thresholds[] = {
    101.541492,
    103.485494592,
    103.377492,
    103.809492,
    103.809492
};

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
        if (p.velocity < -810.0) p.velocity = -810.0;
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
        static double accelerations[] = {-2747.52, -2794.1082, -2786.4, -2799.36, -2799.36};
        p.acceleration = accelerations[p.speed];

        if (p.gravityPortal && p.grav(p.velocity) > 350 && p.speed > 1)
            p.acceleration -= 6.48;

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
            if ((p.buffer && !p.prevPlayer().buffer) ||
                (p.input && p.prevPlayer().grounded))
                jump = true;
            else p.setVelocity(0, true);
            p.buffer = false;
        }

        if (p.upsideDown && p.input && p.coyoteFrames < 10) {
            jump = true;
            p.buffer = false;
        }

        if (jump) {
            static double jumpHeights[] = {573.481728, 603.7217172, 616.681728, 606.421728, 606.421728};
            if (p.slopeData.slope && p.slopeData.slope->orientation == 0) {
                auto time = std::clamp(10*(p.timeElapsed - p.slopeData.elapsed), 0.4, 1.0);
                double vel = 0.9 * std::min(1.12 / p.slopeData.slope->angle(), 1.54)
                           * (p.slopeData.slope->size.y * player_speeds[p.speed] / p.slopeData.slope->size.x);
                p.setVelocity(0.25*time*vel + jumpHeights[p.speed], p.prevPlayer().input);
                p.grounded = false;
            } else {
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
        p.velocity = std::clamp(p.velocity, p.small ? -406.566 : -345.6, p.small ? 508.248 : 432.0);
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
                p.acceleration = sm ? 1643.5872 : 1397.0491;
            else
                p.acceleration = sm ? 1314.86976 : 1117.64328;
        } else {
            if (p.velocity >= p.grav(velocity_thresholds[p.speed]))
                p.acceleration = sm ? -1577.85408 : -1341.1719;
            else
                p.acceleration = sm ? -1051.8984 : -894.11464;
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
        if (p.velocity >=  810) p.velocity =  810;
        if (p.velocity <= -810) p.velocity = -810;
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
            p.acceleration = -1676.46672;

        if (!p.input) p.vehicleBuffer = false;

        bool jump = false;
        if (p.grounded) {
            if (p.input && (p.prevPlayer().buffer || !p.prevPlayer().input || p.vehicleBuffer)) {
                jump = true;
            } else {
                p.setVelocity(0, true);
            }
            p.buffer = false;
        } else if (p.buffer && p.coyoteFrames < (p.upsideDown ? 16u : 1u)) {
            jump = true;
        }

        if (jump) {
            static double jumpHeights[] = {-172.044007, -181.11601, -185.00401, -181.92601, -181.92601};
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
        p.velocity = std::clamp(p.velocity,
            p.small ? -406.56 : -345.6,
            p.small ?  508.24 :  520.0);
        p.input = p.button;
        if (p.gravTop(p) > p.gravCeiling()) {
            if (p.velocity > 0) p.setVelocity(0, false);
            p.pos.y = p.grav(p.gravCeiling()) - p.grav(p.size.y / 2);
        }
    };

    v.update = +[](Player& p) {
        if (p.buffer) {
            p.velocity = std::max(p.velocity, p.small ? 358.992 : 371.034);
            p.velocityOverride = true;
            p.buffer = false;
            p.grounded = false;
        } else {
            // See ship() for why velocity_thresholds must NOT be wrapped in grav(): it's a
            // relative-frame magnitude, same convention as p.velocity itself.
            if (p.velocity > velocity_thresholds[p.speed])
                p.acceleration = p.small ? -1969.92 : -1671.84;
            else
                p.acceleration = p.small ? -1308.96 : -1114.56;
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
            p.size = p.small ? Vec2D(6, 6) : Vec2D(10, 10);
        });
    };

    v.clamp = +[](Player& p) {
        float waveTop    = p.grav(p.pos.y + p.grav(p.size.y));
        float waveBottom = p.grav(p.pos.y - p.grav(p.size.y));
        p.velocity = (p.input * 2 - 1) * player_speeds[p.speed] * (p.small ? 2.f : 1.f);
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
        rotateFly(p, p.small ? 0.4f : 0.25f);
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
        if (p.velocity < -810) p.velocity = -810;
        if (p.gravTop(p.innerHitbox()) >= p.gravCeiling()) { p.dead = true; p.deathCause = "ceiling"; }
    };

    v.update = +[](Player& p) {
        // Robot jump = gdp CHARGE model, confirmed by real truth data:
        //   launch at HALF cube velocity, then SUSTAIN that velocity (gravity
        //   cancelled, accel=0 -> constant-velocity linear rise) while the button is
        //   held, up to a max-charge cap; then fall ballistically at robot gravity
        //   (cube accel * 0.9). Real: dy=+1.263/frame flat for ~35 frames, then
        //   -0.044/frame. The old "launch full + cut on release" was a parabola (wrong).
        static double jumpMax[] = {573.481728, 603.7217172, 616.681728, 606.421728, 606.421728};
        static double accelerations[] = {-2747.52, -2794.1082, -2786.4, -2799.36, -2799.36};
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
        const int ROBOT_CHARGE_FRAMES = p.small ? 67 : 66;
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
            if (p.input && (p.prevPlayer().buffer || !p.prevPlayer().input)
                && p.prevPlayer().grounded) jump = true;
            else p.setVelocity(0, true);
            p.buffer = false;
        }
        if (p.upsideDown && p.input && p.coyoteFrames < 10) {
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
            p.acceleration = accelerations[p.speed] * 0.9;   // ballistic fall
            if (p.gravityPortal && p.grav(p.velocity) > 350 && p.speed > 1)
                p.acceleration -= 6.48;
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
    };

    v.clamp = +[](Player& p) {
        if (p.velocity >=  810) p.velocity =  810;
        if (p.velocity <= -810) p.velocity = -810;
    };

    v.update = +[](Player& p) {
        // gdp updateJump: spider is in the 0.9582-gravity group with float_b=0.6 —
        // identical to Ball. The fork wrongly used cube accel (~1.67x too fast).
        p.acceleration = -1676.46672;  // = ball gravity (0.9582 * 0.6)
        p.rotation = 0;

        bool jump = false;
        if (p.grounded) {
            if (p.input && (p.prevPlayer().buffer || !p.prevPlayer().input || p.vehicleBuffer))
                jump = true;
            else
                p.setVelocity(0, true);
            p.buffer = false;
        } else if (p.buffer && p.coyoteFrames < (p.upsideDown ? 16u : 1u)) {
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
        p.velocity = std::clamp(p.velocity,
            p.small ? -406.566 : -345.6,
            p.small ?  508.248 :  432.0);
        if (p.gravTop(p) > p.gravCeiling()) {
            if (p.velocity > 0) p.setVelocity(0, false);
            p.pos.y = p.grav(p.gravCeiling()) - p.grav(p.size.y / 2);
        }
    };

    v.update = +[](Player& p) {
        p.buffer = false;
        if (p.grounded) p.setVelocity(0, !p.input);
        // See ship() for why velocity_thresholds must NOT be wrapped in grav(). NOTE:
        // unlike Ship, real GD's Swing branch in updateJump does NOT read m_isAccelerating
        // at all (confirmed from the decompiled binary) — it's a much simpler continuous
        // thrust formula. gdsim's swing() is still a copy of the old ship() threshold model
        // here, which is a known-separate inaccuracy, not touched in this pass (Swing has
        // no reported bug; a faithful port needs its own verification).
        if (p.input) {
            if (p.velocity <= velocity_thresholds[p.speed])
                p.acceleration = p.small ? 1643.5872 : 1397.0491;
            else
                p.acceleration = p.small ? 1314.86976 : 1117.64328;
        } else {
            if (p.velocity >= velocity_thresholds[p.speed])
                p.acceleration = p.small ? -1577.85408 : -1341.1719;
            else
                p.acceleration = p.small ? -1051.8984 : -894.11464;
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
