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

// INVESTIGATED then REVERTED 2026-08-21 (DeCode): decompiled updateTimeMod.cpp shows
// wave/dart Y-velocity as `m_playerSpeed * m_speedMultiplier` (player_speedmults[]
// above * a separate per-tier table: 0.9/else->5.77000189, 0.7->5.980002,
// 1.1->5.870002, 1.3 or 1.6->6.000002). Initially assumed the real->gdsim scale was
// the usual x54 (confirmed all session for cube/ship velocities), giving 348.678 for
// the 1.1x tier — which matched ONE real sample deceptively well. But direct
// position-delta math (Δy/Δframe*240) on BOTH 85701165 and DeCode's wave sections
// gives the ACTUAL rate as exactly player_speeds[speed] unmodified (e.g. 387.42 for
// 1.1x, not 348.678) — and player_speedmults[s]*thatTable[s]*60 (not *54) matches
// player_speeds[s] to 6 significant figures for every tier checked. So this decompiled
// formula was never a bug: player_speeds[] already IS this exact product, just at a
// x60 reference (matching GD's 60fps design-frame m_yVelocity convention, not the x54
// used for cube/ship's world-space velocity) — gdsim's ORIGINAL plain player_speeds[]
// lookup was correct the whole time. Keeping this note so the x54 trap isn't re-hit.

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
        // PlayerObject::m_objectSnappedTo (0x888) / m_snapDistance (0x838), written
        // together by Block.cpp's checkSnapJumpToObject port on every cube floor
        // placement. objectId < 0 = m_objectSnappedTo == nullptr. Like the engine,
        // nothing clears them except a fresh Player (init/resetObject).
        Entity object;
        int    objectId = -1;       // Object::id (m_uniqueID stand-in)
        bool   objectSolid = false; // getObjectType() == Solid (a BreakableBlock is not)
        double snapDX = 0.0;        // (double)(player.x - object.x) at that call
        // Frame of the last cube floor placement (landing branch), kept for
        // Slope.cpp's "landed on a block" test; the ground floor resets it.
        int playerFrame = 0;
        // Monotonic Player::frame of that placement — lets Level::stepPlayer undo a
        // record made on the frame the cube launched (see its comment).
        int landingFrame = 0;
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
    // FOUND 2026-08-21 (DeCode, x~12300 Ball->Cube+gravity-flip cluster): the
    // one-frame-stale-gravity resync (Level.cpp's GRAVFLIP-CORR) uses
    // preFrameVelocity from BEFORE this frame's effects — when a vehicle just
    // switched 0-2 frames ago, that velocity reflects the OLD vehicle's own
    // scale/model (already transformed once by the switch's own halving logic),
    // not a clean "this vehicle's normal accumulated velocity" the formula
    // assumes. Real capture shows a SMOOTH position continuation through such a
    // cluster where gdsim's formula introduces a sharp, wrong-direction jump.
    // Default far in the past so a fresh Player never spuriously reads as
    // "just switched." Set by VehiclePortal::collide().
    long lastVehicleSwitchFrame = -1000;

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

    // GD's PlayerObject::m_isAccelerating (offset 0x952). Set by boosts that push
    // the player OUTSIDE the flight velocity band, and it makes updateJump skip
    // the flight clamp entirely until the velocity decays back into that band —
    // which is the only reason a boosted ship/UFO can exceed 432 at all.
    //
    // updateJump clears it at the top of its isFlying() branch whenever
    //     -6.4/sizeDiv < m_yVelocity < 8.0/sizeDiv
    // (sizeDiv = 0.85 mini, 1.0 big), and the clamp block a few lines later is
    // guarded by `if (!m_isAccelerating && !m_isDart)`. The bounds are the same
    // expression, so in practice: while outside the band the clamp is off, and
    // the instant the velocity re-enters it the clamp turns back on.
    //
    // Before this existed, gdsim clamped unconditionally and the UFO's upper
    // bound had been raised 432 -> 520 purely so a red pad's 518.4 could survive.
    // That constant is back at its real 432 now; this flag is what carries the
    // boost instead.
    bool isAccelerating = false;
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
    // The engine keeps these as STEP COUNTDOWNS, not per-touch flags
    // (GJBaseGameLayer::collisionCheckObjects case 0x28 sets them to 2; the end of
    // PlayerObject::update decrements them): m_stateNoAutoJump (J, +0xb74),
    // m_stateDartSlide (D, +0xb78), m_stateHitHead (H, +0xb7c). A touch therefore lasts
    // the touching step and the next one. touchingJBlock / touchingHBlock read them.
    int  stateNoAutoJump = 0;
    int  stateDartSlide  = 0;     // > 0: a wave lands on solids instead of dying
    int  stateHitHead    = 0;

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
    // Gravity step that preCollision ALREADY folded into pos.y before collisions ran
    // (see its comment). postCollision subtracts it back out of whatever correction
    // it applies, so the frame's final Y is unchanged — only what the COLLISION tests
    // see moves. 0 on frames where no pre-step was applied.
    double preAppliedGravStep = 0.0;
    bool   resyncPosition = false;

    // A block's underside clamped this frame's flight ceiling (Block.cpp's
    // ship/UFO/ball branch: pos.y snapped to the block's bottom, velocity zeroed).
    // Real GD reaches that state at the END of the frame — PlayerObject::updateJump
    // has ALREADY run `v += a*dt; y += v*dt` before checkCollisions clamps — so the
    // frame finishes with velocity exactly 0 and Y exactly on the ceiling, and only
    // the NEXT frame's gravity step lifts it off. gdsim runs collisions in the
    // middle of the frame, so postCollision must not re-apply this frame's gravity
    // step, nor its semi-implicit position resync, on top of the snap.
    // Reset every frame in preCollision.
    bool   ceilingSnapped = false;
    // A teleport (portal 747 or trigger 3022) SET pos this step: the engine's position
    // is the target, with no integration on top, so postCollision must not resync it.
    bool   teleported = false;
    // A gravity flip that the engine performs BEFORE this step's update(): a fresh press on
    // a ring already touched last step (PlayerObject::pushButton -> ringJump). Flips from
    // the collision pass (portals, pads, newly touched rings) happen AFTER the move.
    bool   flipBeforeUpdate = false;

    Player();

    void preCollision(bool input);
    void postCollision();

    Entity unrotatedHitbox() const;
    // The player's VISUAL rotation (CCNode rotation, degrees, GD convention: clockwise
    // positive) and PlayerObject+0x9f8, the position at the end of the previous step.
    // GJBaseGameLayer::update calls PlayerObject::updateRotation AFTER the collision pass
    // and then stores the position; the player's OBB (getOrientedBox) uses this rotation,
    // and the engine tests ORIENTED non-solid objects (rotation % 90 != 0: hazards,
    // portals, orbs, pads) against that OBB. See Level::updateVisualRotation.
    float  visRot = 0.f;
    Vec2D  rotLastPos{0.f, 0.f};
    Entity orientedHitbox() const;
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
