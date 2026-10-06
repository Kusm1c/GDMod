// EngineTrace — reader for testlevel/GDMod_enginetrace_<id>.bin (written by src/engine_trace.cpp).
//
// The layout below MUST mirror the writer byte for byte. The state blocks are written and
// read through the same generated SnapFields.inc (EngineSnap.hpp), and the file header
// carries layoutHash() so a trace recorded against a different field list is refused
// rather than silently misread.
#pragma once
#include "EngineSnap.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gdsim::engine {

// Function ids — identical to engine_trace.cpp's enum. Append only.
enum class Fn : uint16_t {
    update = 1, updateJump, postCollision, preSlopeCollision, collidedWithSlopeInternal,
    collidedWithObjectInternal, didHitHead, checkSnapJumpToObject, updateCollide,
    hitGround, hitGroundNoJump, flipGravity, ringJump, bumpPlayer, propellPlayer,
    boostPlayer, toggleFlyMode, toggleRollMode, toggleBirdMode, toggleDartMode,
    toggleRobotMode, toggleSpiderMode, toggleSwingMode, togglePlayerScale,
    spiderTestJumpInternal, playerIsFallingBugged,
    updateTimeMod,                  // 27 — appended; ids are part of the file format
    AttemptStart = 0xFFFF,
};

inline const char* fnName(uint16_t f) {
    static const char* const k[] = {
        "?", "update", "updateJump", "postCollision", "preSlopeCollision",
        "collidedWithSlopeInternal", "collidedWithObjectInternal", "didHitHead",
        "checkSnapJumpToObject", "updateCollide", "hitGround", "hitGroundNoJump",
        "flipGravity", "ringJump", "bumpPlayer", "propellPlayer", "boostPlayer",
        "toggleFlyMode", "toggleRollMode", "toggleBirdMode", "toggleDartMode",
        "toggleRobotMode", "toggleSpiderMode", "toggleSwingMode", "togglePlayerScale",
        "spiderTestJumpInternal", "playerIsFallingBugged", "updateTimeMod"};
    if (f == 0xFFFF) return "AttemptStart";
    return f < sizeof(k) / sizeof(k[0]) ? k[f] : "?";
}

struct TraceRecord {
    uint32_t step = 0;
    uint16_t fn = 0;
    uint8_t  depth = 0, player = 0;
    float    argF[2] = {0, 0};
    int32_t  argI[3] = {0, 0, 0};
    int32_t  objUid = 0, objId = 0, objType = -1;
    float    objX = 0, objY = 0, objW = 0, objH = 0, objRot = 0;
    int32_t  ret = -1;
    float    rect[4] = {0, 0, 0, 0};
    EngineSnap before, after;
    bool isAttemptMarker() const { return fn == (uint16_t)Fn::AttemptStart; }
};

// ── Writing ──────────────────────────────────────────────────────────────────────────
// The mod (src/engine_trace.cpp) writes through THESE functions, and readTrace() below
// reads the same layout — one definition of the format, so the two sides cannot drift.
constexpr uint32_t kTraceVersion = 1;

inline void writeHeader(std::FILE* f, int32_t levelId) {
    const char magic[4] = {'G', 'D', 'E', 'T'};
    const uint32_t hash = layoutHash();
    std::fwrite(magic, 1, 4, f);
    std::fwrite(&kTraceVersion, 4, 1, f);
    std::fwrite(&hash, 4, 1, f);
    std::fwrite(&levelId, 4, 1, f);
}

inline void writeAttemptMarker(std::FILE* f, uint32_t step) {
    const uint16_t fn = (uint16_t)Fn::AttemptStart;
    std::fwrite(&step, 4, 1, f);
    std::fwrite(&fn, 2, 1, f);
}

inline void writeRecord(std::FILE* f, const TraceRecord& r) {
    const float g[5] = {r.objX, r.objY, r.objW, r.objH, r.objRot};
    std::fwrite(&r.step, 4, 1, f);
    std::fwrite(&r.fn, 2, 1, f);
    std::fwrite(&r.depth, 1, 1, f);
    std::fwrite(&r.player, 1, 1, f);
    std::fwrite(r.argF, 4, 2, f);
    std::fwrite(r.argI, 4, 3, f);
    std::fwrite(&r.objUid, 4, 1, f);
    std::fwrite(&r.objId, 4, 1, f);
    std::fwrite(&r.objType, 4, 1, f);
    std::fwrite(g, 4, 5, f);
    std::fwrite(&r.ret, 4, 1, f);
    std::fwrite(r.rect, 4, 4, f);
    write(f, r.before);
    write(f, r.after);
}

struct TraceFile {
    uint32_t version = 0, layout = 0;
    int32_t  levelId = 0;
    std::vector<TraceRecord> records;
    std::string error;                  // non-empty = the file was rejected
};

inline TraceFile readTrace(const std::string& path) {
    TraceFile t;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { t.error = "cannot open " + path; return t; }
    char magic[4] = {};
    bool ok = std::fread(magic, 1, 4, f) == 4
           && std::fread(&t.version, 4, 1, f) == 1
           && std::fread(&t.layout, 4, 1, f) == 1
           && std::fread(&t.levelId, 4, 1, f) == 1;
    if (!ok || std::string(magic, 4) != "GDET") { t.error = "not an engine trace"; std::fclose(f); return t; }
    if (t.version != kTraceVersion) { t.error = "unsupported version " + std::to_string(t.version); std::fclose(f); return t; }
    if (t.layout != layoutHash()) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "field layout mismatch: file %08x, this build %08x — "
                      "the trace was recorded against a different SnapFields.inc", t.layout, layoutHash());
        t.error = buf; std::fclose(f); return t;
    }
    for (;;) {
        TraceRecord r;
        if (std::fread(&r.step, 4, 1, f) != 1) break;             // clean end of file
        if (std::fread(&r.fn, 2, 1, f) != 1) { t.error = "truncated record"; break; }
        if (r.isAttemptMarker()) { t.records.push_back(r); continue; }
        float g[5];
        ok = std::fread(&r.depth, 1, 1, f) == 1
          && std::fread(&r.player, 1, 1, f) == 1
          && std::fread(r.argF, 4, 2, f) == 2
          && std::fread(r.argI, 4, 3, f) == 3
          && std::fread(&r.objUid, 4, 1, f) == 1
          && std::fread(&r.objId, 4, 1, f) == 1
          && std::fread(&r.objType, 4, 1, f) == 1
          && std::fread(g, 4, 5, f) == 5
          && std::fread(&r.ret, 4, 1, f) == 1
          && std::fread(r.rect, 4, 4, f) == 4
          && read(f, r.before)
          && read(f, r.after);
        if (!ok) { t.error = "truncated record at step " + std::to_string(r.step)
                             + " (the game was probably closed mid-write)"; break; }
        r.objX = g[0]; r.objY = g[1]; r.objW = g[2]; r.objH = g[3]; r.objRot = g[4];
        t.records.push_back(r);
    }
    std::fclose(f);
    return t;
}

} // namespace gdsim::engine
