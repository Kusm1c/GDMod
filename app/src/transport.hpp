#pragma once
#include "raylib.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// REPLAY TRANSPORT — playback controls for a deterministic gdsim run.
//
// A pure widget: it draws the bar, reads input, and reports what the user asked
// for. It performs no simulation itself, because "seek to frame N" in a
// deterministic integrator means "rebuild and re-run to N", which only the host
// (which owns the Level and the input track) can do correctly.
// ─────────────────────────────────────────────────────────────────────────────

namespace gdapp {

struct TransportUI {
    // Playback rate multiplier on real time. 1 = the game's own 240Hz pace.
    // 0 means "frozen": only explicit frame-steps advance the sim, which is the
    // mode most of the fidelity work actually happens in.
    float speed = 1.f;
    bool  paused = false;
    // Camera follows the player instead of staying where it was panned. Off by
    // default in this app's replay view (deliberate — see main.cpp), but the bar
    // exposes it because chasing a divergence with a manual camera is painful.
    bool  followCam = false;
    // True while the user is dragging the timeline; the host must not advance the
    // sim on its own during a scrub or the two fight each other.
    bool  scrubbing = false;
    // Hidden with the rest of the HUD, but kept separate so the transport can be
    // shown while the other overlays are hidden.
    bool  visible = true;
};

struct TransportResult {
    bool restart      = false;   // rebuild from frame 0 and play
    bool seek         = false;   // rebuild and fast-forward to seekFrame
    int  seekFrame    = 0;
    int  stepFrames   = 0;       // relative step: + forward, - backward
    bool hoveringBar  = false;   // cursor is over the bar (host should ignore clicks)
};

// `curFrame` is the run's current frame; `totalFrames` an estimate of its full
// length (used for the timeline scale — a rough value is fine, the bar clamps).
// `status` is a free-form right-aligned string (position, velocity, whatever the
// host wants to surface). `alive` greys out the play button once the run ended.
TransportResult drawTransport(TransportUI& ui, int curFrame, int totalFrames,
                              const std::string& status, bool alive,
                              int screenW, int screenH, bool blockMouse);

// Keyboard shortcuts for the transport, kept separate from the bar so the host
// can skip them while a text field has focus. Returns the same action struct;
// the host merges it with the bar's.
TransportResult transportKeys(TransportUI& ui, int curFrame, int totalFrames);

} // namespace gdapp
