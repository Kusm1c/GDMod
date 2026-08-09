// Phase 1 – Path seeker: randomized greedy search with backtracking.
//   Ported from camila314/pathfinder. Instead of enumerating the whole game-state
//   tree (a BFS, which needs an enormous frontier to thread tight flight
//   corridors), it keeps a single trajectory and repeatedly tries bursts of
//   ~30 random press toggles, commits the burst that advances furthest, and
//   backtracks (with an exponentially growing window) out of dead-ends. O(1)
//   memory, millions of frames instead of billions — solves levels the BFS
//   couldn't in well under a second.
//
// Phase 2 – optimizer / consolidate / center: clean up the resulting inputs.

#include "Solver_internal.hpp"
#include <algorithm>
#include <chrono>
#include <random>
#include <set>

namespace gdsim {

// Simulate a set of press-toggle frames from the current state; return the frame
// reached (0 if it flew out of vertical bounds without finishing). Restores the
// simulation to where it started.
//
// NOTE: pathfinder's original stores these toggle-frame sets as std::set<uint16_t>,
// which silently truncates on any level needing more than 65535 frames (long/
// marathon levels wrap around and scatter toggles at the wrong spot). Ours uses
// uint32_t instead — an unambiguous correctness fix, not a change to the algorithm.
static int tryInputs(Level& sim, float end, float dt, bool press, std::set<uint32_t> inputs) {
    const int frame = sim.currentFrame();
    while (!inputs.empty() && !sim.gameStates.back().dead) {
        auto it = inputs.find((uint32_t)sim.currentFrame());
        if (it != inputs.end()) { press = !press; inputs.erase(it); }
        sim.runFrame(press, dt);
    }
    const int   final = sim.currentFrame();
    const float lastX = sim.latestState().pos.x;
    const float lastY = sim.latestState().pos.y;
    sim.rollback(frame);
    if (lastX < end && (lastY > 1300.f || lastY < 0.f)) return 0;
    return final;
}

bool solveLevelPath(Level& sim, float end, const SolverConfig& cfg,
                    std::ofstream& dbg, const std::atomic<bool>* cancelled,
                    SolverProgressReport* prog, std::vector<SolverClick>& out)
{
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    dlog("\n=== PATH SEEKER (randomized greedy + backtracking) ===");
    if (prog) prog->addLog("Phase 1: randomized search...");

    const float dt = cfg.dt;
    sim.rollback(0);

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 999);

    bool press    = sim.gameStates.back().button;
    int  trueBest = 0;
    int  fail     = 1;
    int  numAway  = 1000;
    int  bestEver = 0;

    // Extract press intervals from the trajectory's button transitions currently
    // recorded in sim.gameStates — shared by the success path and the timeout/
    // cancel paths below (so a partial attempt can still be shown in View Level).
    auto extractClicks = [&](std::vector<SolverClick>& dst) {
        dst.clear();
        const auto& gs = sim.gameStates;
        bool     prevB = false;
        uint64_t start = 0;
        for (uint64_t f = 1; f < gs.size(); ++f) {
            bool b = gs[f].button;
            if (b && !prevB) start = f;
            if (!b && prevB) dst.push_back({start, f});
            prevB = b;
        }
        if (prevB) dst.push_back({start, (uint64_t)gs.size()});
    };

    // Periodic live-progress viz, same cadence as the beam search's publishViz —
    // keeps the in-game overlay / solveview.exe animating a trajectory now that
    // the path seeker can be the primary (not just fallback) solver.
    auto publishViz = [&] {
        if (!prog) return;
        SolverProgressReport::VizSnapshot snap;
        const auto& gs = sim.gameStates;
        snap.trajectory.reserve(gs.size() / 4 + 1);
        for (uint64_t f = 1; f < gs.size(); f += 4)
            snap.trajectory.emplace_back(gs[f].pos.x, gs[f].pos.y);
        snap.died         = sim.gameStates.back().dead;
        snap.deathPt       = { sim.latestState().pos.x, sim.latestState().pos.y };
        snap.searchStartX  = gs.size() > 1 ? gs[1].pos.x : 0.f;
        snap.searchEndX    = sim.latestState().pos.x;
        snap.debugInfo     = "PathSeeker  iter=" + std::to_string(trueBest);
        prog->updateViz(std::move(snap));
    };

