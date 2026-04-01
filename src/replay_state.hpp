#pragma once

#include <Geode/Geode.hpp>
#include <atomic>
#include <optional>
#include <filesystem>

using namespace geode::prelude;

struct ReplayPress {
    uint64_t framePress;
    uint64_t frameRelease;
    int player; // 1 or 2
};

struct ReplayLoadResult {
    std::vector<ReplayPress> presses;
    double framerate = 240.0;
};

struct ReplayPlayerState {
    bool isActive = false;
    std::optional<ReplayLoadResult> replay;
    float playbackSpeed = 1.0f;
    bool isPaused = false;
    float pauseTime = 0.0f; // When paused, the frozen time
    float inputIndex = 0.0f; // Current position in replay inputs
    bool isEditMode = false;
    int levelId = 0;
    std::filesystem::path sourceReplayPath;
};

struct ReplayExternalCommands {
    std::atomic<int> pauseToggleRequests {0};
    std::atomic<int> setPausedState {-1}; // -1 ignore, 0 play, 1 pause
    std::atomic<int> stepFrameRequests {0};
    std::atomic<bool> restartRequested {false};
    std::atomic<uint64_t> liveFrame {0};
    std::atomic<bool> livePaused {false};
    std::atomic<bool> liveReplayActive {false};
};

extern ReplayPlayerState g_replayPlayer;
extern ReplayExternalCommands g_replayExternalCommands;
extern std::optional<std::filesystem::path> g_lastMatchedLocalReplayPath;

std::optional<ReplayLoadResult> loadMatchingReplay(int levelId);
bool saveReplayToLocalJson(int levelId, const ReplayLoadResult& replay, std::filesystem::path preferredPath = {});
void convertGdr2File(const std::filesystem::path& path);
