#include "Pad.hpp"
#include "Player.hpp"

namespace gdsim {

PadType padTypeFromId(int id) {
    switch (id) {
        case 35:   return PadType::Yellow;
        case 67:   return PadType::Blue;
        case 140:  return PadType::Pink;
        case 1332: return PadType::Red;
        default:   return PadType::Yellow;
    }
}

Pad::Pad(Vec2D s, std::unordered_map<int, std::string>&& fields) : EffectObject(s, std::move(fields)) {
    type = padTypeFromId(numFromString<int>(fields[1]));
    if (numFromString<int>(fields[5]) == 1) rotation += deg2rad(180);
}

const velocity_map<PadType, VehicleType, bool> pad_velocities = {
    {{PadType::Yellow, VehicleType::Cube,  false}, {864,          864,          864,          864}},
    {{PadType::Yellow, VehicleType::Cube,  true},  {691.2,        691.2,        691.2,        691.2}},
    {{PadType::Yellow, VehicleType::Ship,  false}, {864,          864,          864,          864}},
    {{PadType::Yellow, VehicleType::Ship,  true},  {691.2,        691.2,        691.2,        691.2}},
    {{PadType::Yellow, VehicleType::Ball,  false}, {518.4000206,  518.4000206,  518.4000206,  518.4000206}},
    {{PadType::Yellow, VehicleType::Ball,  true},  {414.7200165,  414.7200165,  414.7200165,  414.7200165}},
    // Real-engine capture (compare_mechanics.cpp): UFO yellow pad = 16 (=864), flat —
    // same as Cube/Ship. Prior rows gave half of real: {573.48,...}, then 438 (=8.11);
    // the fix comment was in place but the literal was never corrected to 864 until the
    // physlab2 capture flagged it at -49.2%. Pink/Red UFO pads were already correct.
    {{PadType::Yellow, VehicleType::Ufo,   false}, {864,          864,          864,          864}},
    {{PadType::Yellow, VehicleType::Ufo,   true},  {691.2,        691.2,        691.2,        691.2}},
    {{PadType::Blue,   VehicleType::Cube,  false}, {-345.6,       -345.6,       -345.6,       -345.6}},
    {{PadType::Blue,   VehicleType::Cube,  true},  {-276.48,      -276.48,      -276.48,      -276.48}},
    {{PadType::Blue,   VehicleType::Ship,  false}, {-229.392,     -345.6,       -345.6,       -345.6}},
    {{PadType::Blue,   VehicleType::Ship,  true},  {-183.519,     -276.48,      -276.48,      -165.888}},
    {{PadType::Blue,   VehicleType::Ball,  false}, {-160.574397,  -207.360008,  -207.360008,  -207.360008}},
    {{PadType::Blue,   VehicleType::Ball,  true},  {-128.463298,  -165.888007,  -165.888007,  -165.888007}},
    {{PadType::Blue,   VehicleType::Ufo,   false}, {-229.392,     -345.6,       -345.6,       -345.6}},
    {{PadType::Blue,   VehicleType::Ufo,   true},  {-183.519,     -276.48,      -276.48,      -276.48}},
    {{PadType::Pink,   VehicleType::Cube,  false}, {561.6,        561.6,        561.6,        561.6}},
    {{PadType::Pink,   VehicleType::Cube,  true},  {449.28,       449.28,       449.28,       449.28}},
    {{PadType::Pink,   VehicleType::Ship,  false}, {302.4,        302.4,        302.4,        302.4}},
    {{PadType::Pink,   VehicleType::Ship,  true},  {241.92,       241.92,       241.92,       241.92}},
    {{PadType::Pink,   VehicleType::Ball,  false}, {362.880014,   362.880014,   362.880014,   362.880014}},
    {{PadType::Pink,   VehicleType::Ball,  true},  {290.304012,   290.304012,   290.304012,   290.304012}},
    {{PadType::Pink,   VehicleType::Ufo,   false}, {345.6,        345.6,        345.6,        345.6}},
    {{PadType::Pink,   VehicleType::Ufo,   true},  {276.48,       276.48,       276.48,       276.48}},
    {{PadType::Red,    VehicleType::Cube,  false}, {1080,         1080,         1080,         1080}},
    {{PadType::Red,    VehicleType::Cube,  true},  {864,          864,          864,          864}},
    {{PadType::Red,    VehicleType::Ship,  false}, {544.32,       544.32,       544.32,       544.32}},
    {{PadType::Red,    VehicleType::Ship,  true},  {656.64,       656.64,       656.64,       656.64}},
    {{PadType::Red,    VehicleType::Ball,  false}, {648.00002575, 648.00002575, 648.00002575, 648.00002575}},
    {{PadType::Red,    VehicleType::Ball,  true},  {518.4000206,  518.4000206,  518.4000206,  518.4000206}},
    {{PadType::Red,    VehicleType::Ufo,   false}, {518.4,        518.4,        518.4,        518.4}},
    {{PadType::Red,    VehicleType::Ufo,   true},  {677.376,      677.376,      677.376,      677.376}},
};

double padVelocityValue(PadType t, VehicleType v, bool mini, int speed) {
    return pad_velocities.get(t, orbPadVehicle(v), mini, std::min(3, speed));
}

void Pad::collide(Player& p) const {
    if (type == PadType::Blue) {
        auto rot = rad2deg(std::abs(rotation));
        if ((rot > 90 && !p.upsideDown) || (rot < 90 && p.upsideDown)) return;
        if (p.upsideDown != p.prevPlayer().upsideDown) return;
        // A WAVE gravity-pad takes effect one frame after first contact (newly-touched
        // deferral, like an orb — see [[orb_touch_timing_sameframe]]): GD resolves the
        // pad in the collision phase after this frame's motion, so a freshly-touched
        // flip only bites next frame. gdsim's SAT overlap fires a frame early, which in
        // a mini-wave gravity-pad corridor (85701165) compounds into a large drift.
        // Return WITHOUT marking the pad used so it fires on the next (still-overlapping)
        // frame. Non-wave pads keep their existing velocityOverride next-frame handling.
        if (p.vehicle.type == VehicleType::Wave && !Object::touching(p.prevPlayer())) return;
        p.upsideDown = !p.upsideDown;
        if (p.vehicle.type == VehicleType::Wave) p.velocity = -p.velocity;
    }

    if (p.vehicle.type != VehicleType::Wave) {
        p.velocity = pad_velocities.get(type, orbPadVehicle(p.vehicle.type), p.small, std::min(3, p.speed));
        // Next-frame boost semantics (see Orb::collide): GD applies a pad in the
        // collision phase, after this frame's position + gravity, so the boost only
        // affects the next frame. Override prevents this frame's gravity from eating
        // it and the semi-implicit resync from moving position with it one frame early.
        p.velocityOverride = true;
    }

    p.grounded = false;
    p.gravityPortal = false;
    EffectObject::collide(p);
}

} // namespace gdsim
