#include "Block.hpp"
#include "Slope.hpp"
#include "Level.hpp"
#include "Player.hpp"
#include <cmath>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace gdsim {

Block::Block(Vec2D s, std::unordered_map<int, std::string>&& fields) : Object(s, std::move(fields)) {
    prio = 1;
    if ((int)fabs(rotation) % 180 != 0) size = {size.y, size.x};
    rotation = 0;
    if (fields[1] == "468" && size.y == 5) size.y -= 3.5f;
}

// TRIED 2026-08-17 (real Watch capture, level 2997354 "DeCode"): a fast-falling
// Ship's hitbox exits a block's X-range ~0.5u before its Y-descent would have closed
// a ~3-4u gap to the block's top; real GD lands it there (m_yVelocity snaps to 0 for
// 3 frames at the position this block's top implies), gdsim's ship sails past because
// Object::touching()'s plain current-frame AABB test never returns true, so
// Block::collide()'s own landing threshold (`clip`) never runs to evaluate it. The
// decompiled PlayerObject::collidedWithObjectInternal (src/gdp-2.2/PlayerObject/
// PlayerObject_collidedWithObjectInternal.cpp:27-141) shows real GD's own candidate
// gate isn't a bare AABB either — `maxSnapY = snapUpThreshold + playerBottom`,
// further extended by the player's per-frame fall distance — evaluated BEFORE the
// fine test. A Block::touching() override widening the gate by `clip` (matching
// collide()'s own already-calibrated threshold, so in theory not a new leniency)
// regressed 6/13 truth levels (146, 174, 21227933, 308, 77236587, 98414841 — several
// MUCH worse, e.g. 174 sev 1.00->9.37), so real GD's actual gate is doing something
// more selective than a flat clip-sized pad in every direction — likely genuinely
// incorporating X-alignment/velocity-direction the way `floatG`/`adjustedYDelta` do,
// not just a bigger box. Reverted; DeCode's specific corner-miss stays open. Do not
// retry with a flat symmetric pad — need to actually port the decompile's directional
// snap math (or get grounded-flag re-scanner truth) before touching this again.
// PlayerObject::checkSnapJumpToObject (0x140393cb0), ported from the disassembly
// (every compare is a float ucomiss/comiss against .rdata; the snap itself is double).
// collidedWithObjectInternal calls it each time it places a CUBE on top of a solid —
// the landing frame AND every grounded frame after, once per object, in collision
// order — with prev = m_objectSnappedTo:
//
//   if prev && prev.uid != obj.uid && prev.type == Solid
//      && (dx,dy) matches a stair (up,+30) / (down,-30) / (up2,+60), dy sign-flipped
//         when upside down, within tol:
//       target = (double)obj.x + m_snapDistance, clamped to player.x +- tol
//   m_objectSnappedTo = obj;  m_snapDistance = (double)(player.x - obj.x)
//
// Per m_playerSpeed (tol/up/down/up2): 0.7 1/90/120/60, 0.9 1/120/150/90,
// 1.1 2/150/195/120, 1.3 2/180/225/135, anything else (4x) 1/120/150/90.
// `up` drops to 90 when m_vehicleSize != 1, except at 4x which skips that test.
// The old gdsim table (from GDCS prose) had big-3x up=90 and big-4x = 225/180/135.
static void checkSnapJumpToObject(Block const& b, Player& p) {
    auto& sd = p.snapData;
    if (sd.objectId >= 0 && sd.objectId != b.id && sd.objectSolid) {
        float tol = 1.f, up = 120.f, down = 150.f, up2 = 90.f;
        bool sizeTest = true;
        switch (p.speed) {
            case 0:  up = 90.f;  down = 120.f; up2 = 60.f;             break;
            case 1:                                                    break;
            case 2:  up = 150.f; down = 195.f; up2 = 120.f; tol = 2.f; break;
            case 3:  up = 180.f; down = 225.f; up2 = 135.f; tol = 2.f; break;
            default: sizeTest = false;                                 break;
        }
        if (sizeTest && p.small) up = 90.f;
        const float s = p.upsideDown ? -1.f : 1.f;
        const float ox = sd.object.pos.x, oy = sd.object.pos.y;
        const float nx = b.pos.x,         ny = b.pos.y;
        const bool stair =
            (std::fabs(nx - (ox + up))   <= tol && std::fabs(ny - (s * 30.f + oy)) <= tol) ||
            (std::fabs(nx - (ox + down)) <= tol && std::fabs(ny - (oy - s * 30.f)) <= tol) ||
            (std::fabs(nx - (ox + up2))  <= tol && std::fabs(ny - (s * 60.f + oy)) <= tol);
        if (stair) {
            double target = (double)nx + sd.snapDX;
            const double px = (double)p.pos.x, t = (double)tol;
            if (std::fabs(target - px) > t) target = target > px ? px + t : px - t;
            p.pos.x = (float)target;
        }
    }
    sd.object      = b;
    sd.objectId    = b.id;
    sd.objectSolid = dynamic_cast<BreakableBlock const*>(&b) == nullptr;
    sd.snapDX      = (double)(p.pos.x - b.pos.x);
}

