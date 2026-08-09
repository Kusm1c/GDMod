#pragma once
#include "EffectObject.hpp"

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

enum class VehicleType;
PadType padTypeFromId(int id);
double  padVelocityValue(PadType t, VehicleType v, bool mini, int speed);

} // namespace gdsim
