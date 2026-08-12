#include "replay_state.hpp"
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <cctype>
#include <utility>

using namespace geode::prelude;

ReplayPlayerState g_replayPlayer;
ReplayExternalCommands g_replayExternalCommands;
std::optional<std::filesystem::path> g_lastMatchedLocalReplayPath;
std::atomic<bool> g_autoRepairRequested{false};
std::atomic<bool> g_confirmRequested{false};
LastSolvedExport g_lastSolvedExport;
std::mutex g_runtimeHitboxSnapshotsMutex;
std::unordered_map<int, ReplayRuntimeHitboxSnapshot> g_runtimeHitboxSnapshots;
#ifdef GEODE_IS_WINDOWS
#endif

void storeRuntimeHitboxSnapshot(ReplayRuntimeHitboxSnapshot snapshot) {
    if (snapshot.levelId <= 0) return;
    std::scoped_lock lock(g_runtimeHitboxSnapshotsMutex);
    g_runtimeHitboxSnapshots[snapshot.levelId] = std::move(snapshot);
}

std::optional<ReplayRuntimeHitboxSnapshot> getRuntimeHitboxSnapshot(int levelId) {
    if (levelId <= 0) return std::nullopt;
    std::scoped_lock lock(g_runtimeHitboxSnapshotsMutex);
    auto it = g_runtimeHitboxSnapshots.find(levelId);
    if (it == g_runtimeHitboxSnapshots.end()) {
        return std::nullopt;
    }
    return it->second;
}

// ============================================================
// Debug File Logging
// ============================================================
static const char* DEBUG_LOG_FILE = "GDMod_audio_debug.log";
[[maybe_unused]] static constexpr bool ENABLE_ULTRA_AUDIO_FILE_DEBUG = false;

[[maybe_unused]] static void debugLogToFileImpl(const std::string& msg) {
    try {
        std::ofstream file(DEBUG_LOG_FILE, std::ios::app);
        if (file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            char timestr[64];
            std::tm tmUtc{};
            gmtime_s(&tmUtc, &time);
            std::strftime(timestr, sizeof(timestr), "%H:%M:%S", &tmUtc);
            file << fmt::format("{}.{:03d} {}\n", timestr, ms.count(), msg);
            file.flush();
        }
    } catch (...) {}
}

#define debugLogToFile(msg) do { if constexpr (ENABLE_ULTRA_AUDIO_FILE_DEBUG) debugLogToFileImpl((msg)); } while (0)

// ============================================================
// Supabase Configuration
// ============================================================

static constexpr auto SUPABASE_URL = "https://ebfuqojxpbcjggwtlweo.supabase.co";
static constexpr auto SUPABASE_ANON_KEY = "sb_publishable_z4myi8_798rpPnDZIzcLFw_4_nbJ-tY";

static web::WebRequest supabaseRequest() {
    return web::WebRequest()
        .header("apikey", SUPABASE_ANON_KEY)
        .header("Authorization", fmt::format("Bearer {}", SUPABASE_ANON_KEY))
        .header("Content-Type", "application/json");
}

// ============================================================
// GDR2 Binary Reader
// ============================================================

class GDR2Reader {
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;

public:
    GDR2Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool empty() const { return m_pos >= m_size; }
    size_t remaining() const { return m_size > m_pos ? m_size - m_pos : 0; }

    bool readRaw(void* out, size_t size) {
        if (remaining() < size) return false;
        std::memcpy(out, m_data + m_pos, size);
        m_pos += size;
        return true;
    }

    // LEB128 variable-length integer
    bool readVarint(uint64_t& result) {
        result = 0;
        for (size_t i = 0; i < 10; i++) {
            if (m_pos >= m_size) return false;
            uint8_t byte = m_data[m_pos++];
            result |= static_cast<uint64_t>(byte & 0x7F) << (i * 7);
            if ((byte & 0x80) == 0) return true;
        }
        return false;
    }

    bool readInt(int& result) {
        uint64_t v;
        if (!readVarint(v)) return false;
        result = static_cast<int>(v);
        return true;
    }

    bool readUint32(uint32_t& result) {
        uint64_t v;
        if (!readVarint(v)) return false;
        result = static_cast<uint32_t>(v);
        return true;
    }

    bool readSize(size_t& result) {
        uint64_t v;
        if (!readVarint(v)) return false;
        result = static_cast<size_t>(v);
        return true;
    }

