// Greedy forward solver + main entry point.
// Calls solveLevelPath (Solver_Path.cpp) first, then falls back to this
// greedy, and finally to solveLevelGA (Solver_GA.cpp) if needed.

#include "Solver_internal.hpp"
#include "Calib.hpp"
#include "DebugPaths.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace gdsim {

// Replays `clicks` through a fresh, zero-margin Level (hazardInflate and
// flightHazardInflate forced to 0) — see ZeroMarginResult's doc comment in
// Solver_internal.hpp. "ok" means it never died; matches the pre-existing inline
// check this replaces (a run that exhausts cfg.maxFrames without dying or without
// explicitly reaching `end` is still treated as ok, since the same clicks already
// reached `end` in the margin-applied sim that found them, at the same frame count).
ZeroMarginResult verifyZeroMargin(const std::string& levelStr,
                                   const std::vector<SolverClick>& clicks,
                                   float end, const SolverConfig& cfg)
{
    ZeroMarginResult r;
    std::vector<bool> inputAt(cfg.maxFrames + 4, false);
    for (auto& c : clicks)
        for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
            inputAt[f] = true;
    Level zeroSim(levelStr);
    zeroSim.hazardInflate = 0.f;
    zeroSim.flightHazardInflate = 0.f;
    for (uint64_t f = 1; f <= cfg.maxFrames; ++f) {
        bool p = f < inputAt.size() && inputAt[f];
        auto& s = zeroSim.runFrame(p, cfg.dt);
        if (s.dead) { r.ok = false; r.deathFrame = f; r.deathX = s.pos.x; r.deathY = s.pos.y; break; }
        if (s.pos.x >= end) break;
    }
    return r;
}

// ── findBestClick ──────────────────────────────────────────────────────────────
// Scans [searchStart..deathFrame) in descending order.
// Pre-condition: sim.gameStates contains [0..deathFrame].
struct FindResult {
    bool        found  = false;
    bool        solves = false;
    SolverClick click{0, 0};
    float       score  = -1.f;
    int         candidatesEval    = 0;
    int         candidatesSkipped = 0;
    struct Alt { SolverClick click{0,0}; float score = -1.f; };
    std::vector<Alt> alts; // top-3 non-winning alternatives
};

static constexpr uint64_t kLocalLookahead = 120;

static FindResult findBestClick(Level& sim,
                                 const std::vector<bool>& inputAt,
                                 const std::vector<SolverClick>& clicks,
                                 uint64_t searchStart,
                                 uint64_t deathFrame,
                                 float end,
                                 float baseX,
                                 const SolverConfig& cfg,
                                 const std::unordered_map<uint64_t,int>& clickFails)
{
    FindResult best;
    if (deathFrame == 0) return best;

    const int mh = cfg.maxHoldFrames;
    const int holdSamples[4] = {1, std::max(1,mh/12), std::max(2,mh/3), mh};

    uint64_t pf = deathFrame - 1;
    bool earlyExit = false;

    while (!earlyExit) {
        if (pf < searchStart) break;

        bool busy = false;
        for (auto& c : clicks)
            if (pf >= c.pressFrame && pf < c.releaseFrame) { busy = true; break; }

        {
            auto it = clickFails.find(pf);
            if (it != clickFails.end() && it->second >= 2) {
                ++best.candidatesSkipped;
                if (pf == 0 || pf <= searchStart) break;
                --pf; continue;
            }
        }

        if (!busy) {
            float bestScoreThisPf = -1.f;
            for (int si = 0; si < 4; ++si) {
                int hold = holdSamples[si];
                uint64_t rf = pf + (uint64_t)hold;
                if (overlaps(pf, rf, clicks)) continue;

                if (hold == 1) {
                    bool formsChain = false;
                    for (auto& c : clicks) {
                        bool prevAdj = (c.releaseFrame == pf && c.releaseFrame - c.pressFrame == 1);
                        bool nextAdj = (c.pressFrame == rf && c.releaseFrame - c.pressFrame == 1);
                        if (prevAdj || nextAdj) { formsChain = true; break; }
                    }
                    if (formsChain) continue;
                }

                int rb = (int)pf;
                sim.rollback(rb);
                uint64_t scanEnd = std::min(cfg.maxFrames, pf + kLocalLookahead);
                float score = -1.f;
                bool  solves = false;

                for (uint64_t f = (uint64_t)rb; f <= scanEnd; ++f) {
                    bool cand    = (f >= pf && f < rf);
                    bool pressed = cand || (f < inputAt.size() && inputAt[f]);
                    auto& s = sim.runFrame(pressed, cfg.dt);
                    if (s.pos.x >= end) { solves = true; score = 1e9f; break; }
                    if (s.dead)         { score = s.pos.x; break; }
                    score = s.pos.x;
                }
                sim.rollback(rb);

                ++best.candidatesEval;

                if (si == 1 && score == bestScoreThisPf) break; // hold-independent score
                if (score > bestScoreThisPf) bestScoreThisPf = score;

                if (score >= 0.f && !solves) {
                    FindResult::Alt a{{pf, rf}, score};
                    best.alts.push_back(a);
                    std::sort(best.alts.begin(), best.alts.end(),
                              [](const FindResult::Alt& x, const FindResult::Alt& y){
                                  return x.score > y.score; });
                    if (best.alts.size() > 3) best.alts.resize(3);
                }

                if (solves || score > best.score) {
                    best.score  = score;
                    best.click  = {pf, rf};
                    best.found  = true;
                    best.solves = solves;
                    if (solves) { earlyExit = true; break; }
                }
            }
            if (!earlyExit && best.found && best.score > baseX + 80.f) earlyExit = true;
        }

        if (pf == 0 || pf <= searchStart) break;
        --pf;
    }
    return best;
}

