#include "Hazard.hpp"
#include "Player.hpp"
#include <algorithm>
#include <cmath>

namespace gdsim {

Hazard::Hazard(Vec2D s, std::unordered_map<int, std::string>&& fields) : Object(s, std::move(fields)) {
    prio = 2;
}

// Circle (this sawblade, centre `pos`, radius size.x/2) vs the player's axis-aligned
// hitbox. `size` is the sawblade's bounding-box DIAMETER (Object.cpp's factory always
// gives it a square size, e.g. Sawblade 32.3x32.3) — using the full size.x AS the
// radius (the pre-existing code, both before and immediately after the geometry fix
// below) doubles the true danger radius, quadrupling the swept area. FOUND 2026-08-06
// via a real, GD-verified-clearing replay (Delirium.gdr, level 68839068) that gdsim
// falsely killed at frame 164/4% — cross-checked the exact geometry: the player's box
// was a clean 15.6u BELOW the sawblade's own visual box (no overlap at all by eye),
// yet the old radius=size.x circle (32.3, reaching ~32u from centre) still caught it;
// radius=size.x/2 (16.15) does not. This affects EVERY sawblade in every level —
// likely the dominant reason gdsim has been rating countless real, clearable wave/
// flight sections as impossible.
//
// FIXED 2026-08-05 (geometry, kept from that pass): the old test only checked "any
// player-box CORNER within radius" plus "circle centre strictly inside the box" — it
// never handled the (very common) case where the closest point on the box is on a
// FLAT EDGE, not a corner. Standard clamp-to-box closest-point test; correct for all
// cases once paired with the correct radius above.
// FOUND 2026-08-10 (level 127323087 "Society"): this used the player's RAW `size`
// (getLeft/Right/Bottom/Top read the 30×30, mini-scaled-18×18, "cube's 30/18"
// bounding box — see Player::Player()'s default and Level.cpp:74's comment) —
// NOT the small ~9×9 inner death hitbox every OTHER hazard test in this file
// correctly uses (see Hazard::collide's own comment: "a hazard kills when the
// player's SMALL inner death hitbox ... intersects it, NOT the full 30×30 icon
// box"). A sawblade is exactly as lethal-hitbox-small as a spike in real GD —
// there's no reason it would use a 3x-larger player box. Confirmed via a real,
// human-verified-clearing macro: it clipped a sawblade using the oversized box
// while the small inner hitbox (checked by hand) does not overlap at all.
bool Sawblade::touching(Player const& p) const {
    float radius = size.x / 2.0f;
    Entity inner = p.innerHitbox();
    float closestX = std::clamp(pos.x, inner.getLeft(), inner.getRight());
    float closestY = std::clamp(pos.y, inner.getBottom(), inner.getTop());
    float dx = pos.x - closestX;
    float dy = pos.y - closestY;
    return (dx * dx + dy * dy) <= radius * radius;
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
    // gdsim model: a hazard kills when the player's SMALL inner death hitbox —
    // ~9×9 (mini-scaled) — intersects it, NOT the full 30×30 icon box that the
    // broad-phase touching() uses. NOTE (2026-08-06): this was previously credited to
    // PlayerObject::collidedWithObjectInternal, but a full read of that function shows
    // `objPassable` objects (hazards) hit an early `return 0` — hazards must be
    // governed by a DIFFERENT, simpler real function this project hasn't decompiled
    // yet. This innerHitbox model is kept because it's validated (13/13 regression,
    // and fixed a real false-death: a flying ship's 30-tall hitbox merely swept a
    // spike's vertical band on level 98414841, false-dying at f299/f1541 — the real
    // 9×9 box clears both by >9u) — but treat it as an approximation, not a port, and
    // do NOT add vehicle-specific graze leniency here without decompiled evidence (a
    // wave-specific exception was tried and reverted this session — no basis found;
    // waves are well known to always die on hazard contact, no exceptions).
    if (!hazardRectHit(*this, p.innerHitbox())) return;
    p.dead = true; p.deathCause = "hazard";
    p.deathObjType = typeId; p.deathObjPos = pos;
}

void Sawblade::collide(Player& p) const {
    p.dead = true; p.deathCause = "sawblade";
    p.deathObjType = typeId; p.deathObjPos = pos;
}

} // namespace gdsim
