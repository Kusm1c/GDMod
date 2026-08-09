#include "Block.hpp"
#include "Player.hpp"

namespace gdsim {

bool BreakableBlock::touching(Player const& p) const {
    if (Block::touching(p))
        return !p.usedEffects.contains(id);
    return false;
}

void BreakableBlock::collide(Player& p) const {
    p.usedEffects.insert(id);
    Block::collide(p);
}

} // namespace gdsim
