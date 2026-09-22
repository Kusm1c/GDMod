#include "uifont.hpp"
#include "tunerui.hpp"
#include "../../src/sim/Tunables.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

using namespace gdsim;

namespace gdapp {
namespace {

// ── palette ──────────────────────────────────────────────────────────────────
// Matches the app's existing dark UI (background {18,18,24}, accent {130,190,230}).
const Color kPanelBg     {24,  24,  32, 246};
const Color kPanelEdge   {70,  74,  92, 255};
const Color kHeaderBg    {32,  34,  46, 255};
const Color kText        {228, 230, 238, 255};
const Color kTextDim     {146, 150, 164, 255};
const Color kTextFaint   {104, 108, 124, 255};
const Color kAccent      {130, 190, 230, 255};
const Color kAccentDim   { 70, 110, 150, 230};
const Color kModified    {255, 190,  90, 255};
const Color kFieldBg     { 16,  16,  22, 255};
const Color kFieldEdge   { 62,  66,  82, 255};
const Color kRowHot      { 40,  44,  58, 255};
const Color kGroupBg     { 30,  32,  42, 255};
const Color kWarn        {255, 130, 110, 255};
const Color kGood        {150, 220, 150, 255};

// ── metrics ──────────────────────────────────────────────────────────────────
constexpr float kPad        = 12.f;
constexpr float kRowH       = 24.f;
constexpr float kGroupH     = 27.f;
constexpr float kValueW     = 168.f;
constexpr float kResetW     = 20.f;
constexpr int   kFontRow    = 13;
constexpr int   kFontHead   = 17;
constexpr int   kFontSmall  = 12;

bool hit(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }

// Shortest text that still round-trips the value for a human: 12 significant
// digits keeps 603.7217172 and 103.485494592 intact (the tables really do carry
// that many), while trailing-zero trimming keeps 0.5 from rendering as
// "0.500000000000".
std::string fmtValue(double v, TunKind kind) {
    char buf[64];
    if (kind == TunKind::I32)  { std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v)); return buf; }
    if (kind == TunKind::Bool) return v != 0.0 ? "true" : "false";
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

// Case-insensitive substring test, so the search box does not care about case.
bool containsFold(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) ==
                                                       std::tolower((unsigned char)b); });
    return it != hay.end();
}

void drawTextClipped(const char* s, float x, float y, int size, Color c, float maxW) {
    if (maxW <= 4) return;
    if (UITextWidth(s, size) <= maxW) { UIText(s, (int)x, (int)y, size, c); return; }
    // Truncate with an ellipsis rather than letting the label run under the value
    // box — a long orb row like "Yellow - Spider (mini)  0.5x" is common.
    std::string t = s;
    while (!t.empty() && UITextWidth((t + "...").c_str(), size) > maxW) t.pop_back();
    UIText((t + "...").c_str(), (int)x, (int)y, size, c);
}

