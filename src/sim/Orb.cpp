#include "Orb.hpp"
#include "Player.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace gdsim {

OrbType orbTypeFromId(int id) {
    switch (id) {
        case 36:   return OrbType::Yellow;
        case 84:   return OrbType::Blue;
        case 141:  return OrbType::Pink;
        case 1333: return OrbType::Red;
        case 3004: return OrbType::Spider;   // real spider/teleport orb (capture: RING 3004 -> ~1 = teleport, objType 43)
        case 1330: return OrbType::Black;    // upstream gd-sim maps 1330->Black (slam, -810); fork had wrongly used it for Spider. CONFIRM in-game.
        case 1022: return OrbType::Green;
        case 1704: return OrbType::Dash;
        case 1751: return OrbType::GravityFlip;  // capture: flips gravity + halves velocity, no jump (was default->Yellow, +5400% error)
        default:   return OrbType::Yellow;
    }
}

Orb::Orb(Vec2D s, std::unordered_map<int, std::string>&& fields) : EffectObject(s, std::move(fields)) {
    type = orbTypeFromId(numFromString<int>(fields[1]));
}

bool Orb::touching(Player const& p) const {
    return EffectObject::touching(p) || EffectObject::touching(p.prevPlayer());
}

// The yellow rows are now DERIVED, not captured: PlayerObject::ringJump sets
// `v = flipMod * m_yStart * (mini ? 0.8 : 1.0)` and applies no type multiplier at
// all for the yellow ring, so yellow == the plain jump impulse == cubeJumpHeight[]
// (m_yStart * 54, transcribed bit-exact from updateTimeMod).
//
// That exposed one real defect. Three of the four mini tiers already matched
// 0.8*jump to 1e-5; the 2x tier did not, and its shortfall was 0.215026 internal
// units against a 2x gravity step of 0.215370 - i.e. that one sample had been
// captured a frame late, after gravity had already decayed it, and 8.92066
// quantises to exactly the 8.921 that was recorded. 481.734 -> 493.3453824,
// a 2.4% correction on mini-2x yellow orbs.
velocity_map<OrbType, VehicleType, bool> orb_velocities = {
    {{OrbType::Yellow, VehicleType::Cube,  false}, {573.481728,   603.7217159271, 616.681728,   606.421728}},
    {{OrbType::Yellow, VehicleType::Cube,  true},  {458.7853824,  482.9773727417, 493.3453824,  485.1373824}},
    {{OrbType::Yellow, VehicleType::Ship,  false}, {573.481728,   603.7217159271, 616.681728,   606.421728}},
    {{OrbType::Yellow, VehicleType::Ship,  true},  {458.7853824,  482.9773727417, 493.3453824,  485.1373824}},
    {{OrbType::Yellow, VehicleType::Ball,  false}, {401.435993,   422.60399,    431.67599,    424.493993}},
    {{OrbType::Yellow, VehicleType::Ball,  true},  {321.148795,   338.08319,    345.34079,    339.59519}},
    {{OrbType::Yellow, VehicleType::Ufo,   false}, {573.481728,   603.7217159271, 616.681728,   606.421728}},
    {{OrbType::Yellow, VehicleType::Ufo,   true},  {458.7853824,  482.9773727417, 493.3453824,  485.1373824}},
    {{OrbType::Blue,   VehicleType::Cube,  false}, {-229.392,     -241.488,     -246.672,     -242.568}},
    {{OrbType::Blue,   VehicleType::Cube,  true},  {-183.519,     -193.185,     -197.343,     -194.049}},
    {{OrbType::Blue,   VehicleType::Ship,  false}, {-229.392,     -241.488,     -246.672,     -242.568}},
    {{OrbType::Blue,   VehicleType::Ship,  true},  {-183.519,     -193.185,     -197.343,     -194.049}},
    {{OrbType::Blue,   VehicleType::Ball,  false}, {-160.574397,  -169.04160,   -172.6704,    -169.7976}},
    {{OrbType::Blue,   VehicleType::Ball,  true},  {-128.463298,  -135.2295,    -138.1401,    -135.8343}},
    {{OrbType::Blue,   VehicleType::Ufo,   false}, {-229.392,     -241.48,      -246.672,     -242.568}},
    {{OrbType::Blue,   VehicleType::Ufo,   true},  {-183.519,     -193.185,     -197.343,     -194.049}},
    {{OrbType::Pink,   VehicleType::Cube,  false}, {412.884,      434.7,        443.988,      436.644}},
    {{OrbType::Pink,   VehicleType::Cube,  true},  {330.318,      347.76,       355.212,      349.272}},
    {{OrbType::Pink,   VehicleType::Ship,  false}, {212.166,      223.398,      228.15,       224.37}},
    {{OrbType::Pink,   VehicleType::Ship,  true},  {169.776,      178.686,      182.52,       179.496}},
    {{OrbType::Pink,   VehicleType::Ball,  false}, {309.090595,   325.42019,    332.37539,    326.85659}},
    {{OrbType::Pink,   VehicleType::Ball,  true},  {247.287596,   260.3286,     265.923,      261.5004}},
    {{OrbType::Pink,   VehicleType::Ufo,   false}, {240.84,       253.584,      258.984,      254.718}},
    {{OrbType::Pink,   VehicleType::Ufo,   true},  {192.672,      202.824,      207.198,      203.742}},
    // DERIVED 2026-09-11 = cubeJumpHeight[tier] * typeMult * sizeMult, with
    // ringJump's red branch giving typeMult = ship 1.0 big / 1.4 mini, UFO 1.02 big
    // / 1.36 mini, ball 1.34, robot 1.28, spider 1.34, everything else 1.38 — then
    // the ball/spider 0.7 post-scale.
    //
    // All 32 old cells were low by EXACTLY one gravity step of their own vehicle
    // and band, which is what makes replacing the row wholesale safe rather than
    // cell by cell:
    //     cube  0.2116 0.2164 0.2146 0.2164   vs cube step  0.2115 0.2156 0.2154 0.2163
    //     ball  0.1296 0.1299 0.1290 0.1298   vs ball step  0.6 * cube = 0.1294
    //     UFO   0.1294 0.1286 0.1294 0.1286   vs UFO strong 0.6 * cube = 0.1294
    //     ship  0.0690 x4                     vs ship down-weak 0.32 * cube = 0.0690
    //     ship mini 0.0814 0.0806 ...         vs the same / 0.85 = 0.0812
    // i.e. the whole row was read one frame after the boost landed, with the
    // capture FALLING (not holding) in ship mode. Not one cell is unexplained.
    {{OrbType::Red,    VehicleType::Cube,  false}, {791.4047846,  833.1359680,  851.0207846,  836.8619846}},
    {{OrbType::Red,    VehicleType::Cube,  true},  {633.1238277,  666.5087744,  680.8166277,  669.4895877}},
    {{OrbType::Red,    VehicleType::Ship,  false}, {573.4817280,  603.7217159,  616.6817280,  606.4217280}},
    {{OrbType::Red,    VehicleType::Ship,  true},  {642.2995354,  676.1683218,  690.6835354,  679.1923354}},
    {{OrbType::Red,    VehicleType::Ball,  false}, {537.9258517,  566.2909599,  578.4474510,  568.8235712}},
    {{OrbType::Red,    VehicleType::Ball,  true},  {430.3406814,  453.0327679,  462.7579608,  455.0588569}},
    {{OrbType::Red,    VehicleType::Ufo,   false}, {584.9513626,  615.7961502,  629.0153626,  618.5501626}},
    {{OrbType::Red,    VehicleType::Ufo,   true},  {623.9481201,  656.8492269,  670.9497201,  659.7868401}},
    // DERIVED 2026-09-11. ringJump's green branch is `if (m_isShip) v *= 0.7;` and
    // nothing else — the green ring carries NO multiplier for cube, UFO or ball, so
    // it equals the yellow ring everywhere except the ship. (Its gravity flip runs
    // BEFORE setYVelocity, so unlike the blue ring it never picks up the flip's 0.5.)
    //
    // Every cell of the old row was captured one frame late, each short by exactly
    // one gravity step of ITS OWN vehicle: cube -0.212 (cube step 0.2115), ball
    // -0.129 (ball step 0.1294). The ship row was late in the other direction,
    // +0.086 = one HELD-thrust step, because the capture was taken with the button
    // down; its 2x cell had additionally been clipped to a flat 432 by the flight
    // clamp. That is four independent confirmations of the same mis-timed capture.
    {{OrbType::Green,  VehicleType::Cube,  false}, {573.481728,   603.7217159271, 616.681728,   606.421728}},
    {{OrbType::Green,  VehicleType::Cube,  true},  {458.7853824,  482.9773727417, 493.3453824,  485.1373824}},
    {{OrbType::Green,  VehicleType::Ship,  false}, {401.4372096,  422.6052011490, 431.6772096,  424.4952096}},
    {{OrbType::Green,  VehicleType::Ship,  true},  {321.1497677,  338.0841609192, 345.3417677,  339.5961677}},
    {{OrbType::Green,  VehicleType::Ball,  false}, {401.435993,   422.60399,      431.67599,    424.493993}},
    {{OrbType::Green,  VehicleType::Ball,  true},  {321.148795,   338.08319,      345.34079,    339.59519}},
    {{OrbType::Green,  VehicleType::Ufo,   false}, {573.481728,   603.7217159271, 616.681728,   606.421728}},
    {{OrbType::Green,  VehicleType::Ufo,   true},  {458.7853824,  482.9773727417, 493.3453824,  485.1373824}},
};

