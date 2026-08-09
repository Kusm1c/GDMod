#pragma once
#include "Object.hpp"

namespace gdsim {

struct Hazard : public Object {
    Hazard(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
};

struct Sawblade : public Hazard {
    using Hazard::Hazard;
    bool touching(Player const&) const override;
    // Sawblade uses a precise circle touching-test, so it kills unconditionally on
    // contact — it must NOT inherit Hazard's box graze leniency (that would forgive
    // real circle hits).
    void collide(Player&) const override;
};

} // namespace gdsim
