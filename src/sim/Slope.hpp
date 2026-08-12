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
    // Shared triangle-vs-box overlap test (the diagonal line-crossing check
    // touching() uses), generalised to any query box instead of only the
    // player's own hitbox — lets a caller test a DIFFERENT box (e.g. the
    // wave's smaller innerHitbox for its death check) against the slope's
    // real triangular shape instead of its rectangular bounding box.
    bool triangleOverlaps(Entity const& box) const;
    // Circle-vs-diagonal-line overlap: per GD Creator School's documented model,
    // the WAVE's slope collision is a circle test (not a box-corner test) — a
    // circle centred at `center` with the given `radius` against the slope's
    // real diagonal line, respecting facing direction (isFacingUp). Used by the
    // wave's slope-death check, where a rectangular corner grazing the line by a
    // sub-radius amount should NOT count as touching (validated against a real,
    // human-verified macro clear — see Slope.cpp's 2026-08-12 fix note).
    bool circleOverlaps(Vec2D center, float radius) const;
};

struct SlopeHazard : public Slope {
    using Slope::Slope;
    void collide(Player&) const override;
    bool touching(Player const&) const override;
    double expectedY(Player const& p) const override;
};

} // namespace gdsim
