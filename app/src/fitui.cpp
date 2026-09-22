#include "uifont.hpp"
#include "fitui.hpp"
#include "../../src/sim/Tunables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace gdsim;

namespace gdapp {
namespace {

const Color kBg      { 24,  24,  32, 244};
const Color kEdge    { 70,  74,  92, 255};
const Color kHead    { 32,  34,  46, 255};
const Color kText    {228, 230, 238, 255};
const Color kDim     {146, 150, 164, 255};
const Color kFaint   {104, 108, 124, 255};
const Color kAccent  {130, 190, 230, 255};
const Color kAccentD { 70, 110, 150, 230};
const Color kPin     {255, 210,  80, 255};
const Color kTargetC {120, 255, 170, 255};
const Color kReal    {255, 150,  60, 255};
const Color kWarn    {255, 130, 110, 255};
const Color kGood    {150, 220, 150, 255};

constexpr int kF   = 13;
constexpr int kFS  = 11;
constexpr float kPanelW = 430.f;

Vector2 toScreen(float wx, float wy, const Camera2DState& c, int W, int H) {
    return { W * 0.5f + (wx - c.x) * c.pixelsPerUnit,
             H * 0.5f - (wy - c.y) * c.pixelsPerUnit };
}
bool hit(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }

bool button(Rectangle r, const char* label, Vector2 m, bool enabled = true,
            bool active = false, Color tint = kAccent) {
    bool hov = enabled && hit(r, m);
    DrawRectangleRec(r, active ? kAccentD : (hov ? Color{56, 60, 76, 255} : Color{40, 42, 54, 255}));
    DrawRectangleLinesEx(r, 1.f, enabled ? (active ? tint : Color{62, 66, 82, 255}) : Color{44, 46, 58, 255});
    int tw = UITextWidth(label, kFS);
    UIText(label, (int)(r.x + (r.width - tw) * 0.5f), (int)(r.y + (r.height - kFS) * 0.5f),
             kFS, enabled ? kText : kFaint);
    return hov && enabled && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

std::string fmt(double v) {
    char b[48]; std::snprintf(b, sizeof b, "%.10g", v); return b;
}

// Nearest real-capture sample to a given X, using the same offset convention the
// physics-trail overlay draws with.
const RealTrailPoint* nearestReal(const std::vector<RealTrailPoint>& rt,
                                  float offX, float x) {
    const RealTrailPoint* best = nullptr; float bd = 1e30f;
    for (auto& p : rt) {
        float d = std::fabs((p.pos.x - offX) - x);
        if (d < bd) { bd = d; best = &p; }
    }
    return best;
}

} // namespace

FitUIState::~FitUIState() {
    if (prog) prog->cancel.store(true);
    if (thread.joinable()) thread.join();
}

void cancelFit(FitUIState& st) {
    if (st.prog) st.prog->cancel.store(true);
    if (st.thread.joinable()) st.thread.join();
    st.running = false;
}

bool fitWantsMouse(const FitUIState& st, const FitUIHost& host) {
    if (!st.enabled) return false;
    if (st.dragging) return true;
    if (st.selectedIdx < 0) return false;
    Vector2 m = GetMousePosition();
    // The panel occupies the left edge whenever something is pinned.
    return m.x < kPanelW && m.y > 90 && m.y < host.screenH - 70;
}

bool updateFitUI(FitUIState& st, const FitUIHost& host) {
    if (!st.enabled) { st.hoverIdx = -1; return false; }
    if (!host.trail || host.trail->empty()) return false;

    const auto& trail = *host.trail;
    const Vector2 m = GetMousePosition();
    const int W = host.screenW, H = host.screenH;
    bool applied = false;

    // ── collect finished fit results ─────────────────────────────────────────
    if (st.running && st.prog && st.prog->finished.load()) {
        if (st.thread.joinable()) st.thread.join();
        std::lock_guard<std::mutex> lk(st.prog->mu);
        st.results = st.prog->results;
        st.baselineResidual = st.prog->baselineResidual;
        st.elapsedMs = st.prog->elapsedMs;
        st.note = st.prog->note;
        st.running = false;
    }

    // ── hover / pick a trail point ───────────────────────────────────────────
    // Sampled, not exhaustive: a long replay holds tens of thousands of points
    // and testing each against the cursor every frame is pure waste when the
    // pick radius is ~16 px.
    st.hoverIdx = -1;
    if (!host.blockMouse && !st.dragging) {
        float bestD2 = 16.f * 16.f;
        const int stride = std::max<int>(1, (int)trail.size() / 4000);
        for (size_t i = 0; i < trail.size(); i += stride) {
            Vector2 s = toScreen(trail[i].pos.x, trail[i].pos.y, host.cam, W, H);
            if (s.x < -40 || s.x > W + 40 || s.y < -40 || s.y > H + 40) continue;
            float dx = s.x - m.x, dy = s.y - m.y, d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; st.hoverIdx = (int)i; }
        }
        // Refine around the coarse hit so the pin lands on the exact frame.
        if (st.hoverIdx >= 0 && stride > 1) {
            int lo = std::max(0, st.hoverIdx - stride), hi = std::min((int)trail.size() - 1, st.hoverIdx + stride);
            for (int i = lo; i <= hi; i++) {
                Vector2 s = toScreen(trail[i].pos.x, trail[i].pos.y, host.cam, W, H);
                float dx = s.x - m.x, dy = s.y - m.y, d2 = dx * dx + dy * dy;
                if (d2 < bestD2) { bestD2 = d2; st.hoverIdx = i; }
            }
        }
    }

