#pragma once
#include "../../src/sim/Level.hpp"
#include "../../src/sim/Solver.hpp"
#include "raylib.h"
#include <vector>

// Minimalist vector-shape rendering for a gdsim Level + Player: no textures,
// just flat rectangles/triangles/circles categorised by object prio/typeId.
namespace gdapp {

struct Camera2DState {
    float pixelsPerUnit = 3.5f;
    float x = 0.f, y = 0.f; // world-space centre of the view
};

// Zoom range, shared by every camera in the app. The old ceiling was 14 px/unit
// — about one screen pixel per two world units — which is far too coarse for
// trajectory work, where the interesting errors are a fraction of a unit and a
// pin has to land on one specific frame. At 400 px/unit a single world unit
// spans 400 pixels, so a 0.01u deviation is still a visible 4px gap. The floor
// goes the other way, far enough out to take in a whole long level at once.
inline constexpr float kMinZoom = 0.05f;
inline constexpr float kMaxZoom = 400.f;

// Multiplies the camera's zoom by `factor` while keeping the world point
// currently under `anchor` (a screen position, normally the cursor) pinned in
// place. Centre-anchored zoom becomes unusable past ~30 px/unit: whatever you
// are inspecting slides off screen the moment you zoom in on it.
void zoomAt(Camera2DState& cam, float factor, Vector2 anchor, int screenW, int screenH);

// One historical sample of the player's position + actual hitbox size
// (Player::size — varies by vehicle, e.g. wave is 10x10/6x6 mini, not the
// cube's 30x30/18x18) — see drawLevel's own player-drawing comment for which
// hitbox is drawn/why.
struct TrailPoint {
    gdsim::Vec2D pos;
    gdsim::Vec2D size;
    bool small;
};

// Draws every collision object within view (static sections + posed movable
// objects) plus the player, in gdsim world space transformed through `cam`.
void drawLevel(const gdsim::Level& level, const gdsim::Player& player,
               const Camera2DState& cam, int screenW, int screenH);

// Draws just the STATIC section geometry (no movable/triggered objects, no
// player) — used by the solver viewer, which has no single live player state
// or frame to pose movable objects at (the search explores many frames/
// branches at once). This is exactly drawLevel's own static-section loop,
// factored out so both share one object-drawing implementation.
void drawLevelGeometry(const gdsim::Level& level, const Camera2DState& cam,
                        int screenW, int screenH);

// Draws a fading trail of the player's inner hitbox history (oldest = most
// transparent), so the exact swept collision path is visible continuously
// instead of only at the current instant.
void drawHitboxTrail(const std::vector<TrailPoint>& trail,
                      const Camera2DState& cam, int screenW, int screenH);

// Draws a thin line through the CENTRE of each historical TrailPoint (no
// hitbox boxes) — an "options" toggle for when the full swept-box trail is
// too visually busy and just the path curve itself is wanted.
void drawCenterPath(const std::vector<TrailPoint>& trail,
                     const Camera2DState& cam, int screenW, int screenH);

// One frame of a REAL re-scanner capture (testlevel/GDMod_physics_<id>.txt),
// already Y-offset-corrected into gdsim world space (real Y is 105 units
// above gdsim's — see the "physics trail" feature in main.cpp).
struct RealTrailPoint {
    gdsim::Vec2D pos;
    int veh;
    bool mini;
    long frame = 0;   // raw frame number from the capture file (its own clock,
                       // not comparable to a sim frame index — see the
                       // deviation-file feature's own notes on this)
    float yVel = 0.f;
};

// Draws a REAL captured trajectory (the "physics trail") as a static
// polyline distinct from the live sim trail, so a real playthrough and
// gdsim's own replay of the same level can be visually compared frame-by-
// frame divergence at a glance. The whole recorded path is drawn (not just a
// short rolling window like drawHitboxTrail) since this is a fixed reference
// line, not something that grows with the current frame.
// `trail` holds RAW (unconverted) real-capture coordinates; offsetX/offsetY
// are subtracted at draw time (not baked in at load time) so an in-game live
// nudge control can re-align it against the sim trail without re-reading the
// capture file on every adjustment.
void drawPhysicsTrail(const std::vector<RealTrailPoint>& trail, float offsetX, float offsetY,
                       const Camera2DState& cam, int screenW, int screenH);

// Draws the beam search's live progress snapshot: the current best trajectory
// (a connected line, sampled every few frames by the solver), its click
// positions, and the death point where its latest attempt ended.
void drawSolverViz(const gdsim::SolverProgressReport::VizSnapshot& snap,
                    const Camera2DState& cam, int screenW, int screenH);

// Inverse of drawLevel's internal worldToScreen — converts a screen-space
// point (e.g. GetMousePosition()) back to gdsim world coordinates under the
// given camera. Used by the hitbox-mismatch flagging tool to find which
// object the user's cursor is pointing at.
gdsim::Vec2D screenToWorld(float sx, float sy, const Camera2DState& cam, int screenW, int screenH);

// Result of pickObjectNear: the level object (static or posed-movable)
// closest to a given world point, within a generous pick tolerance.
struct PickedObject {
    bool found = false;
    int typeId = 0;
    int prio = 0;
    gdsim::Vec2D pos, size;
    float rotation = 0.f;
};

// Finds the object nearest `worldPos` (posing movable objects at `frame`),
// within roughly its own footprint plus a few units of slack — good enough
// for "the thing the user's cursor is roughly over", not pixel-exact hit
// testing. Used by the hitbox-mismatch flagging tool (main.cpp): at the
// moment of death/clear, the user hovers an object and presses a key to
// record "my player passed through this, but shouldn't have" for later
// investigation — a concrete, per-object bug report instead of a screenshot.
PickedObject pickObjectNear(const gdsim::Level& level, gdsim::Vec2D worldPos, int frame);

} // namespace gdapp
