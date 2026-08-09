#pragma once
#include "EffectObject.hpp"

namespace gdsim {

enum class OrbType {
    Yellow,
    Blue,
    Pink,
    Red,
    Green,
    Black,
    Dash,    // GD 2.1 dash orb (ID 1704): hold to dash along the orb's angle. MECHANIC NOT YET IMPLEMENTED (collide has no Dash branch -> currently sets velocity 0). Needs capture: gdp lacks dash physics.
    Spider,  // GD 2.2 spider orb (ID 1330): teleport to opposite surface
    GravityFlip, // ID 1751: flips gravity and HALVES the current velocity; NO jump boost. From real-engine capture (RING 1751: aYVel = bYVel/2 in every vehicle/gravity context). Was falling through to Yellow -> a bogus +11 jump (+5400% error).
};

struct Orb : public EffectObject {
    OrbType type;
    Orb(Vec2D size, std::unordered_map<int, std::string>&& fields);
    bool touching(Player const&) const override;
    void collide(Player&) const override;
};

enum class VehicleType;
// Map a level orb object-id to its gdsim OrbType (same switch as the Orb ctor),
// and read the raw table velocity gdsim would apply. Used by the offline
// mechanic-database comparator (test/compare_mechanics.cpp) to validate gdsim
// against real-engine capture without constructing a full collision.
OrbType orbTypeFromId(int id);
double  orbVelocityValue(OrbType t, VehicleType v, bool mini, int speed);

} // namespace gdsim