    const auto  t0      = std::chrono::steady_clock::now();
    // Scale the search budget with level length: a 20k-unit level genuinely
    // needs more than the ~45s a short one does. PathSeeker advances steadily
    // (it doesn't wedge), so giving long levels proportionally more time lets
    // them finish instead of timing out mid-run. ~150 units/s is a conservative
    // progress floor; clamped to [45s, 180s].
    const double kLimit = std::clamp((double)end / 150.0, 45.0, 180.0);
    auto elapsed = [&]{ return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count(); };

    long iters = 0;
    long lastVizAt = 0;
    while (sim.gameStates.back().pos.x < end) {
        if (cancelled && cancelled->load()) {
            dlog("PathSeeker cancelled"); extractClicks(out); return false;
        }
        if (elapsed() > kLimit) {
            dlog("PathSeeker timeout, best X=" + std::to_string((int)sim.latestState().pos.x));
            if (prog) prog->addLog("Randomized search timeout");
            extractClicks(out);
            return false;
        }

        const int frame = sim.currentFrame();
        std::set<uint32_t> bestInputs;
        int bestFrame = frame;

        auto consider = [&](std::set<uint32_t>&& inputs) {
            int nf = tryInputs(sim, end, dt, press, inputs);
            ++iters;
            if (nf > bestFrame) { bestFrame = nf; bestInputs = std::move(inputs); }
        };

        // Structured "wave fizz" candidates. The random bursts below average one
        // toggle every ~33 frames — far too coarse to thread a wave / mini-wave
        // corridor, which needs a steady high-frequency tap. These emit fixed
        // duty-cycle square waves (straight fizz plus gentle climbs/dives at
        // several amplitudes) that random search almost never stumbles onto.
        // Gated to wave gamemode so cube/ship/etc. pay zero extra cost; they only
        // get committed when they actually reach further anyway.
        if (sim.gameStates.back().vehicle.type == VehicleType::Wave) {
            static const int duties[][2] = {
                {1,1},{2,2},{3,3},{4,4},{6,6},   // fizz → straight-ish, rising amplitude
                {2,1},{3,1},{3,2},{4,3},         // climb (more thrust than release)
                {1,2},{1,3},{2,3},{3,4},         // dive (less thrust than release)
            };
            for (int startPressed = 1; startPressed >= 0 && bestFrame - frame <= 500; --startPressed) {
                for (auto& d : duties) {
                    std::set<uint32_t> inputs;
                    bool cur = press, want = (bool)startPressed;
                    for (int f = frame; f < frame + 1000; ) {
                        if (cur != want) { inputs.insert((uint32_t)f); cur = want; }
                        f += want ? d[0] : d[1];
                        want = !want;
                    }
                    consider(std::move(inputs));
                    if (bestFrame - frame > 500) break;
                }
            }
        }

        for (int i = 0; i < 300; ++i) {
            std::set<uint32_t> inputs;
            for (int j = 0; j < 30; ++j) inputs.insert((uint32_t)(frame + dist(rng)));
            consider(std::move(inputs));
            if (bestFrame - frame > 500 && fail < 1000) break;
        }

        if (bestFrame == frame) {
            // No progress — backtrack with a growing window to escape the dead-end.
            sim.rollback(std::max(std::max(frame - fail, trueBest - numAway), 1));
            press = sim.gameStates.back().button;
            fail += 5;
            if (fail > numAway + 1000) {
                numAway += 1000; fail = 1;
                if (numAway > 10000) { numAway = 1000; trueBest = 0; sim.rollback(1);
                                       press = sim.gameStates.back().button; }
            } else if (fail > 100) {
                fail += 50;
            }
        } else {
            // Commit a fraction of the best burst's progress. Wave (especially
            // mini-wave through slope-lined corridors) is unforgiving: a burst
            // that reaches far often *ends* in a doomed dive into a slope, so
            // committing a third of it locks in death and the search wedges.
            // Commit far less while in wave so we stop well before the doom and
            // re-plan from a still-recoverable spot.
            const bool wave = sim.gameStates.back().vehicle.type == VehicleType::Wave;
            const double div = wave ? 1.15 : 1.5;  // wave ~13% vs ~33% of the gap
            const int commitTo = bestFrame - (int)((bestFrame - frame) / div);
            for (int f = frame; f < commitTo; ++f) {
                if (bestInputs.contains((uint32_t)f)) press = !press;
                sim.runFrame(press, dt);
            }
        }

        const int cf = sim.currentFrame();
        if (cf - lastVizAt >= 240) { lastVizAt = cf; publishViz(); }
        if (cf > trueBest) { trueBest = cf; fail = 0; numAway = 1000; }
        if (cf > bestEver) {
            bestEver = cf;
            const float x = sim.latestState().pos.x;
            if (prog) prog->bestX.store(x);
            if (cf % 1000 < 50) {
                int pct = end > 0.f ? (int)(x * 100.f / end) : 0;
                dlog("  rand f=" + std::to_string(cf) + " X=" + std::to_string((int)x)
                     + " (" + std::to_string(pct) + "%) iters=" + std::to_string(iters));
                if (prog) prog->addLog("Search " + std::to_string(pct) + "%  X="
                                       + std::to_string((int)x));
            }
        }
    }

