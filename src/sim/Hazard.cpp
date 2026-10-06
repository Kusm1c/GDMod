#include "Hazard.hpp"
#include "Player.hpp"
#include <algorithm>
#include <cmath>

namespace gdsim {

Hazard::Hazard(Vec2D s, std::unordered_map<int, std::string>&& fields) : Object(s, std::move(fields)) {
    prio = 2;
}

// SAWBLADES — radius and shape both re-derived from the binary 2026-09-12.
// The full rule set and its evidence live inside Sawblade::touching below; this
// header is the history, because two earlier passes landed on the wrong answer
// from level evidence alone and the trail is worth keeping:
//
//  * 2026-08-06 found gdsim killing 15.6u away from a blade on a real clearing
//    replay (Delirium.gdr, level 68839068) and concluded `size` was a DIAMETER,
//    adding a /2. The observation was real; the diagnosis was not. `size` here
//    holds GD's m_objectRadius, so the /2 made ~30 blade ids half-lethal, and
//    the false death it was chasing came from the SHAPE (next point), not the
//    radius.
//  * 2026-08-05 replaced a corner-only overlap test with the textbook
//    clamp-to-box closest-point test, on the reasoning that the corner test
//    "never handled the very common case where the closest point is on a flat
//    EDGE". That case is real — and GD genuinely does not handle it either.
//    playerCircleCollision has no closest-point term at all.
//  * 2026-08-10/11 then moved the PLAYER box between innerHitbox() and
//    unrotatedHitbox(); unrotatedHitbox() is correct and is confirmed
//    independently — PlayerObject overrides getOrientedBox() without setting
//    m_shouldUseOuterOb, so the player's rect is never rotation-inflated.
//
// Net: the shape was made more lethal than the engine and the radius less, and
// the two errors partly cancelled on the levels each was tested against.
bool Sawblade::touching(Player const& p) const {
    // The table in Object.cpp holds GD's m_objectRadius DIRECTLY, so this is the
    // radius — there is no /2. Verified 2026-09-12 against the binary: blade
    // radii are set in EnhancedGameObject::customSetup (0x1401a4f70) as either a
    // hardcoded float or `m_width * k`, with m_width coming from
    // GameObject::setupSpriteSize (0x1401a36a0):
    //     88,186,740,1705 -> 32.3      397 -> 28.9      675 -> 32.0
    //     678 -> 30.4      1619 -> 25.0      1620 -> 15.0      1582 -> 4.0
    //     89,183,187,679,741 -> m_width*0.36     184,676 -> m_width*0.34
    //     398,677 -> m_width*0.32                everything else -> m_width*0.30
    // Twenty of the ids in this table reproduce their engine radius EXACTLY from
    // size.x (89 = 60*0.36 = 21.6, 677 = 39*0.32 = 12.48, 98 = 40*0.30 = 12.0 …)
    // and NOT ONE reproduces it from size.x/2.
    //
    // So the old `isBigRadiusSawblade(typeId) ? size.x : size.x/2` was halving a
    // value that was already a radius, for every id outside its six-entry list —
    // roughly thirty blade ids running at half their real danger radius. That
    // list, and the long comment that used to justify it, are gone with it.
    float radius = size.x;
    Entity box = p.unrotatedHitbox();

    // GJBaseGameLayer::playerCircleCollision (0x140211df0), transcribed. GD does
    // NOT do the textbook circle-vs-rect test. It checks exactly two things:
    //
    //     if (playerRect.containsPoint(circleCentre)) -> hit
    //     for each of the rect's FOUR CORNERS:
    //         if (distance(corner, circleCentre) < radius) -> hit
    //     otherwise -> miss
    //
    // There is no "closest point on the rect" term, so a circle approaching a
    // FACE head-on — centre outside the rect, nearest point in the middle of an
    // edge, every corner further away than the radius — does not collide at all.
    // Worked example with the full-size 30x30 player and a radius-16 blade
    // centred 8 units above the top edge: nearest edge point is 8 away (the
    // textbook test says hit), but both top corners are sqrt(15^2+8^2) = 17.0
    // away, so GD says MISS.
    //
    // gdsim used the closest-point form, which is strictly more lethal than the
    // engine on exactly that geometry — a flat approach to a blade — and that is
    // the single most common way to meet one. Replaced with the engine's own
    // test; this can only ever remove deaths, never add them.
    const float cx = pos.x, cy = pos.y;
    if (cx >= box.getLeft() && cx <= box.getRight() &&
        cy >= box.getBottom() && cy <= box.getTop()) return true;

    const float r2 = radius * radius;
    const float xs[2] = { box.getRight(), box.getLeft() };
    const float ys[2] = { box.getTop(),   box.getBottom() };
    for (float x : xs)
        for (float y : ys) {
            const float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy < r2) return true;
        }
    return false;
}

// GJBaseGameLayer::checkCollisions' hazard pass (0x1402137f0, after the solids): for
// each hazard with no radius, an inclusive AABB test of the player's full
// getObjectRect() against the hazard's getObjectRect(), then — only when the hazard
// is oriented (rotation not a multiple of 90) — OBB2D::overlaps1Way both ways. That
// is exactly Object::touching()'s SAT on the table rects (already GD's real
// getObjectRect: id 8 = 6x12, 9 = 9x10.8, 39 = 6x5.6, capture-verified). There is no
// further "spike shape" rect: the old hazardRectHit (0.22*w x 0.5*h raised h/4, from
// GD Creator School prose about the 30x30 SPRITE) shrank these real rects a second
// time — gdsim's lethal box for id 8 was 2.64x6 instead of 6x12. Proven on truth
// 274283 f2170: the real cube dies with its right edge 0.24u into spike (2835,105)'s
// 6-wide rect (f2169: 1.06u short), gdsim — on the exact same position — survived.

void Hazard::collide(Player& p) const {
    // Reaching here means touching() found the overlap: that is the engine's whole test.
    // Diagnostic bypass, same purpose/pattern as Sawblade::collide's
    // GDSIM_DIAG_NOSAWBLADE below: lets a replay run PAST a non-sawblade hazard
    // death to study a LATER section in isolation (e.g. a transition thousands of
    // units further into the level that an earlier, separately-tracked death
    // otherwise makes unreachable in one pass). Never affects a real
    // Solve/Watch/regression run.
    if (getenv("GDSIM_DIAG_NOHAZARD")) return;
    p.dead = true; p.deathCause = "hazard";
    p.deathObjType = typeId; p.deathObjPos = pos;
}

void Sawblade::collide(Player& p) const {
    // Diagnostic bypass (2026-08-21): when investigating a real-.gdr2-clears-but-
    // gdsim-dies case, disabling JUST sawblade death lets a replay run PAST one to
    // see whether the rest of the level is even reachable / where the NEXT problem
    // is — see test/transtrack.cpp. Never affects a real Solve/Watch/regression run.
    if (getenv("GDSIM_DIAG_NOSAWBLADE")) return;
    p.dead = true; p.deathCause = "sawblade";
    p.deathObjType = typeId; p.deathObjPos = pos;
}

} // namespace gdsim
