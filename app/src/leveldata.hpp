#pragma once
#include <string>
#include <vector>

// Fetches and decodes a Geometry Dash level's object string directly from
// RobTop's own servers (boomlings.com/database/downloadGJLevel22.php), the
// same endpoint the real game client uses.
//
// Pipeline: HTTP POST -> raw response ("1:<id>:2:<name>:...:4:<data>:...#h1#h2")
// -> extract field "4" -> base64url decode -> gzip inflate -> the ';'-separated
// level string gdsim::Level already parses directly.
namespace gdapp {

struct LevelFetchResult {
    bool success = false;
    bool rateLimited = false;   // true if the failure was a Cloudflare rate-limit response
    bool fromCache = false;     // true if this came from the on-disk cache (no network used)
    std::string error;          // human-readable failure reason, if !success
    std::string name;           // level name (field "2"), if available
    std::string author;         // best-effort; not always present in this response
    std::string levelString;    // decoded ';'-separated object string, ready for gdsim::Level
};

LevelFetchResult fetchLevel(int levelId);

struct CachedLevelEntry {
    int id = 0;
    std::string name;
};

// Scans the on-disk cache directory and returns every level already
// downloaded, newest first (by file write time) — powers the "pick an
// already-downloaded level" menu without touching the network at all.
std::vector<CachedLevelEntry> listCachedLevels();

} // namespace gdapp
