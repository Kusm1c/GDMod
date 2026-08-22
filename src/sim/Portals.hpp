#pragma once
#undef small
#include "EffectObject.hpp"
#include "Vehicle.hpp"
#include "Calib.hpp"

namespace gdsim {

struct VehiclePortal : public EffectObject {
    VehicleType type;
    VehiclePortal(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
    bool touching(Player const&) const override;
};

struct GravityPortal : public EffectObject {
    bool upsideDown;
    GravityPortal(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
    bool touching(Player const&) const override;
};

struct SizePortal : public EffectObject {
    bool small;
    SizePortal(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
};

struct SpeedPortal : public EffectObject {
    int speed;
    SpeedPortal(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
};

// Dual portal: id 286 starts dual (Level spawns the mirrored second player),
// id 287 ends it (merge back to one). Here we just flip the player's dual flag;
// the mirror player is created and stepped by Level::runFrame.
struct DualPortal : public EffectObject {
    bool startDual;
    DualPortal(Vec2D size, std::unordered_map<int, std::string>&& fields);
    void collide(Player&) const override;
};

// Teleport portal (id 747): instantly moves the player vertically. Field 54 is the
// distance — the player exits at portalY + distance (validated against recorded
// real-game teleports). Was previously MISLABELLED as a Spider vehicle portal,
// which desynced every level using one.
struct TeleportPortal : public EffectObject {
    float distance;
    TeleportPortal(Vec2D size, std::unordered_map<int, std::string>&& fields);
    // Unlike state-change portals, a teleport MOVES the player, so firing a few
    // frames early (on first hitbox overlap) lands it at the wrong spot and skips
    // adjacent portals. Fire only once the player's centre crosses the portal's
    // centre X, matching the real game far more closely.
    bool touching(Player const&) const override;
    void collide(Player&) const override;
};

} // namespace gdsim
