#include "Portals.hpp"
#include "Player.hpp"

namespace gdsim {

DualPortal::DualPortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields)), startDual(numFromString<int>(fields[1]) == 286) { triggerCat = PCAT_DUAL; }

void DualPortal::collide(Player& p) const {
    EffectObject::collide(p);
    p.dual = startDual;
}

} // namespace gdsim
