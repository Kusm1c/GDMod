#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace gdsim {

struct SolverClick {
    uint64_t pressFrame;
    uint64_t releaseFrame;
};

struct SolverResult {
    bool solved = false;
    std::vector<SolverClick> clicks;
    int framesTotal = 0;
    float finalX = 0.f;
    float maxXReached = 0.f;    // furthest X seen across all passes
    float levelEndEstimate = 0.f;
    std::string message;
};

// Live progress reported by solveLevel() into a shared struct.
// Thread-safe: solver writes, UI thread reads.
struct SolverProgressReport {
    std::atomic<int>   clicksFound{0};
    std::atomic<float> bestX{0.f};
    std::atomic<float> levelEndX{0.f};
    std::atomic<int>   iteration{0};
    std::atomic<bool>  done{false};

    mutable std::mutex logMtx;
    std::deque<std::string> log; // guarded by logMtx, capped at 48 entries

    void addLog(std::string msg) {
        std::lock_guard<std::mutex> lk(logMtx);
        if (log.size() >= 48) log.pop_front();
        log.push_back(std::move(msg));
    }

    // Visual snapshot — updated after each forward pass so the UI can render
    // the current trajectory, click positions, and search window in real time.
    struct VizClick { float x = 0.f, y = 0.f; };
    struct VizSnapshot {
        std::vector<std::pair<float,float>> trajectory; // sampled every 4 frames
        std::pair<float,float> deathPt{0.f, 0.f};
        bool died = false;
        std::vector<VizClick> clicks;     // world (x,y) of each current click
        float searchStartX = 0.f;         // X where the search window begins
        float searchEndX   = 0.f;         // X of death (search window end)
        std::string debugInfo;            // overlay text for the viewer window
    };

    mutable std::mutex vizMtx;
    VizSnapshot viz;
    std::atomic<bool> vizDirty{false};

    void updateViz(VizSnapshot&& snap) {
        std::lock_guard<std::mutex> lk(vizMtx);
        viz = std::move(snap);
        vizDirty.store(true, std::memory_order_release);
    }

    SolverProgressReport() = default;
    SolverProgressReport(const SolverProgressReport&) = delete;
    SolverProgressReport& operator=(const SolverProgressReport&) = delete;
};

struct SolverConfig {
    float dt = 1.f / 240.f;
    uint64_t maxFrames = 240 * 90;  // 90s timeout
    int maxClicks = 300;            // ship/wave sections can need many alternating clicks
    int lookbackFrames = 96;        // starting lookback before death
    int lookAheadFrames = 480;      // 2s lookahead at 240fps
    int maxHoldFrames = 120;        // up to 0.5s hold

