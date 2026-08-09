#include "EffectObject.hpp"
#include "Player.hpp"

namespace gdsim {

bool EffectObject::touching(Player const& player) const {
    if (Object::touching(player))
        return !player.usedEffects.contains(id);
    return false;
}

void EffectObject::collide(Player& player) const {
    player.usedEffects.insert(id);
}

} // namespace gdsim
