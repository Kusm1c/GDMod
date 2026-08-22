#pragma once
#include "../../src/sim/Solver.hpp"
#include <string>
#include <vector>

// A solve whose replay actually reached the end (CLEARED, not died) — kept in
// memory so it can be re-watched or exported later without re-solving. Stores
// the level string directly (not just id/name) so replaying it doesn't
// depend on the level still being the "currently loaded" one. Shared between
// main.cpp (in-memory list) and macrocache.cpp (on-disk persistence).
struct MacroEntry {
    int levelId;
    std::string levelName;
    std::string levelString;
    std::vector<gdsim::SolverClick> clicks;
    float levelEndEstimate;
    std::string recordedAt;
};
