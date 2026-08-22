#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Standalone (Geode-free) .gdr2 reader, ported from src/replay_core.cpp's
// GDR2Reader/parseGDR2 — that version depends on Geode's Result<T>/ByteVector
// wrappers and <Geode/utils/file.hpp>, which the standalone app can't link
// against. The byte-level format logic is identical; only the error-handling
// shape and the final press/release conversion (done inline here, mirroring
// replay_core.cpp's parseReplayEventsFromJsonArray) differ.
namespace gdapp {

struct Gdr2Press {
    uint64_t framePress;
    uint64_t frameRelease;
};

struct Gdr2ImportResult {
    bool success = false;
    std::string error;
    uint32_t levelId = 0;
    std::string levelName;
    double framerate = 240.0;
    std::vector<Gdr2Press> presses; // player 1, Jump button only — all this app's
                                     // replay/solver machinery understands
};

// Reads and parses a .gdr2 file at `path`. Only the Jump button (id 1) for
// player 1 is extracted — matching every other replay path in this app
// (solver output, macro list), which model input as a single press/hold bit
// per frame, not a multi-button/two-player stream.
Gdr2ImportResult importGdr2(const std::string& path);

} // namespace gdapp
