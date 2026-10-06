#include "Pad.hpp"
#include "Player.hpp"
#include <cstdio>
#include <cstdlib>

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
    // Object rotation is in DEGREES (GD's field 6, negated). This added deg2rad(180) =
    // 3.14 "degrees", which made every flipped pad a 3-degree ORIENTED object.
    if (numFromString<int>(fields[5]) == 1) rotation += 180.f;
}

// Laid out like camila314/pathfinder's gd-sim/src/Objects/Pad.cpp: grouped by pad
// type with the speed columns labelled, because an unlabelled 4-wide numeric row
// gives no way to tell a per-tier table from a flat one at a glance.
//
// Checked row-by-row against that file 2026-08-31: 31 of the 32 rows are already
// IDENTICAL. The single divergence is Yellow/UFO, and it is deliberate — see its
// own note below. Anything else drifting from upstream is a bug, not a choice.
//
//                                                slow(0.5x)    1x            2x            3x
velocity_map<PadType, VehicleType, bool> pad_velocities = {
    {{PadType::Yellow, VehicleType::Cube,  false}, {864,          864,          864,          864}},
    {{PadType::Yellow, VehicleType::Cube,  true},  {691.2,        691.2,        691.2,        691.2}},

    {{PadType::Yellow, VehicleType::Ship,  false}, {864,          864,          864,          864}},
    {{PadType::Yellow, VehicleType::Ship,  true},  {691.2,        691.2,        691.2,        691.2}},

    {{PadType::Yellow, VehicleType::Ball,  false}, {518.4000206,  518.4000206,  518.4000206,  518.4000206}},
    {{PadType::Yellow, VehicleType::Ball,  true},  {414.7200165,  414.7200165,  414.7200165,  414.7200165}},

    // ── THE ONE DELIBERATE DIVERGENCE FROM UPSTREAM ──────────────────────────
    // Real-engine capture (compare_mechanics.cpp) measures the UFO yellow pad at
    // 16 raw yVel (= 864), FLAT across tiers — the same as Cube/Ship. Upstream
    // pathfinder still carries {573.48, 432, 432, 432} / {458.784, 691.2, ...},
    // which the physlab2 capture flagged at -49.2% (about half of real). Do NOT
    // "resync with upstream" here: that would re-break a measured value.
    {{PadType::Yellow, VehicleType::Ufo,   false}, {864,          864,          864,          864}},
    {{PadType::Yellow, VehicleType::Ufo,   true},  {691.2,        691.2,        691.2,        691.2}},

    // CORRECTED 2026-09-11 against the disassembly. Six cells here were not pad
    // values at all: -229.392 / -183.519 / -160.574397 / -128.463298 are the
    // BLUE ORB's tier-0 numbers (Orb.cpp, same column), and Ship-mini's 3x cell
    // held the Ball-mini value. They had been pasted into the 0.5x column of four
    // rows plus one 3x cell, making the gravity pad tier-dependent when it is not.
    //
    // The real thing is a single flat expression, with no m_yStart and therefore
    // no speed tier anywhere in it:
    //     propellPlayer(0.8f, ...) -> setYVelocity(flipMod * 0.8 * 16.0 * sizeMult)
    //     if (ball || spider || swing) m_yVelocity *= 0.6000000238418579
    //     flipGravity(...)           -> m_yVelocity *= 0.5
    // 0.8 * 16 * 54 = 691.2, halved by the flip = 345.6; mini * 0.8 = 276.48;
    // ball * 0.6f = 207.360008, ball mini = 165.888007. (The 0.6 keeps its float
    // dust on purpose - GD stores it as a double promoted from 0.6f.)
    {{PadType::Blue,   VehicleType::Cube,  false}, {-345.6,       -345.6,       -345.6,       -345.6}},
    {{PadType::Blue,   VehicleType::Cube,  true},  {-276.48,      -276.48,      -276.48,      -276.48}},

    {{PadType::Blue,   VehicleType::Ship,  false}, {-345.6,       -345.6,       -345.6,       -345.6}},
    {{PadType::Blue,   VehicleType::Ship,  true},  {-276.48,      -276.48,      -276.48,      -276.48}},

    {{PadType::Blue,   VehicleType::Ball,  false}, {-207.360008,  -207.360008,  -207.360008,  -207.360008}},
    {{PadType::Blue,   VehicleType::Ball,  true},  {-165.888007,  -165.888007,  -165.888007,  -165.888007}},

    {{PadType::Blue,   VehicleType::Ufo,   false}, {-345.6,       -345.6,       -345.6,       -345.6}},
    {{PadType::Blue,   VehicleType::Ufo,   true},  {-276.48,      -276.48,      -276.48,      -276.48}},

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
        const float rot = std::fmod(std::abs(rotation), 360.f);
        const bool facingDown = rot > 90.f && rot < 270.f;
        if ((facingDown && !p.upsideDown) || (!facingDown && p.upsideDown)) return;
        if (p.upsideDown != p.prevPlayer().upsideDown) return;
        // The gravity pad flips on first contact (collisionCheckObjects case 10), AFTER this
        // step's move, so the new direction shows on the next step by itself. The old
        // "wave pads fire one frame later" deferral only cancelled the same-step wave Y
        // re-integration Level::stepPlayer used to apply to every flip (now limited to
        // pushButton flips, see Player::flipBeforeUpdate). Truth 85701165 f6: mini wave
        // overlaps the pad (23,3) by 0.29 and rises from f7.
        if (getenv("GDSIM_ORBTOUCH_DEBUG"))
            std::fprintf(stderr, "PAD-TOUCH f=%d typeId=%d pos=(%.2f,%.2f) playerXY=(%.2f,%.2f) velBefore=%.3f\n",
                         p.frame, typeId, pos.x, pos.y, p.pos.x, p.pos.y, p.velocity);
        p.upsideDown = !p.upsideDown;
        if (p.vehicle.type == VehicleType::Wave) p.velocity = -p.velocity;
    }

    if (p.vehicle.type != VehicleType::Wave) {
        p.velocity = pad_velocities.get(type, orbPadVehicle(p.vehicle.type), p.small, std::min(3, p.speed));
        // PlayerObject::bumpPlayer clears m_isAccelerating for every pad, then
        // sets it again only for the RED one (objectType 0x22). The gravity pad
        // never reaches bumpPlayer at all — it goes through propellPlayer from
        // collisionCheckObjects' case 10 — so it leaves the flag untouched.
        if (type != PadType::Blue) p.isAccelerating = (type == PadType::Red);
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
