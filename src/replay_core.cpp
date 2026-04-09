#include "replay_state.hpp"
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

ReplayPlayerState g_replayPlayer;
ReplayExternalCommands g_replayExternalCommands;
std::optional<std::filesystem::path> g_lastMatchedLocalReplayPath;
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
    auto parseRes = parseGDR2(data);
    if (parseRes.isErr()) {
        FLAlertLayer::create(
            "Error",
            fmt::format("GDR2 parsing error:\n{}", parseRes.unwrapErr()),
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

// ============================================================

