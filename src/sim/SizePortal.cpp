#include "Portals.hpp"
#include "Player.hpp"
#include "Calib.hpp"

namespace gdsim {

SizePortal::SizePortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields)), small(numFromString<int>(fields[1]) == 101) { triggerCat = PCAT_SIZE; }

void SizePortal::collide(Player& p) const {
    EffectObject::collide(p);
    p.small = small;
}

} // namespace gdsim