    // Length-prefixed string (varint length + chars)
    bool readString(std::string& result) {
        size_t len;
        if (!readSize(len)) return false;
        if (len > 0xFFFF || remaining() < len) return false;
        result = std::string(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return true;
    }

    // 4 bytes big-endian float
    bool readFloat(float& result) {
        if (remaining() < 4) return false;
        uint8_t bytes[4];
        readRaw(bytes, 4);
        // reverse big-endian to little-endian
        std::reverse(bytes, bytes + 4);
        std::memcpy(&result, bytes, 4);
        return true;
    }

    // 8 bytes big-endian double
    bool readDouble(double& result) {
        if (remaining() < 8) return false;
        uint8_t bytes[8];
        readRaw(bytes, 8);
        std::reverse(bytes, bytes + 8);
        std::memcpy(&result, bytes, 8);
        return true;
    }

    // Bool stored as varint (0 or 1)
    bool readBool(bool& result) {
        uint64_t v;
        if (!readVarint(v)) return false;
        result = v != 0;
        return true;
    }

    bool skip(size_t n) {
        if (remaining() < n) return false;
        m_pos += n;
        return true;
    }
};

// ============================================================
// GDR2 Data Structures
// ============================================================

struct GDR2Input {
    uint64_t frame;
    uint8_t button; // 1=Jump, 2=Left, 3=Right
    bool player2;
    bool down;
};

struct GDR2Replay {
    int version;
    std::string inputTag;
    std::string author;
    std::string description;
    float duration;
    int gameVersion;
    double framerate;
    int seed;
    int coins;
    bool ldm;
    bool platformer;
    std::string botName;
    int botVersion;
    uint32_t levelId;
    std::string levelName;
    std::vector<uint64_t> deaths;
    std::vector<GDR2Input> inputs;
};

Result<GDR2Replay> parseGDR2(ByteVector& data);   // defined below

// ============================================================
// GDR (v1) Parser — MessagePack format (xdBot / GDReplayFormat)
// ============================================================
// The classic ".gdr" is a MessagePack-encoded map { author, bot{name,version}, framerate,
// gameVersion, level{id,name}, inputs:[{ "2p", "btn", "down", "frame" }], frameFixes:[...], … }.
// We reuse GDR2Replay so replayToJson stays the single conversion path. A minimal reader that
// can extract the fields we need and recursively SKIP everything else (the huge frameFixes
// array, extensions, …). All multi-byte msgpack values are big-endian.
namespace {
class MsgpackReader {
    const uint8_t* d; size_t n; size_t pos = 0;
public:
    MsgpackReader(const uint8_t* data, size_t size) : d(data), n(size) {}
    bool u8(uint8_t& v)  { if (pos >= n) return false; v = d[pos++]; return true; }
    bool peek(uint8_t& v){ if (pos >= n) return false; v = d[pos];   return true; }
    bool be(uint64_t& v, int bytes) { v = 0; for (int i = 0; i < bytes; i++) { if (pos >= n) return false; v = (v << 8) | d[pos++]; } return true; }
    bool skipBytes(uint64_t len) { if (pos + len > n) return false; pos += (size_t)len; return true; }
    bool readBytes(void* out, size_t len) { if (pos + len > n) return false; memcpy(out, d + pos, len); pos += len; return true; }

