// Genetic Algorithm fallback.
// Chromosome: sorted list of press-frames (hold=1 each, cube style).
// Fitness: furthest X reached; end+1 if solved.
// Called when the greedy exhausts its restart budget.

#include "Solver_internal.hpp"
#include <algorithm>
#include <sstream>
#include <random>

namespace gdsim {

SolverResult solveLevelGA(
    Level&                       sim,
    float                        end,
    const SolverConfig&          cfg,
    std::ofstream&               dbg,
    const std::atomic<bool>*     cancelled,
    const std::vector<uint64_t>& seedPfs,
    const std::string&           levelStr)
{
    SolverResult res;
    res.levelEndEstimate = end;
    auto dlog = [&](const std::string& s) { if (dbg.is_open()) dbg << s << "\n"; };
    auto* prog = cfg.progress;
    const uint64_t maxF = cfg.maxFrames;
    const float    dt   = cfg.dt;

    dlog("\n=== GA started  seed=" + std::to_string(seedPfs.size()) + " clicks ===");
    if (prog) prog->addLog("Greedy stuck → GA  seed=" + std::to_string(seedPfs.size()) + " clicks");

    std::mt19937 rng(42);

    struct Ind {
        std::vector<uint64_t> pfs;
        float    fitness = -1.f;
        uint64_t deathF  = 0;
    };

    // Shared-prefix rollback: simulate the current best individual once per
    // generation to populate gameStates[0..deathF].  Subsequent child evals
    // roll back only to their first diverging frame, saving ~50x on frontier mutations.
    std::vector<uint64_t> warmupPfs;
    std::vector<bool>     inWarmup;
    uint64_t              warmupDeathF = 0;

    int totalEvals = 0;
    auto evalInd = [&](Ind& ind) {
        ++totalEvals;
        std::vector<bool> inputAt(maxF + 2, false);
        for (uint64_t pf : ind.pfs) if (pf > 0 && pf < maxF) inputAt[pf] = true;

        // Divergence frame: earliest press that differs from the warmup individual.
        uint64_t divF = 0;
        if (warmupDeathF > 0 && warmupDeathF <= maxF && !inWarmup.empty()) {
            uint64_t firstDiff = maxF;
            for (uint64_t pf : ind.pfs)
                if (pf > 0 && pf < maxF && pf < (uint64_t)inWarmup.size()
                    && !inWarmup[pf] && pf < firstDiff)
                    firstDiff = pf;
            for (uint64_t pf : warmupPfs)
                if (pf > 0 && pf < maxF && pf < (uint64_t)inputAt.size()
                    && !inputAt[pf] && pf < firstDiff)
                    firstDiff = pf;
            if (firstDiff < maxF && firstDiff > 0) divF = firstDiff - 1;
            if (divF >= warmupDeathF) divF = 0;
        }
        // Defensive: never extend gameStates with default-constructed (null-level) Players.
        if (divF > 0 && divF > (uint64_t)sim.currentFrame()) divF = 0;

        sim.rollback((int)divF);
        float maxX = 0.f;
        ind.deathF = 0;
        // Start at currentFrame() (NOT divF+1): step f is computed when currentFrame()==f.
        // After rollback(divF), currentFrame()==divF, so +1 would skip re-applying
        // inputAt[divF] and shift the child's whole input tail one frame late — giving a
        // drifted, wrong fitness for flight (ship/ufo/wave) individuals. See optimizeClicks.
        for (uint64_t f = (uint64_t)sim.currentFrame(); f <= maxF; ++f) {
            bool p = f < (uint64_t)inputAt.size() && inputAt[f];
            auto& s = sim.runFrame(p, dt);
            if (s.pos.x >= end) { ind.fitness = end + 1.f; return; }
            if (s.dead) {
                ind.fitness = s.pos.x;
                ind.deathF  = f;
                ind.pfs.erase(
                    std::remove_if(ind.pfs.begin(), ind.pfs.end(),
                                   [f](uint64_t pf){ return pf >= f; }),
                    ind.pfs.end());
                return;
            }
            maxX = std::max(maxX, s.pos.x);
        }
        ind.fitness = maxX;
    };

    auto randPf = [&]() -> uint64_t {
        return std::uniform_int_distribution<uint64_t>(1, maxF - 1)(rng);
    };
    auto makeRandom = [&](int n) -> Ind {
        Ind ind;
        for (int i = 0; i < n; ++i) ind.pfs.push_back(randPf());
        std::sort(ind.pfs.begin(), ind.pfs.end());
        return ind;
    };

    const int POP = 40;
    std::vector<Ind> pop(POP);

    pop[0].pfs = seedPfs;
    evalInd(pop[0]);
    dlog("GA seed fitness=" + std::to_string((int)pop[0].fitness));

    for (int i = 1; i < POP / 2; ++i) {
        pop[i].pfs = seedPfs;
        int ns = std::uniform_int_distribution<int>(1, 4)(rng);
        for (int k = 0; k < ns && !pop[i].pfs.empty(); ++k) {
            int idx  = std::uniform_int_distribution<int>(0, (int)pop[i].pfs.size()-1)(rng);
            int shft = std::uniform_int_distribution<int>(-25, 25)(rng);
            int64_t np = (int64_t)pop[i].pfs[idx] + shft;
            if (np > 0 && (uint64_t)np < maxF) pop[i].pfs[idx] = (uint64_t)np;
        }
        int extra = std::uniform_int_distribution<int>(0, 3)(rng);
        for (int k = 0; k < extra; ++k) pop[i].pfs.push_back(randPf());
        std::sort(pop[i].pfs.begin(), pop[i].pfs.end());
        evalInd(pop[i]);
    }
    for (int i = POP / 2; i < POP; ++i) {
        pop[i] = makeRandom(std::uniform_int_distribution<int>(10, 40)(rng));
        evalInd(pop[i]);
    }

    const int MAX_EVALS     = 300'000;
    float     bestFit       = -1.f;
    int       stallGen      = 0;
    uint64_t  frontierFrame = 0;

    // Cache the last individual checked against zero-margin physics so an unchanged
    // pop[0] (an elite that survives generation to generation once it hits the
    // end+1 fitness cap) doesn't get re-simulated and re-logged every generation.
    std::vector<uint64_t> lastZMCheckedPfs;
    bool                  lastZMCheckedOk = false;

    auto sortPop = [&]() {
        std::sort(pop.begin(), pop.end(),
                  [](const Ind& a, const Ind& b){ return a.fitness > b.fitness; });
    };

    auto updateViz = [&](int gen) {
        if (!prog) return;
        std::vector<bool> inp2(maxF + 2, false);
        for (uint64_t pf : pop[0].pfs) if (pf > 0 && pf < maxF) inp2[pf] = true;
        sim.rollback(0);
        SolverProgressReport::VizSnapshot snap;
        snap.trajectory.reserve(512);
        float dvX = 0.f, dvY = 0.f;
        bool  dvDied = false;
        for (uint64_t f = 1; f <= maxF; ++f) {
            bool p = f < (uint64_t)inp2.size() && inp2[f];
            auto& s = sim.runFrame(p, dt);
            if (f % 4 == 0) snap.trajectory.emplace_back(s.pos.x, s.pos.y);
            if (f < (uint64_t)inp2.size() && inp2[f])
                snap.clicks.push_back({s.pos.x, s.pos.y});
            if (s.pos.x >= end) { dvX = s.pos.x; dvY = s.pos.y; break; }
            if (s.dead)         { dvX = s.pos.x; dvY = s.pos.y; dvDied = true; break; }
        }
        snap.deathPt  = {dvX, dvY};
        snap.died     = dvDied;
        snap.searchEndX = dvX;
        frontierFrame = pop[0].deathF;
        int pct = end > 0.f ? (int)(pop[0].fitness * 100.f / end) : 0;
        float sumF = 0.f;
        for (auto& ind : pop) sumF += ind.fitness;
        std::ostringstream ov;
        ov << "=== GENETIC ALGORITHM ===\n"
           << "Gen " << gen << "   Evals " << totalEvals << "\n"
           << "Best X = " << (int)pop[0].fitness << "  (" << pct << "%)\n"
           << "Clicks: " << pop[0].pfs.size()
           << "   Frontier: f" << warmupDeathF << "\n"
           << "Pop avg: " << (int)(sumF / POP) << "   Stall: " << stallGen << "/60\n"
           << "Seed: " << seedPfs.size() << " greedy clicks";
        snap.debugInfo = ov.str();
        prog->updateViz(std::move(snap));
    };

    auto runWarmup = [&]() {
        if (pop[0].deathF > 0 && pop[0].deathF <= maxF) {
            warmupPfs    = pop[0].pfs;
            warmupDeathF = pop[0].deathF;
            std::vector<bool> wu(maxF + 2, false);
            for (uint64_t pf : warmupPfs) if (pf > 0 && pf < maxF) wu[pf] = true;
            sim.rollback(0);
            for (uint64_t f = 1; f <= warmupDeathF; ++f)
                sim.runFrame(f < (uint64_t)wu.size() && wu[f], dt);
            inWarmup.assign(maxF + 2, false);
            for (uint64_t pf : warmupPfs)
                if (pf > 0 && pf < (uint64_t)inWarmup.size()) inWarmup[pf] = true;
        } else {
            warmupDeathF = 0;
            warmupPfs.clear();
            inWarmup.clear();
        }
    };

    for (int gen = 0; totalEvals < MAX_EVALS; ++gen) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) break;