    // Extract press intervals from the winning trajectory's button transitions.
    extractClicks(out);

    dlog("PathSeeker SOLVED in " + std::to_string(elapsed()) + "s, "
         + std::to_string(iters) + " tries, " + std::to_string(out.size()) + " clicks");
    if (prog) {
        prog->clicksFound.store((int)out.size());
        prog->addLog("Path found! " + std::to_string(out.size()) + " clicks");
    }
    return true;
}

// ── Optimizer ────────────────────────────────────────────────────────────────

void optimizeClicks(Level& sim, float end, const SolverConfig& cfg,
                    std::ofstream& dbg, const std::atomic<bool>* cancelled,
                    SolverProgressReport* prog, std::vector<SolverClick>& clicks)
{
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    const float    dt   = cfg.dt;
    const uint64_t maxF = cfg.maxFrames;

    // Sorted so the back-to-front pass drops clicks in descending press-frame order;
    // that lets each removal check re-sim only from the dropped click onward (the
    // prefix before it is unaffected), instead of from frame 0 every time.
    std::sort(clicks.begin(), clicks.end(),
              [](const SolverClick& a, const SolverClick& b) { return a.pressFrame < b.pressFrame; });

    std::vector<bool> inp(maxF + 2, false);
    for (auto& c : clicks)
        for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < maxF; ++f)
            inp[f] = true;

    // Prime gameStates with the full base run so we can roll back into its prefix.
    sim.rollback(0);
    for (uint64_t f = 1; f <= maxF; ++f) {
        auto& s = sim.runFrame(f < inp.size() && inp[f], dt);
        if (s.dead || s.pos.x >= end) break;
    }

    int removed = 0;
    for (int i = (int)clicks.size() - 1; i >= 0; --i) {
        if (cancelled && cancelled->load()) break;
        auto c = clicks[i];

        for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < maxF; ++f) inp[f] = false;

        // Re-sim only from just before this click; frames before it are unchanged.
        const uint64_t start = c.pressFrame > 1 ? c.pressFrame - 1 : 0;
        sim.rollback((int)std::min<uint64_t>(start, (uint64_t)sim.currentFrame()));
        bool ok = false;
        // Frame convention (matches the base run above and findBestClick): step f is
        // computed by runFrame(inp[f]) when currentFrame()==f. After rollback(R),
        // currentFrame()==R, so the loop MUST start at currentFrame(), not +1 — a +1
        // skips re-applying inp[R] and shifts the whole input tail one frame late. A
        // cube tolerates that (a jump one frame off often still lands); a ship/UFO/wave
        // integrates thrust every frame, so the shift drifts the trajectory apart and
        // the removal check "still completes" in a fake reality while the true replay
        // dies — silently corrupting the solution (this was the ship-solver bug).
        for (uint64_t f = (uint64_t)sim.currentFrame(); f <= maxF; ++f) {
            bool p = f < inp.size() && inp[f];
            auto& s = sim.runFrame(p, dt);
            if (s.pos.x >= end) { ok = true; break; }
            if (s.dead) break;
        }

        if (ok) { clicks.erase(clicks.begin() + i); ++removed; }
        else {
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < maxF; ++f)
                inp[f] = true;
        }
    }

    dlog("OPTIMIZER: -" + std::to_string(removed) + " final=" + std::to_string(clicks.size()));
    if (prog) prog->addLog("Optimized: " + std::to_string(clicks.size())
                           + " clicks (-" + std::to_string(removed) + ")");
}