    bool readStr(std::string& s) {
        uint8_t b; if (!u8(b)) return false;
        uint64_t len;
        if (b >= 0xa0 && b <= 0xbf)      len = b & 0x1f;
        else if (b == 0xd9) { if (!be(len, 1)) return false; }
        else if (b == 0xda) { if (!be(len, 2)) return false; }
        else if (b == 0xdb) { if (!be(len, 4)) return false; }
        else return false;
        s.resize((size_t)len);
        return len == 0 || readBytes(&s[0], (size_t)len);
    }
    bool readMap(size_t& count) {
        uint8_t b; if (!u8(b)) return false; uint64_t c;
        if (b >= 0x80 && b <= 0x8f) { count = b & 0x0f; return true; }
        if (b == 0xde) { if (!be(c, 2)) return false; count = (size_t)c; return true; }
        if (b == 0xdf) { if (!be(c, 4)) return false; count = (size_t)c; return true; }
        return false;
    }
    bool readArray(size_t& count) {
        uint8_t b; if (!u8(b)) return false; uint64_t c;
        if (b >= 0x90 && b <= 0x9f) { count = b & 0x0f; return true; }
        if (b == 0xdc) { if (!be(c, 2)) return false; count = (size_t)c; return true; }
        if (b == 0xdd) { if (!be(c, 4)) return false; count = (size_t)c; return true; }
        return false;
    }
    bool readInt(int64_t& v) {
        uint8_t b; if (!u8(b)) return false;
        if (b <= 0x7f) { v = b; return true; }
        if (b >= 0xe0) { v = (int8_t)b; return true; }
        uint64_t u;
        switch (b) {
            case 0xcc: if (!be(u,1)) return false; v = (int64_t)u; return true;
            case 0xcd: if (!be(u,2)) return false; v = (int64_t)u; return true;
            case 0xce: if (!be(u,4)) return false; v = (int64_t)u; return true;
            case 0xcf: if (!be(u,8)) return false; v = (int64_t)u; return true;
            case 0xd0: if (!be(u,1)) return false; v = (int8_t)u;  return true;
            case 0xd1: if (!be(u,2)) return false; v = (int16_t)u; return true;
            case 0xd2: if (!be(u,4)) return false; v = (int32_t)u; return true;
            case 0xd3: if (!be(u,8)) return false; v = (int64_t)u; return true;
            case 0xca: { if (!be(u,4)) return false; uint32_t t=(uint32_t)u; float f; memcpy(&f,&t,4); v=(int64_t)std::llround(f); return true; }
            case 0xcb: { if (!be(u,8)) return false; double f; memcpy(&f,&u,8); v=(int64_t)std::llround(f); return true; }
            default: return false;
        }
    }
    bool readNum(double& v) {
        uint8_t b; if (!peek(b)) return false;
        if (b == 0xca) { u8(b); uint64_t u; if (!be(u,4)) return false; uint32_t t=(uint32_t)u; float f; memcpy(&f,&t,4); v=f; return true; }
        if (b == 0xcb) { u8(b); uint64_t u; if (!be(u,8)) return false; double f; memcpy(&f,&u,8); v=f; return true; }
        int64_t iv; if (!readInt(iv)) return false; v = (double)iv; return true;
    }
    bool readBool(bool& v) {
        uint8_t b; if (!u8(b)) return false;
        if (b == 0xc2) { v = false; return true; }
        if (b == 0xc3) { v = true;  return true; }
        if (b <= 0x7f) { v = (b != 0); return true; }   // tolerate int-as-bool
        return false;
    }
    // Recursively skip any single value (used for frameFixes / unknown keys).
    bool skip() {
        uint8_t b; if (!u8(b)) return false;
        if (b <= 0x7f || b >= 0xe0) return true;                     // fixint
        if (b >= 0x80 && b <= 0x8f) { size_t c=b&0x0f; for (size_t i=0;i<c*2;i++) if(!skip()) return false; return true; }
        if (b >= 0x90 && b <= 0x9f) { size_t c=b&0x0f; for (size_t i=0;i<c;  i++) if(!skip()) return false; return true; }
        if (b >= 0xa0 && b <= 0xbf) return skipBytes(b & 0x1f);      // fixstr
        uint64_t l;
        switch (b) {
            case 0xc0: case 0xc2: case 0xc3: return true;            // nil/false/true
            case 0xcc: case 0xd0: return skipBytes(1);
            case 0xcd: case 0xd1: return skipBytes(2);
            case 0xce: case 0xd2: case 0xca: return skipBytes(4);
            case 0xcf: case 0xd3: case 0xcb: return skipBytes(8);
            case 0xd9: if(!be(l,1))return false; return skipBytes(l);
            case 0xda: if(!be(l,2))return false; return skipBytes(l);
            case 0xdb: if(!be(l,4))return false; return skipBytes(l);
            case 0xc4: if(!be(l,1))return false; return skipBytes(l);
            case 0xc5: if(!be(l,2))return false; return skipBytes(l);
            case 0xc6: if(!be(l,4))return false; return skipBytes(l);
            case 0xdc: { if(!be(l,2))return false; for(uint64_t i=0;i<l;i++)   if(!skip())return false; return true; }
            case 0xdd: { if(!be(l,4))return false; for(uint64_t i=0;i<l;i++)   if(!skip())return false; return true; }
            case 0xde: { if(!be(l,2))return false; for(uint64_t i=0;i<l*2;i++) if(!skip())return false; return true; }
            case 0xdf: { if(!be(l,4))return false; for(uint64_t i=0;i<l*2;i++) if(!skip())return false; return true; }
            case 0xd4: return skipBytes(2);  case 0xd5: return skipBytes(3);
            case 0xd6: return skipBytes(5);  case 0xd7: return skipBytes(9);  case 0xd8: return skipBytes(17);
            case 0xc7: if(!be(l,1))return false; return skipBytes(l+1);
            case 0xc8: if(!be(l,2))return false; return skipBytes(l+1);
            case 0xc9: if(!be(l,4))return false; return skipBytes(l+1);
            default: return false;
        }
    }
};
} // namespace

Result<GDR2Replay> parseGDR(ByteVector& data) {
    MsgpackReader r(data.data(), data.size());
    GDR2Replay replay{};
    replay.version = 1;
    replay.framerate = 240.0;
    replay.gameVersion = 22;
    replay.platformer = false;

    size_t topCount;
    if (!r.readMap(topCount)) return Err("Not a GDR file (no MessagePack top-level map)");

    for (size_t i = 0; i < topCount; i++) {
        std::string key;
        if (!r.readStr(key)) return Err("Failed to read a GDR key");

        if      (key == "author")       { if (!r.readStr(replay.author))      return Err("author"); }
        else if (key == "description")  { if (!r.readStr(replay.description)) return Err("description"); }
        else if (key == "duration")     { double v; if (!r.readNum(v)) return Err("duration"); replay.duration = (float)v; }
        else if (key == "framerate" || key == "fps") { double v; if (!r.readNum(v)) return Err("framerate"); if (v > 0) replay.framerate = v; }
        else if (key == "gameVersion")  { double v; if (!r.readNum(v)) return Err("gameVersion"); replay.gameVersion = (v < 100) ? (int)std::llround(v * 10) : (int)v; }
        else if (key == "version")      { double v; if (!r.readNum(v)) return Err("version"); replay.version = (int)std::llround(v); }
        else if (key == "seed")         { int64_t v; if (!r.readInt(v)) return Err("seed"); replay.seed = (int)v; }
        else if (key == "coins")        { int64_t v; if (!r.readInt(v)) return Err("coins"); replay.coins = (int)v; }
        else if (key == "ldm")          { bool v; if (!r.readBool(v)) return Err("ldm"); replay.ldm = v; }
        else if (key == "platformer" || key == "botInfo") { bool v; if (!r.readBool(v)) return Err("platformer"); replay.platformer = v; }
        else if (key == "bot") {
            size_t bc; if (!r.readMap(bc)) return Err("bot");
            for (size_t j = 0; j < bc; j++) {
                std::string bk; if (!r.readStr(bk)) return Err("bot key");
                if (bk == "name") { if (!r.readStr(replay.botName)) return Err("bot.name"); }
                else if (!r.skip()) return Err("bot skip");   // version is a string in GDR v1 → skip
            }
        }
        else if (key == "level") {
            size_t lc; if (!r.readMap(lc)) return Err("level");
            for (size_t j = 0; j < lc; j++) {
                std::string lk; if (!r.readStr(lk)) return Err("level key");
                if (lk == "id")        { int64_t v; if (!r.readInt(v)) return Err("level.id"); replay.levelId = (uint32_t)v; }
                else if (lk == "name") { if (!r.readStr(replay.levelName)) return Err("level.name"); }
                else if (!r.skip()) return Err("level skip");
            }
        }
        else if (key == "inputs") {
            size_t ic; if (!r.readArray(ic)) return Err("inputs");
            replay.inputs.reserve(ic);
            for (size_t j = 0; j < ic; j++) {
                size_t mc; if (!r.readMap(mc)) return Err("input map");
                GDR2Input in{}; in.button = 1;
                for (size_t k = 0; k < mc; k++) {
                    std::string ik; if (!r.readStr(ik)) return Err("input key");
                    if      (ik == "frame")  { int64_t v; if (!r.readInt(v)) return Err("input.frame"); in.frame = (uint64_t)v; }
                    else if (ik == "btn" || ik == "button") { int64_t v; if (!r.readInt(v)) return Err("input.btn"); in.button = v == 0 ? 1 : (uint8_t)v; }
                    else if (ik == "down" || ik == "holding") { bool v; if (!r.readBool(v)) return Err("input.down"); in.down = v; }
                    else if (ik == "2p" || ik == "p2" || ik == "player2") { bool v; if (!r.readBool(v)) return Err("input.2p"); in.player2 = v; }
                    else if (!r.skip()) return Err("input skip");
                }
                replay.inputs.push_back(in);
            }
        }
        else { if (!r.skip()) return Err(fmt::format("Failed to skip GDR key '{}'", key)); }
    }

    if (replay.inputs.empty()) return Err("GDR file has no inputs");
    if (replay.botName.empty()) replay.botName = "GDR";

    std::sort(replay.inputs.begin(), replay.inputs.end(),
        [](const GDR2Input& a, const GDR2Input& b) { return a.frame < b.frame; });

    return Ok(std::move(replay));
}

// Auto-detect: ".gdr2" starts with the raw magic "GDR"; ".gdr" is MessagePack (a map header
// byte 0x80-0x8f / 0xde / 0xdf). Dispatch to the right parser so one path handles both.
Result<GDR2Replay> parseReplayAuto(ByteVector& data) {
    if (data.size() >= 3 && data[0] == 'G' && data[1] == 'D' && data[2] == 'R')
        return parseGDR2(data);
    return parseGDR(data);
}

// ============================================================
// GDR2 Parser
// ============================================================

Result<GDR2Replay> parseGDR2(ByteVector& data) {
    GDR2Reader reader(data.data(), data.size());
    GDR2Replay replay{};

    // Magic "GDR" (3 raw bytes)
    char magic[3];
    if (!reader.readRaw(magic, 3))
        return Err("Failed to read magic bytes");
    if (std::string_view(magic, 3) != "GDR")
        return Err("Not a GDR2 file (invalid magic header)");

    // Header
    if (!reader.readInt(replay.version)) return Err("Failed to read format version");
    if (!reader.readString(replay.inputTag)) return Err("Failed to read input tag");

    // Replay Metadata
    if (!reader.readString(replay.author)) return Err("Failed to read author");
    if (!reader.readString(replay.description)) return Err("Failed to read description");
    if (!reader.readFloat(replay.duration)) return Err("Failed to read duration");
    if (!reader.readInt(replay.gameVersion)) return Err("Failed to read game version");
    if (!reader.readDouble(replay.framerate)) return Err("Failed to read framerate");
    if (!reader.readInt(replay.seed)) return Err("Failed to read seed");
    if (!reader.readInt(replay.coins)) return Err("Failed to read coins");
    if (!reader.readBool(replay.ldm)) return Err("Failed to read LDM flag");
    if (!reader.readBool(replay.platformer)) return Err("Failed to read platformer flag");
    if (!reader.readString(replay.botName)) return Err("Failed to read bot name");
    if (!reader.readInt(replay.botVersion)) return Err("Failed to read bot version");
    if (!reader.readUint32(replay.levelId)) return Err("Failed to read level ID");
    if (!reader.readString(replay.levelName)) return Err("Failed to read level name");

    // Extension block (skip)
    size_t extSize;
    if (!reader.readSize(extSize)) return Err("Failed to read extension size");
    if (!reader.skip(extSize)) return Err("Failed to skip extension data");

    // Death frames (delta-encoded)
    size_t deathCount;
    if (!reader.readSize(deathCount)) return Err("Failed to read death count");
    uint64_t prevDeath = 0;
    for (size_t i = 0; i < deathCount; i++) {
        uint64_t delta;
        if (!reader.readVarint(delta)) return Err("Failed to read death frame delta");
        if (delta > std::numeric_limits<uint64_t>::max() - prevDeath) {
            return Err("Death frame delta overflows");
        }
        replay.deaths.push_back(delta + prevDeath);
        prevDeath += delta;
    }

    // Input data
    size_t inputCount;
    if (!reader.readSize(inputCount)) return Err("Failed to read input count");

    size_t p1InputCount;
    if (!reader.readSize(p1InputCount)) return Err("Failed to read P1 input count");
    if (p1InputCount > inputCount) return Err("P1 input count exceeds total input count");

    bool hasInputExt = !replay.inputTag.empty();

    uint64_t frame = 0;
    size_t p1Read = 0;

    for (size_t i = 0; i < inputCount && !reader.empty(); i++) {
        uint64_t packed;
        if (!reader.readVarint(packed)) return Err("Failed to read packed input");

        GDR2Input input{};

        if (replay.platformer) {
            // Platformer: [ ...delta | button(2) | down(1) ]
            input.down = packed & 1;
            input.button = (packed >> 1) & 3;
            uint64_t delta = packed >> 3;
            if (delta > std::numeric_limits<uint64_t>::max() - frame) {
                return Err("Platformer input frame delta overflows");
            }
            input.frame = delta + frame;
        } else {
            // Non-platformer: [ ...delta | down(1) ]
            input.down = packed & 1;
            input.button = 1; // default to Jump
            uint64_t delta = packed >> 1;
            if (delta > std::numeric_limits<uint64_t>::max() - frame) {
                return Err("Input frame delta overflows");
            }
            input.frame = delta + frame;
        }

        input.player2 = (p1Read >= p1InputCount);

        // Skip input extension data if present
        if (hasInputExt) {
            size_t inputExtSize;
            if (!reader.readSize(inputExtSize)) return Err("Failed to read input extension size");
            if (!reader.skip(inputExtSize)) return Err("Failed to skip input extension data");
        }

        replay.inputs.push_back(input);
        frame = input.frame;

        if (p1Read < p1InputCount) {
            p1Read++;
            if (p1Read == p1InputCount) {
                frame = 0; // reset delta for P2 inputs
            }
        }
    }

    // Sort inputs by frame (interleave P1 and P2)
    std::sort(replay.inputs.begin(), replay.inputs.end(),
        [](const GDR2Input& a, const GDR2Input& b) {
            return a.frame < b.frame;
        }
    );

    return Ok(std::move(replay));
}

static bool parseReplayEventsFromJsonArray(const std::vector<matjson::Value>& arr, ReplayLoadResult& result) {
    std::map<std::pair<int, int>, uint64_t> lastPress;

    for (auto const& inp : arr) {
        if (!inp.contains("action") || !inp.contains("frame")) continue;

        auto action = inp["action"].asString().unwrapOr("");
        auto frameRes = inp["frame"].asInt();
        if (frameRes.isErr()) continue;

        int64_t signedFrame = frameRes.unwrap();
        if (signedFrame < 0) continue;
        uint64_t frame = static_cast<uint64_t>(signedFrame);

        int player = static_cast<int>(inp["player"].asInt().unwrapOr(1));
        if (player != 1 && player != 2) continue;

        int buttonId = static_cast<int>(inp["button_id"].asInt().unwrapOr(1));
        if (buttonId < 1) continue;

        auto key = std::make_pair(player, buttonId);
        if (action == "press") {
            // A "press" while the button is ALREADY held is a redundant no-op in GD's
            // input model (you can't press twice without releasing) — the hold started
            // at the FIRST press and continues. GDR2 replays contain such duplicate
            // down=1 events (e.g. Steel/108166595: held 540->634 with a spurious
            // re-press at 593). Overwriting lastPress here shortened the hold to
            // 593->634, dropping 53 frames of holding — which desynced frame-perfect
            // wave sections and killed the run on Watch while the same replay cleared
            // in Eclipse (which plays the raw down/up events). Only start a new hold
            // when the button isn't already down.
            if (lastPress.find(key) == lastPress.end())
                lastPress[key] = frame;
        } else if (action == "release") {
            auto it = lastPress.find(key);
            if (it != lastPress.end()) {
                uint64_t pressFrame = it->second;
                uint64_t releaseFrame = std::max(frame, pressFrame + 1);
                result.presses.push_back({pressFrame, releaseFrame, player});
                lastPress.erase(it);
            }
        }
    }

    for (auto const& [key, frame] : lastPress) {
        result.presses.push_back({frame, frame + 1, key.first});
    }

    std::sort(result.presses.begin(), result.presses.end(), [](auto const& a, auto const& b) {
        if (a.framePress != b.framePress) return a.framePress < b.framePress;
        if (a.frameRelease != b.frameRelease) return a.frameRelease < b.frameRelease;
        return a.player < b.player;
    });

    return !result.presses.empty();
}

// ============================================================
// JSON Conversion
// ============================================================

static std::string buttonToString(uint8_t button) {
    switch (button) {
        case 1: return "Jump";
        case 2: return "Left";
        case 3: return "Right";
        default: return fmt::format("Unknown({})", button);
    }
}

static matjson::Value replayToJson(const GDR2Replay& replay) {
    matjson::Value json;

    json["format_version"] = replay.version;
    json["input_tag"] = replay.inputTag;
    json["author"] = replay.author;
    json["description"] = replay.description;
    json["duration_seconds"] = replay.duration;
    json["game_version"] = replay.gameVersion;
    json["framerate"] = replay.framerate;
    json["seed"] = replay.seed;
    json["coins"] = replay.coins;
    json["ldm"] = replay.ldm;
    json["platformer"] = replay.platformer;

    matjson::Value bot;
    bot["name"] = replay.botName;
    bot["version"] = replay.botVersion;
    json["bot"] = bot;

    matjson::Value level;
    level["id"] = static_cast<int64_t>(replay.levelId);
    level["name"] = replay.levelName;
    json["level"] = level;

    auto deathsArr = matjson::Value::array();
    for (auto d : replay.deaths) {
        deathsArr.push(matjson::Value(static_cast<int64_t>(d)));
    }
    json["deaths"] = deathsArr;

    auto inputsArr = matjson::Value::array();
    for (const auto& input : replay.inputs) {
        matjson::Value inp;
        inp["frame"] = static_cast<int64_t>(input.frame);
        inp["button"] = buttonToString(input.button);
        inp["button_id"] = input.button;
        inp["player"] = input.player2 ? 2 : 1;
        inp["action"] = input.down ? "press" : "release";
        inputsArr.push(inp);
    }
    json["inputs"] = inputsArr;
    json["input_count"] = static_cast<int64_t>(replay.inputs.size());
    json["death_count"] = static_cast<int64_t>(replay.deaths.size());

    return json;
}

// ============================================================
// GDR2 Writer
// ============================================================

class GDR2Writer {
    std::vector<uint8_t> m_data;
public:
    void writeRaw(const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        m_data.insert(m_data.end(), bytes, bytes + size);
    }

