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
