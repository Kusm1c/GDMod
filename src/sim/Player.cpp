#include "Player.hpp"
#include "Level.hpp"
#include "Slope.hpp"
#include "Calib.hpp"
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace gdsim {

// The inner "solid" hitbox (block death + flight ceiling death) is a fixed 9×9 in
// GD at normal scale, and — like every player hitbox — it shrinks by the mini
// factor (0.6). It was hardcoded 9×9 regardless of size, so a MINI wave (visual
// 6×6) was killed by blocks using a ±4.5u box instead of ±2.7u; combined with a
// ~1.3u residual entry offset that false-killed the mini-wave on 38564711 (f1431,
// real survives). Scaling by mini matches GD and clears it.
Entity Player::innerHitbox() const { return {pos, Vec2D{9, 9} * (small ? 0.6f : 1.0f), 0}; }

// Death hitbox for overlapping a SOLID block (Block::collide). Smaller than the
// 9×9 flight-ceiling innerHitbox: GD lets a cube clip a block CORNER by up to ~1u
// while sliding past it (the famous corner-clip leniency), so a 9×9 killed paths
// the real game survives. Calibrated to 7×7 against verified-correct physics — the
// pink-orb bounce level (kept clean in gd-sim tests) only becomes solvable at ≤7,
// its trajectory is confirmed <0.05u from real (truth 9437278), its orb/pad
// velocities match the real-engine capture, and 7 introduces zero false-deaths
// across 8 recorded real runs. 7 is the LARGEST box that still clears the level's
// tightest platform, so it stays maximally strict (no false block pass-throughs).
Entity Player::blockDeathHitbox() const {
    // GD's WAVE death hitbox is much smaller than the cube's. Using the cube's 7×7
    // killed the mini-wave at spawn on levels where it dives from the corridor
    // ceiling (block bottom): a ±2.1u box grazes the block by a sub-unit hair at the
    // first frame while the real wave (tiny hitbox) threads it. Give the wave a
    // smaller box. (Env override for offline tuning; falls back to the constant.)
    float sc = small ? 0.6f : 1.0f;
    Vec2D c = pos;
    Vec2D sz{7.f * sc, 7.f * sc};

    // Solver-time robustness margin against SOLID BLOCKS in flight modes. gdsim's
    // ship accel is exact, but ~6-7u of click-phase drift accumulates over a long
    // flight section, with the real ship consistently on the grav-up (ceiling) side.
    // Extend the death-box toward that ceiling side ONLY (via grav(), so it flips
    // correctly for upside-down ships) so the solver leaves clearance under block
    // ceilings; the floor/landing side is untouched (no false death when a ship
    // grazes a platform below it). Zero everywhere except during solveLevel().
    if (g_solveShipBlockClearance > 0.f && level &&
        (vehicle.type == VehicleType::Ship || vehicle.type == VehicleType::Ufo ||
         vehicle.type == VehicleType::Swing)) {
        float m = g_solveShipBlockClearance;           // world units, not size-scaled
        sz.y += m;                                     // taller box …
        c.y  += (float)grav(m * 0.5);                  // … all added on the ceiling side
    }

    // Solver-time robustness margin against SOLID BLOCKS in GROUNDED modes. The
    // calibrated-small death box (4.2 wide for a mini) can slip past a block
    // inside-corner that the real full hitbox clips, so the solver threads a path
    // that dies on watch (mini Robot on 13711278 f1403/x=1468). Widen the box on the
    // grav-up (top) side and BOTH horizontal sides — but never the bottom, so a
    // normal platform landing (death box sitting just above the block top) can't
    // false-trigger. Zero everywhere except during solveLevel().
    if (g_solveRobotBlockClearance > 0.f && level &&
        (vehicle.type == VehicleType::Cube || vehicle.type == VehicleType::Robot ||
         vehicle.type == VehicleType::Spider)) {
        float m = g_solveRobotBlockClearance;          // world units, not size-scaled
        sz.x += 2.f * m;                               // wider (both sides, symmetric)
        sz.y += m;                                     // taller on the grav-up side only
        c.y  += (float)grav(m * 0.5);
    }
    return {c, sz, 0};
}
Entity Player::unrotatedHitbox() const { return {pos, size, 0}; }

void Player::setVelocity(double v, bool override_flag) {
    velocityOverride = override_flag;
    velocity = v * (small ? 0.8 : 1);
    if (v != 0) grounded = false;
}

// The mirror player keeps its own history in Level::gameStates2; route lookups
// there so its physics references its own past frames, not the primary's.
Player const& Player::prevPlayer() const {
    // Defensive: a mirror player whose gameStates2 was never populated (the beam
    // search runs single-player, simulateMirror=false) has an EMPTY history vector.
    // The old h[0]/h.back() then dereferenced an empty vector's null data pointer
    // → EXCEPTION_ACCESS_VIOLATION reading 0x0. Fall back to *this (prev == current)
    // instead of crashing; the physics is degraded only in that already-broken state.
    if (!level) return *this;
    auto& h = isMirror ? level->gameStates2 : level->gameStates;
    if (h.empty()) return *this;
    int f = frame - 1;
    if (f <= 0) return h[0];
    if ((int)h.size() < f) return h.back();
    return h[f - 1];
}
Player const* Player::nextPlayer() const {
    if (!level || level->currentFrame() <= frame) return nullptr;
    auto& h = isMirror ? level->gameStates2 : level->gameStates;
    if (h.empty()) return nullptr;
    int f = frame + 1;
    if ((int)h.size() < f) return &h.back();
    return &h[f - 1];
}

double roundVel(double velocity, bool upsideDown) {
    double nVel = velocity / 54.0 * (upsideDown * 2 - 1);
    double floored = (int)nVel;
    if (nVel != floored)
        nVel = (double)std::round((nVel - floored) * 1000.0) / 1000.0 + floored;
    return nVel * 54.0 * (upsideDown * 2 - 1);
}

void Player::preCollision(bool pressed) {
    // Speed used for the X advance: 0 = current speed (immediate), 1 = lagged one
    // frame (GD applies a speed change to X later than elsewhere). Calibratable.
    const int xs = (g_calib.speedXLagFrames <= 0) ? (int)speed : (int)xSpeed;
    pos.x += (float)(player_speeds[xs] * dt);

    frame++;
    timeElapsed += dt;
    grounded = false;
    velocityOverride = false;
    gravityPortal = false;
    roundVelocity = true;
    touchingJBlock = false;
    touchingSBlock = false;
    touchingHBlock = false;

    if (button != pressed) {
        button = pressed;
        input  = button;
        buffer = button;
        if (pressed && pos.x == 0.f && frame == 2)
            logDebug("preCollision: pressed at spawn -> button=" + std::to_string(button)
                     + " input=" + std::to_string(input) + " buffer=" + std::to_string(buffer));
    }

    // Wave velocity is an INSTANT pure function of the current input (no accel),
    // and GD updates it *before* integrating position — the wave reverses on the
    // very frame you click. Our generic pipeline only sets velocity later, in
    // postCollision (clamp), so the wave's Y lagged input by one frame and locked
    // in a ~2-frame (~8u) offset at the first reversal — larger than the whole 6u
    // mini-wave hitbox, so solved wave paths grazed spikes and died on replay.
    // Re-sync from THIS frame's input before the Y integration below. No effect on
    // other modes: their velocity is set in postCollision and is untouched here.
    if (vehicle.type == VehicleType::Wave) {
        velocity = (input * 2 - 1) * player_speeds[speed] * (small ? 2.f : 1.f);
    }

    // Dash orb: sustain the angled glide while the button stays held; releasing
    // exits the dash so normal physics resume from the current velocity.
    if (dashing) {
        if (button) {
            velocity = player_speeds[speed] * dashTan;
            velocityOverride = true;
        } else {
            dashing = false;
        }
    }

    preFrameVelocity = velocity;
    pos.y += (float)(grav(velocity) * dt);

    for (auto& i : actions) i(*this);
    actions.clear();
    potentialSlopes.clear();

    if (slopeData.slope && slopeData.slope->gravOrient(*this) == 1)
        grounded = true;
}

void Player::postCollision() {
    if (small != prevPlayer().small) {
        Vec2D oldSize = size;
        size = small ? (size * 0.6f) : (size / 0.6f);
        if (getenv("GDSIM_SIZE_DEBUG"))
            std::fprintf(stderr, "SIZE-TOGGLE f=%d small=%d oldSize=(%f,%f) newSize=(%f,%f) veh=%d\n",
                         frame, small, oldSize.x, oldSize.y, size.x, size.y, (int)vehicle.type);
    }

    if (gravBottom(*this) <= gravFloor() && !velocityOverride && velocity <= 0) {
        pos.y = grav(gravFloor()) + grav(size.y / 2);
        grounded = true;
        snapData.playerFrame = 0;
    }
#ifdef DEBUG_CUBE_START
    if (frame == 2) {
        std::fprintf(stderr, "DEBUG pre-vehicle frame=%d x=%f y=%f vel=%f grounded=%d velocityOverride=%d buffer=%d input=%d button=%d\n",
                     frame, pos.x, pos.y, velocity, grounded ? 1 : 0, velocityOverride ? 1 : 0,
                     buffer ? 1 : 0, input ? 1 : 0, button ? 1 : 0);
    }
#endif

    if (pos.y > 1476.3f || (upsideDown && getBottom() < floor)) {
        if (getenv("GDSIM_BOUNDS_DEBUG"))
            std::fprintf(stderr, "BOUNDS f=%d pos.y=%f getBottom=%f floor=%f ceiling=%f size.y=%f upsideDown=%d small=%d\n",
                         frame, pos.y, getBottom(), floor, ceiling, size.y, upsideDown, small);
        dead = true; deathCause = "bounds";
        return;
    }

    if (prevPlayer().gravBottom(*this) > prevPlayer().gravFloor()
        && upsideDown == prevPlayer().upsideDown
        && !grounded && velocity <= 0)
    {
        if (prevPlayer().grounded && !prevPlayer().input)
            coyoteFrames = 0;
        coyoteFrames++;
    } else {
        coyoteFrames = INT_MAX;
    }

    vehicle.update(*this);

    if (!velocityOverride) {
        double newVel = velocity + acceleration * dt;

        bool wasGrounded = prevPlayer().grounded;
        bool inputCondition = (!input && (prevPlayer().button || !button)) || buffer;
        bool fellOff = !grounded && wasGrounded && inputCondition
            && prevPlayer().gravBottom(*this) > prevPlayer().gravFloor()
            && size == prevPlayer().size;

        // Walking off a ledge is just the first airborne frame: real GD applies ONE
        // gravity step to velocity (0 → a*dt) and integrates position with it
        // semi-implicitly (proven on truth 77236587 f1374: V 0→-0.22, Y 105→104.95).
        // The old block added an EXTRA velocity step (velocity==0 branch) and an
        // extra a*dt^2 position bump — a local explicit-Euler compensation that now
        // double-counts against the pipeline-wide semi-implicit integration (it made
        // the fall start one gravity step too fast: sim V -0.43 vs real -0.22, a
        // constant 0.22 dV that drifted the whole arc). Keep only the gravity-portal
        // sign flip for the rare fall-off-onto-a-gravity-portal frame.
        if (fellOff && gravityPortal && vehicle.type != VehicleType::Ship)
            newVel = -newVel;
        velocity = newVel;
    }

    if (roundVelocity) velocity = roundVel(velocity, upsideDown);

    if (slopeData.slope) slopeData.slope->calc(*this);

    // A ceiling snap (ship/ufo/swing clamp) SETS pos.y explicitly and zeroes
    // velocity. The semi-implicit resync below would then move that snapped position
    // by grav(preFrameVelocity)*dt (velocity is now 0, preFrameVelocity was the
    // pre-snap ascent), dragging the player ~0.6u off the ceiling (regression from
    // generalizing resyncPosition to every airborne frame; floor snaps are safe —
    // they set grounded). Detect any explicit pos.y snap and skip the resync for it.
    float yPreClamp = pos.y;
    vehicle.clamp(*this);
    bool clampSnapped = (pos.y != yPreClamp);

    // Advance the one-frame X-speed lag: this frame's `speed` only reaches the
    // pos.x integrator next-next frame, matching GD's delayed X speed application.
    xSpeed = pendingXSpeed;
    pendingXSpeed = speed;

    // Retroactive same-frame resync for a velocity impulse (see resyncPosition
    // above): correct this frame's Y for the gap between what preCollision
    // integrated (with the OLD, pre-impulse velocity) and what real GD shows
    // (integrated with the frame's final, post-impulse velocity).
    //
    // EXPERIMENT (integration order): GD advances Y with the POST-gravity velocity
    // (semi-implicit Euler); preCollision integrated with the PRE-gravity velocity
    // (explicit Euler), leaving a constant ~a*dt^2/frame Y drift on every airborne
    // frame that compounds across long arcs (proven on truth 77236587: matched
    // velocity, position drifts 0.05u/frame, landing 2 frames late). Fire the resync
    // on every airborne, gravity-driven frame — not just impulses — so the whole
    // pipeline is semi-implicit. Skipped when grounded/snapped (would push off the
    // floor), on velocity overrides, and for wave (its Y velocity is set instant in
    // preCollision, so preFrameVelocity already equals the final velocity).
    bool semiImplicit = !velocityOverride && !grounded && vehicle.type != VehicleType::Wave;
    if (clampSnapped) {
        resyncPosition = false;   // position was explicitly snapped to the ceiling
    } else if (resyncPosition || semiImplicit) {
        pos.y += (float)(grav(velocity) * dt - grav(preFrameVelocity) * dt);
        resyncPosition = false;
    }
}

Player::Player()
    : Entity({{0, 15}, {30, 30}, 0}),
      frame(1), timeElapsed(0), dead(false),
      vehicle(Vehicle::from(VehicleType::Cube)),
      ceiling(999999), floor(0), grounded(true),
      coyoteFrames(0), acceleration(0), velocity(0),
      velocityOverride(false), button(false), input(false),
      vehicleBuffer(false), upsideDown(false), small(false),
      speed(1), xSpeed(1), pendingXSpeed(1), slopeData({{}, 0, false}), roundVelocity(true),
      level(nullptr), dt(1.f/240.f), buffer(false), gravityPortal(false),
      robotHoldFrames(0), dual(false), isMirror(false)
{}

} // namespace gdsim
