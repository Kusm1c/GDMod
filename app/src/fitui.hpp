#pragma once
#include "raylib.h"
#include "render.hpp"
#include "trajfit.hpp"
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// TRAJECTORY CORRECTION UI
//
// Click a point on the replay trail to pin it, drag the pin to where the player
// SHOULD have been, then ask which physics constant explains the difference
// (trajfit.hpp does the actual inversion).
//
// The honest workflow, and the one the UI steers toward: with a real capture
// loaded, press G to snap the pin onto the REAL recorded trajectory at that
// frame. Then the fit is calibration against ground truth. Dragging by hand is
// supported for exploration, and labelled as such — fitting constants to a
// guessed target is precisely the "tune to chase a symptom" trap this project
// has paid for before.
// ─────────────────────────────────────────────────────────────────────────────

namespace gdapp {

struct FitUIState {
    bool  enabled = false;         // correction mode on (toggled by the host)
    int   hoverIdx = -1;           // trail index under the cursor
    int   selectedIdx = -1;        // pinned trail index (-1 = nothing pinned)
    bool  dragging = false;

    bool  hasTarget = false;
    float targetX = 0.f, targetY = 0.f;
    bool  fitX = false, fitY = true;
    bool  snappedToReal = false;   // target came from the real capture, not a drag

    // ── background fit ────────────────────────────────────────────────────────
    std::unique_ptr<FitProgress> prog;
    std::thread thread;
    bool running = false;
    std::vector<FitCandidate> results;
    double baselineResidual = 0.0, elapsedMs = 0.0;
    std::string note;
    float scroll = 0.f;
    int   scanCount = 0;

    std::string toast;
    double toastUntil = 0.0;

    ~FitUIState();
};

struct FitUIHost {
    const std::vector<TrailPoint>*     trail = nullptr;
    const std::vector<RealTrailPoint>* realTrail = nullptr;
    float realOffsetX = 0.f, realOffsetY = 0.f;
    const std::string*        levelString = nullptr;
    const std::vector<bool>*  inputAt = nullptr;
    Camera2DState cam;
    int  screenW = 0, screenH = 0;
    bool blockMouse = false;   // another panel owns the cursor
    bool blockKeys = false;    // a text field owns the keyboard
};

// Draws the pin/handles and the candidate panel, and services all interaction.
// Returns true if a candidate was APPLIED (the host should re-simulate).
bool updateFitUI(FitUIState& st, const FitUIHost& host);

// True while the correction UI wants the mouse (cursor over its panel, or a drag
// in progress), so the host must not also pan/zoom or pick objects.
bool fitWantsMouse(const FitUIState& st, const FitUIHost& host);

// Stops and joins any running fit. Safe to call repeatedly.
void cancelFit(FitUIState& st);

} // namespace gdapp
