#include "uifont.hpp"
#include "render.hpp"
#include "../../src/sim/Slope.hpp"
#include <cmath>
#include <algorithm>

namespace gdapp {

static constexpr float PI_F = 3.14159265358979323846f;

static Vector2 worldToScreen(float wx, float wy, const Camera2DState& cam, int screenW, int screenH) {
    return {
        screenW * 0.5f + (wx - cam.x) * cam.pixelsPerUnit,
        screenH * 0.5f - (wy - cam.y) * cam.pixelsPerUnit
    };
}

// Rotates a LOCAL-space point by `rotationDegrees` and translates by `origin`,
// mirroring gdsim::Vec2D::rotate() exactly (src/sim/util.cpp) — rotation
// fields throughout gdsim (Object::rotation, Player::rotation) are degrees,
// not radians (set directly from the level string's raw field 6), and this
// app previously mis-treated them as radians, which produced wrong angles
// for every rotated object.
static Vector2 rotateLocal(float lx, float ly, float rotationDegrees, float originX, float originY) {
    float rad = rotationDegrees * (PI_F / 180.f);
    float c = cosf(rad), s = sinf(rad);
    return {originX + lx * c - ly * s, originY + lx * s + ly * c};
}

static void drawWorldQuad(const Vector2 wpts[4], const Camera2DState& cam, int screenW, int screenH,
                           Color fill, Color outline, float outlineThick) {
    Vector2 s[4];
    for (int i = 0; i < 4; i++) s[i] = worldToScreen(wpts[i].x, wpts[i].y, cam, screenW, screenH);
    DrawTriangle(s[0], s[1], s[2], fill);
    DrawTriangle(s[0], s[2], s[3], fill);
    if (outlineThick > 0.f)
        for (int i = 0; i < 4; i++) DrawLineEx(s[i], s[(i + 1) % 4], outlineThick, outline);
}

// Real in-game GD colours for transporters (orbs/pads) and portals, so the
// app's hitbox-only view still reads at a glance which is which — added
// 2026-08-18 alongside the portal hitbox-size fix (Object.cpp) once real
// measured hitboxes made it worth actually drawing them instead of a small
// generic circle. Orb/pad colours are the game's own named colours (exact);
// vehicle-portal colours match each mode's real GD ring tint from memory —
// flag any that look wrong against a real capture, they're the least certain
// entries here.
static Color effectColor(int typeId) {
    switch (typeId) {
        // Transporters: orbs / pads (exact — these ARE named by colour in GD)
        case 36:  case 35:  return YELLOW;                // yellow orb / pad
        case 84:  case 67:  return SKYBLUE;                // blue orb / pad
        case 141: case 140: return MAGENTA;                // pink orb / pad
        case 1333: case 1332: return RED;                  // red orb / pad
        case 1022: return GREEN;                           // green orb
        case 1704: return ORANGE;                          // dash orb
        case 1751: return PURPLE;                           // gravity-flip orb
        case 1330: return BLACK;                            // black orb
        case 3004: return DARKPURPLE;                        // spider/teleport orb

        // Vehicle portals (mode entry rings)
        case 12:   return RAYWHITE;                         // cube
        case 13:   return SKYBLUE;                          // ship
        case 47:   return PURPLE;                           // ball
        case 111:  return ORANGE;                           // ufo
        case 660:  return LIME;                             // wave
        case 745:  return GREEN;                            // robot
        case 1331: return DARKPURPLE;                        // spider
        case 1933: return GOLD;                              // swing (2.2)
        case 2751: return RAYWHITE;                          // (rare vehicle-portal id)

        // Gravity / size / dual / teleport
        case 10:   return SKYBLUE;                          // gravity portal (normal)
        case 11:   return DARKBLUE;                          // gravity portal (flipped)
        case 99:   return PINK;                              // mini size portal
        case 101:  return MAGENTA;                            // big size portal
        case 286:  case 287: return SKYBLUE;                  // dual portal (both halves)
        case 747:  return ORANGE;                             // teleport portal
        case 45:   case 46:  return LIGHTGRAY;                // mirror portal on/off

        // Speed portals (all one teal family, distinguished by hitbox width only
        // — raylib has no predefined TEAL constant, so this is a literal RGBA).
        case 200:  case 201: case 202: case 203: case 1334: return Color{0, 180, 180, 255};

        default:   return RAYWHITE;
    }
}

// Exact typeId set gdsim's Object factory (src/sim/Object.cpp) constructs as
// a Sawblade — a CIRCLE hitbox (radius = size.x/2, see Hazard.cpp's
// Sawblade::touching), unlike every other hazard which uses a small offset
// rectangle (see isSawblade's sibling logic below). Kept in sync with that
// factory table by hand since the renderer can't see gdsim's C++ class
// hierarchy, only typeId/prio/size/rotation.
static bool isSawblade(int typeId) {
    switch (typeId) {
        case 88: case 186: case 740: case 1705:
        case 89: case 1706:
        case 98:
        case 183: case 184: case 185:
        case 187: case 741:
        case 188: case 742:
        case 397: case 1708:
        case 398: case 1709:
        case 399: case 1710:
        case 675: case 1734:
        case 676: case 1735:
        case 677: case 1736:
        case 678: case 679: case 680:
        case 918:
        case 1582: case 1583:
        case 1619: case 1620:
        case 1707:
        case 1701: case 1702: case 1703:
            return true;
        default:
            return false;
    }
}

// Exact typeId sets gdsim's Object factory (src/sim/Object.cpp) constructs as
// Slope / SlopeHazard. These inherit Block's prio=1 (Slope) even though their
// true shape is a right triangle, not the full bounding rect — and their
// `rotation` field is always reset to 0 in the constructor, using the
// separate `orientation` (0-3) field instead (not visible on the generic
// Object interface). Without special-casing these, they render as plain
// grey/solid rectangles: visually indistinguishable from a flat platform,
// i.e. "slopes are missing" (found 2026-08-10, real playtest report).
static bool isSlope(int typeId) {
    switch (typeId) {
        case 289: case 294: case 299: case 305: case 309: case 315: case 321: case 326:
        case 331: case 337: case 343: case 349: case 353: case 363: case 371: case 483:
        case 492: case 651: case 665: case 673: case 709: case 711: case 726: case 728:
        case 886: case 1338: case 1341: case 1344: case 1723: case 1743: case 1745:
        case 1747: case 1749: case 1906:
        case 291: case 295: case 301: case 307: case 311: case 317: case 323: case 327:
        case 333: case 339: case 345: case 351: case 355: case 364: case 366: case 367:
        case 372: case 484: case 493: case 652: case 666: case 674: case 710: case 712:
        case 727: case 729: case 887: case 1339: case 1342: case 1345: case 1724:
        case 1744: case 1746: case 1748: case 1750: case 1907:
            return true;
        default:
            return false;
    }
}
static bool isSlopeHazard(int typeId) { return typeId == 1717 || typeId == 1718; }

// Kept in sync by hand with Hazard.cpp's isBigRadiusSawblade: 675/1734 (32),
// 676/1735 (17.51) and 677/1736 (12.48) are one sawblade sprite at 3 editor
// scales and all use radius=size.x (not size.x/2 like every other sawblade
// id). 675/1734 and 677/1736 are directly capture/flag-evidenced; 676/1735 is
// the bracketed middle tier of the same explicit family. Same reason as
// isSawblade above: the renderer can't see gdsim's C++ class internals, only
// typeId.
static bool isBigRadiusSawblade(int typeId) {
    return typeId == 675 || typeId == 1734
        || typeId == 676 || typeId == 1735
        || typeId == 677 || typeId == 1736;
}

// Right-triangle vertices (LOCAL box space, box already centred at the
// object's own pos) for each of gdsim's 4 slope orientations — derived
// directly from Slope::touching()/expectedY()'s line-test formulas
// (src/sim/Slope.cpp): orientation 0/2 have the diagonal running
// bottom-left-to-top-right, 1/3 top-left-to-bottom-right; 0/1 are floor
// slopes (solid below the line), 2/3 are ceiling slopes (solid above).
static void slopeTriangle(int orientation, float hw, float hh, Vector2 out[3]) {
    Vector2 bl{-hw, -hh}, br{hw, -hh}, tl{-hw, hh}, tr{hw, hh};
    switch (orientation) {
        case 0: out[0] = bl; out[1] = br; out[2] = tr; break;
        case 1: out[0] = bl; out[1] = tl; out[2] = br; break;
        case 2: out[0] = bl; out[1] = tl; out[2] = tr; break;
        default: out[0] = tl; out[1] = br; out[2] = tr; break; // 3
    }
}

static void drawObject(const gdsim::Object* o, float x, float y, float rotDeg,
                        const Camera2DState& cam, int screenW, int screenH) {
    // Cull cheaply in world space before ever touching raylib.
    float halfSpan = (o->size.x + o->size.y) * 0.5f + 4.f;
    float viewHalfW = screenW * 0.5f / cam.pixelsPerUnit + halfSpan;
    if (std::fabs(x - cam.x) > viewHalfW) return;

    if (isSlope(o->typeId) || isSlopeHazard(o->typeId)) {
        // Safe: ObjectContainer's storage genuinely holds a Slope/SlopeHazard
        // instance whenever typeId matches this table (same type-erasure
        // pattern ObjectContainer::operator-> itself already relies on).
        const gdsim::Slope* slope = reinterpret_cast<const gdsim::Slope*>(o);
        float hw = o->size.x * 0.5f, hh = o->size.y * 0.5f;
        Vector2 local[3];
        slopeTriangle(slope->orientation, hw, hh, local);
        Vector2 s[3];
        for (int i = 0; i < 3; i++)
            s[i] = worldToScreen(x + local[i].x, y + local[i].y, cam, screenW, screenH);
        bool hazard = isSlopeHazard(o->typeId);
        Color fill = hazard ? Color{200, 60, 60, 220} : Color{90, 90, 100, 255};
        Color outline = hazard ? Color{255, 140, 140, 255} : Color{160, 160, 175, 255};
        DrawTriangle(s[0], s[1], s[2], fill);
        DrawLineEx(s[0], s[1], 1.5f, outline);
        DrawLineEx(s[1], s[2], 1.5f, outline);
        DrawLineEx(s[2], s[0], 1.5f, outline);
        return;
    }

    if (o->prio == 1) {
        // Solid block: the object's full bounding box IS the real hitbox here
        // (Block.cpp collides against it directly, mod a small graze
        // tolerance that isn't worth drawing at this scale).
        // D block (id 1755, GD Creator School "special letter block" — see
        // SpecialBlock.hpp): the one block a WAVE can safely touch. Highlighted
        // distinctly since it's otherwise an invisible-in-game modifier and this
        // is exactly the thing the user's original wave-through-wall report
        // needed to see.
        bool dBlock = (o->typeId == 1755);
        Color fill    = dBlock ? Color{30, 130, 150, 210} : Color{90, 90, 100, 255};
        Color outline = dBlock ? Color{90, 220, 235, 255} : Color{160, 160, 175, 255};
        float hw = o->size.x * 0.5f, hh = o->size.y * 0.5f;
        Vector2 wpts[4] = {
            rotateLocal(-hw, -hh, rotDeg, x, y), rotateLocal(hw, -hh, rotDeg, x, y),
            rotateLocal(hw, hh, rotDeg, x, y),   rotateLocal(-hw, hh, rotDeg, x, y),
        };
        drawWorldQuad(wpts, cam, screenW, screenH, fill, outline, 1.5f);
        if (dBlock) {
            Vector2 c = worldToScreen(x, y, cam, screenW, screenH);
            int fontSize = 12;
            int tw = UITextWidth("D", fontSize);
            UIText("D", (int)(c.x - tw * 0.5f), (int)(c.y - fontSize * 0.5f), fontSize, Color{220, 255, 255, 255});
        }
    } else if (o->prio == 3) {
        // Special letter block (J/S/H/F/Force — SpecialBlock.hpp): invisible in
        // real GD, shown here as a labelled translucent marker since making
        // invisible-in-game state visible is the whole point of this debug app
        // (same reasoning as the continuous hitbox trail).
        const char* label = "?";
        Color c = RAYWHITE;
        switch (o->typeId) {
            case 1813: label = "J";  c = Color{255, 210, 60, 255};  break; // Stop Jump Buffer
            case 1829: label = "S";  c = Color{255, 120, 60, 255};  break; // Stop Dash
            case 1859: label = "H";  c = Color{120, 200, 255, 255}; break; // Allow Head Collision
            case 2866: label = "F";  c = Color{200, 120, 255, 255}; break; // Gravity Flip
            case 2069: label = "FS"; c = Color{160, 255, 160, 255}; break; // Force (square)
            case 3645: label = "FC"; c = Color{160, 255, 160, 255}; break; // Force (circle)
        }
        float hw = o->size.x * 0.5f, hh = o->size.y * 0.5f;
        Vector2 wpts[4] = {
            rotateLocal(-hw, -hh, rotDeg, x, y), rotateLocal(hw, -hh, rotDeg, x, y),
            rotateLocal(hw, hh, rotDeg, x, y),   rotateLocal(-hw, hh, rotDeg, x, y),
        };
        Vector2 s[4];
        for (int i = 0; i < 4; i++) s[i] = worldToScreen(wpts[i].x, wpts[i].y, cam, screenW, screenH);
        for (int i = 0; i < 4; i++) DrawLineEx(s[i], s[(i + 1) % 4], 1.5f, Color{c.r, c.g, c.b, 150});
        Vector2 center = worldToScreen(x, y, cam, screenW, screenH);
        int fontSize = 12;
        int tw = UITextWidth(label, fontSize);
        UIText(label, (int)(center.x - tw * 0.5f), (int)(center.y - fontSize * 0.5f), fontSize, c);
    } else if (o->prio == 2) {
        // Hazard: draw ONLY the actual gdsim collision shape, filled bright
        // red. (Used to also draw the nominal sprite footprint as a faint
        // context-only outline — removed 2026-08-18: not a hitbox, and the
        // app is meant to show hitboxes only.)
        if (isSawblade(o->typeId)) {
            // Circle hitbox centred at pos (Hazard.cpp Sawblade::touching) —
            // rotation-invariant, no vertex math needed. Radius is size.x/2
            // for every sawblade class EXCEPT 675/1734, which uses the full
            // size.x (see isBigRadiusSawblade).
            float radius = isBigRadiusSawblade(o->typeId) ? o->size.x : (o->size.x * 0.5f);
            Vector2 c = worldToScreen(x, y, cam, screenW, screenH);
            DrawCircleV(c, radius * cam.pixelsPerUnit, Color{230, 40, 40, 220});
        } else {
            // Small offset rectangle: hw=0.22*sizeX, hh=0.25*sizeY, centred
            // 0.25*sizeY above the object's own centre (LOCAL +Y, before
            // rotation) — ported directly from Hazard.cpp's hazardRectHit(),
            // the exact shape gdsim tests against the player. This is
            // deliberately much smaller than the full sprite box: that gap is
            // real (spikes are "mostly safe to stand next to"), not a bug.
            float rhw = o->size.x * 0.22f, rhh = o->size.y * 0.25f;
            float cy = o->size.y * 0.25f;
            Vector2 wpts[4] = {
                rotateLocal(-rhw, cy - rhh, rotDeg, x, y), rotateLocal(rhw, cy - rhh, rotDeg, x, y),
                rotateLocal(rhw, cy + rhh, rotDeg, x, y),  rotateLocal(-rhw, cy + rhh, rotDeg, x, y),
            };
            drawWorldQuad(wpts, cam, screenW, screenH, Color{230, 40, 40, 220}, Color{255, 140, 140, 255}, 1.f);
        }
    } else {
        // Effect object (orb/pad/portal/trigger-ish): draw the REAL touch
        // hitbox — o->size, exactly what Object::touching() tests against —
        // as a rectangle in the object's own GD colour (effectColor above),
        // not a generic circle. Changed 2026-08-18 alongside the portal
        // hitbox-size fix: now that Object.cpp's sizes are the real measured
        // values, showing the actual box (not a stand-in circle) is what
        // makes this app's "hitboxes only" view actually trustworthy for
        // portals/transporters.
        float hw = o->size.x * 0.5f, hh = o->size.y * 0.5f;
        Color c = effectColor(o->typeId);
        Vector2 wpts[4] = {
            rotateLocal(-hw, -hh, rotDeg, x, y), rotateLocal(hw, -hh, rotDeg, x, y),
            rotateLocal(hw, hh, rotDeg, x, y),   rotateLocal(-hw, hh, rotDeg, x, y),
        };
        drawWorldQuad(wpts, cam, screenW, screenH, Color{c.r, c.g, c.b, 130}, c, 1.5f);
    }
}

void drawLevelGeometry(const gdsim::Level& level, const Camera2DState& cam, int screenW, int screenH) {
    float viewHalfW = screenW * 0.5f / cam.pixelsPerUnit + 40.f;
    float loX = cam.x - viewHalfW, hiX = cam.x + viewHalfW;

    int loSec = std::max(0, (int)(loX / gdsim::Level::sectionSize));
    int hiSec = std::min((int)level.sections.size() - 1, (int)(hiX / gdsim::Level::sectionSize) + 1);
    for (int s = loSec; s <= hiSec && s >= 0; s++) {
        for (auto& oc : level.sections[s]) {
            const gdsim::Object* o = oc.operator->();
            drawObject(o, o->pos.x, o->pos.y, o->rotation, cam, screenW, screenH);
        }
    }
}

void drawLevel(const gdsim::Level& level, const gdsim::Player& player,
               const Camera2DState& cam, int screenW, int screenH) {
    drawLevelGeometry(level, cam, screenW, screenH);

    float viewHalfW = screenW * 0.5f / cam.pixelsPerUnit + 40.f;
    float loX = cam.x - viewHalfW, hiX = cam.x + viewHalfW;

    if (level.hasTriggers) {
        for (size_t idx = 0; idx < level.movable.size(); idx++) {
            gdsim::Vec2D pos; float rot; bool active;
            level.poseMovable((int)idx, player.frame, pos, rot, active);
            if (!active) continue;
            if (pos.x < loX || pos.x > hiX) continue;
            const gdsim::Object* o = level.movable[idx].operator->();
            drawObject(o, pos.x, pos.y, rot, cam, screenW, screenH);
        }
    }

    // Player: the actual collision hitboxes gdsim tests (innerHitbox,
    // unrotatedHitbox, blockDeathHitbox — see Player.cpp) are ALWAYS
    // axis-aligned; the cube's visual spin never rotates them (this is a
    // real GD property, not a gdsim shortcut — see Object::touching's own
    // comment). Drawing the player rotated would be exactly the same kind
    // of "looks like a hit/miss but isn't" mismatch as the old hazard
    // triangle, so the full box below is deliberately NOT rotated.
    //
    // REVISED 2026-08-11: hazard/sawblade death now uses the full "Main"
    // hitbox (Hazard.cpp, per GD Creator School's Advanced Hitboxes #1) —
    // hazards are the dominant real-world death cause, so this is the box
    // that matters most now; drawn filled/bright instead of the small one.
    Vector2 pc = worldToScreen(player.pos.x, player.pos.y, cam, screenW, screenH);
    float pw = player.size.x * cam.pixelsPerUnit;
    float ph = player.size.y * cam.pixelsPerUnit;
    DrawRectangle((int)(pc.x - pw * 0.5f), (int)(pc.y - ph * 0.5f), (int)pw, (int)ph,
                  Color{80, 220, 120, 70});
    DrawRectangleLinesEx({pc.x - pw * 0.5f, pc.y - ph * 0.5f, pw, ph}, 1.5f, Color{120, 220, 160, 220});

    // The small ~9x9 (mini-scaled) inner "Solid" hitbox — still what determines
    // BLOCK and CEILING/bounds death (Block.cpp's blockDeathHitbox / innerHitbox
    // ceiling clamp in Vehicle.cpp), just no longer hazards. Drawn as a plain
    // outline now (secondary, not primary) so it's still visible without
    // implying it's the hazard-relevant box.
    float innerSize = 9.f * (player.small ? 0.6f : 1.0f) * cam.pixelsPerUnit;
    DrawRectangleLines((int)(pc.x - innerSize * 0.5f), (int)(pc.y - innerSize * 0.5f),
                        (int)innerSize, (int)innerSize, Color{255, 210, 90, 200});
}

void drawHitboxTrail(const std::vector<TrailPoint>& trail,
                      const Camera2DState& cam, int screenW, int screenH) {
    size_t n = trail.size();
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
        const TrailPoint& t = trail[i];
        // Oldest samples are most transparent, newest fade in toward the
        // current (opaque) hitbox drawn separately by drawLevel.
        float age = (float)i / (float)n; // 0 = oldest, 1 = newest
        unsigned char alpha = (unsigned char)(30 + age * 90);
        // REVISED 2026-08-11: tracks the player's actual full "Main" hitbox
        // now (hazard/sawblade-relevant, per Hazard.cpp), not the small ~9x9
        // one — hazards are the dominant real death cause, so this is the box
        // worth seeing swept out continuously. Uses the real per-vehicle size
        // (e.g. wave is 10x10/6x6, not the cube's 30x30/18x18).
        float sizeW = t.size.x * cam.pixelsPerUnit;
        float sizeH = t.size.y * cam.pixelsPerUnit;
        Vector2 c = worldToScreen(t.pos.x, t.pos.y, cam, screenW, screenH);
        DrawRectangleLines((int)(c.x - sizeW * 0.5f), (int)(c.y - sizeH * 0.5f),
                            (int)sizeW, (int)sizeH, Color{80, 220, 120, alpha});
    }
}

