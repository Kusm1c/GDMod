#include "Portals.hpp"
#include "Player.hpp"
#include "Calib.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace gdsim {

GravityPortal::GravityPortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields)), upsideDown(numFromString<int>(fields[1]) == 11) { triggerCat = PCAT_GRAVITY; }

// The "gravity up" portal (id 11) triggers measurably LATER than the "back to
// normal" one (id 10) — see g_calib.gravityUpReachAdjust for the evidence. Same
// shape as EffectObject::touching / Object::touching, with the extra offset folded
// into the portal's own hitbox before the intersection test.
bool GravityPortal::touching(Player const& p) const {
    // Env-gated trace for portal-trigger forensics: prints the portal's own box, the
    // player's, and the raw SAT result, which is how the a*dt^2 collision-timing bug
    // (see Player::preCollision) was finally pinned — the touch test was being run
    // against a position one gravity step stale, so a portal could be missed by
    // hundredths of a unit. GDSIM_GRAVTOUCH_DEBUG=<x> limits it to the portal at
    // that X (within 1u) to keep the output readable.
    if (const char* dbg = getenv("GDSIM_GRAVTOUCH_DEBUG")) {
        if (std::fabs(pos.x - (float)atof(dbg)) < 1.f) {
            Entity ph = p.unrotatedHitbox();
            std::fprintf(stderr, "GRAVTOUCH f=%d portalXY=(%.1f,%.1f) size=(%.1f,%.1f) rot=%.1f "
                                 "playerXY=(%.3f,%.3f) sz=(%.1f,%.1f) rawIntersect=%d used=%d -> %d\n",
                         p.frame, pos.x, pos.y, size.x, size.y, rotation,
                         p.pos.x, p.pos.y, ph.size.x, ph.size.y,
                         intersects(ph) ? 1 : 0,
                         p.usedEffects.contains(id) ? 1 : 0,
                         EffectObject::touching(p) ? 1 : 0);
        }
    }
    if (!upsideDown || g_calib.gravityUpReachAdjust == 0.f)
        return EffectObject::touching(p);
    if (p.usedEffects.contains(id)) return false;
    Entity self = *this;
    float reach = g_calib.portalReach[triggerCat] + g_calib.gravityUpReachAdjust;
    self.size.x += 2.f * reach;
    self.size.y += 2.f * reach;
    return self.intersects(p.unrotatedHitbox());
}

void GravityPortal::collide(Player& p) const {
    EffectObject::collide(p);
    if (upsideDown != p.upsideDown) {
        if (getenv("GDSIM_ORBTOUCH_DEBUG"))
            std::fprintf(stderr, "GRAVPORTAL-TOUCH f=%d typeId=%d pos=(%.2f,%.2f) playerXY=(%.2f,%.2f) velBefore=%.3f\n",
                         p.frame, typeId, pos.x, pos.y, p.pos.x, p.pos.y, p.velocity);
        // FOUND 2026-08-21: real decompiled source (PlayerObject::flipGravity,
        // FUN_14039a1d0 — see src/gdp-2.2/PlayerObject/PlayerObject_flipGravity.cpp)
        // does `m_yVelocity *= 0.5` on any flip — a flat half of WORLD-space
        // velocity, no negation. Converting through grav() for gdsim's player-
        // relative storage gives exactly this -v*0.5 form in BOTH flip directions
        // (worked out by hand). g_calib.gravityPortalFlipScale is now the
        // decompiled-exact 0.5, not an empirically-averaged ~0.513-0.537 fitted
        // against noisy real captures.
        // REFINED 2026-08-22 (DeCode ship section, x~21675 flip — proven exactly
        // against the raw real capture): flipGravity itself is indeed just the
        // flat *0.5, BUT real GD reaches it with this frame's GRAVITY STEP ALREADY
        // APPLIED. GJBaseGameLayer::update's own order is
        // processCommands -> m_player1->update() [updateJump: applies gravity to
        // m_yVelocity AND integrates position] -> checkCollisions [fires portals ->
        // flipGravity]. So the real operation is (v + a*dt) * 0.5, not v * 0.5.
        // Verified numerically on the capture: real's pre-flip world velocity
        // 31.104 with a*dt = 5.832 gives (31.104 + 5.832) * 0.5 = 18.468, matching
        // the captured post-flip value EXACTLY (real yVel 0.342 * 54 = 18.468).
        // gdsim integrates position in preCollision and only adds `acceleration*dt`
        // later in postCollision, so at this point p.velocity is still PRE-gravity
        // — add that step here before halving. (p.acceleration currently holds the
        // previous frame's value, since vehicle.update() runs in postCollision;
        // for a steady flight mode that equals this frame's, which is the case at
        // every flip measured so far. If a future capture shows a flip on a frame
        // where the input just changed, this may need this-frame accel instead.)
        // ...but ONLY while airborne. A GROUNDED player has no accumulated gravity
        // to carry (gdsim's own model pins velocity to 0 every grounded frame via
        // vehicle.update, and real GD's ground constraint does the same), so adding
        // a gravity step here invents upward velocity out of nothing. Found via
        // truth-bank level 108166595, which flips gravity at f24 while the cube is
        // still sitting on the spawn floor (velBefore=0.000, y=15): the unguarded
        // version handed it +5.82 of phantom velocity and turned a clean run into a
        // FALSE-DEATH at f24. p.grounded isn't meaningful yet at collide time
        // (preCollision clears it, postCollision sets it), so test the previous
        // frame's state, which is what "has this velocity been accumulating?"
        // actually depends on.
        const double gravStep = p.prevPlayer().grounded ? 0.0 : p.acceleration * p.dt;
        p.velocity   = -(p.velocity + gravStep) * g_calib.gravityPortalFlipScale;
        // velocityOverride stays: the gravity step for this frame is now accounted
        // for HERE (inside the halving, as real GD does), so postCollision must not
        // add it a second time.
        p.velocityOverride = true;
        p.upsideDown = upsideDown;
        p.gravityPortal = true;
    }
}

} // namespace gdsim
