#pragma once
#include "Object.hpp"

namespace gdsim {

struct EffectObject : public Object {
    using Object::Object;
    bool touching(Player const&) const override;
    void collide(Player&) const override;
};

} // namespace gdsim
