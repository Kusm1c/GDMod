#include "render.hpp"
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

// A handful of recognisable GD colours for common effect objects; anything
// else falls back to a neutral white so it's still visible without needing a
// full ID table (this app is meant to look like a schematic, not the game).
static Color effectColor(int typeId) {
    switch (typeId) {
        case 36:  case 35:  return YELLOW;               // yellow orb / pad
        case 84:  case 67:  return SKYBLUE;               // blue orb / pad
        case 141: case 140: return MAGENTA;               // pink orb / pad
        case 1333: case 1332: return RED;                 // red orb / pad
        case 1022: return GREEN;                          // green orb
        case 1704: return ORANGE;                         // dash orb
        case 1751: return PURPLE;                         // gravity-flip orb
        case 1330: return BLACK;                          // black orb
        case 3004: return DARKPURPLE;                      // spider/teleport orb
        case 10:   return SKYBLUE;                        // gravity portal (normal)
        case 11:   return SKYBLUE;                        // gravity portal (flipped)
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

static void drawObject(const gdsim::Object* o, float x, float y, float rotDeg,
                        const Camera2DState& cam, int screenW, int screenH) {
    // Cull cheaply in world space before ever touching raylib.
    float halfSpan = (o->size.x + o->size.y) * 0.5f + 4.f;
    float viewHalfW = screenW * 0.5f / cam.pixelsPerUnit + halfSpan;
    if (std::fabs(x - cam.x) > viewHalfW) return;

    if (o->prio == 1) {
        // Solid block: the object's full bounding box IS the real hitbox here
        // (Block.cpp collides against it directly, mod a small graze
        // tolerance that isn't worth drawing at this scale).
        float hw = o->size.x * 0.5f, hh = o->size.y * 0.5f;
        Vector2 wpts[4] = {
            rotateLocal(-hw, -hh, rotDeg, x, y), rotateLocal(hw, -hh, rotDeg, x, y),
            rotateLocal(hw, hh, rotDeg, x, y),   rotateLocal(-hw, hh, rotDeg, x, y),
        };
        drawWorldQuad(wpts, cam, screenW, screenH, Color{90, 90, 100, 255}, Color{160, 160, 175, 255}, 1.5f);
    } else if (o->prio == 2) {
        // Hazard: draw the object's nominal sprite footprint as a faint
        // outline (context only — NOT collision-relevant) and the actual
        // gdsim collision shape filled bright red, so it's obvious which
        // part of a spike you can safely stand next to.
        float hw = o->size.x * 0.5f, hh = o->size.y * 0.5f;
        Vector2 outline[4] = {
            rotateLocal(-hw, -hh, rotDeg, x, y), rotateLocal(hw, -hh, rotDeg, x, y),
            rotateLocal(hw, hh, rotDeg, x, y),   rotateLocal(-hw, hh, rotDeg, x, y),
        };
        {
            Vector2 s[4];
            for (int i = 0; i < 4; i++) s[i] = worldToScreen(outline[i].x, outline[i].y, cam, screenW, screenH);
            for (int i = 0; i < 4; i++) DrawLineEx(s[i], s[(i + 1) % 4], 1.f, Color{120, 60, 60, 160});
        }

        if (isSawblade(o->typeId)) {
            // Circle hitbox: radius = size.x/2, centred at pos (Hazard.cpp
            // Sawblade::touching) — rotation-invariant, no vertex math needed.
            Vector2 c = worldToScreen(x, y, cam, screenW, screenH);
            DrawCircleV(c, (o->size.x * 0.5f) * cam.pixelsPerUnit, Color{230, 40, 40, 220});
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
        // Effect object (orb/pad/portal/trigger-ish): small coloured circle.
        // These aren't rotation-critical to show accurately (touch, not a
        // solid hitbox), so a plain circle stays honest enough.
        float w = o->size.x * cam.pixelsPerUnit, h = o->size.y * cam.pixelsPerUnit;
        Vector2 center = worldToScreen(x, y, cam, screenW, screenH);
        float r = std::max(w, h) * 0.35f;
        DrawCircleV(center, r, effectColor(o->typeId));
        DrawCircleLines((int)center.x, (int)center.y, r, Color{20, 20, 20, 200});
    }
}

void drawLevel(const gdsim::Level& level, const gdsim::Player& player,
               const Camera2DState& cam, int screenW, int screenH) {
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
    Vector2 pc = worldToScreen(player.pos.x, player.pos.y, cam, screenW, screenH);
    float pw = player.size.x * cam.pixelsPerUnit;
    float ph = player.size.y * cam.pixelsPerUnit;
    DrawRectangleLinesEx({pc.x - pw * 0.5f, pc.y - ph * 0.5f, pw, ph}, 1.5f, Color{120, 220, 160, 180});

    // The small 9x9 (mini-scaled) inner hitbox is what actually determines
    // hazard/ceiling death (Player::innerHitbox) — drawn filled and bright
    // since it's the part that matters.
    float innerSize = 9.f * (player.small ? 0.6f : 1.0f) * cam.pixelsPerUnit;
    DrawRectangle((int)(pc.x - innerSize * 0.5f), (int)(pc.y - innerSize * 0.5f),
                  (int)innerSize, (int)innerSize, Color{80, 220, 120, 255});
    DrawRectangleLines((int)(pc.x - innerSize * 0.5f), (int)(pc.y - innerSize * 0.5f),
                        (int)innerSize, (int)innerSize, Color{20, 90, 40, 255});
}

} // namespace gdapp