    // Default pipeline: the FULL chain (beam primary → path-seeker → greedy → GA
    // fallback, then optimize/consolidate/center/robustify post-processing). A prior
    // "pathfinder-only" default (just the raw randomized path-seeker from
    // camila314/pathfinder, no beam, no fallback, no post-processing) measured
    // UNRELIABLE against this mod's own truth-bank levels: 4/13 real, previously
    // solved real levels (108166595, 174, 21227933, 98414841) timed out with the
    // path-seeker alone. Isolated beam-search testing (test/beamonly.cpp) on those
    // same 4 solved 2 of them in 3-12s (vs. path-seeker's 180s+ timeout) and failed
    // FAST (<45s, not a timeout) on the other 2 — meaning the beam→path-seeker→
    // greedy→GA fallback chain gets multiple real tries instead of burning the
    // whole budget on one flaky heuristic. "The bot must actually find a path" beats
    // "the bot is fast" — solve time is not the constraint here, reliability is.
    bool doOptimize  = true;        // run optimize + consolidate (fewer/cleaner clicks)
    bool doCenter    = true;        // run timing-centering (more replay margin)
    bool doRobustify = true;        // maximise flight clearance (no spike-grazing on replay)
    bool doBeam      = true;        // try the reliable beam search first; also gates the
                                    // greedy/GA fallback after the path seeker (see Solver.cpp)
    // Hazard-hitbox safety margin (units) applied to the whole solve. GD's hazard
    // hitboxes run ~½–1u larger than this sim's, so without a margin the solver
    // produces paths that graze spikes by <1u and die in the real game. ~1.5 keeps
    // real clearance; too large can make genuinely tight sections unsolvable.
    float hazardInflate = 2.5f;
    // EXTRA clearance added on top of hazardInflate in flight modes only (ship/ufo/
    // wave/swing). Flight is fast and hitbox-sensitive — a steep mini-wave amplifies
    // any sub-frame sim residual into several units — so flight paths need more
    // margin to stay safe on real-game replay. Too large makes tight flight unsolvable.
    float flightClearance = 4.5f;
    // Vertical robustness margin (world units) for the flight death-box against SOLID
    // BLOCKS (not hazards). gdsim's ship physics are exact, but ~6-7u of click-phase
    // drift accumulates over long flight sections (the real ship ends up that much
    // higher), so a path that threads the top of a block-gap frame-perfect here dies
    // in real GD. This makes the solver keep that much clearance below block ceilings.
    // Applied via g_solveShipBlockClearance; adaptively stepped down (with the hazard
    // margin) if a genuinely tight level won't solve. Too large makes tight flight
    // corridors "impossible" (honestly — their open-loop solution would die anyway).
    float shipBlockClearance = 7.0f;
    // Solid-block clearance (world units) for GROUNDED modes (cube/robot/spider),
    // the ground analog of shipBlockClearance. Keeps the solver from threading a
    // path that grazes a block inside-corner by <1u (survives in-sim, dies on watch
    // once apex/click-phase residual is added — mini Robot on 13711278). Applied via
    // g_solveRobotBlockClearance (top + horizontal only), stepped down in lockstep
    // with the hazard margin so a genuinely tight level still solves (frame-perfect
    // at 0 if that's the only way). Reaches the 13711278 killer edge at ≥3.
    float robotBlockClearance = 3.0f;
    int  beamWidth  = 4000;         // beam frontier width (states kept per frame).
                                    // Easy levels dedup well below this; only tight
                                    // flight corridors use the full width.
    // Flight clearance preference (world units). In ship/ufo/wave/swing sections
    // the beam keeps, per phase-space cell, the state that *clears hazards by this
    // margin* over one that merely got further while grazing a spike — so the
    // solved path threads the centre of gaps and survives replay frame-drift
    // instead of dying on a pixel-perfect graze. A soft preference: grazing states
    // are still kept as a fallback, so solvability is never reduced. 0 disables.
    // NOTE: measured ineffective on genuinely tight corridors (the fatal pinch is
    // sub-margin everywhere, so the tie-break never engages) and costs ~25% solve
    // time — OFF by default; the real replay-desync fix is in playback timing.
    float hazardMargin = 0.0f;
    // "Human limits": minimum frames between two click starts (rising edges).
    // 0 = unlimited (frame-perfect). 18 ≈ 14 clicks/s — keeps solutions humanly
    // achievable and far more replay-robust (no superhuman tap bursts that drift
    // out of sync with the real game). Enforced by the beam search.
    int  minClickGap = 0;

    SolverProgressReport* progress = nullptr; // optional live-progress sink (not owned)

    // User-drawn guide path (world x,y), set by clicking in the View Level window.
    // When non-empty, the beam search keeps only frontier states whose y stays
    // within `waypointCorridor` of the interpolated path at their x — focusing the
    // search along the path the user wants (huge simplification on hard levels).
    // Soft: if the corridor empties at some frame, that frame isn't filtered, and
    // the non-beam fallbacks ignore it entirely, so a bad path never makes a
    // solvable level unsolvable.
    std::vector<std::pair<float, float>> waypoints;
    float waypointCorridor = 90.f;   // half-height of the y-corridor (world units)
};

// Greedy forward solver: finds minimum-click solution.
// Runs best-effort; may not solve every level.
// Thread-safe: pass a shared atomic<bool> to cancel early.
SolverResult solveLevel(
    const std::string& levelStr,
    SolverConfig cfg = {},
    const std::atomic<bool>* cancelled = nullptr
);

// Cube-focused best-first graph search. Expands only at DECISION points (the cube
// grounded, or an orb/effect touching) and coasts ballistically between them, with
// closed-set dedup and NO fixed beam width — so it doesn't drop the exact mid-air
// state a frame-perfect trick needs, and it collapses long ballistic/flat stretches
// to a handful of nodes instead of branching every frame. Built to clear long
// (7-min), frame-perfect, mechanic-heavy CUBE levels that the beam/path solvers time
// out on. Flight segments (ship/ufo/wave/swing) fall back to per-frame branching
// (still deduped), so mostly-cube levels stay fast. Same SolverResult contract as
// solveLevel(); wired to the "Sim Cube" button.
SolverResult solveLevelCube(
    const std::string& levelStr,
    SolverConfig cfg = {},
    const std::atomic<bool>* cancelled = nullptr
);

} // namespace gdsim
