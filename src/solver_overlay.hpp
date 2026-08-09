#pragma once

#include <memory>
#include <atomic>
#include "sim/Solver.hpp"

// In-game Cocos2d overlay that visualizes a running solve.
// Replaces the old native Win32 progress/view windows: themed like Geometry
// Dash, polls the shared SolverProgressReport on the main thread, and shows a
// live progress bar, stats, color-coded log, and a trajectory mini-view.
// Cross-platform (no Win32). Call from the main thread.
void showSolverOverlay(
    int levelId,
    std::shared_ptr<gdsim::SolverProgressReport> progress,
    std::shared_ptr<std::atomic<bool>> cancelled);