        sortPop();

        if (pop[0].fitness >= end) {
            std::vector<SolverClick> gaClicks;
            gaClicks.reserve(pop[0].pfs.size());
            for (uint64_t pf : pop[0].pfs) gaClicks.push_back({pf, pf + 1});
            std::sort(gaClicks.begin(), gaClicks.end(),
                      [](auto& a, auto& b){ return a.pressFrame < b.pressFrame; });

            bool zmOk;
            if (pop[0].pfs == lastZMCheckedPfs) {
                zmOk = lastZMCheckedOk;
            } else {
                auto zm = verifyZeroMargin(levelStr, gaClicks, end, cfg);
                zmOk = zm.ok;
                lastZMCheckedPfs = pop[0].pfs;
                lastZMCheckedOk  = zmOk;
                if (!zmOk) {
                    dlog("GA solution reached end but FAILED zero-margin check (dies f="
                         + std::to_string(zm.deathFrame) + " x=" + std::to_string(zm.deathX)
                         + ") — only survives with the solver's safety margin, not a real solve");
                    if (prog) prog->addLog("GA solution failed zero-margin re-check — still searching...");
                }
            }

            if (zmOk) {
                res.solved      = true;
                res.maxXReached = end + 1.f;
                res.finalX      = end + 1.f;
                res.clicks      = gaClicks;
                res.message = "GA solved gen=" + std::to_string(gen)
                            + "  evals=" + std::to_string(totalEvals)
                            + "  clicks=" + std::to_string(res.clicks.size());
                dlog("GA SOLVED: " + res.message);
                updateViz(gen);
                if (prog) {
                    prog->addLog("*** GA SOLVED! " + std::to_string(pop[0].pfs.size()) + " clicks ***");
                    prog->done.store(true);
                }
                return res;
            }
            // Demote so a genuinely different, zero-margin-clean individual can
            // outrank this one later instead of it staying stuck at rank 0 forever
            // (evalInd caps every "reached end" individual at exactly end+1, so ties
            // would otherwise never let evolution move past a known-bad winner).
            pop[0].fitness = end - 1.f;
        }

