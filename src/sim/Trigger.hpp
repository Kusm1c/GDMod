#pragma once
#include "util.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>

namespace gdsim {

// GD trigger engine (move / rotate / toggle …). gdsim is otherwise a static-level
// simulator; this animates collision objects so trigger-driven obstacles are in
// the right place at each frame.
//
// KEY PROPERTY exploited for the solver: the player's X advances at a fixed,
// input-independent rate, so the frame at which any X-activated trigger fires —
// and therefore every moved object's position at frame F — is a deterministic
// function of F, shared across all search branches and stable under the solver's
// rollback/inject. So positions are precomputed/cached per frame, not per branch.

enum class TriggerKind { Move, Rotate, Toggle, Alpha, Spawn, Follow, Teleport };

struct Trigger {
    TriggerKind kind = TriggerKind::Move;
    float    x = 0.f;             // activation X (the trigger object's position.x)
    float    y = 0.f;             // activation Y (field 3) — only used by touchTriggered
    int      targetGroup = 0;     // group to affect / spawn (field 51)

    // Spawn trigger (1268): when it fires, every spawn-triggered trigger that is a
    // MEMBER of targetGroup (has it in its own groups, field 57) fires at this
    // trigger's fireFrame + spawnDelay. ownGroups = a trigger's own membership, so
    // a spawn can find its targets.
    std::vector<int> ownGroups;   // field 57 (this trigger's own groups)
    float    spawnDelay = 0.f;    // field 63 (seconds)

    // Move (kind == Move)
    float    moveX = 0.f, moveY = 0.f;   // total offset (units), fields 28/29
    // "Lock to Player" / "Lock to Camera". Field ids read out of
    // EffectGameObject::customObjectSetup (the slot index in its parameter table
    // IS the level-string field id): 58/59 player X/Y, 141/142 camera X/Y,
    // 143/144 the multiplier on each axis.
    //
    // These do not animate: GJEffectManager::prepareMoveActions REPLACES that
    // axis's eased delta with `thisStepPlayerDelta * mod` for as long as the
    // command lives (duration seconds, or forever when duration is -1). Only the
    // X form is modelled — the player's X advance is input-independent so it can
    // be precomputed per frame, while Y depends on the path being searched and
    // would make an object's position a function of the branch, which the whole
    // trigger engine is built on not being. lockToPlayerY/camera are parsed so
    // they can be reported, never applied.
    bool     lockToPlayerX = false;  // field 58
    bool     lockToPlayerY = false;  // field 59
    bool     lockToCameraX = false;  // field 141
    bool     lockToCameraY = false;  // field 142
    float    moveModX = 1.f;         // field 143
    float    moveModY = 1.f;         // field 144
    // "Silent" (field 544, NOT 36 — an earlier note here read the id off an
    // editor screenshot and got it wrong; every one of ALLOY's 258 move triggers
    // carries field 36 while their motion is plainly interpolated).
    // GJBaseGameLayer::triggerMoveCommand never builds a move ACTION for a silent
    // trigger: it adds the whole offset to the group's objects on the spot and
    // then writes their m_lastPosition to match, so duration and easing are both
    // ignored and the move does not register as motion (a player riding the
    // object is not carried). gdsim models the instant part.
    bool     silent = false;         // field 544
    // Rotate (kind == Rotate)
    float    degrees = 0.f;       // field 68
    int      centerGroup = 0;     // field 71 (rotate about this group's centre)
    // Follow (kind == Follow): target group copies followGroup's movement, scaled.
    int      followGroup = 0;     // field 71 (group whose motion is copied)
    float    followXMod = 1.f;    // field 72 (X follow multiplier)
    float    followYMod = 1.f;    // field 73 (Y follow multiplier)
    // Toggle/Alpha
    bool     toggleOn = true;     // field 56 (activate group = show/enable)
    float    alpha = 1.f;         // field 35

    // Teleport (3022, kind == Teleport): GJBaseGameLayer::teleportPlayer (0x14020fdb0) moves
    // the player onto an object of targetGroup. Field ids from
    // TeleportPortalObject::customObjectSetup (string slot offset / 0x20).
    bool     tpSaveOffset = false;   // field 351: keep the player's offset from the trigger
    bool     tpIgnoreX    = false;   // field 352
    bool     tpIgnoreY    = false;   // field 353
    int      tpGravity    = 0;       // field 354: 1 normal, 2 flipped, 3 toggle
    float    duration = 0.f;      // field 10 (seconds)
    int      easing  = 0;         // field 30 (easing type)
    float    easeRate = 2.f;      // field 85 (easing rate)

    bool     touchTriggered = false; // field 11 (fires on player touch, not X)
    bool     spawnTriggered = false; // field 62 (fired by a spawn trigger, not X)

    int      fireFrame = -1;      // precomputed frame this trigger activates (X mode)
    // Positioned BEHIND the player's spawn X. GD builds the level's initial state
    // with such triggers already applied (the same fast-forward a Start Pos does) —
    // the player never crosses them, so they must NOT animate from frame 1. Held at
    // their END value from frame 0.
    bool     preApplied = false;

    // FOUND 2026-09-10 (level 123617195 "ALLOY", via the move-trigger analyser —
    // test/moveanalyze.cpp): a Spawn trigger's targetGroup can contain another
    // Spawn trigger whose OWN targetGroup leads back to the first, forming a cycle
    // — a standard GD "pendulum"/"loop" construction (confirmed on ALLOY: spawn A
    // -> group 63 -> spawn B (delay 2.043s) -> group 64 -> spawn C (delay 2.043s)
    // -> group 63 -> back to B). Real GD keeps firing every member of the cycle
    // once per period forever; the propagation walk below (buildTriggerTimeline)
    // only ever recorded the FIRST arrival ("earliest activation wins"), so a
    // repeating Move trigger applied its offset exactly once and then held —
    // measured as gdsim moving a group 12 units while the real object moved 47-57
    // (several accumulated firings; Move offsets are relative, so each firing
    // over real GD adds onto wherever the object already is).
    //
    // 0 = fires once, as before. >0 = after the first fire, fires again every
    // repeatPeriodFrames forever (bounded only by the level's own length via
    // poseMovable's fires-so-far count) — the accumulated period of the spawn
    // cycle this trigger sits on, in 240Hz frames.
    int      repeatPeriodFrames = 0;
};

// Static (start-of-level) record for a movable collision object, kept so its
// per-frame position can be recomputed from scratch each frame as start + offset.
struct MovableObject {
    int   objIndex;   // index into Level::movable (the live, mutated ObjectContainer)
    Vec2D startPos;   // original position
    float startRot;   // original rotation
};

// Eased progress for GD easing `type` with `rate` (field 85). t,result ∈ [0,1].
float easeValue(float t, int type, float rate);

// Parsing helpers (defined in Trigger.cpp).
bool isTriggerId(int id);
std::vector<int> parseGroups(const std::string& field57);
std::optional<Trigger> parseTrigger(int id, const std::unordered_map<int, std::string>& fields);

} // namespace gdsim