// ── solveLevel ─────────────────────────────────────────────────────────────────
// Public entry: ADAPTIVE safety margin. The big hazard inflate (cfg.hazardInflate)
// gives the most real-game robustness, but can make a genuinely tight section (or a
// human-click-rate-limited level) unsolvable — in which case the old code wrongly
// reported the level "impossible". Instead we try the requested margin first, then
// step it down and retry, only failing if even a zero-margin solve can't do it.
static SolverResult solveLevelImpl(const std::string&, SolverConfig, const std::atomic<bool>*);

SolverResult solveLevel(const std::string& levelStr,
                        SolverConfig cfg,
                        const std::atomic<bool>* cancelled)
{
    auto normalizedLevelStr = normalizeLevelString(levelStr);
    if (normalizedLevelStr.empty() || normalizedLevelStr.find(';') == std::string::npos) {
        SolverResult r; r.message = "Invalid level string"; return r;
    }

    const float start = cfg.hazardInflate;
    const float steps[] = { start, 2.0f, 1.5f, 1.0f, 0.5f, 0.0f };
    SolverResult last;
    float prev = 1e9f;
    // Always restore the solve-only ship block margin to 0 on the way out, so the
    // View Level / replay / divergence runs that follow use the true physics.
    struct MarginGuard { ~MarginGuard() { g_solveShipBlockClearance = 0.f; g_solveRobotBlockClearance = 0.f; } } marginGuard;
    for (float inf : steps) {
        if (inf > start || inf >= prev) continue;   // descending, never exceed requested
        prev = inf;
        SolverConfig c = cfg;
        c.hazardInflate   = inf;
        c.flightClearance = (start > 0.f) ? cfg.flightClearance * (inf / start)
                                          : cfg.flightClearance;
        // Step the solid-block flight margin down in lockstep with the hazard margin,
        // so a genuinely tight level that can't solve at full clearance retries with
        // less (and, at inf==0, solves frame-perfect if that's the only way).
        c.shipBlockClearance = (start > 0.f) ? cfg.shipBlockClearance * (inf / start)
                                             : cfg.shipBlockClearance;
        c.robotBlockClearance = (start > 0.f) ? cfg.robotBlockClearance * (inf / start)
                                              : cfg.robotBlockClearance;
        g_solveShipBlockClearance  = c.shipBlockClearance;   // live for this attempt
        g_solveRobotBlockClearance = c.robotBlockClearance;  // grounded-mode analog
        last = solveLevelImpl(normalizedLevelStr, c, cancelled);
        if (last.solved || (cancelled && cancelled->load())) return last;
    }
    return last;   // best-effort: lowest-margin attempt's result/message
}