        // Warmup: must run before any evalInd this generation (including stall injection).
        runWarmup();

        if (pop[0].fitness > bestFit + 5.f) {
            bestFit       = pop[0].fitness;
            stallGen      = 0;
            frontierFrame = pop[0].deathF;
            {
                float sumF = 0.f; for (auto& ind : pop) sumF += ind.fitness;
                std::ostringstream o;
                o << "  GA gen=" << gen << "  best=" << (int)bestFit
                  << "  avg=" << (int)(sumF/POP) << "  n=" << pop[0].pfs.size()
                  << "  evals=" << totalEvals;
                dlog(o.str());
            }
            updateViz(gen);
            if (prog) {
                int pct = end > 0.f ? (int)(bestFit * 100.f / end) : 0;
                prog->bestX.store(bestFit);
                prog->clicksFound.store((int)pop[0].pfs.size());
                prog->addLog("GA +" + std::to_string((int)bestFit)
                             + " (" + std::to_string(pct) + "%)"
                             + "  gen=" + std::to_string(gen));
            }
        } else if (++stallGen > 60) {
            // Frontier injection: replace bottom quarter with perturbed seed individuals
            dlog("  [GA stall] frontier inj gen=" + std::to_string(gen)
                 + "  f=" + std::to_string(frontierFrame));
            for (int i = POP * 3 / 4; i < POP; ++i) {
                pop[i].pfs = seedPfs;
                int extra = std::uniform_int_distribution<int>(1, 5)(rng);
                for (int k = 0; k < extra; ++k) {
                    uint64_t lo = frontierFrame > 500 ? frontierFrame - 500 : 1;
                    uint64_t hi = std::min(maxF - 1, frontierFrame + 200);
                    pop[i].pfs.push_back(
                        std::uniform_int_distribution<uint64_t>(lo, hi)(rng));
                }
                std::sort(pop[i].pfs.begin(), pop[i].pfs.end());
                evalInd(pop[i]);
            }
            for (int i = POP - 2; i < POP; ++i) {
                pop[i] = makeRandom(std::uniform_int_distribution<int>(10, 40)(rng));
                evalInd(pop[i]);
            }
            sortPop();
            runWarmup(); // pop[0] may have changed after injection
            stallGen = 0;
            if (prog) prog->addLog("GA stall → frontier inj gen=" + std::to_string(gen));
        }