// ── Timing centering ───────────────────────────────────────────────────────
// Replays a full input and reports whether the level completes.
static bool runCompletes(Level& sim, float end, const SolverConfig& cfg,
                         const std::vector<bool>& inp) {
    sim.rollback(0);
    for (uint64_t f = 1; f <= cfg.maxFrames; ++f) {
        bool p  = f < inp.size() && inp[f];
        auto& s = sim.runFrame(p, cfg.dt);
        if (s.pos.x >= end) return true;
        if (s.dead)         return false;
    }
    return false;
}

void consolidateClicks(Level& sim, float end, const SolverConfig& cfg,
                       std::ofstream& dbg, const std::atomic<bool>* cancelled,
                       SolverProgressReport* prog, std::vector<SolverClick>& clicks)
{
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    if (clicks.size() < 2) return;

    std::sort(clicks.begin(), clicks.end(),
              [](const SolverClick& a, const SolverClick& b) { return a.pressFrame < b.pressFrame; });

    const uint64_t maxF = cfg.maxFrames;
    auto buildInp = [&](const std::vector<SolverClick>& cs) {
        std::vector<bool> inp(maxF + 2, false);
        for (auto& c : cs)
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < maxF; ++f) inp[f] = true;
        return inp;
    };

    const int before = (int)clicks.size();
    size_t i = 0;
    while (i + 1 < clicks.size()) {
        if (cancelled && cancelled->load()) break;

        // Try to absorb the next tap into this one: lengthen this hold by the
        // next tap's duration (same total thrust) and drop the next tap.
        const uint64_t extra = clicks[i + 1].releaseFrame - clicks[i + 1].pressFrame;
        std::vector<SolverClick> cand = clicks;
        cand[i].releaseFrame += extra;
        cand.erase(cand.begin() + i + 1);

        const bool overlap = (i + 1 < cand.size() &&
                              cand[i].releaseFrame > cand[i + 1].pressFrame);
        if (!overlap && runCompletes(sim, end, cfg, buildInp(cand))) {
            clicks = std::move(cand);   // stay at i — keep absorbing further taps
        } else {
            ++i;
        }
    }

    const int removed = before - (int)clicks.size();
    dlog("CONSOLIDATE: merged " + std::to_string(removed)
         + " taps -> " + std::to_string(clicks.size()) + " clicks");
    if (prog) prog->addLog("Consolidated: -" + std::to_string(removed)
                           + " -> " + std::to_string(clicks.size()) + " clicks");
}

