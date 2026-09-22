#include "trajfit.hpp"
#include "../../src/sim/Level.hpp"
#include "../../src/sim/Tunables.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_set>

using namespace gdsim;

namespace gdapp {
namespace {

// ── one simulation pass ──────────────────────────────────────────────────────
// Steps `lvl` from its start through the input track and records the player
// position at each requested frame. Death is not an error: the last live state
// is what the residual is measured against, which keeps a candidate that kills
// the run from silently scoring as a perfect fit.
struct Sample { float x, y; bool reached; };

void simulate(Level& lvl, const std::vector<bool>& inputAt,
              const std::vector<int>& frames, std::vector<Sample>& out) {
    out.assign(frames.size(), Sample{0.f, 0.f, false});
    int maxFrame = 0;
    for (int f : frames) maxFrame = std::max(maxFrame, f);

    lvl.resetToStart();
    float lx = lvl.latestState().pos.x, ly = lvl.latestState().pos.y;
    bool dead = false;
    for (int f = 1; f <= maxFrame; f++) {
        if (!dead) {
            bool press = (size_t)f < inputAt.size() && inputAt[f];
            Player& p = lvl.runFrame(press, 1.f / 240.f);
            if (p.dead) dead = true; else { lx = p.pos.x; ly = p.pos.y; }
        }
        for (size_t i = 0; i < frames.size(); i++)
            if (frames[i] == f) { out[i] = {lx, ly, !dead}; }
    }
}

double residualOf(const std::vector<Sample>& s, const std::vector<FitTarget>& t) {
    double sse = 0.0; int n = 0;
    for (size_t i = 0; i < t.size() && i < s.size(); i++) {
        if (t[i].fitY) { double d = s[i].y - t[i].targetY; sse += d * d; n++; }
        if (t[i].fitX) { double d = s[i].x - t[i].targetX; sse += d * d; n++; }
        // A candidate whose value kills the run before the target frame gets a
        // heavy penalty rather than whatever the death position happens to be —
        // otherwise "die early near the target" outranks a genuine fix.
        if (!s[i].reached) { sse += 1e6; n++; }
    }
    return n ? std::sqrt(sse / n) : 0.0;
}

// Signed residual, only meaningful for the single-target/single-axis case where
// a root actually exists. Used by the secant solve.
double signedResidual(const std::vector<Sample>& s, const std::vector<FitTarget>& t) {
    if (t.size() != 1 || s.empty()) return 0.0;
    if (!s[0].reached) return 0.0;
    if (t[0].fitY && !t[0].fitX) return s[0].y - t[0].targetY;
    if (t[0].fitX && !t[0].fitY) return s[0].x - t[0].targetX;
    return 0.0;
}

bool singleAxisSingleTarget(const std::vector<FitTarget>& t) {
    return t.size() == 1 && (t[0].fitX != t[0].fitY);
}

// Tier suffix a per-speed row carries in its registry name ("gravity 2x").
const char* kTier[5] = {"0.5x", "1x", "2x", "3x", "4x"};

bool endsWithTier(const std::string& name, int tier) {
    std::string suffix = kTier[tier];
    // Names are either "<thing> <tier>" or, for orb/pad rows, "<...>  <tier>".
    if (name.size() < suffix.size()) return false;
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int tierOfName(const std::string& name) {
    // Longest match first: "1x" is a suffix of "0.5x"? No — but "1x" IS a suffix
    // of "1.1x"-style names elsewhere, so check the longer labels first.
    const int order[5] = {0, 2, 3, 4, 1};   // 0.5x, 2x, 3x, 4x, then 1x last
    for (int k = 0; k < 5; k++)
        if (endsWithTier(name, order[k])) return order[k];
    return -1;
}

// Portal-size rows name their own object ids ("gravity 10/11 - width",
// "speed 200 - height"), so the ids can be read straight back out of the label
// instead of duplicating the table. Returns the ids in the second whitespace-
// delimited token, split on '/'.
std::vector<int> portalIdsFromName(const std::string& name) {
    std::vector<int> ids;
    size_t a = name.find(' ');
    if (a == std::string::npos) return ids;
    a++;
    size_t b = name.find(' ', a);
    std::string tok = name.substr(a, b == std::string::npos ? std::string::npos : b - a);
    size_t p = 0;
    while (p < tok.size()) {
        size_t q = tok.find('/', p);
        std::string one = tok.substr(p, q == std::string::npos ? std::string::npos : q - p);
        try { if (!one.empty()) ids.push_back(std::stoi(one)); } catch (...) {}
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return ids;
}

const char* vehGroupName(VehicleType v) {
    switch (v) {
        case VehicleType::Cube:   return "Cube";
        case VehicleType::Ship:   return "Ship";
        case VehicleType::Ball:   return "Ball";
        case VehicleType::Ufo:    return "UFO";
        case VehicleType::Wave:   return "Wave";
        case VehicleType::Robot:  return "Robot";
        case VehicleType::Spider: return "Spider";
        case VehicleType::Swing:  return "Swing";
    }
    return "";
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

std::vector<int> relevantTunables(const std::string& levelString,
                                  const std::vector<bool>& inputAt, int maxFrame) {
    auto& tun = allTunables();

    // Observe what the run actually uses before the target frame.
    bool seenVeh[8] = {false, false, false, false, false, false, false, false};
    bool seenSpeed[5] = {false, false, false, false, false};
    bool seenMini = false, seenBig = false;
    std::unordered_set<int> idsInReach;
    {
        Level lvl(levelString);
        const Player& s0 = lvl.latestState();
        seenVeh[(int)s0.vehicle.type] = true;
        if (s0.speed >= 0 && s0.speed < 5) seenSpeed[s0.speed] = true;
        (s0.small ? seenMini : seenBig) = true;
        float maxX = s0.pos.x;
        for (int f = 1; f <= maxFrame; f++) {
            Player& p = lvl.runFrame((size_t)f < inputAt.size() && inputAt[f], 1.f / 240.f);
            seenVeh[(int)p.vehicle.type] = true;
            if (p.speed >= 0 && p.speed < 5) seenSpeed[p.speed] = true;
            (p.small ? seenMini : seenBig) = true;
            maxX = std::max(maxX, p.pos.x);
            if (p.dead) break;
        }
        // Object ids the run could actually have reached. Portal-size rows are the
        // only candidates that force a full re-parse per probe (~104 ms on a large
        // level), so pruning the portal classes this level does not even contain
        // before the target frame is worth several seconds per fit.
        const float reachX = maxX + 200.f;
        int hiSec = std::min((int)lvl.sections.size() - 1,
                             (int)(reachX / (float)Level::sectionSize) + 1);
        for (int s = 0; s <= hiSec && s >= 0; s++)
            for (auto& oc : lvl.sections[s])
                if (oc.operator->()->pos.x <= reachX) idsInReach.insert(oc.operator->()->typeId);
    }

    std::vector<int> out;
    for (size_t i = 0; i < tun.size(); i++) {
        const auto& t = tun[i];

        // Solver-only padding must stay 0 for anything measuring fidelity, so it
        // is never a legitimate answer to "why is my trajectory wrong".
        if (t.group == "Solver margins") continue;

        // Vehicle groups the run never enters cannot affect it.
        bool isVehGroup = false;
        for (int v = 0; v < 8; v++) {
            if (t.group == vehGroupName((VehicleType)v)) {
                isVehGroup = true;
                if (!seenVeh[v]) goto skip;
                break;
            }
        }
        (void)isVehGroup;

        // Orb/pad rows name their vehicle and size; drop the combinations this
        // run never reaches. (Whether the ORB itself was touched is not checked —
        // a zero sensitivity will filter those out for free.)
        if (t.group == "Orbs" || t.group == "Pads") {
            bool vehOk = false;
            for (int v = 0; v < 8; v++)
                if (seenVeh[v] && t.name.find(vehGroupName((VehicleType)v)) != std::string::npos)
                    { vehOk = true; break; }
            if (!vehOk) continue;
            if (t.name.find("(mini)") != std::string::npos && !seenMini) continue;
            if (t.name.find("(big)")  != std::string::npos && !seenBig)  continue;
        }

        // A portal size only matters if a portal of that class is actually within
        // reach. These are the expensive candidates, so this prune matters most.
        if (t.group == "Portals") {
            auto ids = portalIdsFromName(t.name);
            bool present = ids.empty();   // unparseable name -> keep it (see header)
            for (int id : ids) if (idsInReach.count(id)) { present = true; break; }
            if (!present) continue;
        }

        // Per-tier rows for a speed the run never uses.
        {
            int tier = tierOfName(t.name);
            if (tier >= 0 && !seenSpeed[tier]) continue;
        }

        out.push_back((int)i);
        continue;
    skip:;
    }
    return out;
}

void runTrajectoryFit(FitRequest req, FitProgress& prog) {
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();

    auto& tun = allTunables();
    if (req.targets.empty() || req.candidates.empty()) {
        prog.finished.store(true);
        return;
    }

    std::vector<int> frames;
    for (auto& t : req.targets) frames.push_back(t.frame);

    // Every value we touch is restored before returning — this is a search, not
    // an application. Snapshot up front so even a cancel unwinds cleanly.
    std::vector<std::pair<int, double>> saved;
    saved.reserve(req.candidates.size());
    for (int idx : req.candidates) saved.emplace_back(idx, tun[idx].get());
    struct Restore {
        std::vector<std::pair<int, double>>* s;
        ~Restore() { for (auto& [i, v] : *s) allTunables()[i].set(v); }
    } restore{&saved};

    // One parsed Level, reused for every probe (see the header's speed note).
    auto base = std::make_unique<Level>(req.levelString);
    std::vector<Sample> samples;

    auto evalWith = [&](int idx, double value, bool needsRebuild) -> double {
        const double old = tun[idx].get();
        tun[idx].set(value);
        double r;
        if (needsRebuild) {
            // Hitbox sizes are baked at construction, so this candidate cannot
            // reuse the cached Level.
            Level fresh(req.levelString);
            simulate(fresh, req.inputAt, frames, samples);
            r = residualOf(samples, req.targets);
        } else {
            simulate(*base, req.inputAt, frames, samples);
            r = residualOf(samples, req.targets);
        }
        tun[idx].set(old);
        return r;
    };
    auto evalSignedWith = [&](int idx, double value, bool needsRebuild) -> double {
        const double old = tun[idx].get();
        tun[idx].set(value);
        double r;
        if (needsRebuild) {
            Level fresh(req.levelString);
            simulate(fresh, req.inputAt, frames, samples);
            r = signedResidual(samples, req.targets);
        } else {
            simulate(*base, req.inputAt, frames, samples);
            r = signedResidual(samples, req.targets);
        }
        tun[idx].set(old);
        return r;
    };

    // ── baseline ─────────────────────────────────────────────────────────────
    simulate(*base, req.inputAt, frames, samples);
    const double baseResid = residualOf(samples, req.targets);
    const double baseSigned = signedResidual(samples, req.targets);
    prog.baselineResidual = baseResid;

    prog.total.store((int)req.candidates.size() + req.refineTop);
    prog.done.store(0);

    // ── pass 1: sensitivity ──────────────────────────────────────────────────
    std::vector<FitCandidate> cands;
    cands.reserve(req.candidates.size());

    for (int idx : req.candidates) {
        if (prog.cancel.load()) { prog.finished.store(true); return; }
        const auto& t = tun[idx];
        const bool rebuild = (t.group == "Portals");
        const double v0 = t.get();

        // Probe size: big enough to move the trajectory measurably, small enough
        // to stay in the locally-linear regime. Escalate once if the first probe
        // produces literally no change (many constants only bite past a
        // threshold, e.g. an accel band edge).
        double eps = std::max(std::fabs(v0) * 1e-3, std::fabs(t.step) * 4.0);
        if (eps == 0.0) eps = 1e-3;

        double r1 = evalWith(idx, v0 + eps, rebuild);
        double sens = (r1 - baseResid) / eps;
        if (std::fabs(r1 - baseResid) < 1e-9) {
            eps *= 50.0;
            r1 = evalWith(idx, v0 + eps, rebuild);
            sens = (r1 - baseResid) / eps;
        }

        prog.done.fetch_add(1);
        if (std::fabs(r1 - baseResid) < 1e-9) continue;   // genuinely no authority

        FitCandidate c;
        c.tunable = idx;
        c.group = t.group;
        c.name = t.name;
        c.current = v0;
        c.suggested = v0;
        c.sensitivity = sens;
        c.residualBefore = baseResid;
        c.residualAfter = baseResid;
        c.needsRebuild = rebuild;

        // First-order guess at the value that zeroes the residual, used only for
        // ranking; the refine pass replaces it with a verified answer.
        if (singleAxisSingleTarget(req.targets)) {
            double dSigned = (evalSignedWith(idx, v0 + eps, rebuild) - baseSigned) / eps;
            if (std::fabs(dSigned) > 1e-12) c.suggested = v0 - baseSigned / dSigned;
        } else if (std::fabs(sens) > 1e-12) {
            c.suggested = v0 - baseResid / sens;
        }
        cands.push_back(std::move(c));
    }

    // Rank by authority: how much of the current error one unit of this constant
    // can erase. Ties broken toward the smaller relative change, because a fix
    // that needs a 0.2% nudge is a far more plausible calibration error than one
    // needing 400%.
    for (auto& c : cands)
        c.relChangePct = std::fabs(c.current) > 1e-12
                       ? 100.0 * std::fabs(c.suggested - c.current) / std::fabs(c.current)
                       : 0.0;
    std::sort(cands.begin(), cands.end(), [](const FitCandidate& a, const FitCandidate& b) {
        if (std::fabs(a.sensitivity) != std::fabs(b.sensitivity))
            return std::fabs(a.sensitivity) > std::fabs(b.sensitivity);
        return a.relChangePct < b.relChangePct;
    });

    {
        std::lock_guard<std::mutex> lk(prog.mu);
        prog.results = cands;
        prog.note = cands.empty()
            ? "No registered constant moves this point — the error is structural "
              "(ordering, a missing mechanic, or a hitbox), not a mis-valued constant."
            : "";
    }

    // ── pass 2: actually solve + verify the top rows ─────────────────────────
    const int refine = std::min<int>(req.refineTop, (int)cands.size());
    for (int i = 0; i < refine; i++) {
        if (prog.cancel.load()) break;
        auto& c = cands[i];
        const double v0 = c.current;

        if (singleAxisSingleTarget(req.targets)) {
            // Secant on the SIGNED residual: a root exists, so aim straight at it.
            double p0 = v0, f0 = baseSigned;
            double p1 = (c.suggested != v0) ? c.suggested : v0 * 1.01 + 1e-6;
            double f1 = evalSignedWith(c.tunable, p1, c.needsRebuild);
            for (int it = 0; it < 8 && std::fabs(f1) > 1e-4; it++) {
                if (std::fabs(f1 - f0) < 1e-15) break;
                double p2 = p1 - f1 * (p1 - p0) / (f1 - f0);
                if (!std::isfinite(p2)) break;
                p0 = p1; f0 = f1; p1 = p2;
                f1 = evalSignedWith(c.tunable, p1, c.needsRebuild);
            }
            c.suggested = p1;
            c.residualAfter = evalWith(c.tunable, p1, c.needsRebuild);
            c.verified = true;
        } else {
            // Multiple constraints: no exact root in general, so minimise the RMS
            // over a bracket around the linear guess with a golden-section search.
            double span = std::fabs(c.suggested - v0);
            if (span < 1e-9) span = std::max(std::fabs(v0) * 0.05, 1e-3);
            double lo = v0 - 2.0 * span, hi = v0 + 2.0 * span;
            const double phi = 0.6180339887;
            double x1 = hi - phi * (hi - lo), x2 = lo + phi * (hi - lo);
            double f_1 = evalWith(c.tunable, x1, c.needsRebuild);
            double f_2 = evalWith(c.tunable, x2, c.needsRebuild);
            for (int it = 0; it < 10 && (hi - lo) > 1e-9 * std::max(1.0, std::fabs(v0)); it++) {
                if (f_1 < f_2) { hi = x2; x2 = x1; f_2 = f_1; x1 = hi - phi * (hi - lo);
                                 f_1 = evalWith(c.tunable, x1, c.needsRebuild); }
                else           { lo = x1; x1 = x2; f_1 = f_2; x2 = lo + phi * (hi - lo);
                                 f_2 = evalWith(c.tunable, x2, c.needsRebuild); }
            }
            c.suggested = (f_1 < f_2) ? x1 : x2;
            c.residualAfter = std::min(f_1, f_2);
            c.verified = true;
        }

        c.relChangePct = std::fabs(c.current) > 1e-12
                       ? 100.0 * std::fabs(c.suggested - c.current) / std::fabs(c.current)
                       : 0.0;
        prog.done.fetch_add(1);

        std::lock_guard<std::mutex> lk(prog.mu);
        prog.results = cands;
    }

    // Verified rows first, then by how much error they actually remove — a row
    // that provably erases the residual should outrank one that merely has a big
    // derivative.
    std::stable_sort(cands.begin(), cands.end(), [](const FitCandidate& a, const FitCandidate& b) {
        if (a.verified != b.verified) return a.verified;
        if (a.verified && b.verified) {
            if (std::fabs(a.residualAfter - b.residualAfter) > 1e-9)
                return a.residualAfter < b.residualAfter;
            return a.relChangePct < b.relChangePct;
        }
        return std::fabs(a.sensitivity) > std::fabs(b.sensitivity);
    });

    {
        std::lock_guard<std::mutex> lk(prog.mu);
        prog.results = cands;
        prog.elapsedMs = std::chrono::duration<double, std::milli>(clk::now() - tStart).count();
    }
    prog.finished.store(true);
}

} // namespace gdapp