static SolverResult solveLevelImpl(const std::string& levelStr,
                                   SolverConfig cfg,
                                   const std::atomic<bool>* cancelled)
{
    SolverResult result;
    if (levelStr.empty() || levelStr.find(';') == std::string::npos) {
        result.message = "Invalid level string";
        return result;
    }

    // Pick up calibrated physics params (if the calibrator has written them) so
    // the solver's model matches the real game without a rebuild.
    loadCalibFromFile(configPath("GDMod_calib.txt").c_str());

    std::ofstream dbg(debugPath("solver_debug.txt"),
                      std::ios::out | std::ios::trunc);
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    dlog("=== solveLevel start ===");

    Level sim(levelStr);
    if (sim.sections.empty()) {
        result.message = "Level has no geometry";
        return result;
    }
    sim.hazardInflate = cfg.hazardInflate;  // keep real clearance from spikes
    sim.flightHazardInflate = cfg.flightClearance;  // extra margin in flight modes

    const float end = levelEnd(sim);
    result.levelEndEstimate = end;

    // Adaptive timeout: the default 90s covers only ~28000 units at normal speed,
    // so long levels would time out *just short of the end*. Size the frame budget
    // from the level length at the slowest speed (player_speeds[0]) plus a margin.
    {
        const double minSpeed = player_speeds[0];
        const uint64_t needed =
            (uint64_t)((double)end / minSpeed / (double)cfg.dt * 1.25) + 480;
        if (needed > cfg.maxFrames) {
            dlog("adaptive maxFrames: " + std::to_string(cfg.maxFrames)
                 + " -> " + std::to_string(needed));
            cfg.maxFrames = needed;
        }
    }

    {
        std::ostringstream o;
        o << "levelEnd=" << (int)end
          << "  sections=" << sim.sections.size()
          << "  objects=" << sim.objectCount
          << "  maxFrames=" << cfg.maxFrames;
        dlog(o.str());
    }

    auto* prog = cfg.progress;
    if (prog) {
        prog->levelEndX.store(end);
        prog->addLog("Level parsed — starting search...");
    }

    // ── Phase timing profiler ─────────────────────────────────────────────────
    // Answers "why is Simulate long": records the wall-time of every phase (beam,
    // optimize, consolidate, center, robustify, path, greedy, GA) with the click
    // count before/after, streams each to the in-game overlay log, and writes a
    // sorted breakdown to GDMod_solver_profile.txt so the slowest phase is obvious.
    struct PhaseRow { std::string name; double seconds; size_t before, after; };
    std::vector<PhaseRow> phases;
    auto now = []{ return std::chrono::steady_clock::now(); };
    auto secsSince = [](std::chrono::steady_clock::time_point s){
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - s).count(); };
    auto f2 = [](double v){ char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return std::string(b); };
    // Record a completed phase + surface it live in the overlay.
    auto recordPhase = [&](const char* name, double sec, size_t before, size_t after) {
        phases.push_back({name, sec, before, after});
        dlog("[PROFILE] " + std::string(name) + " " + f2(sec) + "s  clicks "
             + std::to_string(before) + "->" + std::to_string(after));
        if (prog) prog->addLog(std::string(name) + ": " + f2(sec) + "s  ("
                               + std::to_string(before) + "->" + std::to_string(after) + " clk)");
    };
    // Time a click-transforming phase that mutates `clicks` in place.
    auto timePhase = [&](const char* name, std::vector<SolverClick>& clicks, auto&& fn) {
        size_t before = clicks.size();
        auto t = now(); fn();
        recordPhase(name, secsSince(t), before, clicks.size());
    };
    const auto tSolveStart = now();
    auto writeProfile = [&](const std::string& outcome) {
        std::ofstream pf(debugPath("GDMod_solver_profile.txt"), std::ios::trunc);
        if (!pf.is_open()) return;
        double total = 0; for (auto& r : phases) total += r.seconds;
        double wall  = secsSince(tSolveStart);
        pf << "=== SOLVER PROFILE ===\n"
           << "level end X    : " << (int)end << "\n"
           << "sections       : " << sim.sections.size()
           << "   objects: " << sim.objectCount << "\n"
           << "target fps     : " << (int)std::llround(1.0 / (double)cfg.dt) << "\n"
           << "hazardInflate  : " << f2(cfg.hazardInflate)
           << "   flightClearance: " << f2(cfg.flightClearance)
           << "   beamWidth: " << cfg.beamWidth << "\n"
           << "human limits   : " << (cfg.minClickGap > 0 ? "on" : "off")
           << "   optimize: " << (cfg.doOptimize?1:0)
           << "  center: " << (cfg.doCenter?1:0)
           << "  robustify: " << (cfg.doRobustify?1:0) << "\n"
           << "outcome        : " << outcome << "\n"
           << "WALL TOTAL     : " << f2(wall) << "s   (phases sum " << f2(total) << "s)\n\n"
           << "phase breakdown (slowest first):\n";
        auto sorted = phases;
        std::sort(sorted.begin(), sorted.end(),
                  [](const PhaseRow& a, const PhaseRow& b){ return a.seconds > b.seconds; });
        for (auto& r : sorted) {
            int pct = total > 0 ? (int)(r.seconds * 100.0 / total + 0.5) : 0;
            char line[160];
            std::snprintf(line, sizeof line, "  %-13s %7.2fs  %3d%%   clicks %zu -> %zu\n",
                          r.name.c_str(), r.seconds, pct, r.before, r.after);
            pf << line;
        }
        pf << "\nchronological:\n";
        for (auto& r : phases) {
            char line[96];
            std::snprintf(line, sizeof line, "  %-13s %7.2fs\n", r.name.c_str(), r.seconds);
            pf << line;
        }
    };

    // Shared finalizer: a found click list reaches `end`; size the budget to the
    // actual solution, clean it up, and fill the result. Returns false (leaving
    // `result` untouched) if the zero-margin gate below rejects it — the caller
    // must then treat this attempt as a non-solve and fall through, exactly like
    // any other failure of that algorithm.
    auto finalizeSolved = [&](std::vector<SolverClick>& clicks, const char* via) -> bool {
        const uint64_t solFrames = sim.currentFrame();
        cfg.maxFrames = std::max(cfg.maxFrames, solFrames + 480);
        dlog(std::string("solution via ") + via + "  length=" + std::to_string(solFrames)
             + "  maxFrames=" + std::to_string(cfg.maxFrames));
        if (prog) prog->addLog(std::string("Solved via ") + via + " — post-processing "
                               + std::to_string(clicks.size()) + " clicks...");

        if (cfg.doOptimize) {
            timePhase("optimize",    clicks, [&]{ optimizeClicks(sim, end, cfg, dbg, cancelled, prog, clicks); });
            timePhase("consolidate", clicks, [&]{ consolidateClicks(sim, end, cfg, dbg, cancelled, prog, clicks); });
            timePhase("optimize2",   clicks, [&]{ optimizeClicks(sim, end, cfg, dbg, cancelled, prog, clicks); });
        }
        if (cfg.doCenter)
            timePhase("center",      clicks, [&]{ centerClicks(sim, end, cfg, dbg, cancelled, prog, clicks); });
        if (cfg.doRobustify)
            timePhase("robustify",   clicks, [&]{ robustifyClicks(sim, end, cfg, dbg, cancelled, prog, clicks); });

        // ZERO-MARGIN correctness gate: every phase above (including robustify's own
        // "does it complete" check) runs on `sim`, which has hazardInflate/
        // flightHazardInflate set to the solver's safety margin (cfg.hazardInflate /
        // cfg.flightClearance) for the WHOLE solve — never 0. That margin is meant to
        // buy replay slack, not to be part of the claim "this solution survives".
        // Replay the exact final click list through a fresh, zero-margin Level
        // (matching View Level/Watch's own physics) and ONLY report solved if it
        // actually clears there too. Previously this check ran but was log-only —
        // `result.solved` was set unconditionally regardless of the outcome, which
        // made the outer solveLevel() adaptive-margin loop (2.5→2.0→...→0.0) dead
        // code, since the first attempt always self-reported success. Now a failure
        // here means "this attempt didn't really solve it": the caller falls through
        // to the next solver / the outer loop retries at a lower margin, as the
        // adaptive loop was originally designed to do.
        auto zm = verifyZeroMargin(levelStr, clicks, end, cfg);
        if (!zm.ok) {
            dlog("ZERO-MARGIN CHECK: FAILS — dies at f=" + std::to_string(zm.deathFrame)
                 + " x=" + std::to_string(zm.deathX) + " y=" + std::to_string(zm.deathY)
                 + " (survived only with the solver's hazardInflate="
                 + std::to_string(cfg.hazardInflate) + "/flightClearance="
                 + std::to_string(cfg.flightClearance) + " margin — NOT a real solve)");
            if (prog) prog->addLog("Zero-margin re-check FAILED near x=" + std::to_string((int)zm.deathX)
                + " — solution only survives with solver safety margin, discarding");
            return false;
        }
        dlog("ZERO-MARGIN CHECK: OK — completes at real physics too");

        result.solved      = true;
        result.clicks      = std::move(clicks);
        result.framesTotal = (int)cfg.maxFrames;
        result.maxXReached = end + 1.f;
        result.finalX      = end + 1.f;
        result.message     = std::string(via) + ": " + std::to_string(result.clicks.size()) + " clicks";
        dlog("SOLVED via " + std::string(via) + ": " + result.message);
        if (prog) {
            prog->bestX.store(end + 1.f);
            prog->clicksFound.store((int)result.clicks.size());
            prog->addLog("*** SOLVED! " + result.message + " ***");
            prog->done.store(true);
        }
        return true;
    };

    // ── Phase 0: beam search (reliable) ───────────────────────────────────────
    if (cfg.doBeam) {
        std::vector<SolverClick> beamClicks;
        auto tBeam = now();
        bool beamOk = solveLevelBeam(sim, end, cfg, dbg, cancelled, prog, beamClicks);
        recordPhase("beam", secsSince(tBeam), 0, beamClicks.size());
        if (beamOk && finalizeSolved(beamClicks, "Beam")) {
            writeProfile("SOLVED via Beam (inflate=" + f2(cfg.hazardInflate) + ")");
            return result;
        }
        // Beam failed, was cancelled, or failed the zero-margin re-check (post-
        // processing already ran on beamClicks either way) — keep its furthest-
        // reached partial path so the UI can render the trajectory (so the user can
        // SEE how far it got and why it's "impossible", instead of getting nothing).
        result.clicks = beamClicks;
        if (cancelled && cancelled->load()) {
            result.message = "Cancelled — partial path shown (open View Level).";
            writeProfile("CANCELLED during beam");
            return result;
        }
        // Under human limits, the other solvers don't enforce the click-rate cap,
        // so falling through would produce a superhuman solution. Stop here.
        if (cfg.minClickGap > 0) {
            int cps = (int)((1.0 / (double)cfg.dt) / cfg.minClickGap + 0.5); // actual fps, not hardcoded 240

            result.message = "No solution within human limits (~" + std::to_string(cps)
                           + " clicks/s). Open View Level to see how far it got — it needs "
                             "faster input there.";
            dlog("Human-limits beam failed; not falling back to unconstrained solvers");
            if (prog) { prog->addLog(result.message); prog->done.store(true); }
            writeProfile("FAILED: beam under human limits (inflate=" + f2(cfg.hazardInflate) + ")");
            return result;
        }
        result.clicks.clear();   // unconstrained fall-through reconstructs its own path
        dlog("Beam search did not solve — falling back to randomized path seeker");
        if (prog) prog->addLog("Beam search failed — trying randomized...");
    }

    // ── Phase 1+2: path seeker + optimizer ───────────────────────────────────
    {
        std::vector<SolverClick> pathClicks;
        auto tPath = now();
        bool pathOk = solveLevelPath(sim, end, cfg, dbg, cancelled, prog, pathClicks);
        recordPhase("pathseeker", secsSince(tPath), 0, pathClicks.size());
        if (pathOk && finalizeSolved(pathClicks, "PathSeeker")) {
            writeProfile("SOLVED via PathSeeker (inflate=" + f2(cfg.hazardInflate) + ")");
            return result;
        }
        // Pathfinder-only mode (cfg.doBeam == false, the new default): pathfinder
        // itself has no greedy/GA fallback — a run that doesn't finish just exports
        // whatever the search's best-so-far state was. Match that instead of
        // falling through to GDMod's own extra solvers.
        if (!cfg.doBeam) {
            result.clicks      = std::move(pathClicks); // best-partial, for View Level
            result.maxXReached = sim.latestState().pos.x;
            result.message     = "Path seeker did not finish — best partial shown "
                                 "(open View Level). Set doBeam=true to use the full pipeline.";
            dlog("PathSeeker failed, pathfinder-only mode — not falling back to greedy/GA");
            if (prog) { prog->addLog(result.message); prog->done.store(true); }
            writeProfile("FAILED: pathseeker only (inflate=" + f2(cfg.hazardInflate) + ")");
            return result;
        }
        dlog("Path seeker failed — falling back to greedy");
        if (prog) prog->addLog("Path seeker failed — trying greedy...");
    }

    // ── Greedy ────────────────────────────────────────────────────────────────
    std::vector<bool> inputAt(cfg.maxFrames + 2, false);
    std::vector<SolverClick> clicks;

    int   clicksAdded = 0;
    int   backtracks  = 0;
    float stuckAtX    = -1.f;
    int   stuckTotal  = 0;

    float                    bestGreedyX = -1.f;
    std::vector<SolverClick> bestGreedyClicks;

    int restarts = 0;
    static constexpr int kMaxRestarts   = 4;
    static constexpr int kMaxBacktracks = 60;

    std::unordered_map<uint64_t,int> clickFails;
    const auto tGreedy = now();

    std::vector<float> recentDeathYs;
    float fakeGuardX = -1.f;
    float wallX      = -1.f;
    int   wallHits   = 0;
    bool  giveUpForGA = false;   // set when a "solved" click list fails zero-margin

    auto triggerRestart = [&](const std::string& reason) -> bool {
        if (restarts >= kMaxRestarts) return false;
        ++restarts;
        dlog("\n*** RESTART #" + std::to_string(restarts) + " [" + reason + "]"
             + "  bestX=" + std::to_string((int)result.maxXReached)
             + "  wiping " + std::to_string(clicks.size()) + " clicks ***");
        if (prog) prog->addLog("Restart #" + std::to_string(restarts)
                               + " [" + reason + "]  bestX=" + std::to_string((int)result.maxXReached));
        for (auto& c : clicks)
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
                inputAt[f] = false;
        clicks.clear();
        clicksAdded  = 0;
        backtracks   = 0;
        stuckTotal   = 0;
        stuckAtX     = -1.f;
        recentDeathYs.clear();
        fakeGuardX   = -1.f;
        clickFails.clear();
        wallX        = -1.f;
        wallHits     = 0;
        return true;
    };

    while (clicksAdded <= cfg.maxClicks) {
        if (cancelled && cancelled->load()) {
            result.message = "Cancelled";
            result.clicks  = clicks;
            if (prog) { prog->addLog("Cancelled."); prog->done.store(true); }
            recordPhase("greedy", secsSince(tGreedy), 0, clicks.size());
            writeProfile("CANCELLED during greedy");
            return result;
        }

        // Forward pass ───────────────────────────────────────────────────────
        sim.rollback(0);
        bool     died       = false;
        uint64_t deathFrame = 0;
        float    thisDeathX = 0.f, thisDeathY = 0.f;

        std::vector<std::pair<float,float>> vizTraj;
        if (prog) vizTraj.reserve(512);

        for (uint64_t f = 1; f <= cfg.maxFrames; ++f) {
            bool pressed = f < inputAt.size() && inputAt[f];
            auto& state  = sim.runFrame(pressed, cfg.dt);

            if (prog && f % 4 == 0)
                vizTraj.emplace_back(state.pos.x, state.pos.y);

            if (state.pos.x >= end) {
                auto zm = verifyZeroMargin(levelStr, clicks, end, cfg);
                if (zm.ok) {
                    result.solved      = true;
                    result.clicks      = clicks;
                    result.framesTotal = (int)f;
                    result.finalX      = state.pos.x;
                    result.message     = std::to_string(clicks.size()) + " click(s) in "
                                       + std::to_string(f) + " frames";
                    if (prog) {
                        prog->bestX.store(state.pos.x);
                        prog->addLog("Solution found! " + result.message);
                        prog->done.store(true);
                    }
                    recordPhase("greedy", secsSince(tGreedy), 0, clicks.size());
                    writeProfile("SOLVED via greedy (inflate=" + f2(cfg.hazardInflate) + ")");
                    return result;
                }
                // Reached the end but only survives with the solver's safety margin —
                // not a real solve. Keep it as the best-known seed and fall through to
                // the GA fallback below instead of reporting a false success.
                if (state.pos.x > bestGreedyX) { bestGreedyX = state.pos.x; bestGreedyClicks = clicks; }
                dlog("  greedy reached end but FAILED zero-margin check (dies f="
                     + std::to_string(zm.deathFrame) + " x=" + std::to_string(zm.deathX)
                     + ") — falling through to GA");
                if (prog) prog->addLog("Greedy solution failed zero-margin re-check — trying GA...");
                giveUpForGA = true;
                break;
            }
            if (state.dead) {
                thisDeathX         = state.pos.x;
                thisDeathY         = state.pos.y;
                result.maxXReached = std::max(result.maxXReached, thisDeathX);
                if (thisDeathX > bestGreedyX) { bestGreedyX = thisDeathX; bestGreedyClicks = clicks; }
                deathFrame = f;
                died       = true;
                if (prog) vizTraj.emplace_back(thisDeathX, thisDeathY);
                break;
            }
        }

        if (giveUpForGA) break;   // reached end but failed zero-margin — go to GA fallback

        if (prog) {
            prog->iteration.fetch_add(1);
            prog->bestX.store(result.maxXReached);
        }

        {
            std::ostringstream o;
            o << "\n[PASS rst=" << restarts << " n=" << clicks.size()
              << " bt=" << backtracks << "]"
              << "  f=" << deathFrame << "  X=" << (int)thisDeathX;
            dlog(o.str());
            o.str(""); o << "  clicks:";
            for (auto& c : clicks)
                o << " [f" << c.pressFrame << "+" << (c.releaseFrame-c.pressFrame) << "]";
            dlog(o.str());
        }

        if (!died) {
            std::ostringstream oss;
            int pct = end > 0.f ? (int)(result.maxXReached * 100.f / end) : 0;
            oss << "Timeout - reached " << pct << "% (X=" << (int)result.maxXReached << "/" << (int)end << ")";
            result.message = oss.str();
            if (prog) { prog->addLog("Timeout: " + result.message); prog->done.store(true); }
            break;
        }

        // Stuck tracking ─────────────────────────────────────────────────────
        if (thisDeathX > stuckAtX + 25.f) {
            stuckTotal = 1; stuckAtX = thisDeathX;
            recentDeathYs.clear(); fakeGuardX = -1.f;
            dlog("  [progress] X=" + std::to_string((int)thisDeathX));
        } else if (std::abs(thisDeathX - stuckAtX) <= 25.f) {
            ++stuckTotal;
            recentDeathYs.push_back(thisDeathY);
            if (recentDeathYs.size() > 6) recentDeathYs.erase(recentDeathYs.begin());
            if (recentDeathYs.size() >= 4) {
                float minY = *std::min_element(recentDeathYs.begin(), recentDeathYs.end());
                float maxY = *std::max_element(recentDeathYs.begin(), recentDeathYs.end());
                if (maxY - minY > 40.f) {
                    fakeGuardX = thisDeathX;
                    dlog("  [fake-path?] Y-var=" + std::to_string((int)(maxY-minY)));
                    if (prog) prog->addLog("Fake-path? Y-var=" + std::to_string((int)(maxY-minY))
                                           + " at X=" + std::to_string((int)thisDeathX));
                }
            }
        } else {
            stuckTotal = 1; stuckAtX = thisDeathX;
            recentDeathYs.clear(); fakeGuardX = -1.f;
        }

        // Adaptive lookback ───────────────────────────────────────────────────
        uint64_t lb = (uint64_t)cfg.lookbackFrames;
        if (clicks.empty()) {
            lb = deathFrame;
        } else if (stuckTotal >= 2) {
            uint64_t doublings = std::min((uint64_t)((stuckTotal-2)/2+1), (uint64_t)8);
            lb = std::min(deathFrame, lb << doublings);
        }
        uint64_t searchStart = deathFrame > lb ? deathFrame - lb : 1;

        // Chain-breaker ───────────────────────────────────────────────────────
        {
            int chainLen = 0;
            for (int i = (int)clicks.size()-1; i >= 0; --i) {
                if (clicks[i].releaseFrame - clicks[i].pressFrame != 1) break;
                bool adj = (i+1 >= (int)clicks.size() ||
                            clicks[i].releaseFrame == clicks[i+1].pressFrame);
                if (!adj) break;
                ++chainLen;
            }
            if (chainLen >= 3) {
                for (int i = 0; i < chainLen && !clicks.empty(); ++i) {
                    auto& c = clicks.back();
                    for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
                        inputAt[f] = false;
                    clicks.pop_back();
                }
                clicksAdded = (int)clicks.size();
                ++backtracks;
                if (prog) prog->addLog("Chain-break: -" + std::to_string(chainLen)
                                       + " → " + std::to_string(clicks.size()));
                continue;
            }
        }

        if (prog) {
            gdsim::SolverProgressReport::VizSnapshot snap;
            snap.trajectory = std::move(vizTraj);
            snap.died       = died;
            snap.deathPt    = {thisDeathX, thisDeathY};
            snap.searchEndX = thisDeathX;
            if (!snap.trajectory.empty() && searchStart > 0) {
                size_t idx = std::min((size_t)(searchStart/4), snap.trajectory.size()-1);
                snap.searchStartX = snap.trajectory[idx].first;
            }
            snap.clicks.reserve(clicks.size());
            for (auto const& c : clicks) {
                if (!snap.trajectory.empty()) {
                    size_t idx = std::min((size_t)(c.pressFrame/4), snap.trajectory.size()-1);
                    snap.clicks.push_back({snap.trajectory[idx].first, snap.trajectory[idx].second});
                }
            }
            {
                std::ostringstream di;
                di << "stuck=" << stuckTotal << "  lb=" << lb << "f"
                   << "  n=" << clicks.size() << "  X=" << (int)thisDeathX
                   << "  bt=" << backtracks;
                if (fakeGuardX >= 0.f) di << "  [FAKE?]";
                snap.debugInfo = di.str();
            }
            prog->updateViz(std::move(snap));

            std::ostringstream oss;
            oss << "Pass #" << prog->iteration.load()
                << " - f" << deathFrame << "  X=" << (int)thisDeathX
                << "  n=" << clicks.size();
            if (stuckTotal >= 2) oss << "  [stuck x" << stuckTotal << "]";
            prog->addLog(oss.str());
        }

        auto res = findBestClick(sim, inputAt, clicks, searchStart, deathFrame,
                                  end, thisDeathX, cfg, clickFails);

        {
            std::ostringstream o;
            o << "  findBest: found=" << res.found << " solves=" << res.solves
              << " score=" << (int)res.score << " eval=" << res.candidatesEval;
            if (res.found) o << "  best=f" << res.click.pressFrame
                             << "+" << (res.click.releaseFrame - res.click.pressFrame);
            dlog(o.str());
        }

        if (res.found && res.score <= thisDeathX && stuckTotal < 6) {
            stuckTotal = 6;
            dlog("  [zero-improve] forcing stuckTotal=6");
        }

        bool meaningful  = res.solves || (res.found && res.score > thisDeathX + 3.f);
        bool metaRejected = false;

        // Validate + meta-validate ─────────────────────────────────────────
        if (meaningful && !res.solves) {
            for (uint64_t f = res.click.pressFrame;
                 f < res.click.releaseFrame && f < inputAt.size(); ++f)
                inputAt[f] = true;

            sim.rollback(0);
            float    valX = 0.f;
            uint64_t valDeathFrame = 0;
            for (uint64_t f = 1; f <= cfg.maxFrames; ++f) {
                bool p = f < inputAt.size() && inputAt[f];
                auto& s = sim.runFrame(p, cfg.dt);
                if (s.pos.x >= end) { valX = end + 1.f; break; }
                if (s.dead)         { valX = s.pos.x; valDeathFrame = f; break; }
                valX = s.pos.x;
            }

            {
                std::ostringstream o;
                o << "  validate: valX=" << (int)valX
                  << "  need>" << (int)(thisDeathX+3.f)
                  << "  -> " << (valX >= thisDeathX+3.f ? "ACCEPT" : "PHANTOM");
                dlog(o.str());
            }

            if (valX < thisDeathX + 3.f) {
                for (uint64_t f = res.click.pressFrame;
                     f < res.click.releaseFrame && f < inputAt.size(); ++f)
                    inputAt[f] = false;
                meaningful = false;
                if (prog) prog->addLog("Phantom: f" + std::to_string(res.click.pressFrame));
            } else if (valX < end && valDeathFrame > 0) {
                uint64_t metaStart = valDeathFrame > (uint64_t)cfg.lookbackFrames
                                     ? valDeathFrame - (uint64_t)cfg.lookbackFrames : 0;
                auto metaRes = findBestClick(sim, inputAt, clicks,
                                              metaStart, valDeathFrame,
                                              end, valX, cfg, clickFails);
                dlog("  meta-val: score=" + std::to_string((int)metaRes.score)
                     + "  need>" + std::to_string((int)(valX+3.f))
                     + "  -> " + (metaRes.found && metaRes.score > valX+3.f ? "OK" : "DEAD-END"));

                if (!metaRes.found || metaRes.score <= valX + 3.f) {
                    for (uint64_t f = res.click.pressFrame;
                         f < res.click.releaseFrame && f < inputAt.size(); ++f)
                        inputAt[f] = false;
                    meaningful   = false;
                    metaRejected = true;
                    clickFails[res.click.pressFrame]++;
                    for (int d = -2; d <= 2; ++d) {
                        if (d == 0) continue;
                        int64_t fp = (int64_t)res.click.pressFrame + d;
                        if (fp >= 0) { auto& cnt = clickFails[(uint64_t)fp]; if (cnt < 2) cnt = 2; }
                    }
                    dlog("  meta-val DEAD END: blocked f" + std::to_string(res.click.pressFrame));
                    if (prog) prog->addLog("Dead-end X=" + std::to_string((int)valX)
                                           + " (f" + std::to_string(res.click.pressFrame) + " rejected)");
                }
            }
        }

        if (meaningful) {
            SolverClick best = res.click;
            clicks.push_back(best);
            std::sort(clicks.begin(), clicks.end(),
                      [](auto& a, auto& b){ return a.pressFrame < b.pressFrame; });
            for (uint64_t f = best.pressFrame; f < best.releaseFrame && f < inputAt.size(); ++f)
                inputAt[f] = true;
            ++clicksAdded;
            dlog("  ACCEPTED #" + std::to_string(clicksAdded)
                 + " f" + std::to_string(best.pressFrame)
                 + "+" + std::to_string(best.releaseFrame-best.pressFrame)
                 + " score=" + std::to_string((int)res.score));
            if (prog) {
                prog->clicksFound.store(clicksAdded);
                prog->addLog("+ Click #" + std::to_string(clicksAdded)
                             + "  f" + std::to_string(best.pressFrame)
                             + "  hold=" + std::to_string(best.releaseFrame-best.pressFrame)
                             + "  score=" + std::to_string((int)res.score));
            }
        } else if (metaRejected) {
            dlog("  [meta-reject] retry n=" + std::to_string(clicks.size()));
        } else {
            // Backtrack ────────────────────────────────────────────────────
            if (backtracks < kMaxBacktracks && !clicks.empty()) {
                int removeCount;
                if (stuckTotal >= 6) {
                    if (std::abs(thisDeathX - wallX) < 100.f) ++wallHits;
                    else { wallX = thisDeathX; wallHits = 1; }
                    dlog("  [wall] hits=" + std::to_string(wallHits)
                         + " X=" + std::to_string((int)thisDeathX));
                    if (wallHits >= 3 && triggerRestart("hard-wall")) continue;
                    removeCount = std::max(2, (int)clicks.size() / 3);
                } else {
                    removeCount = 1 + backtracks / 4;
                }
                removeCount = std::min(removeCount, (int)clicks.size());

                for (int i = 0; i < removeCount && !clicks.empty(); ++i) {
                    auto& c = clicks.back();
                    clickFails[c.pressFrame]++;
                    for (int d = -2; d <= 2; ++d) {
                        if (d == 0) continue;
                        int64_t fp = (int64_t)c.pressFrame + d;
                        if (fp >= 0) { auto& cnt = clickFails[(uint64_t)fp]; if (cnt < 2) cnt = 2; }
                    }
                    for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
                        inputAt[f] = false;
                    clicks.pop_back();
                }
                clicksAdded = (int)clicks.size();
                ++backtracks;
                dlog("  BACKTRACK #" + std::to_string(backtracks)
                     + " -" + std::to_string(removeCount)
                     + " → " + std::to_string(clicks.size()));
                if (prog) prog->addLog("Backtrack #" + std::to_string(backtracks)
                                       + " (-" + std::to_string(removeCount)
                                       + ") → " + std::to_string(clicks.size()));
            } else {
                if (triggerRestart("backtracks-" + std::to_string(backtracks))) continue;
                int pct = end > 0.f ? (int)(result.maxXReached * 100.f / end) : 0;
                std::ostringstream oss;
                oss << "Stuck after " << clicks.size() << " click(s) — "
                    << pct << "% (X=" << (int)result.maxXReached << "/" << (int)end << ")";
                result.message = oss.str();
                dlog("GIVE UP: " + result.message);
                if (prog) { prog->addLog("Stuck: " + result.message); prog->done.store(true); }
                break;
            }
        }
    }

    // ── GA fallback ───────────────────────────────────────────────────────────
    if (!result.solved) {
        recordPhase("greedy", secsSince(tGreedy), 0, clicks.size());
        std::vector<uint64_t> seedPfs;
        for (auto& c : bestGreedyClicks) seedPfs.push_back(c.pressFrame);
        dlog("\n=== Greedy exhausted  bestX=" + std::to_string((int)bestGreedyX)
             + "  → GA (seed=" + std::to_string(seedPfs.size()) + " clicks) ===");
        auto tGA = now();
        SolverResult gaRes = solveLevelGA(sim, end, cfg, dbg, cancelled, seedPfs, levelStr);
        recordPhase("GA", secsSince(tGA), 0, gaRes.clicks.size());
        if (gaRes.solved) {
            // GA already verified its raw click list at zero margin (Solver_GA.cpp).
            // Try post-processing it for extra replay robustness, but keep GA's own
            // verified result if post-processing somehow makes it worse instead of
            // discarding a known-good solve.
            auto polished = gaRes.clicks;
            if (finalizeSolved(polished, "GA")) {
                writeProfile("SOLVED via GA (inflate=" + f2(cfg.hazardInflate) + ")");
                return result;
            }
            dlog("GA post-processing failed zero-margin re-check — keeping GA's raw verified solution");
            writeProfile("SOLVED via GA, raw (inflate=" + f2(cfg.hazardInflate) + ")");
            return gaRes;
        }
        writeProfile("FAILED after GA — bestX=" + std::to_string((int)std::max(bestGreedyX, gaRes.maxXReached))
                    + "/" + std::to_string((int)end) + " (inflate=" + f2(cfg.hazardInflate) + ")");
        return gaRes;
    }

    recordPhase("greedy", secsSince(tGreedy), 0, clicks.size());
    writeProfile("SOLVED via greedy (inflate=" + f2(cfg.hazardInflate) + ")");
    result.clicks = clicks;
    if (prog) prog->done.store(true);
    return result;
}

} // namespace gdsim