    void writeVarint(uint64_t value) {
        do {
            uint8_t byte = value & 0x7F;
            value >>= 7;
            if (value != 0) byte |= 0x80;
            m_data.push_back(byte);
        } while (value != 0);
    }

    void writeString(const std::string& s) {
        writeVarint(s.size());
        writeRaw(s.data(), s.size());
    }

    void writeFloat(float v) {
        uint8_t bytes[4];
        memcpy(bytes, &v, 4);
        std::reverse(bytes, bytes + 4);
        writeRaw(bytes, 4);
    }

    void writeDouble(double v) {
        uint8_t bytes[8];
        memcpy(bytes, &v, 8);
        std::reverse(bytes, bytes + 8);
        writeRaw(bytes, 8);
    }

    void writeBool(bool v) { writeVarint(v ? 1 : 0); }

    std::vector<uint8_t> take() { return std::move(m_data); }
};

std::vector<uint8_t> exportReplayToGdr2(const ReplayLoadResult& replay, int levelId, const std::string& levelName) {
    // Split presses into P1 and P2 events sorted by frame
    struct Event { uint64_t frame; bool down; bool player2; };
    std::vector<Event> p1Events, p2Events;
    for (auto& press : replay.presses) {
        bool isP2 = press.player == 2;
        auto& vec = isP2 ? p2Events : p1Events;
        vec.push_back({press.framePress,   true,  isP2});
        vec.push_back({press.frameRelease, false, isP2});
    }
    auto byFrame = [](const Event& a, const Event& b) { return a.frame < b.frame; };
    std::sort(p1Events.begin(), p1Events.end(), byFrame);
    std::sort(p2Events.begin(), p2Events.end(), byFrame);

    GDR2Writer w;

    // Magic header
    w.writeRaw("GDR", 3);

    // Format version 2
    w.writeVarint(2);

    // Input tag (empty = non-platformer)
    w.writeString("");

    // Metadata
    w.writeString("");                           // author
    w.writeString("");                           // description
    w.writeFloat(0.0f);                          // duration
    w.writeVarint(22);                           // GD game version 2.2
    w.writeDouble(replay.framerate);
    w.writeVarint(0);                            // seed
    w.writeVarint(0);                            // coins
    w.writeBool(false);                          // ldm
    w.writeBool(false);                          // platformer
    w.writeString("kusmic.pathfinder");          // bot name
    w.writeVarint(1);                            // bot version
    w.writeVarint(static_cast<uint64_t>(levelId));
    w.writeString(levelName);

    // Extension block (empty)
    w.writeVarint(0);

    // Deaths (none)
    w.writeVarint(0);

    // Input counts
    w.writeVarint(p1Events.size() + p2Events.size());
    w.writeVarint(p1Events.size());

    // P1 events: delta-encoded from 0
    uint64_t prevFrame = 0;
    for (auto& ev : p1Events) {
        uint64_t delta = ev.frame - prevFrame;
        w.writeVarint((delta << 1) | (ev.down ? 1 : 0));
        prevFrame = ev.frame;
    }

    // P2 events: delta-encoded from 0 (separate counter)
    prevFrame = 0;
    for (auto& ev : p2Events) {
        uint64_t delta = ev.frame - prevFrame;
        w.writeVarint((delta << 1) | (ev.down ? 1 : 0));
        prevFrame = ev.frame;
    }

    return w.take();
}

std::string writeSolvedGdr2(int levelId, const std::string& levelName, const ReplayLoadResult& replay) {
    auto gdr2Data = exportReplayToGdr2(replay, levelId, levelName);

    std::string safeName = levelName;
    for (auto& c : safeName)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (safeName.empty()) safeName = "replay";
    auto fileName = fmt::format("{}-{}.gdr2", safeName, levelId);

    auto writeTo = [&](const std::filesystem::path& dir) -> bool {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream ofs(dir / fileName, std::ios::binary);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(gdr2Data.data()), gdr2Data.size());
        return ofs.good();
    };

