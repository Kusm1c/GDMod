#pragma once
#include "Object.hpp"

namespace gdsim {

struct Block : public Object {
    Block(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
};

struct BreakableBlock : public Block {
    using Block::Block;
    void collide(Player&) const override;
    bool touching(Player const& p) const override;
};

} // namespace gdsim
