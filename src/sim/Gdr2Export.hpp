#pragma once
#include "Solver.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace gdsim {

// Standalone, Geode-independent .gdr2 binary encoder — for app/src/main.cpp
// (the standalone player has no Geode/Mod dependency to reuse the real mod's
// own encoder, src/replay_core.cpp's exportReplayToGdr2/GDR2Writer). Same wire
// format, ported by hand rather than shared/refactored to avoid touching that
// already-deployed, working mod code for an unrelated app feature. All clicks
// are written as player 1 (the app has no dual-mode/P2 replay concept).
std::vector<uint8_t> exportClicksToGdr2(const std::vector<SolverClick>& clicks,
                                         double framerate, int levelId,
                                         const std::string& levelName);

} // namespace gdsim
