#pragma once
#undef small
#include "util.hpp"
#include "Vehicle.hpp"
#include "Slope.hpp"
#include <vector>
#include <functional>
#include <optional>

namespace gdsim {

inline double player_speeds[5] = {
    251.16007972276924,
    311.580093712804,
    387.42014039710523,
    468.0001388338566,
    576.00020058307177
};

inline float player_speedmults[5] = {0.7f, 0.9f, 1.1f, 1.3f, 1.6f};

double roundVel(double velocity, bool upsideDown);

struct Object;
class Level;

struct Player : public Entity {
    Vehicle vehicle;
    Level* level;

    double timeElapsed;
    double acceleration;
    double velocity;

    cow_set<int> usedEffects;
    std::vector<Slope const*> potentialSlopes;
    std::vector<std::function<void(Player&)>> actions;

    struct {
        std::optional<Slope> slope;
        double elapsed;
        bool snapDown;
        // Ported from PlayerObject::collidedWithSlopeInternal (real decompile). The
        // SIGNED slope rotation as of the frame we last gripped a slope (includes the
        // uphill/direction/flip multiplier, not just the raw geometric angle) — used to
        // compute the "am I still close enough to count as on the slope" exit tolerance
        // (playerRadOnPrevSlope) with the PREVIOUS grip's angle, not the new candidate's.
        float rotation = 0.f;
    } slopeData;

    struct {
        Entity object;
        int playerFrame = 0;
        // Stair-snap reference, cached so it travels with the state instead of
        // requiring a gameStates lookup (which breaks under solver state
        // injection). landingFrame is the monotonic Player::frame at the last
        // cube landing; refX is that landing state's final pos.x — exactly what
        // getState(playerFrame).nextPlayer()->pos.x used to return.
        int   landingFrame = 0;
        float refX = 0.f;
    } snapData;

    float ceiling;
    float floor;
    float dt;

    unsigned int coyoteFrames;
    int speed;
    // GD applies a speed-portal change to the X-position integrator one frame
    // later than to everything else (wave Y-velocity, jump heights…). xSpeed is
    // `speed` lagged by one frame, used ONLY for the pos.x advance, so the sim
    // doesn't pull ~Δspeed/240 units ahead in X at each speed portal (which would
    // knock later gamemode-portal crossings onto the wrong input frame).
    int xSpeed;
    int pendingXSpeed;
    int frame;
    int robotHoldFrames; // frames since robot jump (Robot vehicle only)

    bool dead;
    const char* deathCause = nullptr; // set wherever dead is raised (debug)
    int   deathObjType = 0;           // typeId of the object that killed us (0 = none/bounds)
    Vec2D deathObjPos{};              // its position — so diagnostics can name the killer
    bool grounded;
    bool velocityOverride;
    bool button, input;
    bool buffer;
    bool vehicleBuffer;
    bool upsideDown;
    bool small;
    bool gravityPortal;
    bool roundVelocity;
    bool dual;            // dual mode active this frame (set/cleared by DualPortal)
    bool isMirror;        // true for the dual mirror player (reads gameStates2 history)

    // Special letter blocks (SpecialBlock.hpp / GD Creator School gameplay-objects
    // #3): set every frame the player's hitbox overlaps the matching modifier
    // object, reset at the top of the NEXT preCollision. Consulted by whichever
    // mechanic each one modifies (Vehicle.cpp's jump buffer, Orb.cpp's dash,
    // Block.cpp's underside/side death checks).
    bool touchingJBlock = false;  // J (1813): suppress buffered auto-jump on landing
    bool touchingSBlock = false;  // S (1829): neutralize a dash orb
    bool touchingHBlock = false;  // H (1859): survive a block's underside/side touch

    // Dash orb (1704): while held, follow the orb's angle at constant velocity, no
    // gravity. dashTan = tan(angle); velocity = player_speeds[speed]*dashTan each
    // frame so the player tracks the correct LINE regardless of X speed. Approx:
    // GD's dash X-speedup and line-snap aren't modelled — needs capture to tune.
    bool   dashing = false;
    double dashTan = 0.0;

    // Real GD applies a same-frame velocity SET (ground jump, orb, pad) to Y
    // position on the very frame it happens; our pipeline integrates Y in
    // preCollision using last frame's velocity, before this frame's impulse
    // runs — leaving the impulse's own frame exactly one frame "behind" (a
    // constant ~2-3u lag that persists for the rest of the section, since
    // later per-frame deltas already match). preFrameVelocity snapshots the
    // velocity preCollision integrated Y with; resyncPosition, when set by an
    // impulse (jump/orb/pad), tells postCollision to retroactively correct
    // THIS frame's Y once the final post-impulse velocity is known.
    double preFrameVelocity = 0.0;
    bool   resyncPosition = false;

    Player();

    void preCollision(bool input);
    void postCollision();

    Entity unrotatedHitbox() const;
    Entity innerHitbox() const;
    Entity blockDeathHitbox() const;

    Player const& prevPlayer() const;
    Player const* nextPlayer() const;

    template <typename T>
    T grav(T value) const { return upsideDown ? -value : value; }
    inline float gravBottom(Entity const& e) const { return upsideDown ? -e.getTop()    : e.getBottom(); }
    inline float gravTop   (Entity const& e) const { return upsideDown ? -e.getBottom() : e.getTop();    }
    inline float gravFloor()   const { return upsideDown ? -ceiling : floor;   }
    inline float gravCeiling() const { return upsideDown ? -floor   : ceiling; }

    void setVelocity(double v, bool override_flag = false);
};

} // namespace gdsim
