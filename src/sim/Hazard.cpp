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

// Real spike/hazard sprites are NOT the full bounding box — Entity::intersects (used
// below) is a plain AABB test, the same class of shape bug already fixed for
// Slope::touching() this session. A rotated or scaled decorative spike's wide CORNERS
// (away from the actual point) register as lethal even when the true hazard has no
// presence there.
//
// FOUND 2026-08-07 via gdrcheck.exe (test/gdrcheck.cpp) replaying Delirium.gdr — a
// REAL, human-verified-clearing run of level 68839068 — through gdsim: it false-killed
// at frame 224 on a scaled (0.2x), rotated (-180°) id=217 spike, a corner-only overlap
// (player box only 2.4u into the object's 6.4u width). Disabling id=217 collision
// entirely made gdsim replay the ENTIRE real clear cleanly end to end (x=6050/6055) —
// this single shape gap was THE dominant remaining blocker for that level, and very
// likely for other levels using rotated/scaled decorative spikes (extremely common in
// wave/flight corridors).
//
// PREVIOUSLY (2026-08-07..09): a full inscribed SAT triangle (apex-up at rotation 0)
// was a GUESS, and was confirmed wrong two ways: (1) it never fully resolved Delirium
// (still died at f224 even shrunk to 40% scale), (2) gdsim_gaps_from_gdcs_docs memory
// (GD Creator School, read 2026-07-17 but not cross-checked before that fix) documents
// real spikes as "small rect hitbox raised ~¼ up (not full triangle)" — a fundamentally
// smaller/different shape. Replaced 2026-08-09 with that documented rect model: a box
// centred at height size.y/4 above the object's base (along the LOCAL +Y before
// rotation, i.e. rotates with the spike).
//
// Width calibration A/B, 2026-08-09: swept hw from size.x*0.30 down to size.x*0.18 and
// re-ran the FULL 150-level real-macro-demonlist batch (testlevel/macrolist/, all
// human-verified 100% clears) for each — the death cause/count breakdown (66 hazard,
// 30 sawblade, 26 block, 16 block-side, 8 bounds, 4 slope-hazard) was IDENTICAL at
// both widths, meaning this parameter is not the bottleneck for real levels: almost
// every hazard death in that batch is a deep, unambiguous overlap, not a sub-unit
// shape-edge graze. Delirium (68839068) f224 stayed a graze of ~0.6u at 0.30 and
// didn't clear even at 0.18 (see git history) — per slope_wave_fidelity_and_autorepair_speed
// memory this specific pinch was already flagged as *likely genuine frame-perfect
// design*, not a gdsim bug, so it was deliberately NOT used to keep shrinking this
// number past what real evidence supports. Settled at 0.22 (documented "small",
// unchanged from the two bracketing values either side). If a future capture finds a
// real reason to move this, re-validate with the SAME whole-batch A/B method, not a
// single level. The batch's real dominant failure mode is still open — see
// testlevel/macrolist/results_2026-08-09.tsv and the roadmap in its README.
static bool hazardRectHit(const Hazard& h, const Entity& box) {
    const float hw = h.size.x * 0.22f, hh = h.size.y * 0.25f;
    const float cy = h.size.y * 0.25f; // rect centre, offset up from the object's base
    const Vec2D rc[4] = {
        Vec2D(-hw, cy - hh).rotate(h.rotation) + h.pos,
        Vec2D( hw, cy - hh).rotate(h.rotation) + h.pos,
        Vec2D( hw, cy + hh).rotate(h.rotation) + h.pos,
        Vec2D(-hw, cy + hh).rotate(h.rotation) + h.pos,
    };
    const Vec2D corners[4] = {
        {box.getLeft(),  box.getBottom()}, {box.getRight(), box.getBottom()},
        {box.getRight(), box.getTop()},    {box.getLeft(),  box.getTop()},
    };
    auto project = [](const Vec2D* pts, int n, Vec2D axis, float& lo, float& hi) {
        lo = hi = pts[0].x * axis.x + pts[0].y * axis.y;
        for (int i = 1; i < n; ++i) {
            float d = pts[i].x * axis.x + pts[i].y * axis.y;
            lo = std::min(lo, d); hi = std::max(hi, d);
        }
    };
    const Vec2D axes[4] = {
        {1.f, 0.f}, {0.f, 1.f},
        {-(rc[1].y - rc[0].y), (rc[1].x - rc[0].x)},
        {-(rc[2].y - rc[1].y), (rc[2].x - rc[1].x)},
    };
    for (auto& ax : axes) {
        float bl, bh, tl, th;
        project(corners, 4, ax, bl, bh);
        project(rc, 4, ax, tl, th);
        if (bh < tl || th < bl) return false; // separating axis found — no overlap
    }
    return true;
}

void Hazard::collide(Player& p) const {
    // REVISED 2026-08-11 (explicit, repeated user instruction: base this ONLY on
    // GD Creator School's "Advanced Hitboxes" #1 "The Player Hitbox", not on this
    // project's own prior empirical calibration): a hazard kills when the player's
    // MAIN hitbox — the full 30×30 (18×18 mini) box, always axis-aligned —
    // intersects it. Per the doc: the Main hitbox "collides with... spikes if they
    // are facing upward"; the Solid hitbox (the small one, previously used here)
    // "will only collide with solid objects and slopes" — it isn't in the hazard
    // path at all.
    //
    // KNOWN, ACCEPTED TRADE-OFF: this reintroduces a previously-fixed false-death
    // on truth level 98414841 (a ship grazes a spike's vertical band at f299/f1541
    // with the big box; the small box cleared both by >9u — see test/regress.sh,
    // now REGRESSED there by design). The hazard's own small rect (hazardRectHit,
    // hw=0.22*sizeX etc.) is untouched — it comes from a different source (the
    // "gameplay-objects" page's spike-shape description) and was NOT re-derived
    // for pairing with this bigger player box; only the PLAYER hitbox choice
    // changed, per direct instruction to trust the doc over the old capture-based
    // fix. No decompiled hazard-collision function exists to independently confirm
    // this (checked 2026-08-11, inconclusive).
    if (!hazardRectHit(*this, p.unrotatedHitbox())) return;
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
