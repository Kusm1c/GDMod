#pragma once

#include <Geode/Geode.hpp>
#include <atomic>
#include <optional>
#include <filesystem>
#include <cstdint>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

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

struct ReplayRuntimeHitboxRect {
    enum class Shape : uint8_t {
        Rectangle,
        Circle,
        OrientedQuad
    };

    Shape shape = Shape::Rectangle;
    int objectId = 0;
    int objectType = -1;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float radius = 0.0f;
    std::array<cocos2d::CCPoint, 4> corners {};
    bool isSolid = false;
    bool isHazard = false;
};

struct ReplayRuntimeHitboxSnapshot {
    int levelId = 0;
    uint64_t captureMs = 0;
    std::vector<ReplayRuntimeHitboxRect> hitboxes;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    size_t solidCount = 0;
    size_t hazardCount = 0;
};

extern ReplayPlayerState g_replayPlayer;
extern ReplayExternalCommands g_replayExternalCommands;
extern std::optional<std::filesystem::path> g_lastMatchedLocalReplayPath;
extern std::mutex g_runtimeHitboxSnapshotsMutex;
extern std::unordered_map<int, ReplayRuntimeHitboxSnapshot> g_runtimeHitboxSnapshots;

void storeRuntimeHitboxSnapshot(ReplayRuntimeHitboxSnapshot snapshot);
std::optional<ReplayRuntimeHitboxSnapshot> getRuntimeHitboxSnapshot(int levelId);

std::optional<ReplayLoadResult> loadMatchingReplay(int levelId);
bool saveReplayToLocalJson(int levelId, const ReplayLoadResult& replay, std::filesystem::path preferredPath = {});
void convertGdr2File(const std::filesystem::path& path);