    std::string primary;
    // Eclipse replays folder (directly playable in-game) if it exists.
    auto eclipseDir = dirs::getGameDir() / "geode" / "config" / "prevter.eclipsemenu" / "replays";
    if (std::filesystem::exists(eclipseDir) && writeTo(eclipseDir))
        primary = (eclipseDir / fileName).string();
    // Always keep a copy in the mod save dir.
    auto modDir = Mod::get()->getSaveDir() / "gdr2";
    if (writeTo(modDir) && primary.empty())
        primary = (modDir / fileName).string();
    return primary;
}

bool writeGdr2ToExactPath(const std::filesystem::path& path, int levelId,
                          const std::string& levelName, const ReplayLoadResult& replay) {
    auto gdr2Data = exportReplayToGdr2(replay, levelId, levelName);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(gdr2Data.data()), gdr2Data.size());
    return ofs.good();
}

// ============================================================
// Conversion Logic
// ============================================================

void convertGdr2File(const std::filesystem::path& path) {
    auto readRes = file::readBinary(path);
    if (readRes.isErr()) {
        FLAlertLayer::create(
            "Error",
            fmt::format("Failed to read file:\n{}", readRes.unwrapErr()),
            "OK"
        )->show();
        return;
    }

    auto data = std::move(readRes.unwrap());
    auto parseRes = parseReplayAuto(data);   // handles both .gdr2 (binary) and .gdr (MessagePack)
    if (parseRes.isErr()) {
        FLAlertLayer::create(
            "Error",
            fmt::format("Replay parsing error:\n{}", parseRes.unwrapErr()),
            "OK"
        )->show();
        return;
    }

    auto& replay = parseRes.unwrap();
    auto json = replayToJson(replay);

    // Save to mod's own "replay" folder as "levelName-levelId.json"
    auto replayDir = Mod::get()->getSaveDir() / "replay";
    std::filesystem::create_directories(replayDir);

    // Sanitize level name for filename (replace invalid chars with _)
    std::string safeName = replay.levelName;
    for (auto& c : safeName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    if (safeName.empty()) safeName = "unknown";

    auto jsonFilename = fmt::format("{}-{}.json", safeName, replay.levelId);
    auto jsonPath = replayDir / jsonFilename;

    auto jsonStr = json.dump(4);

    auto writeRes = file::writeString(jsonPath, jsonStr);
    if (writeRes.isErr()) {
        FLAlertLayer::create(
            "Error",
            fmt::format("Failed to write JSON:\n{}", writeRes.unwrapErr()),
            "OK"
        )->show();
        return;
    }

    FLAlertLayer::create(
        "Success!",
        fmt::format(
            "<cg>Converted to JSON!</c>\n"
            "Level: <cy>{}</c> (ID: {})\n"
            "Bot: <cl>{}</c> v{}\n"
            "Inputs: <co>{}</c> | FPS: <co>{:.0f}</c>\n"
            "File: <cj>{}</c>",
            replay.levelName.empty() ? "(unknown)" : replay.levelName,
            replay.levelId,
            replay.botName.empty() ? "(unknown)" : replay.botName,
            replay.botVersion,
            replay.inputs.size(),
            replay.framerate,
            jsonFilename
        ),
        "OK"
    )->show();

    // Upload to Supabase in the background
    matjson::Value row;
    row["level_id"] = static_cast<int64_t>(replay.levelId);
    row["level_name"] = replay.levelName;
    row["framerate"] = replay.framerate;
    row["replay_data"] = json;

    auto url = fmt::format("{}/rest/v1/replays", SUPABASE_URL);
    auto resp = supabaseRequest()
        .header("Prefer", "return=minimal")
        .bodyJSON(row)
        .postSync(url);
    if (resp.ok()) {
        log::info("[Supabase] Upload OK for level {} ({})", replay.levelName, replay.levelId);
    } else {
        auto body = resp.string().unwrapOr("(no body)");
        log::warn("[Supabase] Upload FAILED code={} body={}", resp.code(), body);
    }
}


// ============================================================
// PlayLayer Hook – Show input circles in-game
// ============================================================

// Struct to hold a press input's frame number

// Try to find a matching replay for the given level ID
// 1. Search locally by filename pattern: *-{levelId}.json
// 2. If not found locally, query Supabase
std::optional<ReplayLoadResult> loadMatchingReplay(int levelId) {
    g_lastMatchedLocalReplayPath.reset();

    // --- Local search first ---
    auto replaysDir = Mod::get()->getSaveDir() / "replay";
    if (std::filesystem::exists(replaysDir)) {
        auto suffix = fmt::format("-{}.json", levelId);
        std::filesystem::path matchedFile;
        std::filesystem::file_time_type matchedTime{};
        bool hasMatch = false;

        for (auto& entry : std::filesystem::directory_iterator(replaysDir)) {
            if (!entry.is_regular_file()) continue;
            auto filename = entry.path().filename().string();
            if (filename.size() <= suffix.size()) continue;
            if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
                auto currentTime = entry.last_write_time();
                if (!hasMatch || currentTime > matchedTime) {
                    matchedFile = entry.path();
                    matchedTime = currentTime;
                    hasMatch = true;
                }
            }
        }

        if (hasMatch) {
            auto readRes = file::readString(matchedFile);
            if (readRes.isOk()) {
                auto parseRes = matjson::parse(readRes.unwrap());
                if (parseRes.isOk()) {
                    auto parsed = std::move(parseRes.unwrap());
                    ReplayLoadResult result;
                    result.framerate = parsed["framerate"].asDouble().unwrapOr(240.0);
                    if (parsed.contains("inputs") && parsed["inputs"].isArray()) {
                        auto arrRes = parsed["inputs"].asArray();
                        if (arrRes.isOk()) {
                            auto& arr = arrRes.unwrap();
                            if (parseReplayEventsFromJsonArray(arr, result)) {
                                g_lastMatchedLocalReplayPath = matchedFile;
                                log::info("[InputCircles] Found local replay for level {}", levelId);
                                return result;
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Supabase fallback ---
    log::info("[Supabase] No local replay, querying Supabase for level_id={}", levelId);
    auto url = fmt::format(
        "{}/rest/v1/replays?level_id=eq.{}&select=replay_data,framerate&limit=1",
        SUPABASE_URL, levelId
    );
    auto response = supabaseRequest().getSync(url);
    if (!response.ok()) {
        log::warn("[Supabase] Request failed: code={}", response.code());
        return std::nullopt;
    }
    auto jsonRes = response.json();
    if (jsonRes.isErr()) {
        log::warn("[Supabase] Failed to parse response JSON");
        return std::nullopt;
    }
    auto respArr = std::move(jsonRes.unwrap());
    if (!respArr.isArray()) return std::nullopt;
    auto arrRes2 = respArr.asArray();
    if (arrRes2.isErr() || arrRes2.unwrap().empty()) {
        log::info("[Supabase] No replay found for level {}", levelId);
        return std::nullopt;
    }
    auto& row = arrRes2.unwrap()[0];
    if (!row.contains("replay_data")) return std::nullopt;
    auto& replayData = row["replay_data"];

    ReplayLoadResult result;
    result.framerate = row["framerate"].asDouble().unwrapOr(240.0);

    if (!replayData.contains("inputs") || !replayData["inputs"].isArray()) return std::nullopt;
    auto arrRes3 = replayData["inputs"].asArray();
    if (arrRes3.isErr()) return std::nullopt;
    auto& arr3 = arrRes3.unwrap();
    if (!parseReplayEventsFromJsonArray(arr3, result)) return std::nullopt;
    if (result.presses.empty()) return std::nullopt;
    log::info("[Supabase] Loaded {} presses from Supabase for level {}", result.presses.size(), levelId);
    return result;
}

bool saveReplayToLocalJson(int levelId, const ReplayLoadResult& replay, std::filesystem::path preferredPath) {
    if (replay.presses.empty()) {
        return false;
    }

    auto replaysDir = Mod::get()->getSaveDir() / "replay";
    std::error_code ec;
    std::filesystem::create_directories(replaysDir, ec);

    std::filesystem::path outPath = preferredPath;
    if (outPath.empty()) {
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        outPath = replaysDir / fmt::format("edited-{}-{}.json", ts, levelId);
    }

    matjson::Value root;
    root["framerate"] = replay.framerate;
    root["level_id"] = levelId;

    auto inputs = matjson::Value::array();
    for (auto const& press : replay.presses) {
        matjson::Value pressEv;
        pressEv["action"] = "press";
        pressEv["frame"] = static_cast<int64_t>(press.framePress);
        pressEv["player"] = static_cast<int64_t>(press.player);
        pressEv["button_id"] = static_cast<int64_t>(1);
        inputs.push(pressEv);

        matjson::Value releaseEv;
        releaseEv["action"] = "release";
        releaseEv["frame"] = static_cast<int64_t>(press.frameRelease);
        releaseEv["player"] = static_cast<int64_t>(press.player);
        releaseEv["button_id"] = static_cast<int64_t>(1);
        inputs.push(releaseEv);
    }
    root["inputs"] = inputs;

    auto writeRes = file::writeString(outPath, root.dump(2));
    if (writeRes.isErr()) {
        log::warn("[ReplayEditor] Failed to save replay {}", outPath.string());
        return false;
    }
    g_lastMatchedLocalReplayPath = outPath;
    log::info("[ReplayEditor] Saved {} inputs to {}", replay.presses.size(), outPath.string());
    return true;
}

bool deleteMatchingReplay(int levelId) {
    auto replaysDir = Mod::get()->getSaveDir() / "replay";
    if (!std::filesystem::exists(replaysDir)) {
        return false;
    }

    auto suffix = fmt::format("-{}.json", levelId);
    std::filesystem::path matchedFile;
    std::filesystem::file_time_type matchedTime{};
    bool hasMatch = false;

    for (auto& entry : std::filesystem::directory_iterator(replaysDir)) {
        if (!entry.is_regular_file()) continue;
        auto filename = entry.path().filename().string();
        if (filename.size() <= suffix.size()) continue;
        if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            auto currentTime = entry.last_write_time();
            if (!hasMatch || currentTime > matchedTime) {
                matchedFile = entry.path();
                matchedTime = currentTime;
                hasMatch = true;
            }
        }
    }

    if (!hasMatch) {
        return false;
    }

    std::error_code ec;
    bool removed = std::filesystem::remove(matchedFile, ec);
    if (!removed || ec) {
        return false;
    }

    if (g_lastMatchedLocalReplayPath && *g_lastMatchedLocalReplayPath == matchedFile) {
        g_lastMatchedLocalReplayPath.reset();
    }

    log::info("[ReplayEditor] Deleted replay {}", matchedFile.string());
    return true;
}

// ============================================================

