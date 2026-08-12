#pragma once
#include "Object.hpp"
#include "EffectObject.hpp"

namespace gdsim {

// "Special letter blocks" — GD Creator School's gameplay-objects guide, section 3
// (gdcreatorschool.com/docs/guides/gameplay-1/gameplay-objects/#3-special-letter-blocks):
// invisible-in-game modifier objects that change how a nearby gamemode interacts
// with solid geometry / mechanics. D block (id 1755, wave-safe solid) predates this
// file and is handled separately in Object.cpp/Block.cpp — this covers the other
// five documented types:
//   J (1813) Stop Jump Buffer     — disable buffered auto-jump when landing on a block
//   S (1829) Stop Dash            — neutralize a dash orb while touching
//   H (1859) Allow Head Collision — survive touching a block's underside/side
//   F (2866) Gravity Flip         — flip gravity on touch (one-shot, like an orb)
//   Force (2069 square / 3645 circle) — push the player while touching
// IDs cross-checked 2026-08-11 against the real decompiled binary's player/object
// collision-dispatch switch (case 0x28, C:\Users\Kusmic\decompGD) AND FlowVix's
// gd-info-explorer object database — both independently agree, including the
// already-known-correct D block appearing in the same set. High confidence on the
// IDs themselves; the gameplay MODELLING below is a best-effort port of the prose
// spec onto already-existing gdsim mechanics, not a decompiled formula — no per-
// object real capture exists yet for any of these five (unlike D, which has one).
//
// All five get prio=3 (a dedicated bucket Level::stepPlayer gathers/resolves BEFORE
// effects/blocks/hazards), not prio=0's generic "effects" bucket: J/S/H/F/Force must
// be resolved before the SAME frame's jump/dash/block-death/gravity logic reads
// them, and the effects bucket's only ordering guarantee is left-to-right BY
// POSITION — two objects placed at the same X (e.g. an S block sitting exactly on
// its dash orb) aren't guaranteed to resolve in the order that would require.

// J/S/H are CONTINUOUS: real GD's dispatch case just writes flag bits on the player
// struct every frame contact holds, not a one-shot "used" trigger like an orb/pad —
// so, unlike EffectObject, these use plain per-frame Object::touching (re-evaluated
// every frame, no usedEffects gating). The flags they set live on Player
// (touchingJBlock/S/H) and are reset every frame in Player::preCollision.
struct SpecialBlock : public Object {
    SpecialBlock(Vec2D size, std::unordered_map<int, std::string>&& fields)
        : Object(size, std::move(fields)) { prio = 3; }
    void collide(Player&) const override;
};

// F block: a gravity flip must fire ONCE on entry, not every frame while still
// overlapping (that would flip-flop every frame) — reuses EffectObject's
// usedEffects one-shot gating, the same model as every other one-shot touch
// effect (orbs/portals).
struct GravityFlipBlock : public EffectObject {
    GravityFlipBlock(Vec2D size, std::unordered_map<int, std::string>&& fields)
        : EffectObject(size, std::move(fields)) { prio = 3; }
    void collide(Player&) const override;
};

// Force blocks (square/circle): continuous push while touching, like a wind zone —
// plain Object touching (re-evaluated every frame), not one-shot.
struct ForceBlock : public Object {
    ForceBlock(Vec2D size, std::unordered_map<int, std::string>&& fields)
        : Object(size, std::move(fields)) { prio = 3; }
    void collide(Player&) const override;
};

} // namespace gdsim