        // Next generation ─────────────────────────────────────────────────
        const int elites = POP / 5;
        std::vector<Ind> next(pop.begin(), pop.begin() + elites);
        next.reserve(POP);
        std::uniform_int_distribution<int> parentDist(0, POP / 2 - 1);

        while ((int)next.size() < POP) {
            int  op = std::uniform_int_distribution<int>(0, 9)(rng);
            Ind  child;

            if (op < 6) {
                // Mutation (60%)
                child = pop[parentDist(rng)];
                if (child.pfs.empty()) {
                    // Empty parent (e.g. levels the sim can't solve, like
                    // move-trigger geometry) — seed a press instead of mutating,
                    // otherwise the shift branch indexes pfs[] out of bounds.
                    child.pfs.push_back(randPf());
                    evalInd(child);
                    next.push_back(std::move(child));
                    continue;
                }
                int mut = std::uniform_int_distribution<int>(0, 2)(rng);
                if (mut == 0 && child.pfs.size() > 2) {
                    child.pfs.erase(child.pfs.begin() +
                        std::uniform_int_distribution<int>(0,(int)child.pfs.size()-1)(rng));
                } else if (mut == 1) {
                    uint64_t newPf;
                    if (frontierFrame > 50 &&
                        std::uniform_int_distribution<int>(0, 4)(rng) < 3) {
                        uint64_t lo = frontierFrame > 200 ? frontierFrame - 200 : 1;
                        newPf = std::uniform_int_distribution<uint64_t>(lo, frontierFrame-1)(rng);
                    } else {
                        newPf = randPf();
                    }
                    child.pfs.push_back(newPf);
                    std::sort(child.pfs.begin(), child.pfs.end());
                } else {
                    int ns = std::uniform_int_distribution<int>(
                        1, std::min(4, (int)child.pfs.size()))(rng);
                    for (int k = 0; k < ns; ++k) {
                        int    idx  = std::uniform_int_distribution<int>(0,(int)child.pfs.size()-1)(rng);
                        int    shft = std::uniform_int_distribution<int>(-50, 50)(rng);
                        int64_t np  = (int64_t)child.pfs[idx] + shft;
                        if (np > 0 && (uint64_t)np < maxF) child.pfs[idx] = (uint64_t)np;
                    }
                    std::sort(child.pfs.begin(), child.pfs.end());
                }
            } else if (op < 9) {
                // Crossover (30%)
                const Ind& p1 = pop[parentDist(rng)];
                const Ind& p2 = pop[parentDist(rng)];
                if (!p1.pfs.empty() && !p2.pfs.empty()) {
                    uint64_t lo = std::min(p1.pfs.front(), p2.pfs.front());
                    uint64_t hi = std::max(p1.pfs.back(),  p2.pfs.back());
                    uint64_t sp = std::uniform_int_distribution<uint64_t>(lo, hi)(rng);
                    for (uint64_t pf : p1.pfs) if (pf <= sp) child.pfs.push_back(pf);
                    for (uint64_t pf : p2.pfs) if (pf  > sp) child.pfs.push_back(pf);
                    std::sort(child.pfs.begin(), child.pfs.end());
                } else {
                    child = p1.pfs.empty() ? p2 : p1;
                }
            } else {
                // Fresh random (10%)
                child = makeRandom(std::uniform_int_distribution<int>(10, 40)(rng));
            }

            evalInd(child);
            next.push_back(std::move(child));
        }

        pop = std::move(next);
    }

    sortPop();
    res.maxXReached = pop[0].fitness;
    for (uint64_t pf : pop[0].pfs) if (pf > 0 && pf + 1 <= maxF) res.clicks.push_back({pf, pf+1});
    std::sort(res.clicks.begin(), res.clicks.end(),
              [](auto& a, auto& b){ return a.pressFrame < b.pressFrame; });
    res.message = "GA: best X=" + std::to_string((int)pop[0].fitness)
                + "  evals=" + std::to_string(totalEvals);
    dlog("GA done  " + res.message);
    if (prog) { prog->addLog("GA done  " + res.message); prog->done.store(true); }
    return res;
}

} // namespace gdsim
