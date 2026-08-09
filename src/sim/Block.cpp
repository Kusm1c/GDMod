#include "Block.hpp"
#include "Slope.hpp"
#include "Level.hpp"
#include "Player.hpp"
#include <cmath>
#include <array>
#include <algorithm>

namespace gdsim {

Block::Block(Vec2D s, std::unordered_map<int, std::string>&& fields) : Object(s, std::move(fields)) {
    prio = 1;
    if ((int)fabs(rotation) % 180 != 0) size = {size.y, size.x};
    rotation = 0;
    if (fields[1] == "468" && size.y == 5) size.y -= 3.5f;
}

enum class SnapType { None, BigStair, LittleStair, DownStair };

static float snapThreshold(Vec2D const& diff, Player const& p) {
    std::array<Vec2D, 3> stairs;
    float threshold;
    switch (p.speed) {
        case 0:  stairs = {Vec2D(120,-30),Vec2D(90,30),Vec2D(60,60)};   threshold=1; break;
        case 1:  stairs = {Vec2D(150,-30),Vec2D(p.small?90.f:120.f,30),Vec2D(90,60)};  threshold=1; break;
        case 2:  stairs = {Vec2D(195,-30),Vec2D(p.small?90.f:150.f,30),Vec2D(120,60)}; threshold=2; break;
        case 3:  stairs = {Vec2D(225,-30),Vec2D(90.f,30),Vec2D(135,60)}; threshold=2; break;
        default: stairs = p.small
            ? std::array<Vec2D,3>{Vec2D(150,-30),Vec2D(120,30),Vec2D(90,60)}
            : std::array<Vec2D,3>{Vec2D(225,-30),Vec2D(180,30),Vec2D(135,60)};
            // GD's exact snap (GDCS Player Snap Bug): x_shift = 1 unit at 4x speed
            // regardless of size (1 for 0.5x/1x/4x, 2 for 2x/3x). Was small?1:2, which
            // over-snapped big 4x by a unit.
            threshold = 1; break;
    }
    for (auto& stair : stairs)
        if (std::abs(diff.x - stair.x) <= threshold && std::abs(diff.y - stair.y) <= threshold)
            return threshold;
    return 0;
}

static void trySnap(Block const& b, Player& p) {
    auto snapData = p.snapData;
    auto diff = b.pos - snapData.object.pos;
    diff.y = p.grav(diff.y);
    if (float threshold = snapThreshold(diff, p); threshold > 0) {
        // refX is the cached pos.x of the previous landing state — identical to
        // the old getState(playerFrame).nextPlayer()->pos.x, but available even
        // when the simulator only holds a single injected state.
        p.pos.x = std::clamp(
            snapData.refX + diff.x,
            p.pos.x - threshold,
            p.pos.x + threshold);
    }
}

void Block::collide(Player& p) const {
    // D block (id 1755): a wave-safe solid (a special "letter block", handled outside
    // the generic collision function this file otherwise ports — GD Creator School
    // confirms D-blocks are a distinct special case). Real GD lets a WAVE touch a D
    // block without dying. Grounded modes fall through to normal handling below.
    if (typeId == 1755 && p.vehicle.type == VehicleType::Wave) return;

    // WAVE: ported from the real decompile (PlayerObject::collidedWithObjectInternal):
    //   if (m_isDart && m_stateDartSlide < 1) boolJ = true;
    //   ...
    //   if (!boolJ || m_isPlatformer || objPassable) return 0;
    //   CCRect smallHitbox = getObjectRect(0.3, 0.3);
    //   if (!objRect.intersectsRect(smallHitbox)) return 0;
    //   ... destroyPlayer ...
    // A wave NOT in an established slope-slide state (dartSlide<1) treats EVERY solid
    // block exactly like a hazard: boolJ is forced true, skipping the entire snap/ride
    // machinery, straight to a small (0.3-scaled = innerHitbox) death check — no
    // leniency, no grazing. Only once genuinely SLIDING a slope (dartSlide>=1, set by
    // Slope::collide's wave grip) does it get the "ride the surface" behaviour. This
    // is the OPPOSITE of what gdsim modelled before this fix (wave always rides
    // blocks, needing hand-tuned graze leniency) — found 2026-08-06 via a full read of
    // the decompiled source after a real, GD-verified-clearing replay (Delirium.gdr,
    // level 68839068) kept exposing more graze cases that were actually a wrong
    // architecture, not missing leniency constants.
    if (p.vehicle.type == VehicleType::Wave) {
        if (p.slopeData.dartSlide < 1) {
            if (p.innerHitbox().intersects(*this)) {
                p.dead = true; p.deathCause = "block";
                p.deathObjType = typeId; p.deathObjPos = pos;
            }
            return;
        }
        // Sliding: ride the surface (top-landing / ceiling-catch), order-independent
        // (see the comment history in git — a wave corridor built from two opposing
        // surfaces can have both register "touching" the same frame).
        double bottomW = p.gravBottom(p);
        constexpr float kWaveClip = 6.f;
        if (p.grav(p.velocity) <= 0 && p.gravTop(*this) - bottomW <= kWaveClip) {
            float target = p.grav(p.gravTop(*this)) + p.grav(p.size.y / 2);
            p.pos.y = p.grav(std::max(p.grav(p.pos.y), p.grav(target)));
            p.grounded = true;
        } else if (p.grav(p.velocity) > 0
                   && p.gravTop(p) - p.gravBottom(*this) <= kWaveClip) {
            // OPEN (2026-08-07): this ceiling-catch target rides the wave's FULL
            // p.size.y (10, from Vehicle.cpp's wave()) below the block's bottom edge.
            // Cross-checked against Delirium.gdr (level 68839068, a REAL human-
            // verified-clearing replay, via test/gdrcheck.cpp): gdsim rides flat at
            // Y=115 under a ceiling panel (block bottom=120) for several frames and
            // clips a rotated decorative spike (id=217) whose true triangular hazard
            // gdsim now models correctly (see Hazard.cpp) — proving the corner-clip
            // theory wrong; even a 60%-shrunk hazard triangle still gets clipped,
            // just one frame later at nearly the same X. That rules out the hazard
            // shape and points here instead: the ride height itself is very likely a
            // few units off from real GD's true wave-ceiling-catch offset. Disabling
            // ALL id=217 collision made the ENTIRE real clear pass end-to-end, so this
            // is the last blocker for that level — but I did not find or verify the
            // correct offset (no decompile coverage for wave's block collision; a
            // blind numeric guess here is high-blast-radius, since every wave level
            // uses this path). Next step: a live capture isolating a wave gliding
            // under a flat ceiling with no other confounders (slopelab-style), or a
            // live memory read of the real m_yVelocity/position at an equivalent
            // moment.
            float target = p.grav(p.gravBottom(*this)) - p.grav(p.size.y / 2);
            p.pos.y = p.grav(std::min(p.grav(p.pos.y), p.grav(target)));
            p.grounded = true;
        }
        return;
    }

    // Real GD (PlayerObject::collidedWithObjectInternal): snapUpThreshold = 10 for
    // non-flying modes (Cube/Ball/Robot/Spider), 6 for flying modes (Ship/Ufo/Swing;
    // Wave has its own death-hitbox path below and never uses this clip).
    int clip = (p.vehicle.type == VehicleType::Ufo || p.vehicle.type == VehicleType::Ship
                || p.vehicle.type == VehicleType::Swing) ? 6 : 10;

    if (p.upsideDown != p.prevPlayer().upsideDown && !p.gravityPortal) return;

    double bottom = p.gravBottom(p);
    if (p.slopeData.slope) {
        if (p.slopeData.slope->angle() > 0) {
            bottom = bottom + sin(p.slopeData.slope->angle()) * p.size.y / 2;
            clip = 7;
            if (p.gravTop(*this) - bottom < 2) return;
        }
    }

    for (auto& entity : p.potentialSlopes) {
        auto block_comp = entity->orientation < 2 ? getTop()         : getBottom();
        auto slope_comp = entity->orientation < 2 ? entity->getBottom() : entity->getTop();
        if (block_comp - slope_comp < 2) return;
    }

    bool padHitBefore = (!p.prevPlayer().grounded && p.prevPlayer().velocity <= 0 && p.velocity > 0);

    bool blockHit = p.blockDeathHitbox().intersects(*this);
    if (blockHit) {
        // GD edge-graze leniency for solid vehicles (cube/robot/spider/ball): sliding
        // PAST a block edge with a sub-unit overlap in the perpendicular axis is
        // survivable in real GD (the famous corner-clip). Forgive when the overlap is
        // tiny in EITHER axis — that's a graze along a face/corner, not a real hit. A
        // deep overlap in BOTH axes (a true landing or wall-smash) still kills, so the
        // calibrated 7×7 stays strict on platforms. Tolerance is well under the wave's
        // 1.0 to keep the box maximally strict for the cube's tight-platform calibration.
        // (Mini robot on 13711278 ~f2437: rising past a ledge, its death box grazed the
        // top-left edge by penY≈0.5 — real survives, gdsim's bare AABB false-killed it.)
        Entity h = p.blockDeathHitbox();
        float penX = std::min(h.getRight(), getRight()) - std::max(h.getLeft(), getLeft());
        float penY = std::min(h.getTop(),   getTop())   - std::max(h.getBottom(), getBottom());
        constexpr float kSolidGraze = 0.75f;
        if (std::min(penX, penY) <= kSolidGraze) blockHit = false;
    }
    if (blockHit) {
        p.dead = true; p.deathCause = "block";
        p.deathObjType = typeId; p.deathObjPos = pos;
    } else if (p.gravTop(*this) - bottom <= clip
               && (padHitBefore || p.velocity <= 0 || p.gravityPortal)) {
        p.pos.y = p.grav(p.gravTop(*this)) + p.grav(p.size.y / 2);
        if (!padHitBefore) p.grounded = true;
        if (p.slopeData.slope && p.slopeData.slope->angle() < 0) p.slopeData.slope = {};
        if (p.vehicle.type == VehicleType::Cube) {
            if (!p.prevPlayer().grounded) {
                // Snap only when ≥1 frame has passed since the tracked landing.
                // Use the monotonic Player::frame (landingFrame) so the check is
                // correct under state injection, where gameStates indices reset.
                if (p.snapData.playerFrame > 0 && p.snapData.landingFrame < p.frame)
                    trySnap(*this, p);
            }
            p.snapData.playerFrame  = p.level->currentFrame();
            p.snapData.landingFrame = p.frame;
            p.snapData.object       = *this;
        }
    } else {
        if (p.vehicle.type == VehicleType::Ship || p.vehicle.type == VehicleType::Ufo || p.vehicle.type == VehicleType::Ball) {
            if (p.gravTop(p) - p.gravBottom(*this) <= clip - 1 && p.velocity > 0) {
                p.pos.y = p.grav(p.gravBottom(*this)) - p.grav(p.size.y / 2);
                p.velocity = 0;
            }
        } else if ((p.vehicle.type == VehicleType::Cube || p.vehicle.type == VehicleType::Robot
                    || p.vehicle.type == VehicleType::Spider) && p.grav(p.velocity) > 0) {
            // The 7×7 inner death-box missed and this isn't a top-landing (the player
            // is RISING into a block's side/corner). The 7×7 is only lenient for the
            // top-corner graze you get while LANDING; a deep overlap on the way UP is a
            // real wall smash that GD kills on. Use the full hitbox with a small graze
            // tolerance. Rising-only, so descending landings (snap branch) are untouched.
            // (Truth 144641895 f436: a robot jump 1.8u too low clipped a block's upper
            // corner 9.7u deep; gdsim survived, real died, so the solver clipped through.)
            Entity full = p.unrotatedHitbox();
            float penX = std::min(full.getRight(), getRight()) - std::max(full.getLeft(), getLeft());
            float penY = std::min(full.getTop(),   getTop())   - std::max(full.getBottom(), getBottom());
            // Threshold measured against real captures (2026-08-06): truth 144641895
            // (Robot, block id=1) and 77236587 (Cube, block id=3) both killed gdsim
            // exactly 3 frames early at the old 6u threshold — X-penetration was the
            // binding side in both (Y was already well past it), climbing ~1.3u/frame
            // in both cases, so the true real-GD threshold sits where gdsim's penX at
            // the REAL death frame does: ~11.0u (144641895) and ~10.6u (77236587) —
            // narrowing to 9.7-10.6u once cross-checked against both levels together.
            // Also lines up with the original comment's own reference point ("9.7u
            // deep" at truth 144641895's real death) once frame-indexing is aligned.
            // 10.0 sits inside both levels' required range.
            constexpr float kSideSmashPen = 10.f;
            if (penX > kSideSmashPen && penY > kSideSmashPen) {
                p.dead = true; p.deathCause = "block-side";
                p.deathObjType = typeId; p.deathObjPos = pos;
            }
        }
    }
}

} // namespace gdsim
