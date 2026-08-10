#pragma once
#include "Object.hpp"
#include "Player.hpp"
#include "Trigger.hpp"
#include <vector>
#include <unordered_map>

namespace gdsim {

std::string normalizeLevelString(std::string const& lvlString);

class Level {
    void initLevelSettings(std::string const& lvlSettings, Player& player);
    // Advance one player by a single frame (collisions, slopes, snap caching).
    // Shared by the primary player and the dual mirror player.
    Player stepPlayer(Player p, bool pressed, float dt);
public:
    std::vector<Player> gameStates;
    // Mirror (second) player history. Stays EMPTY (zero overhead) until the
    // level's first dual portal is hit; from then on it's kept in lockstep with
    // gameStates. `dualEverActive` latches that transition.
    std::vector<Player> gameStates2;
    bool dualEverActive = false;
    bool mirrorDead = false;       // set if the dual mirror ever died (validation signal)
    // The dual mirror is simulated only when this is on. OFF during pathfinding
    // (so the search behaves exactly as a single player and isn't dead-ended by an
    // approximate mirror); turn ON for a validation pass over a found solution.
    bool simulateMirror = false;
    size_t objectCount = 0;
    std::vector<std::vector<ObjectContainer>> sections;
    float length = 0.0f;

    // ── Trigger engine (moving objects) ──────────────────────────────────────
    // Collision objects in groups targeted by move/rotate triggers live here with
    // live (mutated) positions, checked every frame instead of via `sections`
    // (which index by START x). `triggers` are parsed, X-sorted, and given a
    // precomputed fire-frame; movable positions are recomputed per frame (cached).
    std::vector<ObjectContainer> movable;
    std::vector<MovableObject>   movableMeta;            // start pos/rot per movable obj
    std::unordered_map<int, std::vector<int>> groupToMovable; // group → indices into movable
    std::vector<Trigger> triggers;                      // X-sorted
    bool hasTriggers = false;
    // movable index → indices of the triggers that animate it (X-sorted, so the
    // last-fired toggle wins). Lets each object be posed independently from only
    // its own triggers — a pure function of frame, so it survives solver rollback.
    std::vector<std::vector<int>> movableToTriggers;
    // (startX, movable index) sorted by start X, so the per-step scan can binary-
    // search the window around the player and pose ONLY nearby objects instead of
    // every movable object every frame (posing all of them was the hang/slowdown).
    std::vector<std::pair<float,int>> movableStartSortedX;
    // (x, index into `triggers`) for touchTriggered ones only, X-sorted (a subrange
    // of the already-X-sorted `triggers`, so no separate sort needed). Used by
    // stepPlayer to detect the player's hitbox reaching a touch trigger's position
    // and fire it dynamically — see modelTouchTriggers.
    std::vector<std::pair<float,int>> touchTriggerSortedX;
    // Touch triggers fire when the PLAYER'S HITBOX overlaps the (invisible) trigger
    // object's own position, not from a fixed X(frame) timeline — genuinely
    // path-dependent (a solver branch that never reaches a touch trigger's Y never
    // fires it). Modeling this during the solver's beam search would make trigger
    // state branch-dependent, breaking the "pure function of frame, rollback-safe"
    // invariant every other trigger relies on (see buildTriggerTimeline's comment) —
    // so this defaults OFF and the solver never touches it. It exists for FORWARD-
    // ONLY replay validation (gdrcheck, verifyZeroMargin, real gameplay/divergence),
    // where a Level is only ever stepped forward once, never rolled back, so mutating
    // Trigger::fireFrame in place as the player's real path touches triggers is safe.
    // Default hitbox half-size is a placeholder (GD's own touch-trigger collision
    // size isn't in this project's Object.cpp table — triggers aren't Objects) —
    // calibrate against a real capture before trusting this for anything but the
    // 2026-08-09 macro-demonlist batch's aggregate pass/fail rate.
    bool modelTouchTriggers = false;
    static constexpr float kTouchTriggerHalfSize = 10.f;
    // Largest distance any single object's centre can travel in X from its start
    // (sum of its move triggers' |moveX|*moveScale), used to widen the start-X
    // window so an object that moved INTO the player's reach is never missed.
    float maxMoveExtent = 0.f;
    // Per-movable-object X travel extent (same units as maxMoveExtent), indexed by
    // movable index. Lets the per-frame scan cheaply skip an object whose OWN reach
    // can't put it near the player, so one far-traveling object no longer forces the
    // whole level to be posed every frame (the trigger-heavy-level solver killer).
    std::vector<float> movableExtent;

    void buildTriggerTimeline();      // Phase 2: precompute trigger fire-frames from X(frame)
    // Phase 3: pose movable object `idx` for frame `f` from its own triggers only.
    // Pure function of f (no shared/mutated state) → identical across search
    // branches and correct after rollback.
    void poseMovable(int idx, int f, Vec2D& pos, float& rot, bool& active) const;
    // Summed eased MOVE offset (only) of a representative object in `group` at
    // frame `f`. Used by Follow triggers to copy a group's motion. Move-only, so it
    // never recurses through follow triggers.
    Vec2D moveOffsetOfGroupAt(int group, int f) const;

    // Safety margin (world units) added to the player's hitbox for HAZARD checks
    // only (not blocks). GD's effective hazard hitboxes run ~½–1 unit larger than
    // this sim's, so a path that grazes a spike by <1u "survives" here but dies in
    // the real game. The solver sets this so its paths keep real clearance; the
    // divergence detector / playback keep it 0 for an accurate model. 0 = off.
    float hazardInflate = 0.0f;

    // EXTRA hazard clearance applied ONLY in flight modes (ship/ufo/wave/swing),
    // where the sim is least reliable. Set by the solver; 0 for playback/divergence.
    float flightHazardInflate = 0.0f;

    static constexpr uint32_t sectionSize = 100;
    bool debug = false;

    Level(std::string const& lvlString);

    Player& runFrame(bool pressed, float dt = 1.f/240.f);
    void rollback(int frame);
    int currentFrame() const;
    Player const& getState(int frame) const;
    Player& latestState();

    // Largest symmetric margin (capped at maxProbe, world units) the player's
    // hitbox can be inflated on every side before it would touch a hazard. A
    // clearance metric the solver uses to prefer flight paths that thread the
    // CENTRE of spike gaps (replay-robust) instead of grazing them — a grazing
    // path "solves" in the sim but dies in the real game once playback drifts a
    // frame or two. 0 = already overlapping; maxProbe = wide open.
    float hazardClearance(const Player& p, float maxProbe = 6.f) const;

    // Cheap one-shot safety test used by the beam: true if the hitbox inflated by
    // `margin` on every side touches no hazard (i.e. clearance >= margin).
    bool clearsHazards(const Player& p, float margin) const;
};

} // namespace gdsim
