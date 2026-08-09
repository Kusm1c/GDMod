// Cube-focused solver: best-first graph search sparsified to DECISION points.
//
// The beam (Solver_Beam.cpp) and randomized path seeker (Solver_Path.cpp) both
// branch on press/no-press EVERY frame. Over a 7-minute level (~100k frames at 240
// sub-steps/s) that's astronomically many branches, so they blow their time budget
// long before the finish; the beam's fixed width (W=4000) also silently drops the
// exact mid-air state a frame-perfect trick depends on.
//
// This solver instead only branches where input can actually change the cube's
// future — a DECISION point: the cube is grounded (can jump, and *when* it jumps in
// the grounded window matters), or an orb/effect is touching (can tap it). Between
// decisions the cube is ballistic (or riding a pad/portal), so we just coast forward
// with no input in one shot. That collapses the vast no-input majority of frames and
// leaves a sparse graph of meaningful states, searched best-first (expand the
// furthest-x state first) with exact closed-set dedup and NO fixed frontier width —
// so nothing needed is ever pruned, and precise timings are enumerated exactly.
//
// Scope: built for CUBE (fixed-height taps). Ball/spider (discrete flips/teleports)
// work too. Robot's variable jump height isn't modelled (1-frame taps = min jump).
// Flight (ship/ufo/wave/swing) falls back to per-frame branching (every flight frame
// is a decision) — correct but dense, still deduped; fine for the short flight
// segments in an otherwise-cube level. Moving orbs are detected via posed movable
// objects; dash orbs (1704) aren't modelled by the physics core yet.

#include "Solver_internal.hpp"
#include "Calib.hpp"
#include "DebugPaths.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_set>