void drawCenterPath(const std::vector<TrailPoint>& trail,
                     const Camera2DState& cam, int screenW, int screenH) {
    if (trail.size() < 2) return;
    // Bright cyan-white, thin — deliberately distinct from both the green
    // hitbox trail and the orange physics trail.
    const Color kCenterColor{140, 235, 255, 200};
    Vector2 prev = worldToScreen(trail[0].pos.x, trail[0].pos.y, cam, screenW, screenH);
    for (size_t i = 1; i < trail.size(); i++) {
        Vector2 cur = worldToScreen(trail[i].pos.x, trail[i].pos.y, cam, screenW, screenH);
        if ((prev.x < -50 && cur.x < -50) || (prev.x > screenW + 50 && cur.x > screenW + 50)) {
            prev = cur;
            continue;
        }
        DrawLineEx(prev, cur, 1.f, kCenterColor);
        prev = cur;
    }
}

void drawPhysicsTrail(const std::vector<RealTrailPoint>& trail, float offsetX, float offsetY,
                       const Camera2DState& cam, int screenW, int screenH) {
    if (trail.size() < 2) return;
    // Bright orange, thick enough to read clearly against both the level
    // geometry (greys/reds) and the live sim trail (green) — deliberately a
    // colour nothing else in this renderer uses.
    const Color kRealColor{255, 150, 30, 220};
    auto toScreen = [&](const gdsim::Vec2D& p) {
        return worldToScreen(p.x - offsetX, p.y - offsetY, cam, screenW, screenH);
    };
    Vector2 prev = toScreen(trail[0].pos);
    for (size_t i = 1; i < trail.size(); i++) {
        Vector2 cur = toScreen(trail[i].pos);
        // Cheap off-screen skip: both endpoints far outside the viewport.
        if ((prev.x < -50 && cur.x < -50) || (prev.x > screenW + 50 && cur.x > screenW + 50)) {
            prev = cur;
            continue;
        }
        DrawLineEx(prev, cur, 2.f, kRealColor);
        prev = cur;
    }
}

