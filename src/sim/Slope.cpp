#include "Slope.hpp"
#include "Player.hpp"
#include "Calib.hpp"
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
    return triangleOverlaps(p);
}

bool Slope::triangleOverlaps(Entity const& box) const {
    double ratio = (double)size.y / (double)size.x;
    auto lineY = [&](float x) -> double {
        double posRelative = ratio * (x - getLeft());
        return (angle() > 0) ? (getBottom() + posRelative) : (getTop() - posRelative);
    };
    const float xs[2] = {box.getLeft(), box.getRight()};
    const float ys[2] = {box.getBottom(), box.getTop()};
    const bool up = isFacingUp();
    for (float x : xs) {
        double ly = lineY(x);
        for (float y : ys)
            if (up ? (y <= ly) : (y >= ly)) return true;
    }
    return false;
}

bool Slope::circleOverlaps(Vec2D center, float radius) const {
    double ratio = (double)size.y / (double)size.x;
    double posRelative = ratio * ((double)center.x - getLeft());
    double ly = (angle() > 0) ? (getBottom() + posRelative) : (getTop() - posRelative);
    // Signed penetration in the VERTICAL axis (positive = centre already on the
    // solid side of the diagonal), converted to a true PERPENDICULAR distance by
    // the line's own slope (ratio) — the same conversion for any diagonal, not
    // just 45°, since these slopes come in both 45° and ~63.43° (mini) flavours.
    double vertPen = isFacingUp() ? (ly - (double)center.y) : ((double)center.y - ly);
    double perpPen = vertPen / std::sqrt(1.0 + ratio * ratio);
    return perpPen > -(double)radius;
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
    if (getenv("GDSIM_SLOPECALC_DEBUG"))
        std::fprintf(stderr, "SLOPECALC f=%d slopeId=%d slopePos=(%.2f,%.2f) playerXY=(%.2f,%.2f) grounded=%d "
                     "vel=%.3f veh=%d expectedY=%.3f touching=%d gravOrientPrev=%d\n",
                     p.frame, typeId, pos.x, pos.y, p.pos.x, p.pos.y, p.grounded, p.velocity,
                     (int)p.vehicle.type, expectedY(p), touching(p), gravOrient(p.prevPlayer()));
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
                       * (size.y * player_speeds[p.speed] / size.x)
                       * g_calib.slopeExitVelScale;
            if (p.vehicle.type == VehicleType::Ball) vel *= 0.75;
            // FOUND 2026-08-17 (real Watch capture, level 2997354 "DeCode", full
            // per-frame data — not just a sparse sample): real GD's Y barely moves
            // on the exit frame itself (165.000 -> 164.951, essentially flat) even
            // though yVel is already reported as the new launch value there — the
            // NEW velocity's effect on POSITION doesn't land until the FOLLOWING
            // frame. gdsim was integrating position with the new launch velocity
            // on the SAME frame it's set (immediate +1.6u jump vs real's ~0u),
            // exactly the "orb boost on a fresh touch lands next frame" deferral
            // Orb.cpp already implements via velocityOverride (see its own comment:
            // "GD applies the boost in checkCollisions — AFTER this step's
            // updateJump") — same underlying engine-order cause, so the same fix.
            p.actions.push_back([vel](Player& p) {
                p.velocity = roundVel(vel, p.upsideDown);
                p.velocityOverride = true;
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
    } else if (gravOrient(p.prevPlayer()) == 3) {
        // FOUND 2026-08-12 (real playtest, DeCode 2997354, user-flagged ship pass-
        // through via the app's J-flag tool near a slope/shelf junction, x≈22000):
        // gravOrient(prevPlayer()) can be 0, 1, 2, OR 3 (orientation remapped by
        // upsideDown — see gravOrient() above), but this function only ever handled
        // 0/1/2. An upside-down player gripping an orientation-0 slope (this exact
        // case: floor-facing slope + upsideDown maps to gravOrient 3) fell through
        // ALL three branches and did NOTHING — no Y-ride tracking, no exit-velocity
        // handoff, and critically no release-on-!touching() either, since that logic
        // lives inside the unreached case-0 branch. The grip then never lets go: X
        // keeps advancing while Y (and velocity, held at 0 by Ship::update's
        // grounded-zeroing) sits frozen wherever it last was, silently sailing under/
        // through any solid geometry in its path with no collision response at all —
        // confirmed via a direct trace (GDSIM_SLOPECALC_DEBUG): touching=1,
        // gravOrientPrev=3, expectedY climbing 93→105 while pos.y stayed pinned at
        // 73.5 for 28 straight frames, well past the slope's own physical x-span.
        //
        // UPDATED 2026-08-14 (same spot flagged again — release-only wasn't enough:
        // the player still sat with Y untracked for however long it stayed gripped
        // before releasing, which is still a visible pass-through, just bounded
        // instead of infinite). Added active ride-Y tracking, by direct structural
        // analogy with case 0's own upsideDown split just above: gravOrient 0 is
        // reached by EITHER (orientation 0, not upside-down) OR (orientation 3,
        // upside-down), and case 0 already picks max() vs min() purely by testing
        // `prevPlayer().upsideDown` — the object's raw orientation never enters that
        // choice directly. gravOrient 3 is reached by the other two combinations
        // — (orientation 3, not upside-down) or (orientation 0, upside-down, our
        // case) — so the same upsideDown-keyed split is applied here, with the
        // clamp direction INVERTED relative to case 0 (this is gravOrient 3, not 0):
        // case 0 uses max when not-upside-down / min when upside-down; case 3 uses
        // the opposite pairing. This is the most conservative, structurally-
        // consistent choice available (reuses the exact test already validated for
        // case 0) — but it is NOT verified against a real capture of this exact
        // configuration (unlike almost everything else in this file, which is
        // capture-checked; this project has twice shipped-then-reverted a wrong
        // slope-physics sign guess before, see the notes below). If this still
        // looks wrong in real GD, the fix is to capture a real run through this
        // exact spot (RE scanner) rather than re-guess the sign again.
        if (!touching(p)) {
            p.actions.push_back(+[](Player& p) {
                p.slopeData.slope = {};
                p.slopeData.elapsed = 0.0;
                p.slopeData.snapDown = false;
            });
        }
        if (p.gravBottom(p.prevPlayer()) != getTop()) {
            if (p.prevPlayer().upsideDown)
                p.pos.y = std::max((double)p.pos.y, expectedY(p));
            else
                p.pos.y = std::min((double)p.pos.y, expectedY(p));
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
    // FIXED 2026-08-12 (real playtest report: wave surviving slope touches it
    // shouldn't). This used to sit BELOW the three expectedY-based early-return
    // gates further down — those gates are broad-phase reject checks for
    // RESTING/RIDING physics (grounded vehicles: "has the player actually sunk
    // past the ride surface yet"), meaningless for the wave's completely
    // different rule (die on first touch, full stop). Slope::touching() already
    // did the real triangular hitbox test before collide() was ever called — a
    // wave reaching here IS genuinely overlapping the slope's triangle — but the
    // gates below would `return` early whenever the wave's actual Y hadn't yet
    // crossed the ride-line (e.g. skimming the corner of the triangle from
    // "outside" the ramp direction), letting it slip through untouched. Checked
    // first, before any of that grounded-physics gating, so touching() firing is
    // the only thing that matters, exactly like Block.cpp's wave branch.
    if (p.vehicle.type == VehicleType::Wave) {
        // Explicit, repeated, forceful user instruction (see Block.cpp's identical
        // note): the wave dies on ANY collision, block or slope, no exception
        // except a D block. Two more nuanced models were tried and reverted the
        // same day (unconditional-safe, then a dartSlide grace ported loosely from
        // the decompiled collidedWithSlopeInternal) — both still let a wave survive
        // touching plain solid geometry under some condition, which doesn't match
        // real GD: wave difficulty comes specifically from ANY touch being lethal —
        // corridors are threaded WITHOUT overlapping either surface, not ridden
        // like a cube resting on a platform.
        //
        // FIXED 2026-08-12 (regression caught by test/regress.sh on truth
        // 85701165, a wave-corridor level: FALSE-DEATH @f210, gdsim killed by
        // slope id=1339 with dy=dyVel=0.00 — position matched real GD exactly,
        // so this was a pure logic bug, not drift): this used to be a plain
        // rectangular intersects(*this) against the slope's BOUNDING BOX. A
        // slope's real solid shape is only the TRIANGULAR half of that box (the
        // exact reason touching() has its own diagonal line-crossing test,
        // see that function's 2026-08-06 fix note above) — the wave's
        // innerHitbox could sit in the triangle's EMPTY corner, still inside the
        // bounding rectangle, and this raw AABB test would call it a hit anyway.
        // Reuses touching()'s own triangleOverlaps() so the death check respects
        // the same real diagonal every other slope interaction already does.
        // FIXED 2026-08-12, second pass (test/regress.sh still flagged 85701165
        // FALSE-DEATH after the triangleOverlaps swap above, dy=dyVel=0.00, so
        // still a pure logic bug): traced it to a corner of the wave's small
        // innerHitbox poking ~1 unit past the diagonal at frame 304 while real GD
        // survives. GD Creator School documents the wave's slope collision as a
        // CIRCLE test, not a box-corner test (gd_creators_school_physics_spec
        // memory) — a circle's closest approach to a corner is always less than
        // the box's own corner distance, so the same geometry that trips a
        // box-corner test can cleanly miss a circle. Switched to circleOverlaps()
        // with the innerHitbox's own half-width as the radius (keeps the already-
        // validated innerHitbox SIZE for wave-vs-solid collision, per this
        // session's earlier work — only the SHAPE of the test changes, box→circle).
        Entity hb = p.innerHitbox();
        bool hit = hb.intersects(*this) && circleOverlaps(hb.pos, hb.size.x * 0.5f);
        if (getenv("GDSIM_WAVESLOPE_DEBUG"))
            std::fprintf(stderr, "WAVESLOPE f=%d slopeId=%d slopePos=(%.2f,%.2f) slopeSize=(%.2f,%.2f) orient=%d "
                         "hbPos=(%.2f,%.2f) hbSize=(%.2f,%.2f) aabbHit=%d circHit=%d\n",
                         p.frame, typeId, pos.x, pos.y, size.x, size.y, orientation,
                         hb.pos.x, hb.pos.y, hb.size.x, hb.size.y, hb.intersects(*this), hit);
        if (hit) {
            p.dead = true; p.deathCause = "slope";
            p.deathObjType = typeId; p.deathObjPos = pos;
        }
        return;
    }

    p.potentialSlopes.push_back(this);
#ifdef GDSIM_SLOPE_DEBUG
    fprintf(stderr, "[SLOPE] f=%d x=%.2f pos.y=%.2f expectedY=%.3f orient=%d left=%.1f right=%.1f gravTopP=%.2f gravBotThis=%.2f\n",
            p.frame, p.pos.x, p.pos.y, expectedY(p), orientation, getLeft(), getRight(),
            p.gravTop(p), p.gravBottom(*this));
#endif
    if (orientation < 2 && expectedY(p) <= p.pos.y) return;
    else if (orientation >= 2 && expectedY(p) >= p.pos.y) return;
    else if (p.vehicle.type == VehicleType::Cube && p.gravTop(p) - p.gravBottom(*this) < 16) return;

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
