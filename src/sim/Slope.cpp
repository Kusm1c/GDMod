#include "Slope.hpp"
#include "Player.hpp"
#include <cmath>
#include <algorithm>

namespace gdsim {

Slope::Slope(Vec2D s, std::unordered_map<int, std::string>&& fields) : Block(s, std::move(fields)) {
    auto rot = stod_def(fields[6].c_str());
    bool flipX = numFromString<int>(fields[4]) == 1;
    bool flipY = numFromString<int>(fields[5]) == 1;
    orientation = (int)rot / 90;
    if (flipX && flipY) orientation += 2;
    else if (flipX)     orientation += 1;
    else if (flipY)     orientation += 3;
    orientation = orientation % 4;
    if (orientation < 0) orientation += 4;
    rotation = 0;
}

bool Slope::isFacingUp() const { return orientation < 2; }

int Slope::gravOrient(Player const& p) const {
    int orient = orientation;
    if (p.upsideDown) {
        if (orient == 3) orient = 0;
        else if (orient == 2) orient = 1;
        else if (orient == 0) orient = 3;
        else if (orient == 1) orient = 2;
    }
    return orient;
}

double Slope::angle() const {
    auto ang = std::atan((double)size.y / (double)size.x);
    if (orientation == 1 || orientation == 3) ang = -ang;
    return ang;
}

// FOUND 2026-08-06 (user, after the wave collision rewrite still had one more issue):
// a slope's true shape is a TRIANGLE, but this had no touching() override at all —
// it inherited Object::touching()'s plain box-vs-box AABB test, meaning the EMPTY
// (air) corner of the slope's bounding square registered as a solid hit exactly like
// the real diagonal half. collide()'s own accept/reject (expectedY vs pos.y) IS
// diagonal-aware, so this didn't cause outright false rides/deaths by itself — but it
// DID mean `p.potentialSlopes.push_back(this)` (unconditional, at the top of
// collide()) fired from the empty corner too, feeding every consumer of
// potentialSlopes (Block::collide's snapUpThreshold adjustment) bad data, and made
// the broad-phase gate meaningfully less precise than the real triangular shape.
// Reject early using the SAME diagonal line expectedY() is built on (minus the
// player-radius offset, which doesn't belong in a pure shape test): if every corner
// of the query box is on the empty side of the line, there is no real overlap.
bool Slope::touching(Player const& p) const {
    if (!Object::touching(p)) return false;   // cheap broad-phase AABB reject first
    double ratio = (double)size.y / (double)size.x;
    auto lineY = [&](float x) -> double {
        double posRelative = ratio * (x - getLeft());
        return (angle() > 0) ? (getBottom() + posRelative) : (getTop() - posRelative);
    };
    const float xs[2] = {p.getLeft(), p.getRight()};
    const float ys[2] = {p.getBottom(), p.getTop()};
    const bool up = isFacingUp();
    for (float x : xs) {
        double ly = lineY(x);
        for (float y : ys)
            if (up ? (y <= ly) : (y >= ly)) return true;
    }
    return false;
}

// Matches real GD's GameObject::slopeYPos + the player-radius/clamp step done by
// PlayerObject::collidedWithSlopeInternal (from the camila314/gdp decompile). The
// underlying line (geomY) is an UNCLAMPED extrapolation of the tile's diagonal —
// real GD never restricts it to the tile's own x-span. What actually bounds the
// player's Y is a clamp against the tile's bottom/top edges (offset by the flat
// player radius, not the slope-adjusted one): floor slopes clamp to
// [bottom-radius, top], ceiling slopes to [bottom, top+radius].
// isNewSlope: real GD (collidedWithSlopeInternal) — was gripping a DIFFERENT slope
// object whose top/bottom orientation differs from this candidate (a floor→ceiling
// junction). Only meaningful while switching slopes; a fresh grip (nothing gripped
// last frame) is never "new" in this sense.
bool Slope::isNewSlopeTransition(Player const& p) const {
    auto& prevSlope = p.prevPlayer().slopeData.slope;
    return prevSlope.has_value() && prevSlope->id != id && prevSlope->isFacingUp() != isFacingUp();
}

double Slope::expectedY(Player const& p) const {
    double radius = p.size.y / 2.0;
    double radOnSlope = radius / std::cos(angle());
    double posRelative = (size.y / size.x) * (p.pos.x - getLeft());
    double geomY = (angle() > 0) ? (getBottom() + posRelative) : (getTop() - posRelative);
    // newSlopeScalar (real: collidedWithSlopeInternal) — when the player just
    // transitioned from a differently-oriented slope (floor→ceiling junction), the
    // real game shrinks the effective radius by vehicleSize*20 (12 for mini) for one
    // grip so the ride height doesn't jump by the full radius at the seam. Without
    // this a floor→ceiling junction pops/sinks the player by up to ~20u.
    double newSlopeScalar = isNewSlopeTransition(p) ? (p.small ? 12.0 : 20.0) : 0.0;
    double y = geomY + (isFacingUp() ? 1.0 : -1.0) * (radOnSlope - newSlopeScalar);
    // Clamp bounds matched to the real decompile (collidedWithSlopeInternal): a
    // FLOOR slope ridden on top (isFacingUp: centre = surface + radOnSlope) lets the
    // centre climb to maxY+radius, so the cube rides the diagonal all the way up to
    // getTop()+radius — NOT getTop(). The old code clamped floor slopes to getTop(),
    // pinning the ride at the slope base (proven wrong by the slopelab 291 capture:
    // gdsim stuck at y=30 while real rode to y=45, then missed the launch). Ceiling
    // slopes (ride underneath) clamp to [bottom-radius, top]. newSlopeScalar shrinks
    // ONLY the radius-bearing bound (real: floor clamps to [minY, maxY+radius-scalar];
    // ceiling to [minY-radius+scalar, maxY] — the non-radius bound is untouched).
    // See SLOPES_gdsim_vs_decompile.md.
    if (isFacingUp())
        y = std::max(std::min(y, (double)getTop() + radius - newSlopeScalar), (double)getBottom());
    else
        y = std::min(std::max(y, (double)getBottom() - radius + newSlopeScalar), (double)getTop());
    return y;
}

void Slope::calc(Player& p) const {
    if (gravOrient(p.prevPlayer()) == 0) {
        if (!touching(p)) {
            p.actions.push_back(+[](Player& p) {
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0.0;
                p.slopeData.snapDown = false;
            });
        }
        if (p.gravBottom(p.prevPlayer()) != getTop()) {
            if (p.prevPlayer().upsideDown)
                p.pos.y = std::min((double)p.pos.y, expectedY(p));
            else
                p.pos.y = std::max((double)p.pos.y, expectedY(p));
        }
        if (p.grounded && (p.gravBottom(p) == p.gravTop(*this)
            || (p.gravBottom(p) > p.gravTop(*this) && p.snapData.playerFrame > 0)))
        {
            // Real decompile (collidedWithSlopeInternal): m_slopeVelocity is set ONCE,
            // directly, when the slope grip resolves — min(1.12/slopeAngle,1.54) *
            // slopeYVelocity, no extra scale factor and no time-based ramp. gdsim had
            // an unexplained extra 0.9 multiplier and a 10*(elapsed)→[0.4,1.0] ramp
            // with no counterpart in the real formula (real applies the exit velocity
            // directly on exit, it doesn't ramp up over the time spent on the slope) —
            // both dropped. Only Ball can actually reach this (Cube/Robot/Spider are
            // grounded types real doesn't damp; Ship/Ufo/Wave/Swing never populate
            // slopeData.slope — they're intercepted by their own branches above in
            // collide() — so this path is Ball/Cube/Robot/Spider only in gdsim today).
            double vel = std::min(1.12 / p.grav(angle()), 1.54)
                       * (size.y * player_speeds[p.speed] / size.x);
            if (p.vehicle.type == VehicleType::Ball) vel *= 0.75;
            p.actions.push_back([vel](Player& p) {
                p.velocity = roundVel(vel, p.upsideDown);
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0;
                p.slopeData.snapDown = false;
            });
        }
    } else if (gravOrient(p.prevPlayer()) == 1) {
        if (p.velocity > 0) {
            p.actions.push_back(+[](Player& p) {
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0;
                p.slopeData.snapDown = false;
            });
        }
        if (p.gravBottom(p.prevPlayer()) != getTop() || p.slopeData.snapDown) {
            p.pos.y = std::max<float>(p.pos.y, (float)expectedY(p.prevPlayer()));
            p.pos.y = (float)std::max(std::min((double)p.pos.y, expectedY(p)), (double)(pos.y - p.size.y / 2.));
        }
        if (p.getTop() <= pos.y) {
            static double falls[4] = {226.044054, 280.422108, 348.678108, 421.200108};
            double vel = -falls[p.speed] * (size.y / size.x);
            p.velocity = 0;
            p.actions.push_back([vel](Player& p) {
                p.velocity = vel;
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0;
                p.slopeData.snapDown = false;
            });
        }
    } else if (gravOrient(p.prevPlayer()) == 2) {
        if (p.velocity < 0) {
            p.actions.push_back(+[](Player& p) {
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0;
                p.slopeData.snapDown = false;
            });
            return;
        }
        p.velocity = 0;
        if (p.grav(p.pos.y) < p.gravTop(*this))
            p.pos.y = p.grav(std::max<float>(p.grav(p.pos.y), (float)p.grav(expectedY(p))));
        if (p.grav(p.pos.y) >= p.gravTop(*this)) {
            p.pos.y = p.grav(p.gravTop(*this));
            p.velocity = roundVel(p.prevPlayer().acceleration * p.dt, p.prevPlayer().upsideDown);
            p.actions.push_back(+[](Player& p) {
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0;
                p.slopeData.snapDown = false;
            });
        }
    }
}

// NOTE: an earlier attempt at this fix ported real's collidedSlope grace-zone
// entry test (float_g / bool_h), gated on a derived `playerUphill`. Empirically it
// changed NOTHING against the slopelab capture (still DIVERGE @f204 sev=1.14,
// identical to baseline) — meaning either the grace window it computed never
// covered the approach, or `playerUphill`'s derivation (bit-level unknown: real's
// `m_slopeUphill` per-object flag isn't recoverable from the decompile alone) is
// wrong, quite possibly inverted. Reverted rather than ship an unverified guess;
// the raw crossing gate below is unchanged from the previously-validated version.
// newSlopeScalar (expectedY, above) IS kept — it's a direct, low-risk port that
// doesn't depend on this uncertain flag. Left as an open item in
// SLOPES_gdsim_vs_decompile.md gap #4 for a future pass with better ground truth.
void Slope::collide(Player& p) const {
    p.potentialSlopes.push_back(this);
#ifdef GDSIM_SLOPE_DEBUG
    fprintf(stderr, "[SLOPE] f=%d x=%.2f pos.y=%.2f expectedY=%.3f orient=%d left=%.1f right=%.1f gravTopP=%.2f gravBotThis=%.2f\n",
            p.frame, p.pos.x, p.pos.y, expectedY(p), orientation, getLeft(), getRight(),
            p.gravTop(p), p.gravBottom(*this));
#endif
    if (orientation < 2 && expectedY(p) <= p.pos.y) return;
    else if (orientation >= 2 && expectedY(p) >= p.pos.y) return;
    else if (p.vehicle.type == VehicleType::Cube && p.gravTop(p) - p.gravBottom(*this) < 16) return;

    if (p.vehicle.type == VehicleType::Wave) {
        // CONSISTENCY FIX 2026-08-06: this branch used to ALWAYS ride, unconditionally
        // safe — while the same-session rewrite of Block::collide (see
        // wave_collision_architecture_rewrite memory) correctly gated wave-vs-block on
        // `dartSlide`, this slope path was left on the old model, an inconsistency the
        // user caught. Real GD (collidedWithSlopeInternal) kills a wave on slope
        // contact when `stateHitHead<=0 && (isNewSlope || stateDartSlide<=0)` for a
        // non-hazard slope — i.e. a wave that isn't already established-sliding CAN
        // die on a slope too, not just on blocks. Apply the same gate as Block.cpp.
        if (p.slopeData.dartSlide < 1) {
            if (p.innerHitbox().intersects(*this)) {
                p.dead = true; p.deathCause = "slope";
                p.deathObjType = typeId; p.deathObjPos = pos;
            }
            return;
        }
        // Sliding: ride the surface (real GD lets an established-sliding wave slide
        // along a slope like any solid — it does not insta-kill). ORDER-INDEPENDENT
        // CLAMP (not an unconditional set): a wave corridor is routinely built from
        // TWO opposing slopes (floor rising to meet a ceiling descending). Both
        // slopes' broad-phase box can touch the player the same frame; clamping
        // (floor only pushes UP, ceiling only DOWN) is idempotent regardless of call
        // order, unlike an unconditional overwrite (which let call order silently
        // decide the winner).
        if (orientation < 2) p.pos.y = std::max(p.pos.y, (float)expectedY(p));
        else                 p.pos.y = std::min(p.pos.y, (float)expectedY(p));
        p.grounded = true;
        return;
    }

    // Flight modes (ship/ufo/swing) ride a CEILING slope's underside like the wave
    // rather than passing through it. A rising ship that penetrates the diagonal
    // (pos.y > expectedY, so we reached here past the orientation>=2 filter) is
    // clamped back onto the surface and its into-ceiling velocity zeroed — the ceiling
    // bonk — so it slides DOWN the descending diagonal as x advances (expectedY drops),
    // then falls when the slope ends. Matches real GD: 98414841 f1988, a mini ship hits
    // a rot=-90 ceiling slope and rides it down ~3 frames (yVel pinned 0, y dropping)
    // before falling. Without this the ship flew through and died on the block above.
    // slopeData.slope is left unset (like the wave) so a held button can lift it off the
    // next frame. Ceiling-only (orientation>=2); floor-slope flight handling unchanged.
    if (orientation >= 2 && (p.vehicle.type == VehicleType::Ship
            || p.vehicle.type == VehicleType::Ufo || p.vehicle.type == VehicleType::Swing)) {
        p.pos.y = (float)expectedY(p);
        if (p.grav(p.velocity) > 0) p.velocity = 0;
        p.grounded = true;
        return;
    }

    if (!p.prevPlayer().slopeData.slope && gravOrient(p) == 1 && p.velocity <= 0 && p.pos.x - getLeft() < 0) {
        p.pos.y = p.grav(p.gravTop(*this) + p.size.y / 2.f);
        p.grounded = true;
        return;
    }
    if (!p.prevPlayer().slopeData.slope && gravOrient(p) == 2 && p.velocity >= 0 && p.pos.x - getLeft() < 0) {
        p.pos.y = p.grav(p.gravBottom(*this) - p.size.y / 2.f);
        p.velocity = 0;
        return;
    }

    auto pSlope = p.slopeData.slope;
    if (!pSlope || !pSlope->touching(p)
        || (pSlope->gravOrient(p) == gravOrient(p) && p.grav(expectedY(p)) > p.grav(pSlope->expectedY(p)))
        || pSlope->id == id)
    {
        bool hasSlope = p.prevPlayer().slopeData.slope.has_value();
        double pAngle = atan((p.prevPlayer().velocity * p.dt) / (player_speeds[p.speed] * p.dt));
        if (gravOrient(p.prevPlayer()) > 1) pAngle = -pAngle;
        bool projectedHit = orientation == 1 ? (pAngle * 5.0 <= angle()) : (pAngle <= angle());
        bool clip = true;
        bool snapDown = orientation == 1 && p.velocity > 0 && p.pos.x - getLeft() > 0;
        if (hasSlope ? p.velocity <= 0 : projectedHit & clip || snapDown) {
            p.grounded = true;
            p.slopeData.slope = *this;
            if (snapDown && !hasSlope) {
                p.velocity = 0;
                p.pos.y = getTop() + p.size.y / 2;
                p.slopeData.snapDown = true;
            }
            if (!p.slopeData.elapsed)
                p.slopeData.elapsed = p.prevPlayer().timeElapsed;
        }
    }
}

void SlopeHazard::collide(Player& p) const {
    if (orientation < 2 && expectedY(p) <= p.pos.y) return;
    else if (orientation >= 2 && expectedY(p) >= p.pos.y) return;
    p.dead = true; p.deathCause = "slope-hazard";
    p.deathObjType = typeId; p.deathObjPos = pos;   // record killer so diag can name it
}

double SlopeHazard::expectedY(Player const& p) const {
    return Slope::expectedY(p) + (isFacingUp() ? -4 : 4);
}

bool SlopeHazard::touching(Player const& p) const {
    Entity hitbox = p.unrotatedHitbox();
    hitbox.size.y += 8;
    if (!intersects(hitbox)) return false;
    switch (orientation) {
        case 0: case 1: return expectedY(p) > p.pos.y;
        case 2: case 3: return expectedY(p) < p.pos.y;
        default: return false;
    }
}

} // namespace gdsim