// PlayerObject::ringJump's post-scale, applied after setYVelocity:
//     if (ball || spider) m_yVelocity *= 0.699999988079071f
//     else if (swing)     m_yVelocity *= 0.6000000238418579f
// The 0.7 is already folded into every Ball row of the table above (that is where
// its long-unexplained "ball is 0.7x cube" pattern comes from — it is this line,
// not a per-orb constant). Spider shares the Ball row correctly. The SWING does
// not: orbPadVehicle() collapses it onto Ball too, which hands it 0.7 where the
// engine uses 0.6, making every orb 16.7% too strong in swing mode. Rescale it.
//
// Note pads are NOT affected: PlayerObject::propellPlayer scales ball, spider AND
// swing by the same 0.6, so collapsing swing onto ball is right there.
static constexpr double kSwingOrbRescale = 0.6000000238418579 / 0.699999988079071;

double orbVelocityValue(OrbType t, VehicleType v, bool mini, int speed) {
    double vel = orb_velocities.get(t, orbPadVehicle(v), mini, std::min(3, speed));
    if (v == VehicleType::Robot && t == OrbType::Yellow) vel *= 0.9;  // see Orb::collide
    if (v == VehicleType::Swing) vel *= kSwingOrbRescale;
    // ringJump's red branch gives the ROBOT its own 1.28 where the cube gets 1.38.
    // orbPadVehicle() collapses robot onto cube, so undo the difference here — the
    // same shape as the robot's 0.9 yellow rule above. (Spider's red value is 1.34,
    // which is the ball's, so the Spider->Ball collapse already handles it.)
    if (v == VehicleType::Robot && t == OrbType::Red) vel *= (1.28 / 1.38);
    return vel;
}

