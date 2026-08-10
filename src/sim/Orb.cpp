#include "Orb.hpp"
#include "Player.hpp"
#include <cstdio>
#include <cstdlib>

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

const velocity_map<OrbType, VehicleType, bool> orb_velocities = {
    {{OrbType::Yellow, VehicleType::Cube,  false}, {573.48,       603.72,       616.68,       606.42}},
    {{OrbType::Yellow, VehicleType::Cube,  true},  {458.784,      482.976,      481.734,      485.136}},
    {{OrbType::Yellow, VehicleType::Ship,  false}, {573.48,       603.72,       616.68,       606.42}},
    {{OrbType::Yellow, VehicleType::Ship,  true},  {458.784,      482.976,      481.734,      485.136}},
    {{OrbType::Yellow, VehicleType::Ball,  false}, {401.435993,   422.60399,    431.67599,    424.493993}},
    {{OrbType::Yellow, VehicleType::Ball,  true},  {321.148795,   338.08319,    345.34079,    339.59519}},
    {{OrbType::Yellow, VehicleType::Ufo,   false}, {573.48,       603.72,       616.68,       606.42}},
    {{OrbType::Yellow, VehicleType::Ufo,   true},  {458.784,      482.976,      481.734,      485.136}},
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
    {{OrbType::Red,    VehicleType::Cube,  false}, {779.976,      821.448,      839.43,       825.174}},
    {{OrbType::Red,    VehicleType::Cube,  true},  {621.702,      654.858,      669.222,      657.828}},
    {{OrbType::Red,    VehicleType::Ship,  false}, {569.754,      599.994,      612.954,      602.694}},
    {{OrbType::Red,    VehicleType::Ship,  true},  {637.902,      671.814,      686.286,      674.838}},
    {{OrbType::Red,    VehicleType::Ball,  false}, {530.928,      559.278,      571.482,      561.816}},
    {{OrbType::Red,    VehicleType::Ball,  true},  {423.36,       446.04,       455.76,       448.092}},
    {{OrbType::Red,    VehicleType::Ufo,   false}, {577.962,      608.85,       622.026,      611.604}},
    {{OrbType::Red,    VehicleType::Ufo,   true},  {615.762,      648.648,      662.742,      651.564}},
    {{OrbType::Green,  VehicleType::Cube,  false}, {562.032,      592.056,      605.07,       594.756}},
    {{OrbType::Green,  VehicleType::Cube,  true},  {447.336,      471.312,      481.734,      485.136}},
    {{OrbType::Green,  VehicleType::Ship,  false}, {406.08,       427.248,      432,          429.138}},
    {{OrbType::Green,  VehicleType::Ship,  true},  {326.592,      343.548,      350.784,      345.06}},
    {{OrbType::Green,  VehicleType::Ball,  false}, {394.47,       415.638,      424.71,       417.528}},
    {{OrbType::Green,  VehicleType::Ball,  true},  {314.172,      331.074,      331.074,      332.586}},
    // Green UFO (false) was a flat {432} placeholder = 8.01 yVel, but real-engine
    // capture (RING 1022 UFO) gives 11.23 == the Green Cube progression. Mirror Cube.
    {{OrbType::Green,  VehicleType::Ufo,   false}, {562.032,      592.056,      605.07,       594.756}},
    {{OrbType::Green,  VehicleType::Ufo,   true},  {450.576,      474.768,      485.136,      476.928}},
};

double orbVelocityValue(OrbType t, VehicleType v, bool mini, int speed) {
    double vel = orb_velocities.get(t, orbPadVehicle(v), mini, std::min(3, speed));
    if (v == VehicleType::Robot && t == OrbType::Yellow) vel *= 0.9;  // see Orb::collide
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
                p.velocity = orb_velocities.get(type, orbPadVehicle(p.vehicle.type), p.small, std::min(3, p.speed));
                // Robot's YELLOW orb is exactly 0.9x the cube value (capture RING 36
                // Robot: 10.107 vs cube 11.23, and mini 8.086 vs 8.984 — both = x0.9).
                // Only yellow is affected; pink/blue/pad on robot match cube 1:1. The
                // table collapses robot->cube via orbPadVehicle, so scale here.
                if (p.vehicle.type == VehicleType::Robot && type == OrbType::Yellow)
                    p.velocity *= 0.9;
                p.grounded = false;
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
            // EXCEPTION: gravity-flip orbs (blue/green) have an extra flip-vs-velocity
            // timing interaction (see the flip below / wave_gravity_flip work) that the
            // same-frame path gets wrong — keep them always-deferred until that's modelled.
            bool flipOrb = (type == OrbType::Blue || type == OrbType::Green);
            p.velocityOverride = flipOrb ? true : !touching(p.prevPlayer());
        }
        if (type == OrbType::Blue || type == OrbType::Green || type == OrbType::GravityFlip)
            p.upsideDown = !p.upsideDown;
        if (p.vehicle.type == VehicleType::Ball)
            p.input = false;
    }
}

} // namespace gdsim
