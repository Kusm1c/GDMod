#include "replay_generate.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    struct Interval {
        double start = 0.0;
        double end = 0.0;
        int player = 1;
    };

    static uint64_t toFrame(double seconds, double fps) {
        if (seconds <= 0.0 || fps <= 0.0) return 0;
        return static_cast<uint64_t>(std::llround(seconds * fps));
    }

    static void normalizePresses(ReplayLoadResult& replay) {
        for (auto& press : replay.presses) {
            if (press.player != 2) {
                press.player = 1;
            }
            if (press.frameRelease <= press.framePress) {
                press.frameRelease = press.framePress + 1;
            }
        }

        std::sort(replay.presses.begin(), replay.presses.end(), [](const ReplayPress& a, const ReplayPress& b) {
            if (a.player != b.player) return a.player < b.player;
            if (a.framePress != b.framePress) return a.framePress < b.framePress;
            return a.frameRelease < b.frameRelease;
        });
    }

    static std::vector<Interval> mergeIntervals(std::vector<Interval> intervals, double mergeGapSeconds) {
        if (intervals.empty()) return {};

        std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) {
            if (a.start != b.start) return a.start < b.start;
            return a.end < b.end;
        });

        std::vector<Interval> merged;
        merged.reserve(intervals.size());
        merged.push_back(intervals.front());

        for (size_t i = 1; i < intervals.size(); ++i) {
            auto& back = merged.back();
            const auto& cur = intervals[i];
            if (cur.start <= back.end + mergeGapSeconds) {
                back.end = std::max(back.end, cur.end);
            } else {
                merged.push_back(cur);
            }
        }

        return merged;
    }
}

ReplayLoadResult generateUniversalLowClickReplay(const ReplayLoadResult& source, const ReplayGeneratorConfig& config) {
    ReplayLoadResult out;
    if (source.presses.empty()) {
        out.framerate = std::max(60.0, source.framerate);
        return out;
    }

    double sourceFps = source.framerate > 1.0 ? source.framerate : 240.0;
    double targetFps = std::clamp(config.targetFramerate, 120.0, 960.0);
    targetFps = std::max(targetFps, sourceFps);

    uint64_t minHoldFrames = std::max<uint64_t>(1, config.minHoldFrames);
    double mergeGapSeconds = static_cast<double>(config.mergeGapFrames) / sourceFps;

    std::array<std::vector<Interval>, 2> perPlayer;
    for (const auto& press : source.presses) {
        int playerIdx = press.player == 2 ? 1 : 0;
        double start = static_cast<double>(press.framePress) / sourceFps;
        double end = static_cast<double>(std::max(press.frameRelease, press.framePress + 1)) / sourceFps;
        if (end <= start) {
            end = start + (1.0 / sourceFps);
        }
        perPlayer[playerIdx].push_back({start, end, playerIdx + 1});
    }

    for (int p = 0; p < 2; ++p) {
        auto merged = mergeIntervals(std::move(perPlayer[p]), mergeGapSeconds);
        for (const auto& it : merged) {
            uint64_t framePress = toFrame(it.start, targetFps);
            uint64_t frameRelease = toFrame(it.end, targetFps);
            if (frameRelease <= framePress) {
                frameRelease = framePress + minHoldFrames;
            }
            if (frameRelease - framePress < minHoldFrames) {
                frameRelease = framePress + minHoldFrames;
            }
            out.presses.push_back({framePress, frameRelease, it.player});
        }
    }

    out.framerate = targetFps;
    normalizePresses(out);

    // Final pass at target FPS to collapse residual one-frame gaps introduced by quantization.
    ReplayLoadResult secondPass = out;
    out.presses.clear();
    out.framerate = targetFps;

    std::array<std::vector<Interval>, 2> quantizedPerPlayer;
    double mergeGapTargetSeconds = static_cast<double>(config.mergeGapFrames) / targetFps;
    for (const auto& press : secondPass.presses) {
        int playerIdx = press.player == 2 ? 1 : 0;
        double start = static_cast<double>(press.framePress) / targetFps;
        double end = static_cast<double>(press.frameRelease) / targetFps;
        quantizedPerPlayer[playerIdx].push_back({start, end, playerIdx + 1});
    }

    for (int p = 0; p < 2; ++p) {
        auto merged = mergeIntervals(std::move(quantizedPerPlayer[p]), mergeGapTargetSeconds);
        for (const auto& it : merged) {
            uint64_t framePress = toFrame(it.start, targetFps);
            uint64_t frameRelease = toFrame(it.end, targetFps);
            if (frameRelease <= framePress) {
                frameRelease = framePress + minHoldFrames;
            }
            out.presses.push_back({framePress, frameRelease, it.player});
        }
    }

    normalizePresses(out);
    return out;
}

size_t replayClickCount(const ReplayLoadResult& replay) {
    return replay.presses.size();
}