void Orb::collide(Player& p) const {
    if (p.buffer || (p.prevPlayer().buffer && !p.button)
        || (p.vehicle.type == VehicleType::Ball && p.vehicleBuffer))
    {
        if (getenv("GDSIM_ORBTOUCH_DEBUG"))
            std::fprintf(stderr, "ORB-TOUCH f=%d type=%d typeId=%d pos=(%.2f,%.2f) rot=%.2f playerXY=(%.2f,%.2f) velBefore=%.3f\n",
                         p.frame, (int)type, typeId, pos.x, pos.y, rotation, p.pos.x, p.pos.y, p.velocity);
        p.buffer = false;
        p.vehicleBuffer = false;
        EffectObject::collide(p);

        if (type == OrbType::Spider) {
            // Spider orb: teleport to the nearest opposite surface and flip
            // gravity, exactly like a spider-vehicle click (works in any mode).
            float targetY = findSpiderTarget(p);
            p.upsideDown = !p.upsideDown;
            p.pos.y      = targetY;
            p.setVelocity(0, false);
            p.grounded   = true;
            p.input      = false;
        } else if (type == OrbType::Dash && p.touchingSBlock) {
            // S block (SpecialBlock.hpp, id 1829 "Stop Dash") neutralizes a dash
            // orb entirely while touching it — GD Creator School: "S blocks stop
            // dash orbs." The click is still consumed (EffectObject::collide above
            // already marked this orb used), it just doesn't launch a dash.
        } else if (type == OrbType::Dash) {
            // Dash orb: glide along the orb's angle at constant velocity (gravity
            // off) for as long as the button is held. velocity = Xspeed*tan(angle)
            // makes the player track the orb's line. (best-effort; needs capture.)
            //
            // TRIED 2026-08-10 (macro-demonlist level 82172844 "Cobwebs"): snapping
            // p.pos.y onto the line through the orb's own position at touch time
            // (`pos.y + dashTan*(p.pos.x - pos.x)`) fixed a false hazard death in
            // Cobwebs (player touched a 0° dash orb at (1155,1005) while sitting
            // 11.76u above it, at y=1016.76 — gdsim then held the WHOLE dash at
            // 1016.76, clipping a hazard the corridor was clearly built to clear at
            // the orb's own y=1005) — but REGRESSED a different real macro (Cold
            // Sweat, 63996127: touch was ~20u X-past the orb, and snapping there
            // introduced a NEW death, 6.62%->1.14%). Isolated batch validation: 1
            // better / 1 worse — not the clean win the other two fixes this session
            // got, so reverted rather than kept on a coin-flip. Both touches here
            // were far (X and/or Y) from the orb's own position when they fired —
            // real GD's true touch timing/hitbox for Dash specifically, and whether
            // it snaps at all, needs a live capture (memory read or physlab-style
            // no-input course) before trying this again.
            p.dashTan  = std::tan(rotation * 0.017453292519943295);  // rotation is in degrees
            p.dashing  = true;
            p.velocity = player_speeds[p.speed] * p.dashTan;
            p.velocityOverride = true;
            p.grounded = false;
        } else if (type == OrbType::GravityFlip) {
            // ID 1751 (from real-engine capture): flips gravity and halves the
            // current velocity — no jump boost. The halving ratio is unit-free, so
            // it works in gdsim's px/s units directly. Deferred like the other flip
            // orbs for the flip-vs-velocity timing (see the flip below).
            p.velocity *= 0.5;
            p.velocityOverride = true;
            p.grounded = false;
        } else if (p.vehicle.type != VehicleType::Wave) {
            if (type == OrbType::Black) {
                // Black/slam orb (1330): fixed downward (gravity-relative) velocity —
                // speed- and size-independent (capture RING 1330: cube -15 at every
                // speed & mini). The OLD flat -810 was right only for cube; the real
                // slam differs per vehicle (values = captured |aYVel| x54). UFO was the
                // worst at -34%. Unsampled vehicles (ball/robot/swing) keep the cube value.
                switch (p.vehicle.type) {
                    case VehicleType::Ship:   p.velocity = -756; break;   // real -14
                    case VehicleType::Ufo:    p.velocity = -605; break;   // real -11.2
                    case VehicleType::Spider: p.velocity = -891; break;   // real -16.5
                    default:                  p.velocity = -810; break;   // cube -15 (+ unsampled)
                }
            } else {
                // Go through orbVelocityValue() rather than the raw table so the
                // per-vehicle corrections it applies (robot's 0.9 yellow, the
                // swing's 0.6-vs-0.7 rescale) land on the real gameplay path too.
                // This used to read the table directly and duplicate only the robot
                // rule, so any correction added there silently missed live play and
                // applied only to the solver's own lookups.
                p.velocity = orbVelocityValue(type, p.vehicle.type, p.small, p.speed);
                p.grounded = false;
                // PlayerObject::ringJump sets m_isAccelerating only for the RED
                // ring (`if (objectType == 0x23) m_isAccelerating = 1`) and on the
                // dash-ring path. Every other ring leaves the flag alone — it is
                // not cleared either, unlike the pads. See Player::isAccelerating.
                if (type == OrbType::Red || type == OrbType::Dash)
                    p.isAccelerating = true;
            }
            // GD applies the orb boost in checkCollisions — AFTER this step's
            // updateJump (velocity + position). For a NEWLY-touched orb the touch is
            // only recorded there, so the boost lands NEXT frame: the orb frame still
            // moves with the OLD velocity and the orb velocity reads un-decayed a
            // frame later (truth 308 f1373). But if the player was ALREADY overlapping
            // the orb on the previous frame (m_touchedRing set), the click's updateJump
            // consumes it on THIS frame, so the boost lands SAME frame — position moves
            // up with it and it takes one gravity decay (truth 308 f3316; the old
            // always-defer lagged those orbs by 2.77u / 0.22 dV). So defer ONLY on a
            // fresh touch; let an already-touched orb run the normal semi-implicit path.
            // Dropping the Blue/Green always-defer and reusing the generic
            // `!touching(p.prevPlayer())` discriminator fixed an isolated real touch
            // almost exactly (DeCode x816: +1.582 -> +0.049 error) — proven correct
            // for that touch. It IS a net loss across the broader 157-level macro
            // batch (two levels dropped hard), so treat this as DeCode-scoped tuning,
            // not a universally-validated fix, until the batch-regression cases are
            // understood. 2026-08-20: re-enabled per explicit direction to prioritize
            // DeCode's own fidelity over the broader batch for now.
            p.velocityOverride = !touching(p.prevPlayer());
        }
        if (type == OrbType::Blue || type == OrbType::Green || type == OrbType::GravityFlip)
            p.upsideDown = !p.upsideDown;
        if (p.vehicle.type == VehicleType::Ball)
            p.input = false;
    }
}

} // namespace gdsim
