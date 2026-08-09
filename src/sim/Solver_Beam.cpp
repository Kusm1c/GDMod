// Phase 0 — Beam search: width-bounded BFS over the game-state tree.
//
// The randomized greedy seeker (Solver_Path.cpp) keeps ONE trajectory and commits
// greedily, so it can drive into a tight flight corridor and never recover. Beam
// search instead keeps a *frontier* of the K best reachable states at each frame,
// expands every one with press / no-press, de-duplicates physically-identical
// states, and prunes back to K while preserving spread across the (y, velocity)
// envelope. That keeps the exact mid-air state a hard obstacle needs alive, so it
// threads corridors greedy commits past — reliably, and fast (cube/ground sections
// collapse to a handful of states via dedup; only flight sections use the full
// width).
//
// Reconstruction: per layer we store only {parent index, press bit} (a few bytes),
// not full states, so memory stays modest even over 20k+ frames.

#include "Solver_internal.hpp"
#include "DebugPaths.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace gdsim {

// Physics-identity key: states with the same quantized (y, velocity) and the same
// vehicle/flags at the same frame have effectively identical futures.
static uint64_t stateKey(const Player& p) {
    int64_t qy = (int64_t)std::llround(p.pos.y * 2.0);     // 0.5-unit y resolution
    int64_t qv = (int64_t)std::llround(p.velocity / 3.0);  // ~3-unit velocity bins
    uint64_t k = (uint64_t)(qy + 200000);
    k = k * 400000u + (uint64_t)(qv + 200000);
    uint64_t flags =
        ((uint64_t)p.upsideDown) | ((uint64_t)p.small << 1) |
        ((uint64_t)p.grounded   << 2) | ((uint64_t)p.button << 3) |
        ((uint64_t)p.vehicle.type << 4) | ((uint64_t)p.speed << 8);
    return k * 4096u + flags;
}

static inline bool isFlightVeh(VehicleType t) {
    return t == VehicleType::Ship || t == VehicleType::Ufo
        || t == VehicleType::Wave || t == VehicleType::Swing;
}

// Clearance tier for ranking competing states in the same phase-space cell:
//   2 = clears the full safety margin   (or non-flight — no preference applies)
//   1 = clears a reduced margin
//   0 = grazing (clears nothing)
// Higher tiers win; x breaks ties. Keeping the best-clearance state per cell makes
// the solved flight path thread gap centres instead of corner-cutting into spikes.
static uint8_t clearTier(const Level& sim, const Player& c, float margin) {
    if (margin <= 0.f || !isFlightVeh(c.vehicle.type)) return 2;
    if (sim.clearsHazards(c, margin))        return 2;
    if (sim.clearsHazards(c, margin * 0.4f)) return 1;
    return 0;
}

