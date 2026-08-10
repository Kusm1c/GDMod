#include "Portals.hpp"
#include "Player.hpp"
#include <cstdio>
#include <cstdlib>

namespace gdsim {

GravityPortal::GravityPortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields)), upsideDown(numFromString<int>(fields[1]) == 11) { triggerCat = PCAT_GRAVITY; }

void GravityPortal::collide(Player& p) const {
    EffectObject::collide(p);
    if (upsideDown != p.upsideDown) {
        if (getenv("GDSIM_ORBTOUCH_DEBUG"))
            std::fprintf(stderr, "GRAVPORTAL-TOUCH f=%d typeId=%d pos=(%.2f,%.2f) playerXY=(%.2f,%.2f) velBefore=%.3f\n",
                         p.frame, typeId, pos.x, pos.y, p.pos.x, p.pos.y, p.velocity);
        p.velocity   = -p.velocity / 2;
        p.upsideDown = upsideDown;
        p.gravityPortal = true;
    }
}

} // namespace gdsim
