// PlayerEngine.cpp — see PlayerEngine.hpp for the method. Every constant cites its .rdata
// address in GeometryDash.exe 2.2081; every non-obvious expression cites the instruction.
#include "PlayerEngine.hpp"
#include <cmath>

namespace gdsim::engine {

namespace {
// .rdata constants used by updateJump, read directly from the binary.
constexpr float  kFlightGravity = 0.958199024f;       // @0x140622bf8  (ball/ship/bird/dart/swing/spider)
constexpr float  kOne           = 1.0f;               // @0x140622c24
constexpr float  kAsym          = 0.8f;               // @0x140622ba4  (flight clamp asymmetry, reused as x0.8)
constexpr float  kMiniSize      = 0.8f;               // @0x140622ba4  (grounded mini size divisor)
constexpr float  kMiniFlySize   = 0.85f;              // @0x140622bc4  (flight mini size divisor)
constexpr float  kBand          = 8.0f;               // @0x140623068
constexpr float  kBandDown      = -6.4f;              // @0x1406237d4
constexpr float  kClampLo       = -8.0f;              // @0x1406237fc
constexpr float  kSwingBig      = 0.4f;               // @0x140622ad0
constexpr float  kSwingMini     = 0.6f;               // @0x140622b38
constexpr float  kHalf          = 0.5f;               // @0x140622b08
constexpr float  kUfoRise       = 1.2f;               // @0x140622c84
constexpr float  kUfoFlapBig    = 7.0f;               // @0x140623038
constexpr float  kUfoFlapMini   = 8.0f;               // xmm13 (= kBand)
constexpr float  kNegOne        = -1.0f;              // @0x1406236f0
constexpr float  kShipK         = 0.4f;               // @0x140622ad0
constexpr float  kShipKFalling  = 0.5f;               // @0x140622b08
constexpr float  kShipThrust    = 0.8f;
constexpr float  kShipUp        = -1.0f;
constexpr float  kShipFall      = 1.2f;               // @0x140622c84
constexpr float  kBallMult      = 0.6f;               // @0x140622b38
constexpr float  kRobotMult     = 0.9f;
constexpr float  kRobotHalf     = 0.5f;               // @0x140622b08 (robot jump = yStart * 0.5)
constexpr float  kRobotChargeK  = 0.1f;               // @0x140622a10
constexpr double kRobotChargeMax = 1.5;               // @0x140622e48
constexpr float  kSlopeBoostK   = 0.25f;              // @0x140622a88
constexpr double kSlopeCap      = 1.399999976158142;  // @0x140622e40 (double)
constexpr double kRampWindow    = 0.10000000149011612;// @0x140622cf0
constexpr double kRampScale     = 10.0;               // @0x140622ec8
constexpr double kRampFloor     = 0.4000000059604645; // @0x140622d78
constexpr double kBallFlipMult  = 0.6000000238418579; // (double)0.6f, direct store after the flip
constexpr double kTerminal      = 15.0;               // @0x140622ef0 / -15.0 @0x140623778
constexpr double kFallAnim      = 4.0;                // @0x140622e90

inline float upSign(const EngineSnap& s) { return s.m_isUpsideDown ? -1.0f : 1.0f; }

// x86 MAXSD dst,src : dst = (dst > src) ? dst : src ;  MINSD: (dst < src) ? dst : src.
// Spelled out rather than std::max/min so ties and signed zeros resolve like the engine.
inline double maxsd(double dst, double src) { return dst > src ? dst : src; }
inline double minsd(double dst, double src) { return dst < src ? dst : src; }
} // namespace

const char* const kNonPhysicsFields[] = {
    "m_jumpRelatedAC2",          // anti-cheat: rand() every updateJump
    "m_lastJumpTime",            // _ftime64 wall clock
    nullptr,
};

double quantizeYVelocity(double v) {
    // cvttsd2si -> int32 truncation toward zero; round() is the CRT's half-away-from-zero.
    const double whole = (double)(int)v;
    if (v != whole) return std::round((v - whole) * 1000.0) / 1000.0 + whole;
    return v;
}

void setYVelocity(EngineSnap& s, double v) { s.m_yVelocity = quantizeYVelocity(v); }

bool playerIsFallingBugged(const EngineSnap& s) {
    if (s.m_isSideways || s.m_isPlatformer || s.m_isSwing || s.m_fixGravityBug) {
        const float g2 = (float)(s.m_gravity + s.m_gravity);
        if (!s.m_isUpsideDown) return s.m_yVelocity < (double)g2;
        return (double)-g2 < s.m_yVelocity;
    }
    // The "bug": the classic path compares an upside-down player against +2g, not -2g.
    const double g  = s.m_gravity;
    const double gs = s.m_unkA99 ? -g : g;
    if (!s.m_isUpsideDown) return s.m_yVelocity < (double)((float)gs + (float)gs);
    return g + g < s.m_yVelocity;
}

void flipGravity(EngineSnap& s, bool flip, bool /*noEffects: portal circle only*/) {
    if (s.m_isUpsideDown == flip) return;
    s.m_isUpsideDown       = flip;
    s.m_lastFlipTime       = s.m_totalTime;
    s.m_collidedBottomMaxY = 0.0;
    s.m_collidedTopMinY    = 0.0;
    s.m_unkA29             = false;
    if (s.m_wasOnSlope || s.m_isOnSlope) s.m_slopeFlipGravityRelated = !s.m_slopeFlipGravityRelated;
    // The four CCDictionary collision logs are cleared here; they are not carried state.
    // Three 8-byte stores of -1, each covering two consecutive int fields:
    s.m_lastCollisionBottom = -1; s.m_lastCollisionTop   = -1;   // @0x5d0
    s.m_lastCollisionLeft   = -1; s.m_lastCollisionRight = -1;   // @0x5d8
    s.m_unk50C              = -1; s.m_unk510             = -1;   // @0x5e0
    // Halved by a DIRECT store (not setYVelocity, so not quantised), and only when
    // m_maybeReducedEffects is clear.
    if (!s.m_maybeReducedEffects) s.m_yVelocity = s.m_yVelocity * 0.5;
    // updatePlayerScale / updatePlayerArt / streak / swing fire: presentation. The streak
    // branch can also set m_shouldTryPlacingCheckpoint under a GameManager option, which is
    // not player state and is not modelled.
    s.m_lastGroundedPos_x = s.m_lastPortalPos_x;
    s.m_lastGroundedPos_y = s.m_lastPortalPos_y;
    s.m_isOnGround = false;
    if (s.m_isBall) {
        s.m_isRotating = false; s.m_isBallRotating2 = false;   // 2-byte store @0x728
        s.m_isBallRotating = false;
        s.m_rotationSpeed = 0.0f;
        // runBallRotation2: presentation
    }
}

void updateJump(EngineSnap& s, float dt, const Callees& callees) {
    // [platformer-only prologue omitted: left/right hold resolution, steep-slope ground reset]
    // [anti-cheat: m_jumpRelatedAC2 and two neighbours are refreshed from rand(); not modelled]

    // ── gravity and size selection (0x14038ba20 .. 0x14038bacd) ──────────────────────────
    float g = (float)s.m_gravity;                               // cvtpd2ps -> xmm12
    bool jump = s.m_jumpBuffered;                               // CVar8
    if (s.m_isRobot) jump = s.m_stateRingJump && s.m_jumpBuffered;
    if (s.m_isBall || s.m_isShip || s.m_isBird || s.m_isDart || s.m_isSwing || s.m_isSpider)
        g = kFlightGravity;
    const float gNoMod = g;                                     // xmm5
    float gMod = g;                                             // xmm12
    if (s.m_gravityMod != kOne) gMod = g * s.m_gravityMod;
    float asym = kAsym;                                         // xmm7
    float sizeDiv = (s.m_vehicleSize != kOne) ? kMiniSize : kOne;   // xmm9

    // ══ FLIGHT: ship / bird (UFO) / dart (wave) / swing ═══════════════════════════════════
    if (s.m_isShip || s.m_isBird || s.m_isDart || s.m_isSwing) {
        if (s.m_vehicleSize != kOne) sizeDiv = kMiniFlySize;
        {   // m_isAccelerating is cleared as soon as the velocity is back inside the band.
            const float up = kBand / sizeDiv, dn = kBandDown / sizeDiv;   // divss
            const double v = s.m_yVelocity;
            if (!s.m_isUpsideDown) {
                if ((0.0 <= v && v < (double)up) || (v <= 0.0 && (double)dn < v)) s.m_isAccelerating = false;
            } else {
                if ((v <= 0.0 && (double)-up < v) || (0.0 <= v && v < (double)-dn)) s.m_isAccelerating = false;
            }
        }

        if (!s.m_isShip) {
            if (!s.m_isBird) {
                if (!s.m_isSwing) {
                    // ── DART (wave) ──
                    const int us = s.m_isUpsideDown ? -1 : 1, jb = s.m_jumpBuffered ? 1 : -1;
                    // Two writes; the first is immediately overwritten but it is a real store.
                    setYVelocity(s, (double)((float)us * 8.0f * (float)jb));
                    setYVelocity(s, (double)s.m_playerSpeed * s.m_speedMultiplier * (double)us * (double)jb);
                    // (game event on a press edge: presentation)
                    goto clamp;                                 // the clamp itself is skipped for dart
                }
                // ── SWING ── its gravity flip lives HERE, not in a separate function:
                if (s.m_stateRingJump && s.m_jumpBuffered) {
                    s.m_stateRingJump = false;
                    const float v0 = (float)s.m_yVelocity;      // cvtpd2ps before the flip
                    flipGravity(s, !s.m_isUpsideDown, true);    // halves...
                    setYVelocity(s, (double)(v0 * asym));       // ...then overwritten: old velocity x 0.8
                }
                sizeDiv = kOne;                                 // 0x14038c959: xmm9 = xmm7 = 1.0
                asym = kOne;
                {   // gravity step, NO size divide: big 0.4 / mini 0.6 (0x14038c961..c9c0)
                    const float k = (s.m_vehicleSize != kOne) ? kSwingMini : kSwingBig;
                    const float step = (upSign(s) * (dt * gMod)) * k;
                    setYVelocity(s, (double)(-step) + s.m_yVelocity);
                }
            } else {
                // ── BIRD (UFO) ──
                if (s.m_stateRingJump && s.m_jumpBuffered) {
                    s.m_stateRingJump = false;
                    const float flap = (s.m_vehicleSize != kOne) ? kUfoFlapMini : kUfoFlapBig;
                    // A FLOOR, not an impulse: only raises the velocity up to the flap value.
                    double target; bool below;
                    if (!s.m_isUpsideDown) { target = (double)(sizeDiv * flap);            below = target > s.m_yVelocity; }
                    else                   { target = (double)((flap * kNegOne) * sizeDiv); below = s.m_yVelocity > target; }
                    if (below) {
                        setYVelocity(s, target);                // Ghidra dropped this argument
                        if ((s.m_wasOnSlope || s.m_isOnSlope) && 0.0f < s.m_slopeVelocity) {
                            const double v1 = s.m_yVelocity;
                            const float cap = (float)(v1 * kSlopeCap);
                            setYVelocity(s, (double)(s.m_slopeVelocity * kHalf) + v1);
                            setYVelocity(s, minsd(s.m_yVelocity, (double)cap));
                        }
                    }
                    // (click particles, flap animation, game event: presentation)
                }
                {   // gravity step (0x14038c892..c8df): rises at 1.2x while not "falling"
                    const float t = playerIsFallingBugged(s) ? asym : kUfoRise;
                    const float step = (((upSign(s) * (dt * gMod)) * t) * kHalf) / sizeDiv;
                    setYVelocity(s, (double)(-step) + s.m_yVelocity);
                }
            }
            if (s.m_jumpBuffered) s.m_isOnGround2 = false;
        } else {
            // ── SHIP ──
            float thrust = kShipThrust;                         // fVar20
            const double v = s.m_yVelocity;
            const bool movingDown = s.m_isUpsideDown ? (0.0 < v) : (v < 0.0);
            if (s.m_jumpBuffered && !s.m_isAccelerating) {
                thrust = kShipUp;
            } else {
                if (s.m_isAccelerating && movingDown) thrust = kShipUp;
                if (!s.m_jumpBuffered && !playerIsFallingBugged(s)) thrust = kShipFall;
            }
            // [platformer: gMod *= 0.8 here]
            // m_gravityMod only applies while not thrusting upward (thrust >= 0).
            const float gSel = (0.0f <= thrust) ? gMod : gNoMod;
            const float k = (s.m_jumpBuffered && playerIsFallingBugged(s)) ? kShipKFalling : kShipK;
            const float step = ((((upSign(s) * (dt * gSel)) * thrust) * k) / sizeDiv);
            setYVelocity(s, (double)(-step) + s.m_yVelocity);
            if (s.m_jumpBuffered) s.m_isOnGround2 = false;
            // (game event on a press edge: presentation)
        }

    clamp:   // 0x14038ca9f — skipped while accelerating (a boost may exceed it) and for the wave
        if (!s.m_isAccelerating && !s.m_isDart) {
            float hi;
            if (!s.m_isUpsideDown) {
                const float lo = (asym * kClampLo) / sizeDiv;
                setYVelocity(s, maxsd(s.m_yVelocity, (double)lo));
                hi = kBand / sizeDiv;
            } else {
                const float lo = kClampLo / sizeDiv;
                setYVelocity(s, maxsd(s.m_yVelocity, (double)lo));
                hi = (asym * kBand) / sizeDiv;
            }
            setYVelocity(s, minsd(s.m_yVelocity, (double)hi));
        }
        if (playerIsFallingBugged(s)) s.m_maybeIsBoosted = false;
        goto end;
    }

    // ══ GROUNDED: cube / ball / robot / spider ═══════════════════════════════════════════
    {
        const float mult = s.m_isBall ? kBallMult : s.m_isSpider ? kBallMult
                         : s.m_isRobot ? kRobotMult : kOne;     // xmm7
        // [platformer movement flags: in classic mode they reduce to "may jump"]

        if (jump && s.m_isOnGround) {
            if (!s.m_isSpider) {
                if (!s.m_isDashing) {
                    s.m_maybeIsBoosted = true;
                    s.m_isOnGround2 = false;
                    s.m_isOnGround = false;
                    s.m_stateRingJump = false;
                    s.m_touchedPad = false;
                    s.m_accelerationOrSpeed = 0.0;
                    float j = (float)s.m_yStart;
                    if (s.m_isRobot) j = j * kRobotHalf;
                    // (anti-tamper: a 9999 jump when the on-ground seed does not check out;
                    //  unreachable in normal play — the branch requires m_isOnGround.)
                    setYVelocity(s, (double)((upSign(s) * j) * sizeDiv));

                    if (s.m_wasOnSlope || s.m_isOnSlope) {
                        const float sv = s.m_slopeVelocity;
                        if (0.0f < upSign(s) * sv /* [platformer: && |xVel| > 4] */) {
                            const double v0 = s.m_yVelocity;
                            double ramp = 1.0;
                            const double el = s.m_totalTime - s.m_slopeStartTime;
                            if (el < kRampWindow) ramp = maxsd(kRampFloor, el * kRampScale);
                            float boost = (float)((double)sv * ramp);
                            if (s.m_isBall) boost = sv;         // the ball gets no ramp
                            setYVelocity(s, (double)(boost * kSlopeBoostK) + v0);
                            const double cap = (double)(float)(v0 * kSlopeCap);
                            const double v = s.m_yVelocity;
                            setYVelocity(s, !s.m_isUpsideDown ? minsd(v, cap) : maxsd(v, cap));
                        }
                    }
                    s.m_shouldTryPlacingCheckpoint = true;
                    // (m_lastJumpTime = wall clock; incrementJumps = stats)

                    if (!s.m_isBall) {
                        if (!s.m_isRobot && !s.m_isSpider && !s.m_isLocked && !s.m_isDashing) {
                            s.m_isRotating = false; s.m_isBallRotating2 = false;   // 2-byte store
                            s.m_isBallRotating = false;
                            s.m_rotationSpeed = 0.0f;
                            // runNormalRotation: presentation
                        }
                    } else {
                        // The ball's "jump" is a gravity flip: v = jump * 0.5 (flipGravity) * 0.6.
                        flipGravity(s, !s.m_isUpsideDown, true);
                        s.m_yVelocity = s.m_yVelocity * kBallFlipMult;          // direct store
                        s.m_isOnGround3 = false;
                        s.m_jumpBuffered = false;
                    }
                    goto end;
                }
            } else if (!s.m_isDashing) {
                if (callees.spiderTestJump) callees.spiderTestJump(s);
                goto end;
            }
        }

        if (s.m_maybeIsBoosted) {
            // Robot charge: while held, add back exactly the gravity step it is about to take,
            // so gravity is cancelled; +0.1*dt per step up to 1.5 (67 steps at dt 0.225).
            if (s.m_isRobot && s.m_jumpBuffered && !s.m_touchedPad && s.m_accelerationOrSpeed < kRobotChargeMax) {
                s.m_accelerationOrSpeed = (double)(dt * kRobotChargeK) + s.m_accelerationOrSpeed;
                setYVelocity(s, (double)((upSign(s) * (dt * gMod)) * mult) + s.m_yVelocity);
            }
            setYVelocity(s, s.m_yVelocity - (double)((upSign(s) * (dt * gMod)) * mult));
            // (robot landing-pad game event: presentation)
            if (!playerIsFallingBugged(s)) goto end;            // [platformer: extra check]
            s.m_maybeIsBoosted = false;
            s.m_maybeIsFalling = true;
            s.m_isOnGround2 = false;
            s.m_fallStartY = s.nodePosition_y;
            // (robot / spider fall animation: presentation)
            goto end;
        }

        if (s.m_isOnGround) s.m_fallStartY = s.nodePosition_y;
        if (playerIsFallingBugged(s) /* [platformer: || landing grace] */) s.m_isOnGround = false;
        setYVelocity(s, s.m_yVelocity - (double)((upSign(s) * (dt * gMod)) * mult));

        {   // terminal velocity, then the inlined 3-decimal quantisation, stored directly
            double v = s.m_yVelocity;
            v = !s.m_isUpsideDown ? maxsd(v, -kTerminal) : minsd(v, kTerminal);
            s.m_yVelocity = quantizeYVelocity(v);
        }
        {
            const double v = s.m_yVelocity;
            const bool fallingFast = !s.m_isUpsideDown ? (v < -0.25) : (0.25 < v);
            if (fallingFast && !s.m_isBall && !s.m_isSpider && !s.m_isRobot && !s.m_isRotating
                && !s.m_isOnSlope && !s.m_isCollidingWithSlope && !s.m_isLocked && !s.m_isDashing) {
                s.m_isRotating = false; s.m_isBallRotating2 = false;           // 2-byte store
                s.m_isBallRotating = false;
                s.m_rotationSpeed = 0.0f;
                // runNormalRotation: presentation
            }
        }
        if (playerIsFallingBugged(s)) {
            const double v = s.m_yVelocity;
            const bool beyond = !s.m_isUpsideDown ? (v < -kFallAnim) : (kFallAnim < v);
            if (beyond) {
                // (robot / spider fall animation: presentation)
                s.m_isOnGround2 = false;
            }
        }
    }

end:   // 0x14038cb36
    s.m_wasJumpBuffered = s.m_jumpBuffered;
    s.m_wasRobotJump    = s.m_touchedPad;
}

namespace {
constexpr float  kUpdateDtScale  = 0.9f;               // @0x140622bd8 (xmm6 at 0x140388deb / 0x14038911c)
constexpr double kVelLimit       = 1000.0;             // immediates 0x408f400000000000 / 0xc08f...
constexpr float  kForceShip      = 0.47f;              // @0x140622afc
constexpr float  kForceBird      = 0.58f;              // @0x140622b2c
constexpr float  kForceSwing     = 0.4f;               // @0x140622ad0 (xmm15, 0x1403891f1)
constexpr float  kForceBallSpider = 0.6f;             // @0x140622b38
constexpr float  kForceRobot     = 0.9f;               // @0x140622bd8 (xmm6)
constexpr float  kForceMiniShip  = 0.5875f;            // @0x140622b30
constexpr float  kForceMiniBird  = 0.72499996f;        // @0x140622b84
constexpr float  kForceMiniSwing = 0.61538464f;        // @0x140622b48
constexpr double kForceAccelDiv  = 12.9399995803833;   // @0x140622ed8
constexpr double kForceAccelMul  = 1.5;                // @0x140622e48
constexpr double kReverseStep    = 0.019999999552965164; // @0x140622c78 (double)0.02f
constexpr double kGoingLeft      = -1.0;               // @0x140623728
} // namespace

void update(EngineSnap& s, float dt, const Callees& callees) {
    // 0x140388df3: every gameplay use of the step below is scaled by 0.9f, in FLOAT.
    const float dt9 = dt * kUpdateDtScale;
    // (flash colour interpolation: presentation)

    // Direct stores, not through setYVelocity (no quantisation).
    if (s.m_yVelocity > kVelLimit) s.m_yVelocity = kVelLimit;
    else if (s.m_yVelocity < -kVelLimit) s.m_yVelocity = -kVelLimit;
    // [platformer: m_platformerXVelocity clamped to +-1000 the same way]
    if (s.m_isDead) return;
    // GameObject+0x4d0 = getPosition() (a ccpDistance to it is computed and discarded). A
    // GameObject base member, not a PlayerObject one: not snapshotted.
    s.m_yVelocityRelated3 = 0.0f;

    if (!s.m_isLocked) {
        updateJump(s, dt9, callees);                         // 0x140389216, xmm1 = dt*0.9f

        if (s.m_stateForce > 0) {                            // force blocks
            float mult = 1.0f;
            if (s.m_isShip)                         mult = kForceShip;
            else if (s.m_isBird)                    mult = kForceBird;
            else if (s.m_isSwing)                   mult = kForceSwing;
            else if (s.m_isBall || s.m_isSpider)    mult = kForceBallSpider;
            else if (s.m_isRobot)                   mult = kForceRobot;
            if (s.m_vehicleSize != 1.0f) {
                if (s.m_isShip)       mult = kForceMiniShip;
                else if (s.m_isBird)  mult = kForceMiniBird;
                else if (s.m_isSwing) mult = kForceMiniSwing;
            }
            // 0x1403892db: (dt9 * forceY) * mult in float, then widened.
            const double dv = (double)((dt9 * s.m_stateForceVector_y) * mult);
            setYVelocity(s, dv + s.m_yVelocity);
            if (dv != 0.0) s.m_isAccelerating = true;
            if (!s.m_isUpsideDown ? (0.0 < dv) : (dv < 0.0))
                s.m_accelerationOrSpeed = (dv / kForceAccelDiv) * kForceAccelMul + s.m_accelerationOrSpeed;
            // [platformer: m_stateForceVector.x feeds m_platformerXVelocity + cube rotation]
        }

        if (s.m_isDashing) s.m_yVelocity = 0.0;              // direct store
        // Semi-implicit: the position uses the velocity updateJump has just produced.
        double dy = (double)dt9 * s.m_yVelocity;
        // [platformer: updateMove, and dx = m_platformerXVelocity * dt]
        double dx = ((double)s.m_playerSpeed * s.m_speedMultiplier) * (double)dt;   // FULL dt
        const double reverseStep = dx * kReverseStep;
        if (s.m_isDart && !s.m_isDashing) {
            // The wave ignores its velocity: 45 degrees (63.43 mini) off |dx|.
            const int up   = s.m_isUpsideDown ? -1 : 1;
            const int hold = s.m_jumpBuffered ? 1 : -1;
            dy = std::fabs(dx) * (double)up * (double)hold;
            if (s.m_vehicleSize != 1.0f) dy = dy + dy;
        } else if (s.m_isDashing) {
            dy = s.m_dashY * dx;                             // [platformer: dash X/Y velocities]
        }
        s.m_yVelocityRelated3 = (float)dy;
        if (s.m_isGoingLeft) dx = dx * kGoingLeft;
        double px = dx + s.m_maybeReverseAcceleration, py = dy;
        if (s.m_isSideways) { py = px; px = dy; }
        // CCPoint(float, float) added to getPosition() in float, then the virtual setPosition.
        s.nodePosition_x = s.nodePosition_x + (float)px;
        s.nodePosition_y = s.nodePosition_y + (float)py;

        s.m_maybeReverseAcceleration = 0.0;
        const double rs = s.m_maybeReverseSpeed;
        if (rs != 0.0) {
            double a = minsd(std::fabs(rs), reverseStep);    // 0x140389647
            if (rs <= 0.0) a = -a;
            s.m_maybeReverseAcceleration = a;
            s.m_maybeReverseSpeed = rs - a;
        }
    }

    // (vehicle particles, wave trail, robot fire, ghost trail, dash animation, swing burst:
    // presentation)
    s.m_maybeSpriteRelated = false;
    s.m_stateJumpBuffered = s.m_jumpBuffered;
    s.m_stateRingJump2    = s.m_stateRingJump;
    // One 4-byte store of 0 at 0x98b: these four bools are adjacent.
    s.m_touchedRing = false;
    s.m_touchedCustomRing = false;
    s.m_touchedGravityPortal = false;
    s.m_maybeTouchedBreakableBlock = false;
    --s.m_stateNoAutoJump;
    --s.m_stateDartSlide;
    --s.m_stateFlipGravity;
    --s.m_stateHitHead;
    --s.m_stateOnGround;
    --s.m_stateBoostX;
    --s.m_stateBoostY;
    --s.m_maybeStateForce2;
    --s.m_stateScale;
    --s.m_stateForce;
    s.m_stateForceVector_x = 0.0f;
    s.m_stateForceVector_y = 0.0f;
    // m_jumpPadRelated (a map<int,bool>) is cleared: container, not snapshotted.
    // [platformer: a timed dash ring ends the dash and clears m_jumpBuffered]
}

} // namespace gdsim::engine
