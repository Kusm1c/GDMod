#pragma once
#include "Block.hpp"

namespace gdsim {

struct Slope : public Block {
    int orientation;
    Slope(Vec2D size, std::unordered_map<int, std::string>&& fields);
    bool touching(Player const&) const override;
    void collide(Player&) const override;
    void calc(Player& p) const;
    int gravOrient(Player const& p) const;
    double angle() const;
    bool isFacingUp() const;
    bool isNewSlopeTransition(Player const& p) const;
    virtual double expectedY(Player const& p) const;
};

struct SlopeHazard : public Slope {
    using Slope::Slope;
    void collide(Player&) const override;
    bool touching(Player const&) const override;
    double expectedY(Player const& p) const override;
};

} // namespace gdsim