void zoomAt(Camera2DState& cam, float factor, Vector2 anchor, int screenW, int screenH) {
    const float before = cam.pixelsPerUnit;
    const float after = std::clamp(before * factor, kMinZoom, kMaxZoom);
    if (after == before) return;   // already at a limit — don't drift the centre
    // screenToWorld is  wx = cam.x + (sx - W/2)/ppu ,  wy = cam.y - (sy - H/2)/ppu.
    // Holding wx/wy fixed while ppu changes gives exactly this shift of the centre.
    const float dx = anchor.x - screenW * 0.5f;
    const float dy = anchor.y - screenH * 0.5f;
    cam.x += dx * (1.f / before - 1.f / after);
    cam.y -= dy * (1.f / before - 1.f / after);
    cam.pixelsPerUnit = after;
}

gdsim::Vec2D screenToWorld(float sx, float sy, const Camera2DState& cam, int screenW, int screenH) {
    return {
        cam.x + (sx - screenW * 0.5f) / cam.pixelsPerUnit,
        cam.y - (sy - screenH * 0.5f) / cam.pixelsPerUnit
    };
}

PickedObject pickObjectNear(const gdsim::Level& level, gdsim::Vec2D worldPos, int frame) {
    PickedObject best;
    float bestDist = 1e18f;
    auto consider = [&](const gdsim::Object* o, gdsim::Vec2D pos) {
        float dx = pos.x - worldPos.x, dy = pos.y - worldPos.y;
        float tolX = o->size.x * 0.5f + 3.f, tolY = o->size.y * 0.5f + 3.f;
        if (std::fabs(dx) > tolX || std::fabs(dy) > tolY) return;
        float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = PickedObject{true, o->typeId, o->prio, pos, o->size, o->rotation};
        }
    };

    int sec = (int)(worldPos.x / gdsim::Level::sectionSize);
    int loSec = std::max(0, sec - 1), hiSec = std::min((int)level.sections.size() - 1, sec + 1);
    for (int s = loSec; s <= hiSec && s >= 0; s++)
        for (auto& oc : level.sections[s])
            consider(oc.operator->(), oc.operator->()->pos);

    if (level.hasTriggers) {
        for (size_t idx = 0; idx < level.movable.size(); idx++) {
            gdsim::Vec2D pos; float rot; bool active;
            level.poseMovable((int)idx, frame, pos, rot, active);
            if (!active) continue;
            consider(level.movable[idx].operator->(), pos);
        }
    }
    return best;
}