bool solveLevelBeam(Level& sim, float end, const SolverConfig& cfg,
                    std::ofstream& dbg, const std::atomic<bool>* cancelled,
                    SolverProgressReport* prog, std::vector<SolverClick>& out)
{
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    dlog("\n=== BEAM SEARCH ===");
    if (prog) prog->addLog("Phase 0: beam search...");

    const float    dt   = cfg.dt;
    const uint64_t maxF = cfg.maxFrames;
    const int      W    = cfg.beamWidth;
    sim.simulateMirror  = false; // single-player frontier; dual handled separately
    sim.rollback(0);

    // Beam expansion overwrites gameStates[0] (state injection), so save the real
    // spawn and restore it on every exit — otherwise the optimizer / fallbacks
    // would re-simulate from a stale mid-level state.
    const Player spawn = sim.gameStates[0];
    auto restoreSpawn = [&]{ sim.rollback(0); sim.gameStates[0] = spawn; };

    struct Link { int32_t parent; uint8_t pressed; };
    std::vector<std::vector<Link>> hist;
    hist.reserve(maxF + 2);
    hist.push_back({ {-1, 0} });             // layer 0 = spawn state

    std::vector<Player> beam = { sim.gameStates[0] };
    const int minGap = cfg.minClickGap;   // 0 = unlimited; else min frames between click starts
    // Frames since each frontier node's last click start (rising edge). Spawn is
    // "ready". Only meaningful when minGap > 0 ("human limits").
    std::vector<int16_t> beamSince = { (int16_t)(minGap > 0 ? minGap : 0) };
    // Total presses (rising edges + held frames — here, input-frame count) spent to
    // reach each frontier node. Drives the anti-over-input tie-break below: when two
    // branches are otherwise equal, the LAZIER one (fewer inputs) is kept, so the
    // solver never adds a click a no-input branch could have avoided. Fewer inputs =
    // fewer chances for the real replay to drift off gdsim's trajectory.
    std::vector<int32_t> beamPresses = { 0 };

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]{ return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count(); };
    const double kLimit = std::clamp((double)end / 150.0, 45.0, 180.0);

    // Expansion scratch reused each layer to avoid per-frame allocation churn.
    struct Cand { Player st; int32_t parent; uint8_t pressed; int16_t since; uint8_t tier; int32_t presses; };
    std::vector<Cand> cands;
    std::unordered_map<uint64_t, int> bestByKey;

    float    bestX     = 0.f;
    int      bestLayer = -1, bestIdx = -1;   // furthest committed state, for partial reconstruct
    int      solveLayer = -1, solveParent = -1, solvePressed = 0;
    int      solvePresses = 0x3fffffff;      // fewest-press finisher wins (anti-over-input)

    // Perf + failure diagnostics: how hard did the beam actually work, and — when it
    // fails — WHAT is the furthest state stuck on. Surfaced to the overlay + debug log
    // so a slow/failed beam explains itself (huge frontier vs many layers; which
    // vehicle/hazard the frontier died against) instead of just "extinct at X".
    Player   bestState = spawn;              // furthest-reached state (for stuck report)
    uint64_t statesExpanded = 0;             // total runFrame() expansions
    int      peakFrontier   = 0;             // largest frontier width seen
    long long frontierSum   = 0;             // for average frontier width
    int      layersDone      = 0;
    auto f2 = [](double v){ char b[40]; std::snprintf(b, sizeof b, "%.2f", v); return std::string(b); };

    // One-line "why is the frontier stuck here" description: the furthest state's
    // vehicle/mini/velocity + the tightest hazard just ahead of it (the pinch that
    // likely killed every branch). Only meaningful on failure.
    auto describeStuck = [&]() -> std::string {
        static const char* kVeh[] = {"Cube","Ship","Ball","Ufo","Wave","Robot","Spider","Swing"};
        int vi = (int)bestState.vehicle.type;
        std::string s = "furthest state: veh=" + std::string((vi >= 0 && vi < 8) ? kVeh[vi] : "?")
                      + " mini=" + std::to_string(bestState.small ? 1 : 0)
                      + " vel=" + f2(bestState.velocity)
                      + " y=" + f2(bestState.pos.y);
        const auto& secs = sim.sections;
        if (secs.empty()) return s;
        int si = std::clamp((int)(bestState.pos.x / (float)Level::sectionSize), 0, (int)secs.size() - 1);
        float bestGap = 1e9f; std::string haz;
        for (int sc = si; sc <= std::min((int)secs.size() - 1, si + 1); ++sc)
            for (auto& oc : secs[sc]) {
                const Object* o = oc.operator->();
                if (o->prio != 2) continue;                          // hazards only
                if (o->pos.x < bestState.pos.x - 10.f) continue;     // just ahead
                if (o->pos.x > bestState.pos.x + 70.f) continue;
                float gy = std::max({0.f, o->getBottom() - bestState.getTop(),
                                          bestState.getBottom() - o->getTop()});
                if (gy < bestGap) {
                    bestGap = gy;
                    haz = "  tightest hazard ahead: id=" + std::to_string(o->typeId)
                        + " at X=" + std::to_string((int)o->pos.x)
                        + " yGap=" + f2(gy);
                }
            }
        return s + haz;
    };
    auto perfLine = [&]() -> std::string {
        double avg = layersDone > 0 ? (double)frontierSum / (double)layersDone : 0.0;
        return "perf: expanded=" + std::to_string(statesExpanded)
             + " layers=" + std::to_string(layersDone)
             + " peakFrontier=" + std::to_string(peakFrontier)
             + "/" + std::to_string(W)
             + " avgFrontier=" + f2(avg);
    };

    // Reconstruct the press sequence of the FURTHEST state reached (bestLayer,bestIdx)
    // into `out`, so a cancelled/failed solve can still display how far it got (the
    // "why is this impossible" trajectory). Mirrors the success reconstruction below.
    auto fillPartialInto = [&](std::vector<SolverClick>& target) {
        target.clear();
        if (bestLayer <= 0 || bestLayer >= (int)hist.size()
            || bestIdx < 0 || bestIdx >= (int)hist[bestLayer].size()) return;
        std::vector<bool> inputAt(bestLayer + 2, false);
        const Link& lk0 = hist[bestLayer][bestIdx];
        inputAt[bestLayer] = (bool)lk0.pressed;
        int curParent = lk0.parent;
        for (int tt = bestLayer - 1; tt >= 1; --tt) {
            const Link& lk = hist[tt][curParent];
            inputAt[tt]    = (bool)lk.pressed;
            curParent      = lk.parent;
        }
        bool prevB = false; uint64_t start = 0;
        for (uint64_t f = 1; f <= (uint64_t)bestLayer; ++f) {
            bool b = inputAt[f];
            if (b && !prevB) start = f;
            if (!b && prevB) target.push_back({ start, f });
            prevB = b;
        }
        if (prevB) target.push_back({ start, (uint64_t)bestLayer + 1 });
    };
    auto fillBestPartial = [&]() { fillPartialInto(out); };

    // Live search-progress viz: re-simulate the furthest-reached partial path
    // (same reconstruction fillBestPartial uses) through `sim` and publish a
    // sampled trajectory, so a UI can render the beam search making progress in
    // real time. `sim` is safe to borrow here — every layer's expansion always
    // starts with `sim.rollback(0)` right before use, so nothing here needs to
    // be restored for the search itself to keep working.
    std::vector<SolverClick> vizClicks;
    auto publishViz = [&]() {
        if (!prog) return;
        fillPartialInto(vizClicks);
        SolverProgressReport::VizSnapshot snap;
        if (!vizClicks.empty() && bestLayer > 0) {
            std::vector<bool> inputAt(bestLayer + 2, false);
            for (auto& c : vizClicks)
                for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
                    inputAt[f] = true;
            sim.rollback(0);
            for (uint64_t f = 1; f <= (uint64_t)bestLayer; ++f) {
                bool p = f < inputAt.size() && inputAt[f];
                auto& s = sim.runFrame(p, dt);
                if (f % 4 == 0) snap.trajectory.emplace_back(s.pos.x, s.pos.y);
                if (s.dead) { snap.died = true; snap.deathPt = {s.pos.x, s.pos.y}; break; }
            }
        }
        snap.searchStartX = spawn.pos.x;
        snap.searchEndX   = bestX;
        snap.debugInfo    = "Beam  X=" + std::to_string((int)bestX)
                           + "  layer=" + std::to_string(bestLayer)
                           + "  frontier=" + std::to_string(beam.size());
        prog->updateViz(std::move(snap));
    };

    // ── Stuck report ──────────────────────────────────────────────────────────
    // A compact, AI-oriented post-mortem written to the repo's testlevel/ on every
    // FAILED solve (regenerated each run; a success does not touch it). Unlike the
    // verbose solver_debug.txt, this answers ONE question tersely: why couldn't the
    // frontier get past bestX? It dumps the furthest state, the tightest hazard
    // ahead, the last frames of the furthest path, and two escape probes — from the
    // furthest reached point, hold the click for a while vs never click, and report
    // what kills each. The verdict line distinguishes a SEARCH gap (an escape probe
    // survives further than the frontier best → widen/relax the search) from a
    // FIDELITY suspect (both probes die at the same pinch → likely a hitbox/physics
    // bug at that id, not a search problem). Written to testlevel/debug/ via
    // [[DebugPaths.hpp]] (falls back silently to Documents if the repo is unwritable).
    auto writeStuck = [&](const char* outcome) {
        std::ofstream f(debugPath("GDMod_stuck.txt"), std::ios::trunc);
        if (!f.is_open()) return;
        static const char* kVeh[] = {"Cube","Ship","Ball","Ufo","Wave","Robot","Spider","Swing"};
        const int pct = end > 0.f ? (int)(bestX * 100.f / end) : 0;
        const int L   = std::max(0, bestLayer);

        f << "# GDMod stuck report — why the solver frontier could not pass bestX.\n"
          << "# Compact, for AI diagnosis. Regenerated every FAILED solve; a solve\n"
          << "# that succeeds leaves this file stale (check the header frame/X).\n\n"
          << "outcome    " << outcome << "\n"
          << "reached    X=" << (int)bestX << "  " << pct << "%  of end=" << (int)end
          << "   (spawn X=" << (int)spawn.pos.x << ")\n"
          << "elapsed    " << f2(elapsed()) << "s   furthestLayer=" << L << "\n";
        {
            int vi = (int)bestState.vehicle.type;
            f << "furthest   veh=" << ((vi >= 0 && vi < 8) ? kVeh[vi] : "?")
              << " mini=" << (bestState.small ? 1 : 0)
              << " vel=" << f2(bestState.velocity)
              << " y=" << f2(bestState.pos.y)
              << " grounded=" << (bestState.grounded ? 1 : 0)
              << " up=" << (bestState.upsideDown ? 1 : 0) << "\n";
        }
        f << describeStuck() << "\n"
          << perfLine() << "\n";

        if (L <= 0) { f << "\nfrontier never advanced past spawn — spawn-kill or an\n"
                        << "unmodeled start mechanic (portal/pad at X=0).\n"; return; }

        // Reconstruct the furthest path and re-simulate to (a) capture the approach
        // tail and (b) run the two escape probes past the furthest point.
        std::vector<SolverClick> path; fillPartialInto(path);
        std::vector<bool> inAt(L + 2, false);
        for (auto& c : path)
            for (uint64_t fr = c.pressFrame; fr < c.releaseFrame && fr < inAt.size(); ++fr)
                inAt[fr] = true;

        // The beam injects frontier states into gameStates[0] during expansion, so
        // restore the REAL spawn before re-simulating (same reason as restoreSpawn).
        // rollback(0) below then always resets to this genuine spawn.
        restoreSpawn();

        struct TF { int f, veh, mini, press, dead; double x, y, v; };
        std::vector<TF> tail;
        sim.rollback(0);
        for (uint64_t fr = 1; fr <= (uint64_t)L; ++fr) {
            bool p = fr < inAt.size() && inAt[fr];
            const Player& s = sim.runFrame(p, dt);
            tail.push_back({(int)fr, (int)s.vehicle.type, s.small?1:0, p?1:0, s.dead?1:0,
                            s.pos.x, s.pos.y, s.velocity});
            if (s.dead) break;
        }
        const int N = 24, from = std::max(0, (int)tail.size() - N);
        f << "\n# approach: last " << (int)tail.size() - from
          << " frames of the furthest path (f x y vel veh mini press dead):\n";
        for (int i = from; i < (int)tail.size(); ++i) {
            const TF& r = tail[i];
            char b[128];
            std::snprintf(b, sizeof b, "  %6d %8.1f %7.1f %7.2f  %d %d %d %d\n",
                          r.f, r.x, r.y, r.v, r.veh, r.mini, r.press, r.dead);
            f << b;
        }

        // Escape probes: replay the furthest path, then FORCE a fixed input for K
        // frames and see how far it gets / what kills it. Two extremes bracket the
        // local option space the frontier had at the pinch.
        const int K = 40;
        float probeReach = bestX;        // furthest X the current probe reached
        auto probe = [&](bool hold) -> std::string {
            sim.rollback(0);
            Player last{};
            for (uint64_t fr = 1; fr <= (uint64_t)(L + K); ++fr) {
                bool p = fr <= (uint64_t)L ? (fr < inAt.size() && inAt[fr]) : hold;
                last = sim.runFrame(p, dt);
                probeReach = last.pos.x;
                if (last.dead) {
                    char b[160];
                    std::snprintf(b, sizeof b, "dies f%llu X=%.0f on %s id=%d",
                        (unsigned long long)fr, last.pos.x,
                        last.deathCause ? last.deathCause : "?", last.deathObjType);
                    return b;
                }
            }
            char b[64]; std::snprintf(b, sizeof b, "survives to X=%.0f (+%.0f)",
                                      last.pos.x, last.pos.x - bestX);
            return b;
        };
        std::string pHold = probe(true);  float reachHold = probeReach; probeReach = bestX;
        std::string pRel  = probe(false); float reachRel  = probeReach;
        f << "\nprobe hold  " << pHold << "\n"
          << "probe rel   " << pRel  << "\n";

        // Verdict heuristic: search gap vs fidelity suspect vs capacity-bound.
        float bestProbe = std::max(reachHold, reachRel);
        f << "\nverdict    ";
        if (bestProbe > bestX + 20.f)
            f << "SEARCH GAP — an escape probe reaches X=" << (int)bestProbe
              << " (> frontier best " << (int)bestX << "). The move existed but the\n"
              << "           frontier pruned it: raise beamWidth, relax minClickGap, or\n"
              << "           refine the phase-cell key (stateKey y/vel resolution).";
        else if (peakFrontier >= W)
            f << "CAPACITY-BOUND — peakFrontier hit the width cap (" << W
              << ") yet no branch passed the pinch. Likely a genuinely tight/impossible\n"
              << "           corridor OR a fidelity bug making it look impossible; inspect the\n"
              << "           tightest-hazard id above against a real hitbox capture.";
        else
            f << "FIDELITY SUSPECT — both hold & release die at the pinch and the search\n"
              << "           was not capacity-bound. If the real level is passable here, the bug\n"
              << "           is gdsim (hitbox/physics at the tightest-hazard id), not the solver.";
        f << "\n";
    };

    for (uint64_t t = 0; t < maxF && solveLayer < 0; ++t) {
        if (cancelled && cancelled->load()) { dlog("Beam cancelled"); writeStuck("CANCELLED"); fillBestPartial(); restoreSpawn(); return false; }
        if (elapsed() > kLimit) {
            int pct = end > 0.f ? (int)(bestX * 100.f / end) : 0;
            dlog("Beam TIMEOUT at X=" + std::to_string((int)bestX) + " (" + std::to_string(pct)
                 + "%)  layer=" + std::to_string(t) + "  beam=" + std::to_string(beam.size())
                 + "  after=" + f2(elapsed()) + "s");
            dlog("  " + describeStuck());
            dlog("  " + perfLine());
            if (prog) {
                prog->addLog("Beam TIMEOUT " + std::to_string(pct) + "% (" + f2(elapsed()) + "s)");
                prog->addLog(describeStuck());
            }
            writeStuck("TIMEOUT");
            fillBestPartial();
            restoreSpawn();
            return false;
        }

        cands.clear();
        cands.reserve(beam.size() * 2);

        // Expand every frontier node with no-press and press. Under human limits,
        // a rising edge (start of a new click) is only allowed once enough frames
        // have passed since this state's previous click start.
        const std::vector<Link>& curLinks = hist.back();
        for (int i = 0; i < (int)beam.size(); ++i) {
            const int lastIn = curLinks[i].pressed;   // input bit that produced beam[i]
            const int sc     = beamSince[i];
            // NOTE: no `&& solveLayer < 0` guard — once a finisher is found we keep
            // scanning the rest of this layer to pick the fewest-press one (below),
            // then break out of the frame loop via `if (solveLayer >= 0) break;`.
            for (int pb = 0; pb < 2; ++pb) {
                const bool rising = (pb == 1 && lastIn == 0);
                if (minGap > 0 && rising && sc < minGap) continue; // too soon to click again
                sim.rollback(0);
                sim.gameStates[0] = beam[i];
                Player& c = sim.runFrame((bool)pb, dt);
                ++statesExpanded;
                if (c.dead) continue;
                const int32_t presses = beamPresses[i] + pb;
                if (c.pos.x >= end) {            // reached the finish
                    // Anti-over-input: among every branch crossing the end THIS layer,
                    // keep the one with the fewest total presses (the laziest finisher)
                    // rather than the first enumerated. All finishers share this frame,
                    // so x is identical between them — this never costs progress.
                    if (solveLayer < 0 || presses < solvePresses) {
                        solveLayer = (int)t + 1; solveParent = i;
                        solvePressed = pb;       solvePresses = presses;
                    }
                    continue;
                }
                int16_t ns = (int16_t)(rising ? 0 : std::min(sc + 1, 30000));
                uint8_t tier = clearTier(sim, c, cfg.hazardMargin);
                cands.push_back({ c, i, (uint8_t)pb, ns, tier, presses });
            }
        }
        if (solveLayer >= 0) break;
        if (cands.empty()) {
            int pct = end > 0.f ? (int)(bestX * 100.f / end) : 0;
            dlog("Beam DEAD-END (frontier extinct — every branch died) at layer "
                 + std::to_string(t) + "  bestX=" + std::to_string((int)bestX)
                 + " (" + std::to_string(pct) + "%)");
            dlog("  " + describeStuck());
            dlog("  " + perfLine());
            if (prog) {
                prog->addLog("Beam DEAD-END at " + std::to_string(pct) + "% (X="
                             + std::to_string((int)bestX) + ") — every branch dies here");
                prog->addLog(describeStuck());
            }
            writeStuck("DEAD-END");
            fillBestPartial();
            restoreSpawn();
            return false;
        }

        // ── Waypoint corridor (user-drawn guide path) ────────────────────────
        // SOFT bias, never a hard filter: flag each candidate as in/out of the
        // y-corridor around the interpolated guide path, then PREFER in-corridor
        // states in the ranking below. The phase-space prune still keeps the best
        // state per (y,velocity) cell, so out-of-corridor states needed to set up
        // future manoeuvres are never removed — the guide only decides which cells
        // to drop FIRST when the frontier overflows W. A hard filter here dead-ended
        // solvable levels (a sparse guide's straight-line interp leaves the true
        // curved path outside the band), so we only rank, never cull.
        std::vector<uint8_t> inCorr(cands.size(), 1);
        if (!cfg.waypoints.empty()) {
            const auto& wp = cfg.waypoints;
            auto yTargetAt = [&](float x, float& yt) -> bool {
                if (x <= wp.front().first || x >= wp.back().first) return false;
                for (size_t k = 1; k < wp.size(); ++k) {
                    if (x <= wp[k].first) {
                        float x0 = wp[k - 1].first, x1 = wp[k].first;
                        float f = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.f;
                        yt = wp[k - 1].second + f * (wp[k].second - wp[k - 1].second);
                        return true;
                    }
                }
                return false;
            };
            for (size_t i = 0; i < cands.size(); ++i) {
                float yt = 0.f;
                inCorr[i] = (!yTargetAt(cands[i].st.pos.x, yt) ||
                             std::abs(cands[i].st.pos.y - yt) <= cfg.waypointCorridor) ? 1 : 0;
            }
        }

        // Rank competing states by clearance tier first, then guide-corridor
        // membership, then progress (x). Used by both the dedup and the phase-space
        // prune so safe (non-grazing) flight states are kept over ones that merely
        // reached further by hugging a spike — without ever discarding the only
        // state in a cell (solvability preserved).
        auto better = [&](int A, int B) {
            if (cands[A].tier != cands[B].tier) return cands[A].tier > cands[B].tier;
            if (inCorr[A] != inCorr[B]) return inCorr[A] > inCorr[B];
            if (cands[A].st.pos.x != cands[B].st.pos.x) return cands[A].st.pos.x > cands[B].st.pos.x;
            return cands[A].presses < cands[B].presses;   // anti-over-input: laziest wins ties
        };

        // Deduplicate by exact physics key first (collapses truly-identical
        // states), keeping the best (tier, x) representative.
        bestByKey.clear();
        bestByKey.reserve(cands.size() * 2);
        for (int i = 0; i < (int)cands.size(); ++i) {
            uint64_t key = stateKey(cands[i].st);
            if (minGap > 0)  // distinguish click-readiness so the constraint doesn't merge away
                key = key * (uint64_t)(minGap + 1) + (uint64_t)std::min((int)cands[i].since, minGap);
            auto it = bestByKey.find(key);
            if (it == bestByKey.end() || better(i, it->second))
                bestByKey[key] = i;
        }
        std::vector<int> uniq;
        uniq.reserve(bestByKey.size());
        for (auto& kv : bestByKey) uniq.push_back(kv.second);

        // Prune to W via a 2D (y, velocity) reachability grid. Ship/UFO velocity
        // is continuous, so a hard corridor needs the frontier to evenly cover the
        // (position, velocity) phase space — keeping the best (furthest) state in
        // each cell rather than just the globally-fastest few (which all funnel
        // into the same dead-end). Bins adapt to the frontier's current spread.
        std::vector<int> keep;
        if ((int)uniq.size() <= W) {
            keep = std::move(uniq);
        } else {
            float ymin=1e9f, ymax=-1e9f, vmin=1e9f, vmax=-1e9f;
            for (int idx : uniq) {
                const Player& s = cands[idx].st;
                ymin = std::min(ymin, s.pos.y); ymax = std::max(ymax, s.pos.y);
                vmin = std::min(vmin, (float)s.velocity); vmax = std::max(vmax, (float)s.velocity);
            }
            int nyb = std::max(1, (int)std::sqrt((double)W * 1.6) + 1); // ~ y resolution
            int nvb = std::max(1, W / nyb + 1);                         // ~ v resolution
            const float yspan = std::max(1.f, ymax - ymin);
            const float vspan = std::max(1.f, vmax - vmin);
            std::unordered_map<int, int> cellBest;  // cellId -> uniq idx (best x)
            cellBest.reserve(uniq.size() * 2);
            for (int idx : uniq) {
                const Player& s = cands[idx].st;
                int yb = std::clamp((int)((s.pos.y - ymin) / yspan * (nyb - 1)), 0, nyb - 1);
                int vb = std::clamp((int)(((float)s.velocity - vmin) / vspan * (nvb - 1)), 0, nvb - 1);
                int cell = yb * nvb + vb;
                if (minGap > 0)  // keep a "ready" and a "not-ready" representative per cell
                    cell = cell * 2 + (cands[idx].since >= minGap ? 1 : 0);
                auto it = cellBest.find(cell);
                if (it == cellBest.end() || better(idx, it->second))
                    cellBest[cell] = idx;
            }
            keep.reserve(cellBest.size());
            for (auto& kv : cellBest) keep.push_back(kv.second);
            if ((int)keep.size() > W) {           // rare: trim least-progressed cells
                std::partial_sort(keep.begin(), keep.begin() + W, keep.end(),
                    [&](int a, int c){ return better(a, c); });
                keep.resize(W);
            }
        }

        // Commit the new frontier + its back-links.
        std::vector<Player>  next;
        std::vector<Link>    links;
        std::vector<int16_t> nextSince;
        std::vector<int32_t> nextPresses;
        next.reserve(keep.size());
        links.reserve(keep.size());
        nextSince.reserve(keep.size());
        nextPresses.reserve(keep.size());
        for (int j = 0; j < (int)keep.size(); ++j) {
            int idx = keep[j];
            if (cands[idx].st.pos.x > bestX) {
                bestX     = cands[idx].st.pos.x;
                bestLayer = (int)hist.size();   // index this links vector gets after push_back
                bestIdx   = j;
                bestState = cands[idx].st;      // copy the furthest state for the stuck report
            }
            nextPresses.push_back(cands[idx].presses);
            next.push_back(std::move(cands[idx].st));
            links.push_back({ cands[idx].parent, cands[idx].pressed });
            nextSince.push_back(cands[idx].since);
        }
        beam      = std::move(next);
        beamSince = std::move(nextSince);
        beamPresses = std::move(nextPresses);
        hist.push_back(std::move(links));
        peakFrontier = std::max(peakFrontier, (int)beam.size());
        frontierSum += (long long)beam.size();
        ++layersDone;

        if (prog) prog->bestX.store(bestX);
        if (t % 240 == 0) {
            int pct = end > 0.f ? (int)(bestX * 100.f / end) : 0;
            dlog("  beam t=" + std::to_string(t) + " X=" + std::to_string((int)bestX)
                 + " (" + std::to_string(pct) + "%) frontier=" + std::to_string(beam.size()));
            if (prog) prog->addLog("Beam " + std::to_string(pct) + "%  X=" + std::to_string((int)bestX)
                                   + "  w=" + std::to_string(beam.size()));
        }
        // Live viz less often than the text log (re-simulating the partial path
        // has real cost) — every ~1s of search progress is plenty for a live view.
        if (prog && t % 240 == 0) publishViz();
    }

    if (solveLayer < 0) {
        int pct = end > 0.f ? (int)(bestX * 100.f / end) : 0;
        dlog("Beam ran out of frames without reaching the end  bestX="
             + std::to_string((int)bestX) + " (" + std::to_string(pct) + "%)");
        dlog("  " + describeStuck());
        dlog("  " + perfLine());
        if (prog) { prog->addLog("Beam exhausted frames at " + std::to_string(pct) + "%");
                    prog->addLog(describeStuck()); }
        writeStuck("EXTINCT");
        fillBestPartial(); restoreSpawn(); return false;
    }

    // Reconstruct the press sequence by walking parent links back to the spawn.
    std::vector<bool> inputAt(solveLayer + 2, false);
    inputAt[solveLayer] = (bool)solvePressed;
    int curParent = solveParent;
    for (int t = solveLayer - 1; t >= 1; --t) {
        const Link& lk = hist[t][curParent];
        inputAt[t]     = (bool)lk.pressed;
        curParent      = lk.parent;
    }

    // Press intervals -> clicks.
    out.clear();
    bool prevB = false; uint64_t start = 0;
    for (uint64_t f = 1; f <= (uint64_t)solveLayer; ++f) {
        bool b = inputAt[f];
        if (b && !prevB) start = f;
        if (!b && prevB) out.push_back({ start, f });
        prevB = b;
    }
    if (prevB) out.push_back({ start, (uint64_t)solveLayer + 1 });

    dlog("BEAM SOLVED in " + f2(elapsed()) + "s, "
         + std::to_string(solveLayer) + " frames, " + std::to_string(out.size()) + " clicks");
    dlog("  " + perfLine());
    if (prog) {
        prog->clicksFound.store((int)out.size());
        prog->addLog("Beam solved! " + std::to_string(out.size()) + " clicks in " + f2(elapsed()) + "s");
        prog->addLog(perfLine());

        // Final full-path viz so a live viewer shows the complete solved run,
        // not just the last periodic partial sample.
        SolverProgressReport::VizSnapshot snap;
        std::vector<bool> finalInput(solveLayer + 2, false);
        for (auto& c : out)
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < finalInput.size(); ++f)
                finalInput[f] = true;
        sim.rollback(0);
        for (uint64_t f = 1; f <= (uint64_t)solveLayer; ++f) {
            bool p = f < finalInput.size() && finalInput[f];
            auto& s = sim.runFrame(p, dt);
            if (f % 4 == 0) snap.trajectory.emplace_back(s.pos.x, s.pos.y);
        }
        snap.searchStartX = spawn.pos.x;
        snap.searchEndX   = end;
        snap.debugInfo    = "SOLVED  " + std::to_string(out.size()) + " clicks";
        prog->updateViz(std::move(snap));
    }
    restoreSpawn();   // hand a clean spawn state to the optimize/center passes
    return true;
}

} // namespace gdsim
