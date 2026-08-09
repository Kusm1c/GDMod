#include "Portals.hpp"
#include "Player.hpp"

namespace gdsim {

GravityPortal::GravityPortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields)), upsideDown(numFromString<int>(fields[1]) == 11) { triggerCat = PCAT_GRAVITY; }

void GravityPortal::collide(Player& p) const {
    EffectObject::collide(p);
    if (upsideDown != p.upsideDown) {
        p.velocity   = -p.velocity / 2;
        p.upsideDown = upsideDown;
        p.gravityPortal = true;
    }
}

} // namespace gdsim
