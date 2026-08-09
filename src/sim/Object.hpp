#pragma once
#include "util.hpp"
#include <unordered_map>
#include <cstring>
#include <optional>

namespace gdsim {

struct ObjectContainer;
struct Player;

struct Object : public Entity {
    int id;     // sequential instance index (used by EffectObject for usedEffects uniqueness)
    int typeId; // GD object type ID (field 1 in level string, used for display categorisation)
    int prio;
    int triggerCat = 0; // PortalCat for calibratable trigger reach (0 = not a portal)

    Object() = default;
    Object(Vec2D size, std::unordered_map<int, std::string>&& fields);

    virtual bool touching(Player const&) const;
    virtual void collide(Player&) const;

    static std::optional<ObjectContainer> create(std::unordered_map<int, std::string>&& ob);
};

struct ObjectContainer {
    char buffer[sizeof(Object) + 0x8] = {0};

    ObjectContainer(ObjectContainer& cont)       { memcpy(buffer, cont.buffer, sizeof(buffer)); }
    ObjectContainer(ObjectContainer const& cont) { memcpy(buffer, cont.buffer, sizeof(buffer)); }

    template <class T>
    ObjectContainer(T&& obj) {
        static_assert(sizeof(T) <= sizeof(buffer));
        memcpy(buffer, (void*)&obj, sizeof(T));
    }
    Object const* operator->() const { return reinterpret_cast<Object const*>(buffer); }
    Object*       operator->()       { return reinterpret_cast<Object*>(buffer); }
};

} // namespace gdsim