namespace gdsim {

static inline bool isFlightVeh(VehicleType t) {
    return t == VehicleType::Ship || t == VehicleType::Ufo
        || t == VehicleType::Wave || t == VehicleType::Swing;
}

// Does any non-block, non-hazard effect object (orb/pad/portal) touch the player
// this frame? Mirrors Level::stepPlayer's effect-gathering exactly (same section
// window + movable posing) so decision detection matches the physics — respects
// usedEffects and geometry. Pads/portals fire regardless of input, so flagging them
// is harmless: the press/no-press children they produce are identical and merge in
// dedup. Only orbs actually make the press a real choice.
static bool effectTouching(Level& L, const Player& p) {
    if (L.sections.empty()) return false;
    const int last = (int)L.sections.size() - 1;
    const int si   = std::clamp((int)(p.pos.x / (float)Level::sectionSize), 0, last);
    for (int s = std::max(0, si - 1); s <= std::min(last, si + 1); ++s)
        for (auto& oc : L.sections[s]) {
            const Object* o = oc.operator->();
            if (o->prio == 1 || o->prio == 2) continue;   // blocks / hazards aren't effects
            if (o->touching(p)) return true;
        }
    // Trigger-animated (moving) effect objects — pose only the ones near the player,
    // same as the physics step. Poses mutate L.movable in place; safe here (this
    // Level is the solver's own, re-posed every real step anyway).
    if (L.hasTriggers && !L.movableStartSortedX.empty()) {
        const float reach = 220.f + L.maxMoveExtent;
        const float lo = p.pos.x - reach, hi = p.pos.x + reach;
        auto first = std::lower_bound(L.movableStartSortedX.begin(), L.movableStartSortedX.end(), lo,
                         [](const std::pair<float,int>& e, float v){ return e.first < v; });
        for (auto it = first; it != L.movableStartSortedX.end() && it->first <= hi; ++it) {
            const int idx = it->second;
            // Per-object gate (see Level::stepPlayer): skip objects whose own extent
            // can't put them near the player before the expensive poseMovable.
            if (std::abs(it->first - p.pos.x) > 220.f + L.movableExtent[idx]) continue;
            Vec2D pos; float rot; bool active;
            L.poseMovable(idx, p.frame, pos, rot, active);
            if (!active) continue;
            if (std::abs(pos.x - p.pos.x) > 220.f) continue;
            Object* o = L.movable[idx].operator->();
            if (o->prio == 1 || o->prio == 2) continue;
            o->pos = pos; o->rotation = rot;
            if (o->touching(p)) return true;
        }
    }
    return false;
}

// Exact closed-set key: two states at the SAME frame with the same quantized
// (y, velocity) and flags have identical futures → merge. Fine resolution (0.1u y,
// 0.25u velocity) preserves frame-perfect distinctions the beam's coarse key (0.5u /
// 3u) merged away, while still collapsing genuinely-equal states.
static uint64_t cubeKey(const Player& p) {
    uint64_t frame = (uint64_t)(uint32_t)p.frame & 0x3FFFFull;                 // 18b (~18min@240)
    int64_t  qy    = std::llround(p.pos.y * 10.0)  + 100000; if (qy < 0) qy = 0; // 18b
    int64_t  qv    = std::llround(p.velocity * 4.0) + 20000; if (qv < 0) qv = 0; // 16b
    uint64_t flags = (uint64_t)(p.upsideDown ? 1 : 0)
                   | (uint64_t)(p.small       ? 2 : 0)
                   | (uint64_t)(p.grounded    ? 4 : 0)
                   | (uint64_t)(p.button      ? 8 : 0)
                   | ((uint64_t)p.vehicle.type << 4)
                   | ((uint64_t)p.speed        << 7);                            // 10b
    return frame | ((uint64_t)(qy & 0x3FFFF) << 18)
                 | ((uint64_t)(qv & 0xFFFF)   << 36)
                 | ((flags & 0x3FF)           << 52);
}

// Reusable core: search with a fixed hazard-inflate margin. solveLevelCube wraps
// this in a small margin-descent (try the requested safety margin, then 0).
static bool s_debugCubeSolver = true;

static SolverResult cubeSearch(const std::string& levelStr, const SolverConfig& cfg,
                               float inflate, const std::atomic<bool>* cancelled)
{
    SolverResult result;

    Level sim(levelStr);
    if (sim.sections.empty()) { result.message = "Level has no geometry"; return result; }
    sim.simulateMirror = false;
    sim.hazardInflate  = inflate;
    // No flight-only extra margin here: this solver targets cube. Flight fallback
    // uses the plain hazard margin like everything else.
    sim.flightHazardInflate = 0.f;

    const float    dt   = cfg.dt;
    const float    end  = levelEnd(sim);
    result.levelEndEstimate = end;
    auto* prog = cfg.progress;
    if (prog) prog->levelEndX.store(end);

    if (s_debugCubeSolver) {
        std::ofstream dbg2(debugPath("solver_cube_first.txt"), std::ios::out | std::ios::trunc);
        dbg2 << "spawn x=" << sim.gameStates[0].pos.x << " y=" << sim.gameStates[0].pos.y
             << " ground=" << sim.gameStates[0].grounded << " dead=" << sim.gameStates[0].dead
             << " vehicle=" << (int)sim.gameStates[0].vehicle.type
             << " speed=" << sim.gameStates[0].speed << " xSpeed=" << sim.gameStates[0].xSpeed
             << " frame=" << sim.gameStates[0].frame << "\n";
        auto step = [&](Player p, bool press) {
            sim.rollback(0);
            sim.gameStates[0] = std::move(p);
            auto &s = sim.runFrame(press, dt);
            dbg2 << "step press=" << press << " => frame=" << s.frame << " x=" << s.pos.x
                 << " y=" << s.pos.y << " ground=" << s.grounded << " dead=" << s.dead
                 << " vehicle=" << (int)s.vehicle.type << " speed=" << s.speed << "\n";
            return s;
        };
        step(sim.gameStates[0], false);
        step(sim.gameStates[0], true);
    }

    sim.rollback(0);
    const Player spawn = sim.gameStates[0];

    std::ofstream dbg(debugPath("solver_debug.txt"),
                      std::ios::out | std::ios::trunc);
    auto dlog = [&](const std::string& s){ if (dbg.is_open()) dbg << s << "\n"; };
    dlog("=== CUBE SOLVER (decision-point best-first) inflate=" + std::to_string(inflate) + " ===");
    dlog("levelEnd=" + std::to_string((int)end) + " sections=" + std::to_string(sim.sections.size()));
    if (prog) prog->addLog("Cube solver: decision-point search (margin "
                           + std::to_string((int)(inflate * 10) / 10.0f) + "u)...");

    // Step one frame from an arbitrary stored state (the beam's injection idiom):
    // inject as gameStates[0], run, copy the result out before the next rollback.
    auto step = [&](Player p, bool press) -> Player {
        sim.rollback(0);
        sim.gameStates[0] = std::move(p);
        return sim.runFrame(press, dt);   // returns Player& into gameStates[1]; copied out
    };
    auto decision = [&](const Player& p) -> bool {
        return p.grounded || isFlightVeh(p.vehicle.type) || effectTouching(sim, p);
    };
    // Coast (no input) until the next decision / death / finish, starting AFTER
    // taking `press` on the current frame.
    auto rollout = [&](Player p, bool press, bool& dead, bool& won) -> Player {
        Player c = step(std::move(p), press);
        for (;;) {
            if (c.dead)        { dead = true; return c; }
            if (c.pos.x >= end){ won  = true; return c; }
            if (decision(c))   return c;
            c = step(std::move(c), false);
        }
    };

    // ── Search state ──────────────────────────────────────────────────────────
    // Light permanent nodes (parent + the tap frame on the incoming edge, if any) —
    // millions fit. Heavy Player states live only in the open priority queue and are
    // freed on expansion.
    struct Node { int32_t parent; uint32_t pressFrame; };  // pressFrame 0 = coast/none
    std::vector<Node> nodes;
    nodes.reserve(1 << 20);

    struct Open { float x; int32_t node; Player st; };
    struct OpenCmp { bool operator()(const Open& a, const Open& b) const { return a.x < b.x; } };
    std::priority_queue<Open, std::vector<Open>, OpenCmp> open;

    std::unordered_set<uint64_t> seen;
    seen.reserve(1 << 21);

    int   solvedNode = -1;
    float bestX      = spawn.pos.x;
    int   bestNode   = 0;

    std::ofstream dbgAdd(debugPath("solver_cube_add.txt"), std::ios::out | std::ios::trunc);
    auto dlogAdd = [&](const std::string& s){ if (dbgAdd.is_open()) dbgAdd << s << "\n"; };
    auto addChild = [&](int32_t parent, uint32_t pressFrame, Player&& st, bool won) {
        const uint64_t k = cubeKey(st);
        bool inserted = won || seen.insert(k).second;
        dlogAdd("addChild parent=" + std::to_string(parent) + " pressFrame=" + std::to_string(pressFrame)
            + " x=" + std::to_string(st.pos.x) + " y=" + std::to_string(st.pos.y)
            + " frame=" + std::to_string(st.frame) + " won=" + std::to_string(won)
            + " seen=" + std::to_string(inserted));
        if (!inserted) return;   // already reached this exact state
        const int32_t idx = (int32_t)nodes.size();
        nodes.push_back({ parent, pressFrame });
        if (st.pos.x > bestX) { bestX = st.pos.x; bestNode = idx; }
        if (won) { solvedNode = idx; return; }
        open.push({ st.pos.x, idx, std::move(st) });
    };

    // Seed with the spawn state.
    nodes.push_back({ -1, 0 });
    seen.insert(cubeKey(spawn));
    open.push({ spawn.pos.x, 0, spawn });

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed  = [&]{ return std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t0).count(); };
    // Long precise levels legitimately take a while; scale with length, cap generously.
    const double  kLimit     = std::clamp((double)end / 150.0, 60.0, 300.0);
    const size_t  kMaxNodes  = 6'000'000;
    const int     kGroundWin = 400;   // per-node grounded jump-timing window (frames)

