#pragma once
#include "Solver.hpp"
#include "Level.hpp"
#include <fstream>
#include <vector>
#include <cstdint>

// Internal helpers and cross-file declarations for the solver subsystem.
// Not part of the public API.

namespace gdsim {

inline float levelEnd(const Level& lvl) {
    if (lvl.length > 0.f) return lvl.length;
    if (!lvl.sections.empty()) return (float)lvl.sections.size() * 600.f - 300.f;
    return 3000.f;
}

inline bool overlaps(uint64_t pf, uint64_t rf, const std::vector<SolverClick>& clicks) {
    for (auto& c : clicks)
        if (pf < c.releaseFrame && rf > c.pressFrame) return true;
    return false;
}

// Genetic Algorithm fallback — declared in Solver_GA.cpp
SolverResult solveLevelGA(Level& sim, float end, const SolverConfig& cfg,
                           std::ofstream& dbg,
                           const std::atomic<bool>* cancelled,
                           const std::vector<uint64_t>& seedPfs,
                           const std::string& levelStr);

// Zero-margin correctness gate — declared in Solver.cpp, used by every solver
// algorithm's success path (not just the ones that go through finalizeSolved).
// Replays `clicks` through a FRESH Level built from `levelStr` with
// hazardInflate/flightHazardInflate forced to 0 — the physics View Level/Watch/a
// real solve claim actually need to survive. A solution that only clears with the
// solver's safety margin applied is not a real solve.
struct ZeroMarginResult {
    bool     ok         = true;
    uint64_t deathFrame = 0;
    float    deathX     = 0.f;
    float    deathY     = 0.f;
};
ZeroMarginResult verifyZeroMargin(const std::string& levelStr,
                                   const std::vector<SolverClick>& clicks,
                                   float end, const SolverConfig& cfg);

// Beam search (reliable width-bounded BFS) — declared in Solver_Beam.cpp
bool solveLevelBeam(Level& sim, float end, const SolverConfig& cfg,
                    std::ofstream& dbg,
                    const std::atomic<bool>* cancelled,
                    SolverProgressReport* prog,
                    std::vector<SolverClick>& out);

// Two-phase path solver — declared in Solver_Path.cpp
bool solveLevelPath(Level& sim, float end, const SolverConfig& cfg,
                    std::ofstream& dbg,
                    const std::atomic<bool>* cancelled,
                    SolverProgressReport* prog,
                    std::vector<SolverClick>& out);

void optimizeClicks(Level& sim, float end, const SolverConfig& cfg,
                    std::ofstream& dbg,
                    const std::atomic<bool>* cancelled,
                    SolverProgressReport* prog,
                    std::vector<SolverClick>& clicks);

// Consolidation pass — declared in Solver_Path.cpp.
// Ship/flight sections come out of the BFS as a sawtooth of 1-frame taps. This
// merges consecutive taps into fewer, longer holds (preserving total thrust) so
// the replay is smoother and far less frame-perfect-fragile.
void consolidateClicks(Level& sim, float end, const SolverConfig& cfg,
                       std::ofstream& dbg,
                       const std::atomic<bool>* cancelled,
                       SolverProgressReport* prog,
                       std::vector<SolverClick>& clicks);

// Timing-centering pass — declared in Solver_Path.cpp.
// The BFS finds the *laziest* path (presses delayed to the last survivable
// frame). This shifts each press toward the middle of its valid window so the
// replay has margin on both sides instead of being frame-perfect.
void centerClicks(Level& sim, float end, const SolverConfig& cfg,
                  std::ofstream& dbg,
                  const std::atomic<bool>* cancelled,
                  SolverProgressReport* prog,
                  std::vector<SolverClick>& clicks);

// Clearance / replay-robustness pass — declared in Solver_Path.cpp.
// Ship/UFO/wave/swing flight integrates velocity over the whole hold, so a path
// can "solve" while grazing a spike within a pixel — fine in the deterministic
// sim, fatal in the real game where replay inputs land a sub-step or two off and
// the trajectory drifts into the spike. This nudges the timing/hold of flight
// clicks to MAXIMISE the minimum hazard clearance along the path (threading the
// centre of gaps), without ever breaking the solve.
void robustifyClicks(Level& sim, float end, const SolverConfig& cfg,
                     std::ofstream& dbg,
                     const std::atomic<bool>* cancelled,
                     SolverProgressReport* prog,
                     std::vector<SolverClick>& clicks);

} // namespace gdsim
