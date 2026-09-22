#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// TRAJECTORY FIT — "I moved this point; which physics constant explains it?"
//
// You cannot literally drag a simulated point: the trajectory is a deterministic
// function of (spawn state, input track, physics constants). Moving a point is an
// OBSERVATION — "at frame F the player should be at Y, not where gdsim put it" —
// so the useful question is the inverse one: which registered constant, changed
// by how much, would make gdsim pass through that point?
//
// That is what this does:
//   1. Simulate once with current values -> baseline residual at each target.
//   2. For each candidate constant, perturb it, re-simulate, and measure
//      d(residual)/d(param) — a numerical Jacobian column.
//   3. Rank by how much authority each constant has over that point.
//   4. For the top few, actually SOLVE for the value that lands on the target
//      (secant when there is a single target+axis, golden-section on RMS
//      otherwise) and VERIFY it by re-simulating at the answer.
//
// HONEST LIMITS, stated here because the UI must not imply otherwise:
//   - Many different constants can move one point. This ranks which ones CAN
//     explain it; it cannot tell you which one is physically correct. That
//     judgement needs ground truth (the real capture overlay) or the decompiled
//     source — see how the cube-gravity table was actually settled.
//   - Fitting to a hand-dragged target is curve-fitting. Fitting to a target
//     snapped onto the REAL captured trajectory is calibration. The UI offers
//     the snap for exactly this reason.
//   - If every sensitivity comes back ~0, no registered constant moves that
//     point. That is a real result: the error is structural (ordering, a missing
//     mechanic, a hitbox) rather than a mis-valued constant.
//
// Speed note: this leans entirely on Level::resetToStart() — re-parsing the level
// per probe would cost ~104 ms each (measured on DeCode) and make the scan take
// half a minute; reusing one parsed Level makes a probe well under a millisecond.
// Portal HITBOX sizes are the one exception (baked in at construction), so those
// candidates are flagged and pay for a fresh Level per evaluation.
// ─────────────────────────────────────────────────────────────────────────────

namespace gdapp {

struct FitTarget {
    int   frame = 1;                 // 1-based, matching the replay's frame counter
    float targetX = 0.f, targetY = 0.f;
    bool  fitX = false, fitY = true; // which axes this target constrains
};

struct FitCandidate {
    int         tunable = -1;        // index into gdsim::allTunables()
    std::string group, name;
    double      current = 0.0;
    double      suggested = 0.0;
    // World units of residual change per unit of parameter change. Large |value|
    // = this constant has a lot of authority over the selected point.
    double      sensitivity = 0.0;
    double      residualBefore = 0.0;
    double      residualAfter = 0.0;
    // True once residualAfter came from a real re-simulation at `suggested`
    // rather than the linear prediction. Only verified rows should be trusted.
    bool        verified = false;
    bool        needsRebuild = false;   // portal-size entry: costs a full re-parse
    double      relChangePct = 0.0;     // |suggested-current| / |current| * 100
};

struct FitProgress {
    std::atomic<int>  done{0};
    std::atomic<int>  total{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::mutex        mu;                   // guards `results` and `note`
    std::vector<FitCandidate> results;      // ranked, best first
    std::string       note;
    double            baselineResidual = 0.0;
    double            elapsedMs = 0.0;
};

struct FitRequest {
    std::string       levelString;
    std::vector<bool> inputAt;              // press state per frame index
    std::vector<FitTarget> targets;
    std::vector<int>  candidates;           // registry indices to scan
    int               refineTop = 14;       // how many top rows get the real solve
};

// Which registry entries are worth scanning for a run up to `maxFrame`: drops
// vehicle groups the run never enters, speed tiers it never uses, orb/pad rows
// for vehicles/sizes it never sees, and the solver-only margins (which must stay
// at 0 for anything measuring fidelity). Anything it cannot classify is KEPT —
// a false include only costs time, a false exclude hides the answer.
std::vector<int> relevantTunables(const std::string& levelString,
                                  const std::vector<bool>& inputAt, int maxFrame);

// Blocking; run it on a worker thread and poll `prog`. Restores every tunable to
// its entry value before returning, including on cancel — the scan explores, it
// never applies. Applying is the caller's explicit action.
void runTrajectoryFit(FitRequest req, FitProgress& prog);

} // namespace gdapp