void centerClicks(Level& sim, float end, const SolverConfig& cfg,
                  std::ofstream& dbg, const std::atomic<bool>* cancelled,
                  SolverProgressReport* prog, std::vector<SolverClick>& clicks)
{
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    if (clicks.empty()) return;

    const uint64_t maxF = cfg.maxFrames;
    constexpr uint64_t kMaxShift = 60; // search at most 0.25s earlier per click

    // Build the full input bitmap with click `overrideIdx` replaced by `ov`.
    auto buildInp = [&](size_t overrideIdx, SolverClick ov) {
        std::vector<bool> inp(maxF + 2, false);
        for (size_t i = 0; i < clicks.size(); ++i) {
            SolverClick c = (i == overrideIdx) ? ov : clicks[i];
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < maxF; ++f) inp[f] = true;
        }
        return inp;
    };

    int      centered = 0;
    uint64_t prevRf   = 0; // keep clicks from colliding with the previous one
    for (size_t i = 0; i < clicks.size(); ++i) {
        if (cancelled && cancelled->load()) break;

        const SolverClick cur  = clicks[i];
        const uint64_t    hold = cur.releaseFrame - cur.pressFrame;
        const uint64_t    low  = std::max(prevRf,
            cur.pressFrame > kMaxShift ? cur.pressFrame - kMaxShift : 1);

        // Prefix caching — moving click i can't affect any frame before `low`, so
        // simulate that prefix ONCE and let every candidate re-sim only from there.
        // Turns each completion check from O(levelLength, from frame 0) into
        // O(levelLength − low). This is the fix for centering being far too slow on
        // long, many-click solutions (it was re-simming from frame 0 per check).
        const uint64_t cacheAt = (low > 1) ? low - 1 : 0;
        {
            auto binp = buildInp(i, clicks[i]);
            sim.rollback(0);
            for (uint64_t f = 1; f <= cacheAt; ++f)
                sim.runFrame(f < binp.size() && binp[f], cfg.dt);
        }
        auto completesFrom = [&](SolverClick ov) -> bool {
            auto inp = buildInp(i, ov);
            sim.rollback((int)cacheAt);           // truncate to the cached prefix (no re-sim)
            // Start at currentFrame() (== cacheAt after the rollback), NOT cacheAt+1:
            // step f must be run when currentFrame()==f, else the input tail is applied
            // one frame late and flight (ship/ufo/wave) completion checks read a drifted,
            // fake trajectory. See the optimizeClicks note.
            for (uint64_t f = (uint64_t)sim.currentFrame(); f <= maxF; ++f) {
                auto& s = sim.runFrame(f < inp.size() && inp[f], cfg.dt);
                if (s.pos.x >= end) return true;
                if (s.dead)         return false;
            }
            return false;
        };

        // Binary-search the earliest start in [low, pressFrame] that still
        // completes (assumes the valid window is contiguous up to pressFrame).
        uint64_t lo = low, hi = cur.pressFrame, earliest = cur.pressFrame;
        while (lo <= hi) {
            uint64_t mid = (lo + hi) / 2;
            if (completesFrom({mid, mid + hold})) {
                earliest = mid;
                if (mid == 0) break;
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }

        // ...and the LATEST start in [pressFrame, +kMaxShift] that still completes.
        // The search/beam emits clicks at the EARLY edge of their valid window, so
        // centering only toward `earliest` (the old behaviour) left them early — an
        // orb/pad click then fires before the player reaches it on real-game replay.
        // Cap so this click's release can't run into the next click.
        const uint64_t nextPf = (i + 1 < clicks.size()) ? clicks[i + 1].pressFrame
                                                         : (maxF + 1);
        const uint64_t latestCap = (nextPf > hold + 1) ? (nextPf - hold - 1)
                                                       : cur.pressFrame;
        uint64_t llo = cur.pressFrame,
                 lhi = std::min<uint64_t>(cur.pressFrame + kMaxShift, latestCap),
                 latest = cur.pressFrame;
        while (llo <= lhi) {
            uint64_t mid = (llo + lhi) / 2;
            if (completesFrom({mid, mid + hold})) {
                latest = mid;
                llo = mid + 1;
            } else {
                if (mid == 0) break;
                lhi = mid - 1;
            }
        }

        // Centre of the true valid window [earliest, latest] at the original hold
        // = maximum timing margin on both sides → robust against replay drift.
        uint64_t bestHold   = hold;
        uint64_t bestCenter = (earliest + latest) / 2;
        uint64_t bestWidth  = latest - earliest + 1;

        // If the click is still tight at its current hold, a SHORTER tap can be
        // far more tolerant: a long hold pins both the press AND the release,
        // while a 1-frame tap frees the release entirely, often re-opening a wide
        // press window (measured: holds of 18–42 collapsing to 1 turned 0–1 frame
        // windows into 6–7). Scan a couple of short holds and keep the (hold,
        // press) with the widest contiguous completing window. Gated to tight
        // clicks so the extra full-run sims are only paid where they matter.
        if (bestWidth <= 4 && hold > 1) {
            auto widest = [&](uint64_t h, uint64_t& center) -> uint64_t {
                const uint64_t hiCap = (nextPf > h + 1)
                    ? std::min<uint64_t>(cur.pressFrame + kMaxShift, nextPf - h - 1)
                    : cur.pressFrame;
                uint64_t w = 0, c = 0, runStart = 0; bool inRun = false;
                for (uint64_t pf = low; pf <= hiCap; ++pf) {
                    if (completesFrom({pf, pf + h})) {
                        if (!inRun) { inRun = true; runStart = pf; }
                        uint64_t cw = pf - runStart + 1;
                        if (cw > w) { w = cw; c = (runStart + pf) / 2; }
                    } else inRun = false;
                }
                center = c; return w;
            };
            for (uint64_t h : {uint64_t(1), uint64_t(2)}) {
                if (h >= hold) continue;
                uint64_t c = 0, w = widest(h, c);
                if (w > bestWidth) { bestWidth = w; bestHold = h; bestCenter = c; }
            }
        }

        if ((bestCenter != cur.pressFrame || bestHold != hold) &&
            completesFrom({bestCenter, bestCenter + bestHold})) {
            clicks[i] = {bestCenter, bestCenter + bestHold};
            ++centered;
        }
        prevRf = clicks[i].releaseFrame;
    }

    dlog("CENTER: shifted " + std::to_string(centered) + "/"
         + std::to_string(clicks.size()) + " clicks toward window center");
    if (prog) prog->addLog("Centered timing: " + std::to_string(centered)
                           + "/" + std::to_string(clicks.size()) + " clicks");
}

