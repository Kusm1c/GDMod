#include "Portals.hpp"
#include "Player.hpp"

namespace gdsim {

SpeedPortal::SpeedPortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields))
{
    triggerCat = PCAT_SPEED;
    switch (numFromString<int>(fields[1])) {
        case 200:  speed = 0; break;
        case 201:  speed = 1; break;
        case 202:  speed = 2; break;
        case 203:  speed = 3; break;
        case 1334: speed = 4; break;
        default:   speed = 1; break;
    }
}

void SpeedPortal::collide(Player& p) const {
    EffectObject::collide(p);
    p.speed = speed;
}

} // namespace gdsim