void Block::collide(Player& p) const {
    if (getenv("GDSIM_COLLIDE_ENTRY_DEBUG") && pos.x > 21990.f && pos.x < 22150.f)
        std::fprintf(stderr, "COLLIDE-ENTRY f=%d typeId=%d objPos=(%.2f,%.2f) playerXY=(%.2f,%.2f) veh=%d\n",
                     p.frame, typeId, pos.x, pos.y, p.pos.x, p.pos.y, (int)p.vehicle.type);
    // D block (id 1755): a wave-safe solid (a special "letter block", handled outside
    // the generic collision function this file otherwise ports — GD Creator School
    // confirms D-blocks are a distinct special case). Real GD lets a WAVE touch a D
    // block without dying. Grounded modes fall through to normal handling below.

    // WAVE: REVERTED 2026-08-11 (explicit, repeated, forceful user instruction: the
    // wave dies on ANY collision — block or slope — no exception whatsoever except a
    // D block, full stop). Earlier the same day this went through two more nuanced
    // models (a dartSlide "established sliding" grace, ported loosely from the
    // decompiled collidedWithObjectInternal/collidedWithSlopeInternal's
    // `m_stateDartSlide` gate) — both still let the wave survive touching plain solid
    // geometry under some condition, which the user directly confirmed does not match
    // real GD: a wave's difficulty comes specifically from ANY touch being lethal:
    // real wave corridors are threaded WITHOUT actually overlapping either surface,
    // not ridden the way a cube rests on a platform. Simplified to the literal rule:
    // unconditional death on touch, D block (Block::collide's own early return above)
    // the only exception.
    // collidedWithObjectInternal: `if (isDart && m_stateDartSlide < 1) -> death path`.
    // With a D block's slide state active the wave takes the ordinary solid path below
    // (it lands on / slides along the surface, flying snap threshold).
    if (p.vehicle.type == VehicleType::Wave && p.stateDartSlide < 1) {
        if (p.blockDeathHitbox().intersects(*this)) {
            p.dead = true; p.deathCause = "block";
            p.deathObjType = typeId; p.deathObjPos = pos;
        }
        return;
    }

    // Real GD (PlayerObject::collidedWithObjectInternal): snapUpThreshold = 10 for
    // non-flying modes (Cube/Ball/Robot/Spider), 6 for flying modes (Ship/Ufo/Swing;
    // Wave has its own death-hitbox path below and never uses this clip).
    int clip = (p.vehicle.type == VehicleType::Ufo || p.vehicle.type == VehicleType::Ship
                || p.vehicle.type == VehicleType::Swing || p.vehicle.type == VehicleType::Wave) ? 6 : 10;

    if (p.upsideDown != p.prevPlayer().upsideDown && !p.gravityPortal) return;

    double bottom = p.gravBottom(p);
    if (p.slopeData.slope) {
        if (getenv("GDSIM_SHIPCEIL_DEBUG"))
            std::fprintf(stderr, "SHIPCEIL f=%d typeId=%d objPos=(%.2f,%.2f) grippedSlopeAngle=%.3f "
                         "playerXY=(%.2f,%.2f) gravTopThis=%.2f bottom=%.2f\n",
                         p.frame, typeId, pos.x, pos.y, p.slopeData.slope->angle(),
                         p.pos.x, p.pos.y, p.gravTop(*this), bottom);
        if (p.slopeData.slope->angle() > 0) {
            bottom = bottom + sin(p.slopeData.slope->angle()) * p.size.y / 2;
            clip = 7;
            if (p.gravTop(*this) - bottom < 2) return;
        }
    }

    bool padHitBefore = (!p.prevPlayer().grounded && p.prevPlayer().velocity <= 0 && p.velocity > 0);

    // REVERTED 2026-08-11 (same day): this used to also OR in a swept/subframe
    // check against last frame's position, built to explain a real playtest
    // report (level 2997354 "DeCode": passing clean through a 1.5-unit-thin
    // wall). Reading the actual decompiled PlayerObject::collidedWithObjectInternal
    // (src/gdp-2.2/PlayerObject/PlayerObject_collidedWithObjectInternal.cpp:
    // 616-619) shows the real death check is a PURE single discrete test —
    // `objRect.intersectsRect(getObjectRect(0.3, 0.3))` at the CURRENT position
    // only, no previous-position/sweep term anywhere in the function (the
    // `getLastPosition()` reads earlier in it are for a MOVING PLATFORM's own
    // velocity, not the player's). GD's documented "subframes" (GD Creator
    // School "Advanced Hitboxes") are GJBaseGameLayer::update's stepCount loop
    // reconciling variable render FPS with the fixed 240Hz physics tick — gdsim
    // already simulates purely in 240Hz-tick space with no render-frame
    // coarsening, so it already has that mechanism by construction and was
    // never missing it. The thin-wall pass-through is therefore not a gdsim
    // bug: real GD's own finest-resolution collision check has the identical
    // gap for a wall thinner than one frame's travel. Restored to the plain
    // discrete check this decompiled function actually performs.
    Entity curHb = p.blockDeathHitbox();
    bool blockHit = curHb.intersects(*this);
    constexpr float kSolidGraze = 0.75f;
    // GD edge-graze leniency for solid vehicles (cube/robot/spider/ball): sliding
    // PAST a block edge with a sub-unit overlap in the perpendicular axis is
    // survivable in real GD (the famous corner-clip). Forgive when the overlap is
    // tiny in EITHER axis — that's a graze along a face/corner, not a real hit. A
    // deep overlap in BOTH axes (a true landing or wall-smash) still kills, so the
    // calibrated 7×7 stays strict on platforms. Tolerance is well under the wave's
    // 1.0 to keep the box maximally strict for the cube's tight-platform calibration.
    // (Mini robot on 13711278 ~f2437: rising past a ledge, its death box grazed the
    // top-left edge by penY≈0.5 — real survives, gdsim's bare AABB false-killed it.)
    float penX = std::min(curHb.getRight(), getRight()) - std::max(curHb.getLeft(), getLeft());
    float penY = std::min(curHb.getTop(),   getTop())   - std::max(curHb.getBottom(), getBottom());
    if (getenv("GDSIM_BLOCKDEATH_DEBUG"))
        std::fprintf(stderr, "BLOCKAPPROACH f=%llu typeId=%d rawHit=%d penX=%.3f penY=%.3f playerXY=(%.2f,%.2f)\n",
                     (unsigned long long)p.frame, typeId, curHb.intersects(*this), penX, penY, p.pos.x, p.pos.y);
    if (std::min(penX, penY) <= kSolidGraze) blockHit = false;

    // REMOVED 2026-08-14 (real playtest, level 2997354 "DeCode", user flagged
    // typeId=468 at (870.75,105) via the app's J-flag tool — the TOP segment of
    // the same stacked wall fixed on 2026-08-12, this time right where it meets
    // slope id=665 above it, box bottom=120): a block-to-slope "seam" leniency
    // used to live here — `for (auto& entity : p.potentialSlopes) { ... if
    // (block_comp - slope_comp < 2) return; }`, unconditionally skipping this
    // block's ENTIRE death check whenever some nearby touched slope's edge was
    // within 2 units of this block's own edge, with NO check on the player's
    // actual position or penetration depth at all. Intent was presumably to
    // forgive a technical hit at a clean architectural join (wall leading
    // straight into a ramp) — but that case is already covered by the graze
    // check just above (any shallow hit is forgiven regardless of a nearby
    // slope). This rule's only ADDITIONAL effect beyond the graze check was
    // forgiving DEEP hits whenever a slope merely happened to be broadly
    // "potential" (touching()'s own broad-phase reaches roughly ±15 units
    // around a slope's box, not just its exact seam) — exactly the bug: a
    // player ramming solidly into the middle of this wall segment near the
    // slope above it sailed through untouched. No comment here ever cited a
    // specific truth level or decompiled source for this rule (unlike nearly
    // everything else in this file), so removed outright rather than guessing
    // at a narrower gate — the graze check already handles the legitimate case.
    // Pre-existing overlap (the player was ALSO deeply inside this exact block on the
    // PREVIOUS frame) is not a new collision — GD kills on the TRANSITION into contact,
    // not a static "currently overlapping" state (same discriminator already used for
    // orb touch timing, see orb_touch_timing_sameframe — touching(prevPlayer())).
    // FOUND 2026-08-08 by batch-replaying ~150 real, human-verified-clearing macros
    // (Paul's Macro Demonlist) through gdsim via test/gdrcheck.cpp: "block" was by far
    // the single most common false-death cause, overwhelmingly on frame 1 — a level's
    // spawn point sitting flush against/inside a placed ground-row block (extremely
    // common, purely decorative in that position) killed the run before any real
    // collision could occur. Two unrelated levels (BOOBAWAMBA, Saul Goodman) showed
    // the IDENTICAL frame-1 death coordinates, confirming this is systemic, not a
    // per-level geometry fluke.
    //
    // notNewCollision tracks WHY blockHit went false here, separately from the
    // graze-tolerance case above. FOUND 2026-08-10 (macro-demonlist level 82172844
    // "Cobwebs", ground truth Cobwebs.gdr2): once the small 7×7 hitbox is fully
    // swallowed by a block (common right after a gravity-flip orb's same-frame Y
    // jump), it reports the SAME saturated penX/penY on every subsequent frame, so
    // this rule keeps forgiving it as "not new" frame after frame — correctly, since
    // by ITS OWN reasoning (no transition into contact) there's nothing to kill on.
    // But the code below used to fall through into the block-SIDE branch (the
    // oversized-hitbox rising-corner check) regardless of WHY blockHit was false,
    // re-evaluating the exact same continuing contact with a totally different
    // (much bigger) hitbox and re-killing it anyway — directly contradicting the
    // "not new, don't kill" verdict this rule just made. The block-side branch's
    // actual job is to catch what the GRAZE tolerance (above) wrongly excused, not
    // to second-guess this rule, so only let it run in that case.
    //
    // TRIED 2026-08-11 (same level/report): a "forgive only if penetration isn't
    // GROWING vs last frame" refinement. Reverted — it broke the ORIGINAL spawn-flush
    // case this rule exists for: at frame 1, prevPlayer() resolves to the
    // pre-simulation h[0] state, so even a stationary spawn overlap shows "growth"
    // from h[0] to frame 1, indistinguishable from genuine tunneling with only a
    // 1-frame lookback. 28 levels regressed. Superseded 2026-08-12 by gating on
    // PREVIOUS-frame penetration DEPTH instead of growth direction (see below) —
    // depth survives the frame-1 case (a real spawn-flush overlap is deep, not a
    // graze) without needing growth comparison at all.
    bool notNewCollision = false;
    if (blockHit) {
        Entity prevHb = p.prevPlayer().blockDeathHitbox();
        if (prevHb.intersects(*this)) {
            // FIXED 2026-08-12 (real playtest, level 2997354 "DeCode", user flagged the
            // exact wall via the app's new J-flag tool — typeId 468, a 1.5-unit-thin
            // solid stacked into a tall wall at x=870.75): this used to forgive ANY
            // previous-frame intersection as "pre-existing, don't kill" — but a raw
            // intersects() is true for a shallow EDGE GRAZE too, not just a genuine deep
            // overlap. A thin/fast wall gets tunnelled in exactly two frames: frame N-1
            // grazes the near face (forgiven above by kSolidGraze, but intersects() was
            // still true), frame N is already through to the far side and deep enough to
            // fail the graze check — and this rule then wrongly read frame N-1's mere
            // graze as "this block has ALWAYS been touching", forgiving the real hit and
            // letting the player pass clean through. Gate on the PREVIOUS frame's own
            // penetration depth instead of the raw boolean: only a previous frame that
            // was ALREADY a genuine (non-graze) overlap in both axes — the spawn-flush
            // case this rule exists for, where the player starts deeply embedded in a
            // decorative block before physics even runs — counts as "not new". A
            // previous frame that was itself only a graze is the first half of a tunnel,
            // not a resting state, and must not suppress this frame's real hit.
            float prevPenX = std::min(prevHb.getRight(), getRight()) - std::max(prevHb.getLeft(), getLeft());
            float prevPenY = std::min(prevHb.getTop(),   getTop())   - std::max(prevHb.getBottom(), getBottom());
            if (std::min(prevPenX, prevPenY) > kSolidGraze) {
                blockHit = false;
                notNewCollision = true;
            }
        }
    }
    // KNOWN TRADE-OFF (measured 2026-08-12 against the 157-level real-macro batch,
    // see local_rerun.sh): this closes the DeCode tunnel above but costs exactly one
    // real, human-verified clear — 79997992, frame~19203: penX grazes in at 0.607
    // (forgiven, under kSolidGraze) then deepens to 1.654 one frame later while
    // p.grounded flips false (a jump is landing/lifting off right at this contact) —
    // gdsim kills it now where it used to (accidentally) survive via the old
    // any-previous-touch forgiveness. Read as a DIFFERENT, pre-existing fidelity gap
    // (jump-liftoff Y not yet reflected when this frame's block collision is
    // checked, so the horizontal graze is evaluated one frame before the real jump
    // would have cleared it) that the old rule happened to paper over here, not
    // something this fix broke on its own terms — but it's open, unresolved, and a
    // real net loss on this one level. 156/157 net-preserved; DeCode's case (a fully
    // solid, gapless wall silently walked through) was unambiguously wrong under any
    // reading, so this trade was taken.

    // H block (SpecialBlock.hpp, id 1859 "Allow Head Collision"): lets Cube/Robot/
    // Spider survive touching a block's underside/side where they'd normally die.
    // Global per-frame flag rather than "is this SPECIFIC block H-flagged" (gdsim
    // has no per-block modifier association) — a safe approximation since an H
    // block is placed to neutralize exactly one nearby death, and a false-positive
    // (suppressing a DIFFERENT block's death the same frame) needs two solid blocks
    // overlapping the player at once, already a rare/degenerate case.
    if ((blockHit || notNewCollision) && getenv("GDSIM_BLOCKDEATH_DEBUG"))
        std::fprintf(stderr, "BLOCKDEATH-CHECK f=%llu typeId=%d objPos=(%.2f,%.2f) objSize=(%.2f,%.2f) "
                     "blockHit=%d notNewCollision=%d playerXY=(%.2f,%.2f) prevXY=(%.2f,%.2f) grounded=%d prevGrounded=%d\n",
                     (unsigned long long)p.frame, typeId, pos.x, pos.y, size.x, size.y, blockHit, notNewCollision,
                     p.pos.x, p.pos.y, p.prevPlayer().pos.x, p.prevPlayer().pos.y, p.grounded, p.prevPlayer().grounded);
    // FOUND 2026-08-17 (real Watch capture, level 2997354 "DeCode", full
    // per-frame real data): a KNIFE-THIN object (id 468, 1.5u wide — the same
    // stacked wall already fixed twice this project for tunnel/seam issues) let
    // the player's box "land on top" of it and snap Y up, freezing/interrupting
    // a fall the real capture shows continuing completely uninterrupted at
    // that exact spot (smooth, unbroken velocity — no grounding at all in
    // reality). Two separate instances confirmed: one a hairline 0.26u corner
    // graze, the other the object's FULL 1.5u width genuinely inside the
    // player's box — so this isn't about graze depth, it's that an object this
    // thin apparently never registers as a landable "floor" in real GD at all
    // (it reads as a wall/support you fall past, not a step you catch), no
    // matter how much of its sliver of width your box currently contains.
    // FIRST tried gating only a sub-kSolidGraze corner touch — regressed 3
    // truth levels (legit edge-of-platform landings on NORMAL-width blocks
    // also start as a small first-contact overlap; that's real, intended
    // platforming). So this exemption is scoped by WIDTH alone, not by
    // penetration depth: only objects too thin for any real platform in
    // Object.cpp's factory table (>=8u everywhere else) are exempt, and for
    // those, unconditionally — a real player was never meant to land on a
    // 1.5u knife-edge, full stop.
    constexpr float kThinObjectMaxWidth = 5.f;
    bool thinGraze = size.x < kThinObjectMaxWidth;

    // FOUND 2026-09-22 (real per-frame capture, level 2997354 "DeCode" x=24538,
    // user flagged typeId=661 via the app's J-flag tool; the deviation export
    // shows gdsim within 0.0005u of the real player for the whole approach, so
    // only the death RULE was wrong).
    //
    // Real GD has NO "a solid overlaps me, therefore I die" rule at all. The
    // whole solid pass was read end to end in the decompile:
    //   GJBaseGameLayer::collisionCheckObjects (0x140214960) does NOT handle
    //   GameObjectType 0/0x15 (solid) in its per-type switch — it pushes them
    //   into a separate list (m_collidedObjects, +0x634) and the caller then
    //   runs PlayerObject::collidedWithObject -> collidedWithObjectInternal
    //   (0x140391a70) on each. Neither that function nor hitGround/didHitHead/
    //   updateCollide contains a single destroyPlayer/playerDestroyed call
    //   (grepped whole-function). The ONLY destroyPlayer in the pass is in the
    //   HAZARD list loop (GameObjectType 2/0x2f). A solid can therefore only
    //   kill through PlayerObject::postCollision (0x14038d580), which is a
    //   CRUSH test: you must have been pushed up onto a floor (m_snapFloorY)
    //   AND down under a ceiling (m_snapCeilY) in the SAME frame, and
    //   `fabs(floor - ceil) < getObjectRect().height * 0.7` (0.8 in platformer).
    //
    // What collidedWithObjectInternal DOES do first is classify the object as a
    // FLOOR CANDIDATE: `objTop <= playerBottom + snapUpThreshold` (the same 10 /
    // 6 / 7 already ported into `clip` right above). An object that passes that
    // gate is a surface you are going to be stood on, not an obstacle — and it
    // stays a floor candidate while you are still RISING toward it; GD simply
    // does nothing until your velocity turns over, then snaps you onto its top.
    //
    // Measured on the real capture (GD coords, block 661 at y[210..225], mini
    // cube feet rising 216.855 -> 217.350): objTop-feet stayed 7.65..8.15 (always
    // under clip=10) for SIX frames while the full 18x18 hitbox was up to
    // 13.2 x 8.1 units inside the block, then frame 16975 snapped feet to exactly
    // 225.0 with grounded=1. gdsim killed on the FIRST of those frames because
    // the 7x7 inner box clipped the same corner by 1.00 x 1.25 — over
    // kSolidGraze, so the graze tolerance above could not save it.
    //
    // So: never let the inner-box overlap kill an object the landing branch below
    // would accept on geometry alone. This is not a new leniency — it reuses that
    // branch's own already-calibrated gate, and only removes the death for the
    // frames BEFORE the landing it is already committed to. thinGraze objects are
    // excluded exactly as they are from the landing branch (a 1.5u knife-edge is
    // never a floor in real GD), so their behaviour is unchanged.
    const bool floorCandidate = !thinGraze && (p.gravTop(*this) - bottom) <= clip;
    if (blockHit && floorCandidate) blockHit = false;

    if (blockHit && !p.touchingHBlock) {
        p.dead = true; p.deathCause = "block";
        p.deathObjType = typeId; p.deathObjPos = pos;
    } else if (p.gravTop(*this) - bottom <= clip && !thinGraze
               && (padHitBefore || p.velocity <= 0 || p.gravityPortal)) {
        p.pos.y = p.grav(p.gravTop(*this)) + p.grav(p.size.y / 2);
        if (!padHitBefore) p.grounded = true;
        if (p.slopeData.slope && p.slopeData.slope->angle() < 0) p.slopeData.slope = {};
        // Defensive (2026-08-18, real crash): p.level should always be set (see
        // Level::rollback's fix, same date), but a null check here is a free,
        // permanent guard against ANY future path that reaches collide() with an
        // improperly-linked Player — costs nothing in the normal case, and turns
        // a hard crash into "this frame's stair-snap cache just doesn't update".
        if (p.vehicle.type == VehicleType::Cube && p.level) {
            checkSnapJumpToObject(*this, p);
            p.snapData.playerFrame  = p.level->currentFrame();
            p.snapData.landingFrame = p.frame;
        }
    } else {
        if (p.vehicle.type == VehicleType::Ship || p.vehicle.type == VehicleType::Ufo || p.vehicle.type == VehicleType::Ball) {
            if (getenv("GDSIM_SHIPCEIL2_DEBUG"))
                std::fprintf(stderr, "SHIPCEIL2 f=%d typeId=%d objPos=(%.2f,%.2f) objSize=(%.2f,%.2f) "
                             "playerXY=(%.2f,%.2f) vel=%.3f gravTopP=%.3f gravBottomThis=%.3f gap=%.3f clip=%d willClamp=%d\n",
                             p.frame, typeId, pos.x, pos.y, size.x, size.y, p.pos.x, p.pos.y, p.velocity,
                             p.gravTop(p), p.gravBottom(*this), p.gravTop(p) - p.gravBottom(*this), clip,
                             (p.gravTop(p) - p.gravBottom(*this) <= clip - 1 && p.velocity > 0));
            if (p.gravTop(p) - p.gravBottom(*this) <= clip - 1 && p.velocity > 0) {
                p.pos.y = p.grav(p.gravBottom(*this)) - p.grav(p.size.y / 2);
                p.velocity = 0;
                // FOUND 2026-09-23 (level 13519 "The Nightmare", real per-frame
                // capture, ship at x=13764): this snap is the END state of the real
                // frame — updateJump already ran its `v += a*dt; y += v*dt` BEFORE
                // checkCollisions clamped — so real GD reports y EXACTLY on the
                // ceiling and yVel EXACTLY 0 for every frame the ship is held under
                // it (17 straight frames in the capture, y=345.000 / vy=0.000).
                // gdsim then ran postCollision on top: it re-added this frame's
                // gravity step to the zeroed velocity AND applied the semi-implicit
                // resync, which subtracts `grav(preFrameVelocity)*dt` — the whole
                // pre-snap ascent — dragging the ship 1.111u straight back off the
                // ceiling on the snap frame. That 1.1u error let the ship clip the
                // NEXT wall's ceiling corner ~100 frames later, reset its velocity a
                // second time, and end the corridor 37u above the real player: gdsim
                // flew the gap clean while the real run sank into the spike at
                // x=14034 (Simulate cleared, Watch died). Flag it so postCollision
                // leaves both the snapped Y and the zeroed velocity alone.
                p.ceilingSnapped = true;
            }
        } else if ((p.vehicle.type == VehicleType::Cube || p.vehicle.type == VehicleType::Robot
                    || p.vehicle.type == VehicleType::Spider) && p.grav(p.velocity) > 0 && !notNewCollision
                   && !p.touchingHBlock) {
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
            if (getenv("GDSIM_BLOCKSIDE_DEBUG"))
                std::fprintf(stderr, "BLOCKSIDE-CHECK f=%llu typeId=%d pos=(%.2f,%.2f) rot=%.2f size=(%.2f,%.2f) penX=%.2f penY=%.2f playerXY=(%.2f,%.2f) upsideDown=%d velraw=%.2f\n",
                             (unsigned long long)p.frame, typeId, pos.x, pos.y, rotation, size.x, size.y,
                             penX, penY, p.pos.x, p.pos.y, p.upsideDown, p.velocity);
            if (penX > kSideSmashPen && penY > kSideSmashPen) {
                p.dead = true; p.deathCause = "block-side";
                p.deathObjType = typeId; p.deathObjPos = pos;
            }
        }
    }
}

} // namespace gdsim
