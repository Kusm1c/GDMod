#pragma once
#include "../../src/sim/Level.hpp"
#include "raylib.h"

// Minimalist vector-shape rendering for a gdsim Level + Player: no textures,
// just flat rectangles/triangles/circles categorised by object prio/typeId.
namespace gdapp {

struct Camera2DState {
    float pixelsPerUnit = 3.5f;
    float x = 0.f, y = 0.f; // world-space centre of the view
};

// Draws every collision object within view (static sections + posed movable
// objects) plus the player, in gdsim world space transformed through `cam`.
void drawLevel(const gdsim::Level& level, const gdsim::Player& player,
               const Camera2DState& cam, int screenW, int screenH);

} // namespace gdapp
