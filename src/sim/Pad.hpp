#pragma once
#include "EffectObject.hpp"
#include "util.hpp"
#include "Vehicle.hpp"

namespace gdsim {

enum class PadType {
    Yellow,
    Blue,
    Pink,
    Red,
};

struct Pad : public EffectObject {
    PadType type;
    Pad(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
};

PadType padTypeFromId(int id);
double  padVelocityValue(PadType t, VehicleType v, bool mini, int speed);

// Exposed (and non-const) for the live-tunables registry — see Orb.hpp.
extern velocity_map<PadType, VehicleType, bool> pad_velocities;

} // namespace gdsim