    if (st.hoverIdx >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        st.selectedIdx = st.hoverIdx;
        st.targetX = trail[st.selectedIdx].pos.x;
        st.targetY = trail[st.selectedIdx].pos.y;
        st.hasTarget = true;
        st.snappedToReal = false;
        st.results.clear();
        st.note.clear();
    }

    // ── the pin ──────────────────────────────────────────────────────────────
    if (st.selectedIdx >= 0 && st.selectedIdx < (int)trail.size()) {
        const auto& sp = trail[st.selectedIdx];
        Vector2 simS = toScreen(sp.pos.x, sp.pos.y, host.cam, W, H);
        Vector2 tgtS = toScreen(st.targetX, st.targetY, host.cam, W, H);

        // drag the target handle
        Rectangle grab{tgtS.x - 11, tgtS.y - 11, 22, 22};
        if (!host.blockMouse && hit(grab, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            st.dragging = true;
        if (st.dragging) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Vec2D w = screenToWorld(m.x, m.y, host.cam, W, H);
                st.targetY = w.y;
                // X is locked unless the user asks to fit it: at a fixed frame the
                // player's X is whatever the run produced, so moving it sideways
                // only means something when X is an explicit constraint.
                if (st.fitX) st.targetX = w.x;
                st.snappedToReal = false;
                tgtS = toScreen(st.targetX, st.targetY, host.cam, W, H);
            } else st.dragging = false;
        }

        // sim point
        DrawCircleLines((int)simS.x, (int)simS.y, 7.f, kPin);
        DrawCircleV(simS, 2.5f, kPin);
        // link
        DrawLineEx(simS, tgtS, 1.5f, Color{kTargetC.r, kTargetC.g, kTargetC.b, 150});
        // target handle
        DrawCircleLines((int)tgtS.x, (int)tgtS.y, 9.f, kTargetC);
        DrawLineEx({tgtS.x - 12, tgtS.y}, {tgtS.x + 12, tgtS.y}, 1.5f, kTargetC);
        DrawLineEx({tgtS.x, tgtS.y - 12}, {tgtS.x, tgtS.y + 12}, 1.5f, kTargetC);

        float dy = st.targetY - sp.pos.y, dx = st.targetX - sp.pos.x;
        const char* lbl = TextFormat("f%d  dY %+.3f%s", st.selectedIdx + 1, dy,
                                     st.fitX ? TextFormat("  dX %+.3f", dx) : "");
        UIText(lbl, (int)(tgtS.x + 14), (int)(tgtS.y - 8), kFS,
                 st.snappedToReal ? kReal : kTargetC);
        if (st.snappedToReal)
            UIText("snapped to real capture", (int)(tgtS.x + 14), (int)(tgtS.y + 5), kFS, kReal);
    }

    // hover preview
    if (st.hoverIdx >= 0 && st.hoverIdx != st.selectedIdx) {
        Vector2 s = toScreen(trail[st.hoverIdx].pos.x, trail[st.hoverIdx].pos.y, host.cam, W, H);
        DrawCircleLines((int)s.x, (int)s.y, 6.f, Color{255, 255, 255, 160});
        UIText(TextFormat("f%d", st.hoverIdx + 1), (int)(s.x + 9), (int)(s.y - 7), kFS,
                 Color{255, 255, 255, 190});
    }