// ── Clearance / replay-robustness ───────────────────────────────────────────
static bool isFlight(VehicleType t) {
    return t == VehicleType::Ship || t == VehicleType::Ufo
        || t == VehicleType::Wave || t == VehicleType::Swing;
}

namespace {
struct ClearScore {
    bool     completes = false;
    float    minClear  = 1e9f;   // min hazard clearance over flight frames (1e9 = no flight)
    uint64_t worstFrame = 0;     // frame of that minimum
    float    worstX     = -1.f;
};
}

// Simulate `inp` and report completion + the minimum hazard clearance over all
// flight (ship/ufo/wave/swing) frames. Once the running minimum can no longer
// beat `clearFloor`, clearance probing stops and we just finish the run to
// confirm completion — so non-improving candidates cost ~a plain replay.
static ClearScore runClearScore(Level& sim, float end, const SolverConfig& cfg,
                                const std::vector<bool>& inp, float probe, float clearFloor) {
    sim.rollback(0);
    ClearScore r;
    bool measuring = true, sawFlight = false;
    for (uint64_t f = 1; f <= cfg.maxFrames; ++f) {
        bool p  = f < inp.size() && inp[f];
        auto& s = sim.runFrame(p, cfg.dt);
        if (s.dead) { r.completes = false; r.worstFrame = f; r.worstX = s.pos.x; return r; }
        if (measuring && isFlight(s.vehicle.type)) {
            sawFlight = true;
            float c = sim.hazardClearance(s, probe);
            if (c < r.minClear) { r.minClear = c; r.worstFrame = f; r.worstX = s.pos.x; }
            if (r.minClear <= clearFloor) measuring = false;  // can't beat floor — stop probing
        }
        if (s.pos.x >= end) { r.completes = true; if (!sawFlight) r.minClear = 1e9f; return r; }
    }
    return r;
}

