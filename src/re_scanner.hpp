#pragma once

// Central switch for the background "reverse-engineering scanner". When the
// "re-scanner" mod setting is on, the mod continuously captures the REAL engine
// while you play — mechanics (mech_recorder.cpp), move-trigger object motion,
// per-frame physics, and level parameters — into GDMod_*.txt files. Everything
// is passive (read engine state, write text files); it never touches gdsim or
// the solver, so it cannot break gameplay or solving.
namespace rescan {
    // Cached read of the "re-scanner" bool setting (live-updated via a Geode
    // setting-change listener). Cheap enough to call every frame.
    bool enabled();
}