bool button(Rectangle r, const char* label, Vector2 m, bool enabled = true, Color tint = kAccent) {
    bool hov = enabled && hit(r, m);
    DrawRectangleRec(r, hov ? kAccentDim : Color{40, 42, 54, 255});
    DrawRectangleLinesEx(r, 1.f, enabled ? tint : kFieldEdge);
    int tw = UITextWidth(label, kFontSmall);
    UIText(label, (int)(r.x + (r.width - tw) * 0.5f), (int)(r.y + (r.height - kFontSmall) * 0.5f),
             kFontSmall, enabled ? kText : kTextFaint);
    return hov && enabled && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// A focusable single-line text field. Returns true if the text changed.
bool textField(Rectangle r, std::string& buf, bool& focused, const char* placeholder,
               Vector2 m, size_t maxLen = 64) {
    if (hit(r, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focused = true;
    else if (!hit(r, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focused = false;

    DrawRectangleRec(r, kFieldBg);
    DrawRectangleLinesEx(r, 1.f, focused ? kAccent : kFieldEdge);

    bool changed = false;
    if (focused) {
        int c;
        while ((c = GetCharPressed()) != 0)
            if (c >= 32 && c < 127 && buf.size() < maxLen) { buf.push_back((char)c); changed = true; }
        // Held backspace should repeat — deleting a long value one key-press at a
        // time is the kind of friction that makes a tool annoying to live in.
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !buf.empty()) {
            buf.pop_back(); changed = true;
        }
    }

    const std::string& shown = buf;
    if (shown.empty() && !focused)
        UIText(placeholder, (int)(r.x + 6), (int)(r.y + (r.height - kFontSmall) * 0.5f), kFontSmall, kTextFaint);
    else {
        // Right-scroll the view so the caret end stays visible while typing.
        std::string vis = shown;
        while (!vis.empty() && UITextWidth(vis.c_str(), kFontSmall) > r.width - 14)
            vis.erase(vis.begin());
        UIText(vis.c_str(), (int)(r.x + 6), (int)(r.y + (r.height - kFontSmall) * 0.5f), kFontSmall, kText);
        if (focused && fmod(GetTime(), 1.0) < 0.5) {
            float cx = r.x + 6 + UITextWidth(vis.c_str(), kFontSmall) + 1;
            DrawLineEx({cx, r.y + 4}, {cx, r.y + r.height - 4}, 1.f, kAccent);
        }
    }
    return changed;
}

std::filesystem::path presetDir() { return std::filesystem::path("cache") / "presets"; }

} // namespace

void refreshTunerPresets(TunerUI& ui) {
    ui.presetFiles.clear();
    std::error_code ec;
    if (!std::filesystem::exists(presetDir(), ec)) return;
    for (auto& e : std::filesystem::directory_iterator(presetDir(), ec)) {
        if (ec) break;
        if (e.is_regular_file() && e.path().extension() == ".tun")
            ui.presetFiles.push_back(e.path().stem().string());
    }
    std::sort(ui.presetFiles.begin(), ui.presetFiles.end());
}

bool tunerWantsKeyboard(const TunerUI& ui) {
    return ui.open && (ui.searchFocused || ui.presetFocused || ui.editIndex >= 0);
}

bool tunerWantsMouse(const TunerUI& ui, int screenW) {
    if (ui.anim <= 0.01f) return false;
    return GetMousePosition().x >= screenW - ui.width * ui.anim;
}

bool updateTunerPanel(TunerUI& ui, int screenW, int screenH) {
    // ── open/close animation ─────────────────────────────────────────────────
    const float target = ui.open ? 1.f : 0.f;
    const float rate = GetFrameTime() * 7.f;
    if (ui.anim < target)      ui.anim = std::min(target, ui.anim + rate);
    else if (ui.anim > target) ui.anim = std::max(target, ui.anim - rate);
    if (ui.anim <= 0.001f) { ui.hotIndex = -1; return false; }

    // Ease-out so the slide feels settled rather than linear.
    const float e = 1.f - (1.f - ui.anim) * (1.f - ui.anim);
    const float px = screenW - ui.width * e;
    const Rectangle panel{px, 0.f, ui.width, (float)screenH};
    const Vector2 m = GetMousePosition();
    const bool over = hit(panel, m);

    auto& tun = allTunables();
    bool changed = false;

    DrawRectangleRec(panel, kPanelBg);
    DrawLineEx({panel.x, 0}, {panel.x, (float)screenH}, 1.5f, kPanelEdge);

    // ── header ───────────────────────────────────────────────────────────────
    const float hdrH = 34.f;
    DrawRectangleRec({panel.x, 0, panel.width, hdrH}, kHeaderBg);
    UIText("PHYSICS LAB", (int)(panel.x + kPad), 9, kFontHead, kText);

    const int nMod = modifiedTunableCount();
    if (nMod > 0) {
        const char* s = TextFormat("%d modified", nMod);
        UIText(s, (int)(panel.x + kPad + UITextWidth("PHYSICS LAB", kFontHead) + 12), 12, kFontSmall, kModified);
    }
    Rectangle closeBtn{panel.x + panel.width - 28, 7, 20, 20};
    if (button(closeBtn, "X", m)) ui.open = false;

    // ── search + filters ─────────────────────────────────────────────────────
    float y = hdrH + 8;
    Rectangle searchR{panel.x + kPad, y, panel.width - kPad * 2 - 96, 24};
    textField(searchR, ui.search, ui.searchFocused, "search  (name or group)", m);
    if (ui.searchFocused && IsKeyPressed(KEY_ESCAPE)) { ui.search.clear(); ui.searchFocused = false; }

    Rectangle modBtn{searchR.x + searchR.width + 6, y, 88, 24};
    {
        bool hov = hit(modBtn, m);
        DrawRectangleRec(modBtn, ui.onlyModified ? Color{90, 70, 30, 255} : (hov ? kAccentDim : Color{40, 42, 54, 255}));
        DrawRectangleLinesEx(modBtn, 1.f, ui.onlyModified ? kModified : kFieldEdge);
        const char* s = "modified";
        UIText(s, (int)(modBtn.x + (modBtn.width - UITextWidth(s, kFontSmall)) * 0.5f),
                 (int)(modBtn.y + 6), kFontSmall, ui.onlyModified ? kModified : kTextDim);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.onlyModified = !ui.onlyModified;
    }
    y += 32;

    // ── footer geometry (reserved before the list so the list can be clipped) ─
    const float footH = 92.f;
    const float listTop = y;
    const float listBot = screenH - footH;
    const float listH = listBot - listTop;

    // ── build the visible row set ────────────────────────────────────────────
    // Groups keep registry order (which is the order buildRegistry() declares
    // them: engine-meaningful, not alphabetical) while rows inside a group stay
    // in their declared order too — a speed-tier table reads wrong sorted.
    struct Row { int idx; };
    struct Group { std::string name; std::vector<Row> rows; int modified = 0; };
    std::vector<Group> groups;
    std::map<std::string, size_t> groupAt;

    for (size_t i = 0; i < tun.size(); i++) {
        const auto& t = tun[i];
        if (ui.onlyModified && !t.modified()) continue;
        if (!ui.search.empty() &&
            !containsFold(t.name, ui.search) && !containsFold(t.group, ui.search))
            continue;
        auto it = groupAt.find(t.group);
        if (it == groupAt.end()) {
            groupAt[t.group] = groups.size();
            groups.push_back({t.group, {}, 0});
            it = groupAt.find(t.group);
        }
        groups[it->second].rows.push_back({(int)i});
        if (t.modified()) groups[it->second].modified++;
    }

    // ── scrolling ────────────────────────────────────────────────────────────
    float content = 0.f;
    for (auto& g : groups) {
        content += kGroupH;
        if (!ui.collapsed.count(g.name)) content += kRowH * g.rows.size();
        content += 4;
    }
    ui.contentHeight = content;
    const float maxScroll = std::max(0.f, content - listH);

    // Wheel scrolls the list, EXCEPT over a value box (there it nudges the value
    // — see the row loop). That split is what makes the panel usable: scrolling a
    // 400-row list and fine-tuning a number are both wheel-shaped gestures.
    bool wheelOverValue = false;

    // An in-progress edit whose row just got filtered away (the user typed in the
    // search box, or flipped "modified") can never receive Enter or Escape — and
    // since tunerWantsKeyboard() stays true while editIndex is set, that would
    // silently lock the keyboard out of the rest of the app. Drop the edit
    // whenever its row is no longer on screen.
    if (ui.editIndex >= 0) {
        bool stillVisible = false;
        for (auto& g : groups) {
            if (ui.collapsed.count(g.name)) continue;
            for (auto& r : g.rows) if (r.idx == ui.editIndex) { stillVisible = true; break; }
            if (stillVisible) break;
        }
        if (!stillVisible) ui.editIndex = -1;
    }

    ui.scroll = std::clamp(ui.scroll, 0.f, maxScroll);

    // ── row list ─────────────────────────────────────────────────────────────
    BeginScissorMode((int)panel.x, (int)listTop, (int)panel.width, (int)listH);
    float ry = listTop - ui.scroll;
    ui.hotIndex = -1;
    int hoveredForHelp = -1;

    for (auto& g : groups) {
        // ── group header ──
        Rectangle gr{panel.x + 4, ry, panel.width - 8, kGroupH - 2};
        const bool folded = ui.collapsed.count(g.name) > 0;
        if (ry + kGroupH > listTop && ry < listBot) {
            bool hov = over && hit(gr, m) && m.y >= listTop && m.y < listBot;
            DrawRectangleRec(gr, hov ? kRowHot : kGroupBg);
            UIText(folded ? "+" : "-", (int)(gr.x + 8), (int)(gr.y + 6), kFontRow, kAccent);
            UIText(g.name.c_str(), (int)(gr.x + 22), (int)(gr.y + 6), kFontRow, kAccent);
            const char* cnt = TextFormat("%d", (int)g.rows.size());
            UIText(cnt, (int)(gr.x + gr.width - UITextWidth(cnt, kFontSmall) - 10), (int)(gr.y + 7),
                     kFontSmall, kTextFaint);
            if (g.modified > 0) {
                const char* ms = TextFormat("%d*", g.modified);
                UIText(ms, (int)(gr.x + gr.width - UITextWidth(cnt, kFontSmall) - 10
                                    - UITextWidth(ms, kFontSmall) - 10), (int)(gr.y + 7), kFontSmall, kModified);
            }
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (folded) ui.collapsed.erase(g.name); else ui.collapsed.insert(g.name);
            }
        }
        ry += kGroupH;
        if (folded) { ry += 4; continue; }

        for (auto& row : g.rows) {
            auto& t = tun[row.idx];
            Rectangle rr{panel.x + 4, ry, panel.width - 8, kRowH};
            // Cull rows scrolled out of view — with ~400 tunables this is the
            // difference between a smooth panel and a per-frame text-measuring
            // storm.
            if (ry + kRowH < listTop || ry > listBot) { ry += kRowH; continue; }

            const bool rowHov = over && hit(rr, m) && m.y >= listTop && m.y < listBot;
            if (rowHov) { ui.hotIndex = row.idx; hoveredForHelp = row.idx; DrawRectangleRec(rr, kRowHot); }

            const bool mod = t.modified();
            Rectangle valR{rr.x + rr.width - kValueW - kResetW - 6, ry + 2, kValueW, kRowH - 4};
            Rectangle resR{rr.x + rr.width - kResetW - 2, ry + 3, kResetW, kRowH - 6};

            // label
            drawTextClipped(t.name.c_str(), rr.x + 10, ry + 6, kFontRow,
                            mod ? kModified : kText, valR.x - rr.x - 16);

            // value box
            const bool editing = (ui.editIndex == row.idx);
            const bool valHov = over && hit(valR, m) && m.y >= listTop && m.y < listBot;
            DrawRectangleRec(valR, kFieldBg);
            DrawRectangleLinesEx(valR, 1.f, editing ? kAccent : (valHov ? kAccent : (mod ? kModified : kFieldEdge)));

            if (editing) {
                int c;
                while ((c = GetCharPressed()) != 0)
                    // Only characters a number can contain — silently ignoring the
                    // rest beats letting the user type something that then fails
                    // to parse on Enter with no explanation.
                    if (std::strchr("0123456789.eE+-", c) && ui.editBuf.size() < 32)
                        ui.editBuf.push_back((char)c);
                if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !ui.editBuf.empty())
                    ui.editBuf.pop_back();
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                    try {
                        double v = std::stod(ui.editBuf);
                        if (v != t.get()) { t.set(v); changed = true; }
                    } catch (...) { /* unparseable -> keep the old value, no crash */ }
                    ui.editIndex = -1;
                }
                if (IsKeyPressed(KEY_ESCAPE)) ui.editIndex = -1;
                std::string vis = ui.editBuf;
                while (!vis.empty() && UITextWidth(vis.c_str(), kFontRow) > valR.width - 12)
                    vis.erase(vis.begin());
                UIText(vis.c_str(), (int)(valR.x + 6), (int)(valR.y + 4), kFontRow, kAccent);
                if (fmod(GetTime(), 1.0) < 0.5) {
                    float cx = valR.x + 6 + UITextWidth(vis.c_str(), kFontRow) + 1;
                    DrawLineEx({cx, valR.y + 3}, {cx, valR.y + valR.height - 3}, 1.f, kAccent);
                }
            } else {
                std::string vs = fmtValue(t.get(), t.kind);
                // Right-align: comparing a column of numbers is the whole job here.
                float tw = (float)UITextWidth(vs.c_str(), kFontRow);
                if (tw > valR.width - 10) {
                    drawTextClipped(vs.c_str(), valR.x + 5, valR.y + 4, kFontRow, mod ? kModified : kText,
                                    valR.width - 10);
                } else {
                    UIText(vs.c_str(), (int)(valR.x + valR.width - tw - 6), (int)(valR.y + 4), kFontRow,
                             mod ? kModified : kText);
                }
                // A thin fill showing where the value sits inside its soft range —
                // instant "is this near its default or way out?" feedback.
                if (t.hi != t.lo) {
                    float f = (float)((t.get() - t.lo) / (t.hi - t.lo));
                    f = std::clamp(f, 0.f, 1.f);
                    DrawRectangle((int)valR.x + 1, (int)(valR.y + valR.height - 3),
                                  (int)((valR.width - 2) * f), 2, mod ? kModified : kAccentDim);
                }
            }

            // reset arrow (only when it would do something)
            if (mod) {
                bool rhov = over && hit(resR, m) && m.y >= listTop && m.y < listBot;
                UIText("<", (int)(resR.x + 6), (int)(resR.y + 3), kFontRow, rhov ? kAccent : kTextDim);
                if (rhov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    t.reset(); changed = true;
                    if (ui.editIndex == row.idx) ui.editIndex = -1;
                }
            }

            // ── drag-to-scrub ──
            if (valHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !editing) {
                ui.dragIndex = row.idx;
                ui.dragStartValue = t.get();
                ui.dragAccum = 0.f;
                ui.dragMoved = false;
            }
            // Right-click resets — faster than aiming at the little arrow.
            if (valHov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && mod) { t.reset(); changed = true; }

            // ── wheel-over-value steps ──
            if (valHov && !editing) {
                float w = GetMouseWheelMove();
                if (w != 0.f) {
                    wheelOverValue = true;
                    double st = t.step;
                    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) st *= 0.1;
                    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) st *= 10.0;
                    if (t.kind == TunKind::I32) st = std::max(1.0, std::round(st));
                    t.set(t.get() + st * w);
                    changed = true;
                }
            }
            ry += kRowH;
        }
        ry += 4;
    }
    EndScissorMode();

    // ── active drag (handled outside the row loop so it survives the cursor
    //    leaving the row, which is what makes a long scrub usable) ────────────
    if (ui.dragIndex >= 0) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float dx = GetMouseDelta().x;
            if (std::fabs(dx) > 0.f) ui.dragMoved = true;
            double st = tun[ui.dragIndex].step;
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) st *= 0.1;
            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) st *= 10.0;
            if (tun[ui.dragIndex].kind == TunKind::I32) {
                // Integers need pixel accumulation or a slow drag never crosses a
                // whole step and the field looks frozen.
                ui.dragAccum += dx;
                double per = 6.0;   // px per unit
                int whole = (int)(ui.dragAccum / per);
                if (whole != 0) {
                    ui.dragAccum -= whole * (float)per;
                    tun[ui.dragIndex].set(tun[ui.dragIndex].get() + whole);
                    changed = true;
                }
            } else if (dx != 0.f) {
                tun[ui.dragIndex].set(tun[ui.dragIndex].get() + st * dx);
                changed = true;
            }
        } else {
            // Released without moving = a click, which means "let me type it".
            if (!ui.dragMoved) {
                ui.editIndex = ui.dragIndex;
                ui.editBuf = fmtValue(tun[ui.dragIndex].get(), tun[ui.dragIndex].kind);
            }
            ui.dragIndex = -1;
        }
    }

    // Scroll the list with the wheel unless it was spent on a value box.
    if (over && !wheelOverValue && ui.dragIndex < 0) {
        float w = GetMouseWheelMove();
        if (w != 0.f) ui.scroll = std::clamp(ui.scroll - w * 48.f, 0.f, maxScroll);
    }
    // Clicking empty panel space drops focus out of an edit, matching every other
    // property editor.
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui.editIndex >= 0 && ui.dragIndex < 0
        && ui.hotIndex != ui.editIndex)
        ui.editIndex = -1;

    // ── scrollbar ────────────────────────────────────────────────────────────
    if (maxScroll > 0.f) {
        float trackH = listH;
        float thumbH = std::max(28.f, trackH * (listH / content));
        float t01 = maxScroll > 0 ? ui.scroll / maxScroll : 0.f;
        float ty = listTop + (trackH - thumbH) * t01;
        DrawRectangle((int)(panel.x + panel.width - 5), (int)listTop, 3, (int)trackH, Color{40, 42, 54, 255});
        DrawRectangle((int)(panel.x + panel.width - 5), (int)ty, 3, (int)thumbH, kAccentDim);
    }

    // ── footer ───────────────────────────────────────────────────────────────
    const float fy = screenH - footH;
    DrawRectangleRec({panel.x, fy, panel.width, footH}, kHeaderBg);
    DrawLineEx({panel.x, fy}, {panel.x + panel.width, fy}, 1.f, kPanelEdge);

    // Help line for the hovered row — a footer line, not a floating tooltip, so a
    // long sentence has room and never covers the value the user is reading.
    if (hoveredForHelp >= 0) {
        const auto& t = tun[hoveredForHelp];
        drawTextClipped(t.help.c_str(), panel.x + kPad, fy + 6, kFontSmall, kTextDim, panel.width - kPad * 2);
        const char* d = TextFormat("default %s     step %.4g", fmtValue(t.defVal, t.kind).c_str(), t.step);
        UIText(d, (int)(panel.x + kPad), (int)(fy + 21), kFontSmall, kTextFaint);
    } else {
        UIText("drag a value to scrub  -  click to type  -  wheel to step  -  right-click resets",
                 (int)(panel.x + kPad), (int)(fy + 6), kFontSmall, kTextFaint);
        UIText("hold SHIFT for fine, CTRL for coarse",
                 (int)(panel.x + kPad), (int)(fy + 21), kFontSmall, kTextFaint);
    }

    // preset row
    float by = fy + 40;
    Rectangle nameR{panel.x + kPad, by, 150, 22};
    textField(nameR, ui.presetName, ui.presetFocused, "preset name", m, 40);

    Rectangle saveB{nameR.x + nameR.width + 6, by, 54, 22};
    Rectangle loadB{saveB.x + 60, by, 54, 22};
    Rectangle resetB{loadB.x + 60, by, 74, 22};

    if (button(saveB, "save", m, !ui.presetName.empty())) {
        std::error_code ec;
        std::filesystem::create_directories(presetDir(), ec);
        auto p = (presetDir() / (ui.presetName + ".tun")).string();
        ui.toast = saveTunablePreset(p) ? ("saved " + ui.presetName + ".tun") : "save failed";
        ui.toastUntil = GetTime() + 3.0;
        refreshTunerPresets(ui);
    }
    if (button(loadB, "load", m, !ui.presetName.empty())) {
        auto p = (presetDir() / (ui.presetName + ".tun")).string();
        if (loadTunablePreset(p)) { ui.toast = "loaded " + ui.presetName + ".tun"; changed = true; }
        else ui.toast = "no such preset";
        ui.toastUntil = GetTime() + 3.0;
    }
    if (button(resetB, "reset all", m, nMod > 0, kWarn)) {
        resetAllTunables(); changed = true;
        ui.toast = "all values back to defaults";
        ui.toastUntil = GetTime() + 3.0;
    }

    // resim toggle
    Rectangle resimR{panel.x + kPad, by + 26, panel.width - kPad * 2, 20};
    {
        bool hov = over && hit(resimR, m);
        Rectangle box{resimR.x, resimR.y + 3, 14, 14};
        DrawRectangleRec(box, ui.resimOnChange ? kAccentDim : kFieldBg);
        DrawRectangleLinesEx(box, 1.f, ui.resimOnChange ? kAccent : kFieldEdge);
        if (ui.resimOnChange) UIText("x", (int)(box.x + 4), (int)(box.y + 1), kFontSmall, kAccent);
        UIText("re-simulate from frame 0 on every change", (int)(box.x + 21), (int)(resimR.y + 4),
                 kFontSmall, hov ? kText : kTextDim);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.resimOnChange = !ui.resimOnChange;
    }

    // toast
    if (GetTime() < ui.toastUntil && !ui.toast.empty())
        UIText(ui.toast.c_str(), (int)(panel.x + kPad), (int)(fy - 18), kFontSmall, kGood);

    // Available presets, as a hint line (click a name to put it in the field).
    if (!ui.presetFiles.empty()) {
        float lx = panel.x + kPad;
        UIText("presets:", (int)lx, (int)(fy + 68), kFontSmall, kTextFaint);
        lx += UITextWidth("presets:", kFontSmall) + 8;
        for (auto& p : ui.presetFiles) {
            int w = UITextWidth(p.c_str(), kFontSmall);
            if (lx + w > panel.x + panel.width - kPad) break;
            Rectangle pr{lx - 3, fy + 66, (float)w + 6, 15};
            bool hov = over && hit(pr, m);
            UIText(p.c_str(), (int)lx, (int)(fy + 68), kFontSmall, hov ? kAccent : kTextDim);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.presetName = p;
            lx += w + 12;
        }
    }

    return changed;
}

} // namespace gdapp
