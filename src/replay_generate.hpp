#pragma once

#include "replay_state.hpp"

struct ReplayGeneratorConfig {
    // Canonical replay FPS used by the generated output.
    double targetFramerate = 480.0;
    // Minimum press duration in generated frames.
    uint64_t minHoldFrames = 2;
    // Merge tiny release gaps up to this many source frames.
    uint64_t mergeGapFrames = 1;
};

// Generates a cleaner, low-click replay that is more stable across playback rates.
ReplayLoadResult generateUniversalLowClickReplay(const ReplayLoadResult& source, const ReplayGeneratorConfig& config = {});

// Number of press events (one click per press).
size_t replayClickCount(const ReplayLoadResult& replay);