void robustifyClicks(Level& sim, float end, const SolverConfig& cfg,
                     std::ofstream& dbg, const std::atomic<bool>* cancelled,
                     SolverProgressReport* prog, std::vector<SolverClick>& clicks)
{
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    if (clicks.size() < 1) return;

    std::sort(clicks.begin(), clicks.end(),
              [](const SolverClick& a, const SolverClick& b) { return a.pressFrame < b.pressFrame; });

    const uint64_t maxF      = cfg.maxFrames;
    const float    probe     = 6.0f;   // clearance we care to thread (world units)
    const float    wantClear = 4.0f;   // good enough margin; stop pushing past this
    const int      minGap    = cfg.minClickGap;

    auto buildInp = [&](const std::vector<SolverClick>& cs) {
        std::vector<bool> inp(maxF + 2, false);
        for (auto& c : cs)
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < maxF; ++f) inp[f] = true;
        return inp;
    };
    auto valid = [&](const std::vector<SolverClick>& cs) -> bool {
        for (size_t i = 0; i < cs.size(); ++i) {
            if (cs[i].releaseFrame <= cs[i].pressFrame || cs[i].pressFrame < 1) return false;
            if (i + 1 < cs.size()) {
                if (cs[i].releaseFrame > cs[i + 1].pressFrame) return false;        // overlap
                if (minGap > 0 &&
                    (int64_t)cs[i + 1].pressFrame - (int64_t)cs[i].pressFrame < minGap) return false;
            }
        }
        return true;
    };

    ClearScore base = runClearScore(sim, end, cfg, buildInp(clicks), probe, -1.f);
    if (!base.completes) { dlog("ROBUSTIFY: baseline doesn't complete — skip"); return; }
    if (base.minClear >= 1e8f) { dlog("ROBUSTIFY: no flight sections — skip"); return; }

    dlog("ROBUSTIFY: baseline minClear=" + std::to_string(base.minClear)
         + " worstFrame=" + std::to_string(base.worstFrame)
         + " worstX=" + std::to_string((int)base.worstX));
    if (prog) prog->addLog("Robustify: clearance " + std::to_string((int)(base.minClear * 100) / 100.0f)
                           + "u at X=" + std::to_string((int)base.worstX));

    // Whole-click time shifts and thrust (hold-length) tweaks. Both reshape the
    // local flight arc; the variant that lifts the tightest point wins.
    static const int kShift[]  = {-4, -3, -2, -1, 1, 2, 3, 4};
    static const int kThrust[] = {-3, -2, -1, 1, 2, 3};

    const int kMaxSteps = 14;
    int steps = 0, applied = 0;

    while (base.minClear < wantClear && steps < kMaxSteps) {
        if (cancelled && cancelled->load()) break;
        ++steps;

        // Flight clearance at frame F is shaped by the inputs leading into it, so
        // gather clicks whose hold lies in a window before/around the worst frame,
        // and try the nearest handful first.
        const int64_t wf   = (int64_t)base.worstFrame;
        const int64_t back = 360, ahead = 60;
        std::vector<size_t> cand;
        for (size_t i = 0; i < clicks.size(); ++i) {
            int64_t pf = (int64_t)clicks[i].pressFrame, rf = (int64_t)clicks[i].releaseFrame;
            if (pf <= wf + ahead && rf >= wf - back) cand.push_back(i);
        }
        std::sort(cand.begin(), cand.end(), [&](size_t a, size_t b) {
            auto mid = [&](size_t i){ return (int64_t)(clicks[i].pressFrame + clicks[i].releaseFrame) / 2; };
            return std::llabs(mid(a) - wf) < std::llabs(mid(b) - wf);
        });
        if (cand.size() > 8) cand.resize(8);

        float                    bestClear = base.minClear;
        std::vector<SolverClick> bestCand;
        ClearScore               bestScore;

        for (size_t idx : cand) {
            const SolverClick cur  = clicks[idx];
            const uint64_t    hold = cur.releaseFrame - cur.pressFrame;

            auto tryCand = [&](SolverClick nc) {
                std::vector<SolverClick> cs = clicks;
                cs[idx] = nc;
                if (!valid(cs)) return;
                ClearScore sc = runClearScore(sim, end, cfg, buildInp(cs), probe, bestClear);
                if (sc.completes && sc.minClear > bestClear + 0.05f) {
                    bestClear = sc.minClear; bestCand = std::move(cs); bestScore = sc;
                }
            };
            for (int d : kShift)
                if ((int64_t)cur.pressFrame + d >= 1)
                    tryCand({cur.pressFrame + d, cur.pressFrame + d + hold});
            for (int g : kThrust)
                if ((int64_t)hold + g >= 1)
                    tryCand({cur.pressFrame, (uint64_t)((int64_t)cur.releaseFrame + g)});
        }

        if (bestCand.empty()) { dlog("ROBUSTIFY: no improvement at worst spot — stop"); break; }

        clicks = std::move(bestCand);
        base   = bestScore;
        ++applied;
        dlog("ROBUSTIFY: step " + std::to_string(steps) + " -> minClear="
             + std::to_string(base.minClear) + " worstX=" + std::to_string((int)base.worstX));
        if (prog) prog->addLog("Robustify +margin -> " + std::to_string((int)(base.minClear * 100) / 100.0f)
                               + "u (X=" + std::to_string((int)base.worstX) + ")");
    }

    dlog("ROBUSTIFY: applied " + std::to_string(applied) + " step(s), final minClear="
         + std::to_string(base.minClear));
    if (prog) prog->addLog("Robustify done: clearance " + std::to_string((int)(base.minClear * 100) / 100.0f)
                           + "u (" + std::to_string(applied) + " fixes)");
}

} // namespace gdsim