    // ── keys ─────────────────────────────────────────────────────────────────
    if (!host.blockKeys && st.selectedIdx >= 0) {
        // G: snap the target onto the real capture — the calibration path.
        if (IsKeyPressed(KEY_G) && host.realTrail && !host.realTrail->empty()) {
            const auto& sp = trail[st.selectedIdx];
            if (const RealTrailPoint* rp = nearestReal(*host.realTrail, host.realOffsetX, sp.pos.x)) {
                st.targetY = rp->pos.y - host.realOffsetY;
                st.targetX = rp->pos.x - host.realOffsetX;
                st.snappedToReal = true;
                st.toast = TextFormat("snapped to real capture (frame %ld)", rp->frame);
                st.toastUntil = GetTime() + 3.0;
            }
        }
        if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_ESCAPE)) {
            cancelFit(st);
            st.selectedIdx = -1; st.hasTarget = false; st.results.clear(); st.note.clear();
        }
    }

    if (st.selectedIdx < 0) return false;

    // ── panel ────────────────────────────────────────────────────────────────
    const float px = 8.f, py = 96.f;
    const float ph = (float)H - py - 74.f;
    Rectangle panel{px, py, kPanelW, ph};
    DrawRectangleRec(panel, kBg);
    DrawRectangleLinesEx(panel, 1.f, kEdge);
    DrawRectangleRec({panel.x, panel.y, panel.width, 26}, kHead);
    UIText("TRAJECTORY CORRECTION", (int)(panel.x + 10), (int)(panel.y + 7), kF, kText);

    float y = panel.y + 32;
    const auto& sp = trail[st.selectedIdx];
    UIText(TextFormat("frame %d    sim  x %.3f  y %.3f", st.selectedIdx + 1, sp.pos.x, sp.pos.y),
             (int)(panel.x + 10), (int)y, kFS, kDim);
    y += 15;
    UIText(TextFormat("target      x %.3f  y %.3f   (dY %+.3f)", st.targetX, st.targetY,
                        st.targetY - sp.pos.y),
             (int)(panel.x + 10), (int)y, kFS, st.snappedToReal ? kReal : kTargetC);
    y += 18;

    // Provenance of the target decides whether a fit is calibration or guesswork,
    // so it is stated on the panel rather than buried in documentation.
    if (st.snappedToReal)
        UIText("target = real capture  ->  fitting is calibration", (int)(panel.x + 10), (int)y, kFS, kGood);
    else
        UIText("target = hand-placed  ->  exploratory, not evidence", (int)(panel.x + 10), (int)y, kFS, kWarn);
    y += 20;

    // axis toggles + actions
    Rectangle bY{panel.x + 10, y, 46, 20};
    Rectangle bX{panel.x + 60, y, 46, 20};
    Rectangle bSnap{panel.x + 112, y, 96, 20};
    Rectangle bFit{panel.x + 212, y, 92, 20};
    Rectangle bClr{panel.x + 308, y, 70, 20};
    if (button(bY, "fit Y", m, true, st.fitY)) st.fitY = !st.fitY;
    if (button(bX, "fit X", m, true, st.fitX)) st.fitX = !st.fitX;
    const bool haveReal = host.realTrail && !host.realTrail->empty();
    if (button(bSnap, "snap real (G)", m, haveReal, st.snappedToReal, kReal) && haveReal) {
        if (const RealTrailPoint* rp = nearestReal(*host.realTrail, host.realOffsetX, sp.pos.x)) {
            st.targetY = rp->pos.y - host.realOffsetY;
            st.targetX = rp->pos.x - host.realOffsetX;
            st.snappedToReal = true;
        }
    }
    if (button(bFit, st.running ? "cancel" : "find cause", m,
               (st.fitX || st.fitY) && host.levelString && host.inputAt, st.running)) {
        if (st.running) {
            cancelFit(st);
        } else {
            cancelFit(st);
            FitRequest req;
            req.levelString = *host.levelString;
            req.inputAt = *host.inputAt;
            FitTarget t;
            t.frame = st.selectedIdx + 1;
            t.targetX = st.targetX; t.targetY = st.targetY;
            t.fitX = st.fitX; t.fitY = st.fitY;
            req.targets.push_back(t);
            req.candidates = relevantTunables(req.levelString, req.inputAt, t.frame);
            st.scanCount = (int)req.candidates.size();
            st.results.clear(); st.note.clear();
            st.prog = std::make_unique<FitProgress>();
            st.running = true;
            FitProgress* pp = st.prog.get();
            st.thread = std::thread([req = std::move(req), pp]() mutable {
                runTrajectoryFit(std::move(req), *pp);
            });
        }
    }
    if (button(bClr, "clear", m)) {
        cancelFit(st);
        st.selectedIdx = -1; st.results.clear(); st.note.clear();
        return false;
    }
    y += 26;

    // ── progress / results ───────────────────────────────────────────────────
    if (st.running && st.prog) {
        int d = st.prog->done.load(), tt = std::max(1, st.prog->total.load());
        float f = std::clamp((float)d / (float)tt, 0.f, 1.f);
        DrawRectangle((int)(panel.x + 10), (int)y, (int)(panel.width - 20), 6, Color{44, 46, 60, 255});
        DrawRectangle((int)(panel.x + 10), (int)y, (int)((panel.width - 20) * f), 6, kAccent);
        y += 12;
        UIText(TextFormat("scanning %d constants...  %d/%d", st.scanCount, d, tt),
                 (int)(panel.x + 10), (int)y, kFS, kDim);
        y += 18;
    }

    if (!st.note.empty()) {
        // Wrap the "nothing explains this" message — it is the most informative
        // outcome the tool has and must not be clipped.
        std::string s = st.note; size_t start = 0;
        while (start < s.size()) {
            size_t n = s.size() - start, cut = n;
            while (cut > 0 && UITextWidth(s.substr(start, cut).c_str(), kFS) > panel.width - 20) cut--;
            if (cut < n) { size_t sp2 = s.rfind(' ', start + cut); if (sp2 != std::string::npos && sp2 > start) cut = sp2 - start; }
            UIText(s.substr(start, cut).c_str(), (int)(panel.x + 10), (int)y, kFS, kWarn);
            y += 14; start += cut + 1;
        }
        y += 6;
    }

    if (!st.results.empty()) {
        UIText(TextFormat("residual now %.4f u     %d candidates   %.0f ms",
                            st.baselineResidual, (int)st.results.size(), st.elapsedMs),
                 (int)(panel.x + 10), (int)y, kFS, kDim);
        y += 16;
        UIText("constant                        new value        left   change",
                 (int)(panel.x + 10), (int)y, kFS, kFaint);
        y += 14;

        const float listTop = y, listBot = panel.y + panel.height - 8;
        BeginScissorMode((int)panel.x + 1, (int)listTop, (int)panel.width - 2, (int)(listBot - listTop));
        float ry = listTop - st.scroll;
        for (size_t i = 0; i < st.results.size(); i++) {
            const auto& c = st.results[i];
            const float rh = 30.f;
            if (ry + rh < listTop || ry > listBot) { ry += rh; continue; }
            Rectangle rr{panel.x + 4, ry, panel.width - 8, rh - 2};
            bool hov = !host.blockMouse && hit(rr, m) && m.y >= listTop && m.y < listBot;
            if (hov) DrawRectangleRec(rr, Color{40, 44, 58, 255});

            Color nameC = c.verified ? kText : kFaint;
            UIText((c.group + " / " + c.name).c_str(), (int)(rr.x + 6), (int)(ry + 2), kFS, nameC);

            UIText(fmt(c.suggested).c_str(), (int)(rr.x + 6), (int)(ry + 15), kFS, kAccent);
            const char* was = TextFormat("was %s", fmt(c.current).c_str());
            UIText(was, (int)(rr.x + 6 + UITextWidth(fmt(c.suggested).c_str(), kFS) + 10),
                     (int)(ry + 15), kFS, kFaint);

            // "left" = residual remaining after this change, verified by a real
            // re-simulation. This is the column that actually matters.
            const char* left = c.verified ? TextFormat("%.3f", c.residualAfter) : "-";
            UIText(left, (int)(rr.x + rr.width - 118), (int)(ry + 8), kFS,
                     c.verified && c.residualAfter < st.baselineResidual * 0.1 ? kGood : kDim);
            UIText(TextFormat("%+.3f%%", c.suggested >= c.current ? c.relChangePct : -c.relChangePct),
                     (int)(rr.x + rr.width - 66), (int)(ry + 8), kFS,
                     c.relChangePct < 1.0 ? kGood : (c.relChangePct < 10.0 ? kDim : kWarn));

            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && c.tunable >= 0) {
                allTunables()[c.tunable].set(c.suggested);
                applied = true;
                st.toast = c.group + " / " + c.name + " -> " + fmt(c.suggested);
                st.toastUntil = GetTime() + 4.0;
            }
            ry += rh;
        }
        EndScissorMode();

        float content = st.results.size() * 30.f;
        float maxScroll = std::max(0.f, content - (listBot - listTop));
        if (!host.blockMouse && m.x < panel.x + panel.width && m.y >= listTop && m.y < listBot) {
            float w = GetMouseWheelMove();
            if (w != 0.f) st.scroll = std::clamp(st.scroll - w * 60.f, 0.f, maxScroll);
        }
        st.scroll = std::clamp(st.scroll, 0.f, maxScroll);
    } else if (!st.running && st.note.empty()) {
        UIText("press \"find cause\" to see which constants can move this point",
                 (int)(panel.x + 10), (int)y, kFS, kFaint);
        y += 14;
        UIText("click a row to apply it  -  G snaps the target to the real capture",
                 (int)(panel.x + 10), (int)y, kFS, kFaint);
    }

    if (GetTime() < st.toastUntil && !st.toast.empty())
        UIText(st.toast.c_str(), (int)(panel.x + 10), (int)(panel.y + panel.height - 16), kFS, kGood);

    return applied;
}

} // namespace gdapp
