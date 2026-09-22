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
    // FOUND 2026-08-20 (DeCode, level 2997354): `(int)nVel` TRUNCATES toward zero,
    // not floor — for negative nVel (falling while upside-down, or rising while
    // upright) that's a ceiling, not the floor this code's own naming/intent
    // ("floored") expects. A real capture showed a smooth, steadily GROWING Y drift
    // (0 -> +2.02 over one ~85-unit free-fall arc) specifically during the falling
    // half of an upside-down flight — the exact signature of a per-frame rounding
    // bias that only bites on one sign of nVel, not a single discrete event. The
    // SAME arc while upright showed only a flat, non-growing ~1u offset (ordinary
    // noise), confirming the asymmetry is orientation-dependent, matching this bug
    // exactly. std::floor is sign-correct for both cases.
    double floored = std::floor(nVel);
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
        // TRIED a DeCode-scoped ×0.9 here, and separately a decompiled-source
        // "m_playerSpeed*m_speedMultiplier*54" reformulation (see Player.hpp history)
        // — both looked exact against isolated samples but direct position-delta
        // math proved player_speeds[] was already correct (the decompiled product
        // needs a x60 reference, not x54; see Player.hpp note above wave_speedmults'
        // old location). Reverted; formula matches real as originally written.
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
    // FOUND 2026-08-23 (DeCode, rotated gravity portal at x=1905): real GD's
    // GJBaseGameLayer::update runs updateJump — which does `v += a*dt` AND THEN
    // `y += v*dt` — BEFORE checkCollisions. So every collision test in the real game
    // sees the fully semi-implicit position. gdsim integrated Y here with the OLD
    // velocity and only added the missing `a*dt^2` later, in postCollision's resync —
    // i.e. AFTER collisions had already been resolved. Every portal/hazard/block test
    // was therefore run against a position one `a*dt^2` stale (~0.0485u for a cube).
    // Tiny, but it is a real systematic offset and it is exactly what made the
    // rotated portal above miss its Y threshold by 0.003u (player top 182.497 vs the
    // portal's 182.5) and fire a frame late. Fold that step in HERE so collisions see
    // what real GD sees; postCollision subtracts it back so the final Y is identical.
    // This frame's own acceleration isn't known yet (vehicle.update runs in
    // postCollision), so use the previous frame's — identical on every steady frame,
    // and the frames where it differs are impulse frames, which postCollision's
    // override branch already handles explicitly.
    preAppliedGravStep = 0.0;
    if (!grounded && !prevPlayer().grounded && vehicle.type != VehicleType::Wave) {
        double est = prevPlayer().acceleration * dt;
        // Respect terminal velocity, same reasoning as postCollision's override path:
        // a saturated fall gains nothing from the step, so neither does the position.
        if (vehicle.type == VehicleType::Cube || vehicle.type == VehicleType::Robot)
            est = std::max(velocity + est, -810.0) - velocity;
        preAppliedGravStep = est;
    }
    pos.y += (float)(grav(velocity + preAppliedGravStep) * dt);

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

    // FOUND 2026-08-23 (level 13519 "The Nightmare", user-reported cube->ball
    // problem, measured straight off the real capture): on the frame a vehicle
    // PORTAL switches vehicles, real GD has already run updateJump for the OLD
    // vehicle before checkCollisions performs the switch — so that frame still uses
    // the OLD vehicle's acceleration, and only the NEXT frame uses the new one.
    // Evidence at the airborne cube->ball at x=9674 (f7906): the switch frame's
    // world-velocity step is +11.664 (the CUBE's accel) and only the frame after it
    // becomes +6.966 (the BALL's). gdsim runs vehicle.update() for the NEW vehicle
    // here, in postCollision, so it applied the new accel one frame early — a 4.698
    // velocity error injected at every vehicle transition. prevPlayer().acceleration
    // is the old vehicle's value for exactly this frame (same orientation, since a
    // portal switch does not flip gravity), which is what real GD used.
    // Same ordering principle as the gravity-flip, impulse and collision-position
    // fixes above; this is the vehicle-switch instance of it.
    if (frame == lastVehicleSwitchFrame)
        acceleration = prevPlayer().acceleration;

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
    if (getenv("GDSIM_WAVE_RESYNC_DEBUG") && vehicle.type == VehicleType::Wave)
        std::fprintf(stderr, "WAVE-RESYNC f=%d x=%.2f yPreClamp=%.3f yPostClamp=%.3f preFrameVel=%.3f velPostClamp=%.3f resyncPos=%d clampSnapped=%d input=%d\n",
                     frame, pos.x, yPreClamp, pos.y, preFrameVelocity, velocity, resyncPosition, clampSnapped, input);

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
    if (getenv("GDSIM_GRAVFLIP_DEBUG") && frame >= 1930 && frame <= 1935)
        std::fprintf(stderr, "RESYNC-CHECK f=%d velOverride=%d grounded=%d resyncPos=%d semiImplicit=%d clampSnapped=%d velocity=%.3f preFrameVel=%.3f yBefore=%.3f\n",
                     frame, velocityOverride, grounded, resyncPosition, semiImplicit, clampSnapped, velocity, preFrameVelocity, pos.y);
    // Every branch below subtracts `preAppliedGravStep` because preCollision already
    // moved pos.y by grav(preAppliedGravStep)*dt so that COLLISIONS would see the
    // real game's semi-implicit position (see its comment). Netting it out here keeps
    // each frame's final Y exactly what it was before that change.
    // MUST use the PRE-flip orientation: preCollision applied this step before any
    // gravity portal/orb in the effects phase could flip `upsideDown`, so grav() here
    // would pick the post-flip sign on a flip frame and ADD the step instead of
    // removing it — a clean 2x double-count (measured 0.097 = 2 * 0.0485 at DeCode's
    // x=816.61 flip). Identical to grav() on every non-flip frame.
    const double gsignPre = prevPlayer().upsideDown ? -1.0 : 1.0;
    const double preApplied = gsignPre * preAppliedGravStep * dt;
    if (clampSnapped) {
        resyncPosition = false;   // position was explicitly snapped to the ceiling
    } else if (resyncPosition || semiImplicit) {
        pos.y += (float)(grav(velocity) * dt - grav(preFrameVelocity) * dt - preApplied);
        resyncPosition = false;
    } else if (velocityOverride && !grounded && !prevPlayer().grounded
               && vehicle.type != VehicleType::Wave) {
        // FOUND 2026-08-22 (DeCode, the level's FIRST divergence — f523/x=679, a
        // mid-air orb/jump impulse): on an impulse frame real GD still moves the
        // player by this frame's GRAVITY step, because GJBaseGameLayer::update runs
        // updateJump (v += a*dt; y += v*dt — semi-implicit) BEFORE checkCollisions
        // applies the impulse. The impulse therefore only changes velocity for the
        // NEXT frame's integration; it does NOT retro-apply to this frame's Y.
        // gdsim's velocityOverride suppressed the resync entirely, so preCollision's
        // explicit `y += preFrameVelocity*dt` stood alone and the frame lost exactly
        // one a*dt^2. Measured on the capture: real dY = -1.71261, sim dY = -1.6641,
        // difference -0.04851; cube accel at this tier is -2794.1082, and
        // -2794.1082/240/240 = -0.048509 — an exact match. Same ordering principle
        // as the gravity-flip fix in GravityPortal.cpp.
        // NOTE both grounded tests are required: an override SKIPS postCollision's
        // floor snap (its condition includes !velocityOverride), so `grounded` can
        // still read false for a player that is plainly sitting on the floor —
        // truth-bank 108166595 flips gravity at f24 on the spawn ground and turned
        // into a FALSE-DEATH until prevPlayer().grounded was added here.
        // Excludes: grounded (no free-fall step) and wave (velocity is a pure
        // function of input, already resolved in preCollision). resyncPosition
        // frames are the deliberate "impulse lands same-frame" path handled above.
        // GRAVITY-PORTAL frames are INCLUDED (2026-08-22) — real GD integrates this
        // frame's Y in the PRE-flip orientation, so the sign must come from the
        // orientation at the START of the frame, not from p.upsideDown (already
        // flipped by the portal in the effects phase). Using grav() here would pick
        // the post-flip sign and double the error instead of removing it. Level.cpp's
        // old `2*grav(preFrameVelocity)*dt` GRAVFLIP-CORR is zeroed for portal flips
        // in favour of this (see its comment); measured exact on DeCode's x=1905
        // portal: real dY = +2.2473 = (527.688 + 11.664)/240.

        // ...and the step must respect TERMINAL VELOCITY. Real GD's updateJump does
        // `v += a*dt` and then clamps; once v is saturated at the fall cap the clamp
        // eats the whole step, so the frame's position does NOT move by the extra
        // a*dt^2 either. Found on DeCode's blue-orb gravity flip at x=884.12, taken
        // at the cube's terminal 810: real dY = +3.3750 (= 810/240 exactly, pure
        // pre-flip velocity), sim dY = +3.4235 — a 0.0485 overshoot, exactly
        // a*dt^2 (2794.1082/240/240). That 0.049 then propagated untouched for 800
        // frames and made the ROTATED gravity portal at x=1905 miss its Y threshold
        // by 0.003 (player top 182.497 vs the portal's 182.5), firing a frame late.
        // Clamp the step the same way the vehicle would, so a saturated fall
        // contributes nothing. Cube/Robot share the 810 cap (Vehicle.cpp's cube
        // clamp); other vehicles keep the raw step until a capture shows otherwise.
        double gravStep = acceleration * dt;
        if (vehicle.type == VehicleType::Cube || vehicle.type == VehicleType::Robot) {
            const double capped = std::max(preFrameVelocity + gravStep, -810.0);
            gravStep = capped - preFrameVelocity;
        }
        pos.y += (float)(gsignPre * gravStep * dt - preApplied);
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
