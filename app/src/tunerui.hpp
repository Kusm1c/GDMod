#pragma once
#include "raylib.h"
#include <string>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PHYSICS LAB — a live editor for every constant in gdsim.
//
// Enumerates gdsim::allTunables() (see src/sim/Tunables.hpp) and lets any of
// them be scrubbed, typed, or reset while the app is running. It owns no physics
// knowledge of its own: adding a constant to the registry makes it appear here
// automatically, with a sensible drag range derived from its default.
//
// The panel reports back a single bit — "something changed this frame" — and the
// host decides what that means (in practice: re-simulate from frame 0, because a
// deterministic integrator only shows a constant's true effect from the start).
// ─────────────────────────────────────────────────────────────────────────────

namespace gdapp {

struct TunerUI {
    bool  open = false;
    float anim = 0.f;              // 0..1 slide-in progress (eased in draw)
    float width = 520.f;

    // ── filtering ─────────────────────────────────────────────────────────────
    std::string search;
    bool searchFocused = false;
    bool onlyModified = false;
    std::unordered_set<std::string> collapsed;   // group names the user folded away

    // ── interaction state ─────────────────────────────────────────────────────
    int    hotIndex  = -1;         // registry index of the row under the cursor
    int    editIndex = -1;         // registry index being typed into (-1 = none)
    std::string editBuf;
    int    dragIndex = -1;         // registry index being scrubbed (-1 = none)
    double dragStartValue = 0.0;
    float  dragAccum = 0.f;        // sub-pixel drag remainder, so slow drags still move
    bool   dragMoved = false;      // distinguishes a click (edit) from a drag (scrub)

    float scroll = 0.f;
    float contentHeight = 0.f;

    // ── presets ───────────────────────────────────────────────────────────────
    std::string presetName = "preset";
    bool presetFocused = false;
    std::vector<std::string> presetFiles;   // refreshed when the panel opens
    std::string toast;                       // transient one-line status
    double toastUntil = 0.0;

    // ── behaviour ─────────────────────────────────────────────────────────────
    // When set, the host re-simulates the current run from frame 0 on every edit
    // so the effect is visible immediately. Turning it off is useful while
    // dragging through a very long level.
    bool resimOnChange = true;
};

// Draws and services the panel. Returns true if any tunable's value changed this
// frame (so the host can resim). Safe to call every frame regardless of `open` —
// it handles its own open/close animation and does nothing while fully closed.
bool updateTunerPanel(TunerUI& ui, int screenW, int screenH);

// True while the panel is consuming typed input (a text field is focused), so
// the host must not also act on those keystrokes as gameplay/camera shortcuts.
bool tunerWantsKeyboard(const TunerUI& ui);

// True while the cursor is over the panel, so the host must not treat the click
// or scroll wheel as a world interaction.
bool tunerWantsMouse(const TunerUI& ui, int screenW);

// Re-scan cache/presets for *.tun files. Called on open; exposed so the host can
// refresh after writing one itself.
void refreshTunerPresets(TunerUI& ui);

} // namespace gdapp