void drawSolverViz(const gdsim::SolverProgressReport::VizSnapshot& snap,
                    const Camera2DState& cam, int screenW, int screenH) {
    // Trajectory: the solver's current best attempt, sampled every few frames
    // (SolverProgressReport's own contract) — yellow if that attempt ended in
    // death, green if it's still the live leading edge of the search.
    if (snap.trajectory.size() >= 2) {
        Color lineColor = snap.died ? Color{230, 210, 70, 255} : Color{90, 230, 120, 255};
        for (size_t i = 1; i < snap.trajectory.size(); i++) {
            Vector2 a = worldToScreen(snap.trajectory[i - 1].first, snap.trajectory[i - 1].second, cam, screenW, screenH);
            Vector2 b = worldToScreen(snap.trajectory[i].first, snap.trajectory[i].second, cam, screenW, screenH);
            DrawLineEx(a, b, 2.f, lineColor);
        }
    }

    // Click markers: where each current click in the attempt was made.
    for (auto& c : snap.clicks) {
        Vector2 p = worldToScreen(c.x, c.y, cam, screenW, screenH);
        DrawCircleV(p, 4.f, Color{120, 200, 255, 220});
        DrawCircleLines((int)p.x, (int)p.y, 4.f, Color{20, 20, 30, 200});
    }

    // Death point: where this attempt's trajectory ended.
    if (snap.died) {
        Vector2 dp = worldToScreen(snap.deathPt.first, snap.deathPt.second, cam, screenW, screenH);
        DrawLineEx({dp.x - 6, dp.y - 6}, {dp.x + 6, dp.y + 6}, 2.5f, Color{255, 70, 70, 255});
        DrawLineEx({dp.x - 6, dp.y + 6}, {dp.x + 6, dp.y - 6}, 2.5f, Color{255, 70, 70, 255});
    }
}

} // namespace gdapp
