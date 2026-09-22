#include "uifont.hpp"
#include "transport.hpp"

#include <algorithm>
#include <cmath>

namespace gdapp {
namespace {

const Color kBarBg    { 24,  24,  32, 236};
const Color kBarEdge  { 70,  74,  92, 255};
const Color kText     {228, 230, 238, 255};
const Color kTextDim  {146, 150, 164, 255};
const Color kTextFaint{104, 108, 124, 255};
const Color kAccent   {130, 190, 230, 255};
const Color kAccentDim{ 70, 110, 150, 230};
const Color kBtnBg    { 40,  42,  54, 255};
const Color kTrack    { 44,  46,  60, 255};
const Color kFill     { 70, 110, 150, 255};

constexpr int kFont = 13;
constexpr int kFontS = 11;

// The speed ladder. Deliberately geometric and deliberately reaching far in both
// directions: 0.05x is for reading a single gravity flip frame by frame, 32x is
// for getting past four minutes of level you already trust.
const float kSpeeds[] = {0.f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f};
constexpr int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

int nearestSpeedIndex(float s) {
    int best = 0; float bd = 1e9f;
    for (int i = 0; i < kSpeedCount; i++) {
        float d = std::fabs(kSpeeds[i] - s);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

bool hit(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }

bool btn(Rectangle r, const char* label, Vector2 m, bool enabled, bool active = false) {
    bool hov = enabled && hit(r, m);
    DrawRectangleRec(r, active ? kAccentDim : (hov ? Color{56, 60, 76, 255} : kBtnBg));
    DrawRectangleLinesEx(r, 1.f, active ? kAccent : (enabled ? Color{62, 66, 82, 255} : Color{44, 46, 58, 255}));
    int tw = UITextWidth(label, kFont);
    UIText(label, (int)(r.x + (r.width - tw) * 0.5f), (int)(r.y + (r.height - kFont) * 0.5f),
             kFont, enabled ? kText : kTextFaint);
    return hov && enabled && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

std::string speedLabel(float s) {
    if (s == 0.f) return "FROZEN";
    char b[24];
    if (s < 1.f) std::snprintf(b, sizeof b, "%.2gx", s);
    else         std::snprintf(b, sizeof b, "%gx", s);
    return b;
}

} // namespace

TransportResult transportKeys(TransportUI& ui, int curFrame, int totalFrames) {
    TransportResult r;
    if (IsKeyPressed(KEY_SPACE)) ui.paused = !ui.paused;
    if (IsKeyPressed(KEY_R))     r.restart = true;

    // Frame stepping. Held keys repeat, because walking 40 frames through a
    // divergence one tap at a time is the single most common action here.
    const bool fast = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const int  mag  = fast ? 10 : 1;
    if (IsKeyPressed(KEY_PERIOD) || IsKeyPressedRepeat(KEY_PERIOD)) r.stepFrames += mag;
    if (IsKeyPressed(KEY_COMMA)  || IsKeyPressedRepeat(KEY_COMMA))  r.stepFrames -= mag;

    // Speed ladder.
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) ui.speed = kSpeeds[std::min(kSpeedCount - 1, nearestSpeedIndex(ui.speed) + 1)];
    if (IsKeyPressed(KEY_LEFT_BRACKET))  ui.speed = kSpeeds[std::max(0, nearestSpeedIndex(ui.speed) - 1)];

    if (IsKeyPressed(KEY_HOME)) { r.seek = true; r.seekFrame = 0; }
    if (IsKeyPressed(KEY_END))  { r.seek = true; r.seekFrame = std::max(0, totalFrames - 1); }
    if (IsKeyPressed(KEY_F))    ui.followCam = !ui.followCam;
    return r;
}

TransportResult drawTransport(TransportUI& ui, int curFrame, int totalFrames,
                              const std::string& status, bool alive,
                              int screenW, int screenH, bool blockMouse) {
    TransportResult r;
    if (!ui.visible) return r;

    const float h = 56.f;
    const Rectangle bar{0.f, (float)screenH - h, (float)screenW, h};
    const Vector2 m = GetMousePosition();
    const bool over = !blockMouse && hit(bar, m);
    r.hoveringBar = over;

    DrawRectangleRec(bar, kBarBg);
    DrawLineEx({0, bar.y}, {(float)screenW, bar.y}, 1.f, kBarEdge);

    // ── timeline (top strip of the bar) ──────────────────────────────────────
    const Rectangle track{12.f, bar.y + 8.f, (float)screenW - 24.f, 8.f};
    const int span = std::max(1, totalFrames);
    DrawRectangleRec(track, kTrack);

    float prog = std::clamp((float)curFrame / (float)span, 0.f, 1.f);
    DrawRectangle((int)track.x, (int)track.y, (int)(track.width * prog), (int)track.height, kFill);

    // A generous grab band: the visible track is 8px, but demanding 8px of aim
    // for the control you use most would be hostile.
    const Rectangle grab{track.x, track.y - 6, track.width, track.height + 12};
    const bool overTrack = !blockMouse && hit(grab, m);
    if (overTrack && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.scrubbing = true;
    if (ui.scrubbing) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float f = std::clamp((m.x - track.x) / track.width, 0.f, 1.f);
            r.seek = true;
            r.seekFrame = (int)std::lround(f * span);
            prog = f;
        } else {
            ui.scrubbing = false;
        }
    }

    // playhead
    float px = track.x + track.width * prog;
    DrawRectangle((int)(px - 1), (int)(track.y - 4), 3, (int)(track.height + 8), kAccent);
    if (overTrack || ui.scrubbing) {
        // Frame the cursor is pointing at, so a scrub can be aimed before commit.
        int hf = (int)std::lround(std::clamp((m.x - track.x) / track.width, 0.f, 1.f) * span);
        const char* t = TextFormat("f%d", hf);
        int tw = UITextWidth(t, kFontS);
        float bx = std::clamp(m.x - tw * 0.5f, track.x, track.x + track.width - tw);
        DrawRectangle((int)bx - 4, (int)(track.y - 22), tw + 8, 15, Color{16, 16, 22, 230});
        UIText(t, (int)bx, (int)(track.y - 20), kFontS, kAccent);
    }

    // ── buttons ──────────────────────────────────────────────────────────────
    float x = 12.f;
    const float by = bar.y + 24.f;
    const float bh = 24.f;

    if (btn({x, by, 30, bh}, "|<", m, true)) { r.seek = true; r.seekFrame = 0; }            x += 34;
    if (btn({x, by, 30, bh}, "<<", m, true)) r.stepFrames -= 10;                              x += 34;
    if (btn({x, by, 30, bh}, "<",  m, true)) r.stepFrames -= 1;                               x += 34;
    if (btn({x, by, 40, bh}, ui.paused ? ">" : "||", m, true, !ui.paused && alive)) ui.paused = !ui.paused;
    x += 44;
    if (btn({x, by, 30, bh}, ">",  m, true)) r.stepFrames += 1;                               x += 34;
    if (btn({x, by, 30, bh}, ">>", m, true)) r.stepFrames += 10;                              x += 34;
    if (btn({x, by, 34, bh}, "R",  m, true)) r.restart = true;                                x += 42;

    // ── speed ladder ─────────────────────────────────────────────────────────
    UIText("speed", (int)x, (int)(by + 6), kFontS, kTextFaint);
    x += UITextWidth("speed", kFontS) + 8;

    if (btn({x, by, 22, bh}, "-", m, true)) ui.speed = kSpeeds[std::max(0, nearestSpeedIndex(ui.speed) - 1)];
    x += 24;
    {
        // Current speed, click-and-drag-free: the two arrows and the ladder chips
        // cover it, and a draggable number here would fight the timeline above.
        Rectangle sr{x, by, 62, bh};
        DrawRectangleRec(sr, kBtnBg);
        DrawRectangleLinesEx(sr, 1.f, ui.speed == 1.f ? Color{62, 66, 82, 255} : kAccent);
        std::string sl = speedLabel(ui.speed);
        int tw = UITextWidth(sl.c_str(), kFont);
        UIText(sl.c_str(), (int)(sr.x + (sr.width - tw) * 0.5f), (int)(sr.y + 5), kFont,
                 ui.speed == 0.f ? Color{255, 190, 90, 255} : (ui.speed == 1.f ? kText : kAccent));
        x += 66;
    }
    if (btn({x, by, 22, bh}, "+", m, true)) ui.speed = kSpeeds[std::min(kSpeedCount - 1, nearestSpeedIndex(ui.speed) + 1)];
    x += 30;

    // one-click presets for the speeds actually used in this workflow
    const float presets[] = {0.f, 0.25f, 1.f, 4.f, 16.f};
    for (float p : presets) {
        std::string l = speedLabel(p);
        if (p == 0.f) l = "0";
        float w = (float)UITextWidth(l.c_str(), kFontS) + 14;
        if (btn({x, by + 2, w, bh - 4}, l.c_str(), m, true, ui.speed == p)) ui.speed = p;
        x += w + 4;
    }
    x += 8;

    if (btn({x, by, 68, bh}, ui.followCam ? "follow ON" : "follow", m, true, ui.followCam))
        ui.followCam = !ui.followCam;
    x += 76;

    // ── frame counter + host status, right-aligned ───────────────────────────
    {
        const char* fc = TextFormat("f%d / %d", curFrame, totalFrames);
        int fw = UITextWidth(fc, kFont);
        UIText(fc, screenW - 12 - fw, (int)(by + 5), kFont, kText);
        if (!status.empty()) {
            int sw = UITextWidth(status.c_str(), kFontS);
            UIText(status.c_str(), screenW - 12 - std::max(fw, sw), (int)(bar.y - 18), kFontS, kTextDim);
        }
    }

    if (!alive) {
        const char* s = "run ended  -  R to restart, or scrub back";
        UIText(s, (int)x, (int)(by + 6), kFontS, Color{255, 170, 90, 255});
    }

    return r;
}

} // namespace gdapp