    // Live viz: re-sim the best partial path and publish a sampled trajectory. Costs
    // a full replay, so only every ~0.5s of wall time.
    double lastViz = 0.0;
    auto reconstruct = [&](int leaf, std::vector<SolverClick>& out) {
        out.clear();
        std::vector<uint32_t> pf;
        for (int i = leaf; i >= 0; i = nodes[i].parent)
            if (nodes[i].pressFrame) pf.push_back(nodes[i].pressFrame);
        std::sort(pf.begin(), pf.end());
        for (uint32_t f : pf) out.push_back({ (uint64_t)f, (uint64_t)f + 1 });  // 1-frame taps
    };
    auto publishViz = [&]{
        if (!prog) return;
        std::vector<SolverClick> clk; reconstruct(bestNode, clk);
        SolverProgressReport::VizSnapshot snap;
        std::vector<bool> inp;
        uint32_t hi = 0; for (auto& c : clk) hi = std::max<uint32_t>(hi, (uint32_t)c.releaseFrame);
        inp.assign(hi + 2, false);
        for (auto& c : clk)
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inp.size(); ++f) inp[f] = true;
        sim.rollback(0); sim.gameStates[0] = spawn;
        for (uint32_t f = 1; ; ++f) {
            bool p = f < inp.size() && inp[f];
            auto& s = sim.runFrame(p, dt);
            if (f % 4 == 0) snap.trajectory.emplace_back(s.pos.x, s.pos.y);
            if (s.dead) { snap.died = true; snap.deathPt = { s.pos.x, s.pos.y }; break; }
            if (s.pos.x >= bestX || s.pos.x >= end) break;
            if (f > (uint32_t)end * 4 + 4000) break;   // safety
        }
        snap.searchStartX = spawn.pos.x;
        snap.searchEndX   = bestX;
        snap.debugInfo    = "Cube  X=" + std::to_string((int)bestX)
                          + "  nodes=" + std::to_string(nodes.size())
                          + "  open=" + std::to_string(open.size());
        prog->updateViz(std::move(snap));
    };

    // ── Best-first loop ───────────────────────────────────────────────────────
    uint64_t pops = 0;
    while (!open.empty() && solvedNode < 0) {
        if (cancelled && cancelled->load()) { dlog("Cube solver cancelled"); break; }
        if (elapsed() > kLimit) {
            dlog("Cube solver TIMEOUT bestX=" + std::to_string((int)bestX));
            if (prog) prog->addLog("Cube solver timeout at X=" + std::to_string((int)bestX));
            break;
        }
        if (nodes.size() > kMaxNodes) {
            dlog("Cube solver node budget hit bestX=" + std::to_string((int)bestX));
            if (prog) prog->addLog("Cube solver hit node budget at X=" + std::to_string((int)bestX));
            break;
        }

        Open cur = std::move(const_cast<Open&>(open.top()));
        open.pop();
        const int32_t nidx = cur.node;
        const Player  S     = std::move(cur.st);
        ++pops;
        dlog("POP nidx=" + std::to_string(nidx) + " x=" + std::to_string(S.pos.x)
             + " y=" + std::to_string(S.pos.y) + " frame=" + std::to_string(S.frame)
             + " grounded=" + std::to_string(S.grounded) + " dead=" + std::to_string(S.dead)
             + " vehicle=" + std::to_string((int)S.vehicle.type));

        if (S.grounded && !isFlightVeh(S.vehicle.type)) {
            // Enumerate jump timing across the contiguous grounded window: coast
            // frame-by-frame (no jump), branching a jump off each grounded frame.
            Player ride = S;
            for (int g = 0; g < kGroundWin; ++g) {
                bool d = false, w = false;
                Player j = rollout(ride, true, d, w);           // jump at ride.frame
                dlog("  GROUND g=" + std::to_string(g) + " ride.frame=" + std::to_string(ride.frame)
                     + " ride.x=" + std::to_string(ride.pos.x) + " jumpDead=" + std::to_string(d)
                     + " jumpWon=" + std::to_string(w));
                if (!d) addChild(nidx, (uint32_t)ride.frame, std::move(j), w);
                if (solvedNode >= 0) break;

                Player nx = step(ride, false);                  // don't jump; advance one frame
                dlog("  NEXT x=" + std::to_string(nx.pos.x) + " y=" + std::to_string(nx.pos.y)
                     + " grounded=" + std::to_string(nx.grounded) + " dead=" + std::to_string(nx.dead)
                     + " vehicle=" + std::to_string((int)nx.vehicle.type));
                if (nx.dead)         { ride.dead = true; break; }
                if (nx.pos.x >= end) { addChild(nidx, 0, std::move(nx), true); break; }
                // Left the ground, or an orb came into reach → hand off to the coast child.
                if (!nx.grounded || isFlightVeh(nx.vehicle.type) || effectTouching(sim, nx)) {
                    ride = std::move(nx); break;
                }
                ride = std::move(nx);
            }
            // Coast child: kept riding without jumping. `ride` is now off the ground
            // (or at a fresh decision, or grounded at the window cap). Advance it to
            // the next decision so the "didn't jump here" branch keeps searching.
            if (solvedNode < 0 && !ride.dead && ride.pos.x < end) {
                if (decision(ride)) {
                    dlog("  COAST DECISION x=" + std::to_string(ride.pos.x) + " frame=" + std::to_string(ride.frame));
                    addChild(nidx, 0, std::move(ride), false);
                } else {
                    bool d = false, w = false;
                    Player c = rollout(ride, false, d, w);
                    dlog("  COAST ROLLOUT x=" + std::to_string(c.pos.x) + " frame=" + std::to_string(c.frame)
                         + " dead=" + std::to_string(d) + " won=" + std::to_string(w));
                    if (!d) addChild(nidx, 0, std::move(c), w);
                }
            }
        } else {
            // Airborne at an orb, or a flight frame: plain press / no-press.
            for (int pb = 0; pb < 2 && solvedNode < 0; ++pb) {
                bool d = false, w = false;
                Player ch = rollout(S, pb == 1, d, w);
                dlog("  AIR pb=" + std::to_string(pb) + " x=" + std::to_string(ch.pos.x)
                     + " frame=" + std::to_string(ch.frame) + " dead=" + std::to_string(d)
                     + " won=" + std::to_string(w));
                if (!d) addChild(nidx, pb == 1 ? (uint32_t)S.frame : 0, std::move(ch), w);
            }
        }

        if (prog && (pops & 0x3FF) == 0) prog->bestX.store(bestX);
        if (prog && elapsed() - lastViz > 0.5) { lastViz = elapsed(); publishViz(); }
        if ((pops & 0xFFF) == 0)
            dlog("  pop=" + std::to_string(pops) + " nodes=" + std::to_string(nodes.size())
                 + " open=" + std::to_string(open.size()) + " bestX=" + std::to_string((int)bestX));
    }

    if (solvedNode >= 0) {
        reconstruct(solvedNode, result.clicks);
        // Frame budget for the downstream re-sim = winning frame + margin.
        uint32_t hi = 0; for (auto& c : result.clicks) hi = std::max<uint32_t>(hi, (uint32_t)c.releaseFrame);
        result.solved      = true;
        result.framesTotal = (int)std::max<uint32_t>(hi + 480, (uint32_t)(end / player_speeds[0] / dt) + 480);
        result.maxXReached = end + 1.f;
        result.finalX      = end + 1.f;
        result.message     = "Cube solver: " + std::to_string(result.clicks.size()) + " taps";
        dlog("CUBE SOLVED: " + std::to_string(result.clicks.size()) + " taps in "
             + std::to_string(elapsed()) + "s, " + std::to_string(nodes.size()) + " nodes");
        if (prog) {
            prog->clicksFound.store((int)result.clicks.size());
            prog->bestX.store(end + 1.f);
            prog->addLog("*** CUBE SOLVED! " + std::to_string(result.clicks.size()) + " taps ***");
            prog->done.store(true);
        }
        return result;
    }

    // No solution: hand back the furthest partial so View Level can show how far it got.
    reconstruct(bestNode, result.clicks);
    result.maxXReached = bestX;
    result.finalX      = bestX;
    result.framesTotal = (int)((end / player_speeds[0] / dt) + 480);
    result.message     = "No cube solution (reached X=" + std::to_string((int)bestX)
                       + "). Open View Level to see the furthest path.";
    dlog("CUBE FAILED bestX=" + std::to_string((int)bestX));
    return result;
}

SolverResult solveLevelCube(const std::string& levelStr, SolverConfig cfg,
                            const std::atomic<bool>* cancelled)
{
    auto normalizedLevelStr = normalizeLevelString(levelStr);
    if (normalizedLevelStr.empty() || normalizedLevelStr.find(';') == std::string::npos) {
        SolverResult r; r.message = "Invalid level string"; return r;
    }
    loadCalibFromFile(configPath("GDMod_calib.txt").c_str());

    // Try the requested hazard safety margin first (robust on real-game replay),
    // then a zero-margin pass so a genuinely tight, frame-perfect gap still solves.
    SolverResult last;
    float prev = 1e9f;
    for (float inf : { cfg.hazardInflate, 0.f }) {
        if (inf >= prev) continue;
        prev = inf;
        last = cubeSearch(normalizedLevelStr, cfg, inf, cancelled);
        if (last.solved || (cancelled && cancelled->load())) return last;
    }
    return last;
}

} // namespace gdsim
