#include "SpecialBlock.hpp"
#include "Player.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gdsim {

void SpecialBlock::collide(Player& p) const {
    if (getenv("GDSIM_SPECIALBLOCK_DEBUG"))
        std::fprintf(stderr, "SPECIALBLOCK-TOUCH f=%d typeId=%d pos=(%.2f,%.2f) playerXY=(%.2f,%.2f)\n",
                     p.frame, typeId, pos.x, pos.y, p.pos.x, p.pos.y);
    switch (typeId) {
        case 1813: p.touchingJBlock = true; break;  // J: Stop Jump Buffer
        case 1829: p.touchingSBlock = true; break;  // S: Stop Dash
        case 1859: p.touchingHBlock = true; break;  // H: Allow Head Collision
        default: break;
    }
}

void GravityFlipBlock::collide(Player& p) const {
    // Modelled identically to GravityPortal.cpp (flip + halve velocity) — the
    // closest validated real analogue: an F block is functionally "a gravity
    // portal triggered by an invisible touch zone instead of a visible portal
    // shape". UNVERIFIED against real capture for this specific object; portals
    // are the best available reference, not a confirmed match.
    EffectObject::collide(p);
    p.velocity   = -p.velocity / 2;
    p.upsideDown = !p.upsideDown;
    p.gravityPortal = true;
}

void ForceBlock::collide(Player& p) const {
    // Real force blocks push in whatever direction the object is rotated, using
    // GD's full 2D physics. gdsim has no free player X-force (X always advances
    // at the fixed speed-portal rate — see Player::preCollision), so only the
    // force's VERTICAL component is modelled here; a force block that pushes
    // mostly sideways is under-modelled. Magnitude is a deliberately modest,
    // UNVERIFIED approximation — unlike J/S/H/F (which map onto already-
    // validated mechanics), no decompile/capture coverage exists yet for this
    // object at all. Continuous (reapplied every touching frame), like a wind
    // zone, rather than a one-shot impulse.
    constexpr double kForceAccel = 1200.0;  // px/s^2, approximate
    double dirY = -std::sin(deg2rad(rotation));  // rotation: 0=right, CCW+ (degrees)
    p.velocity += dirY * kForceAccel * p.dt;
    p.velocityOverride = true;
    p.grounded = false;
}

} // namespace gdsim
