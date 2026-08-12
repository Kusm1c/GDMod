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
// Set by the level menu's "Auto-repair" action and consumed once in PlayLayer::init.
// When true, the level launches in auto-repair mode: it replays the current gdsim
// solution in the REAL engine and locally brute-forces clicks near each death until
// it clears (or gets stuck), guaranteeing the saved replay actually completes in GD.
extern std::atomic<bool> g_autoRepairRequested;
// Set by doSimulate's default (non-repair) path and consumed once in PlayLayer::init.
// The .gdr2 has ALREADY been written by the time this fires — this just runs ONE
// real-game playthrough for confirmation + telemetry (divergence/truth capture),
// never blocking on it and never brute-forcing edits. See confirmOnlyFinish.
extern std::atomic<bool> g_confirmRequested;
extern std::mutex g_runtimeHitboxSnapshotsMutex;
extern std::unordered_map<int, ReplayRuntimeHitboxSnapshot> g_runtimeHitboxSnapshots;
void storeRuntimeHitboxSnapshot(ReplayRuntimeHitboxSnapshot snapshot);
std::optional<ReplayRuntimeHitboxSnapshot> getRuntimeHitboxSnapshot(int levelId);

// The most recently solved replay, kept around purely so the "Export..." button in
// PathfinderMenuPopup (hooks_menu.cpp) can re-encode it to a user-chosen path at any
// point after the solve finishes — doSimulate itself no longer launches the level
// automatically (see that function's own comment), so this is the only way to reach
// a fresh solve's data afterward.
struct LastSolvedExport {
    bool available = false;
    int levelId = 0;
    std::string levelName;
    ReplayLoadResult replay;
};
extern LastSolvedExport g_lastSolvedExport;

std::optional<ReplayLoadResult> loadMatchingReplay(int levelId);
bool saveReplayToLocalJson(int levelId, const ReplayLoadResult& replay, std::filesystem::path preferredPath = {});
void convertGdr2File(const std::filesystem::path& path);
std::vector<uint8_t> exportReplayToGdr2(const ReplayLoadResult& replay, int levelId, const std::string& levelName);
// Write a solved replay as a .gdr2, no dialog (bot workflow: solve → file on disk).
// Prefers the Eclipse replays folder so it's directly playable in-game; always also
// writes a copy in the mod save dir. Returns the primary output path, or "" on failure.
std::string writeSolvedGdr2(int levelId, const std::string& levelName, const ReplayLoadResult& replay);
// Write a solved replay as a .gdr2 to an EXACT user-chosen path (the "Export..."
// button's save-file dialog) — same binary encoding as writeSolvedGdr2, just no
// folder-guessing/fallback logic. Returns true on success.
bool writeGdr2ToExactPath(const std::filesystem::path& path, int levelId,
                          const std::string& levelName, const ReplayLoadResult& replay);
