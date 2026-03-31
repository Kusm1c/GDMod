#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/Slider.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <optional>
#include <map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <limits>

#ifdef GEODE_IS_WINDOWS
#include <commdlg.h>
#include <shlobj.h>
#endif

using namespace geode::prelude;

// ============================================================
// Replay Player State
// ============================================================

// Struct to hold a press input's frame number
struct ReplayPress {
    uint64_t framePress;
    uint64_t frameRelease;
    int player; // 1 or 2
};

// Result of loading a matching replay: presses + framerate
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

static ReplayPlayerState g_replayPlayer;
static std::optional<std::filesystem::path> g_lastMatchedLocalReplayPath;

// ============================================================
// Debug File Logging
// ============================================================
static const char* DEBUG_LOG_FILE = "GDMod_audio_debug.log";
static constexpr bool ENABLE_ULTRA_AUDIO_FILE_DEBUG = false;

static void debugLogToFile(const std::string& msg) {
    if (!ENABLE_ULTRA_AUDIO_FILE_DEBUG) return;
    try {
        std::ofstream file(DEBUG_LOG_FILE, std::ios::app);
        if (file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            char timestr[64];
            std::strftime(timestr, sizeof(timestr), "%H:%M:%S", std::gmtime(&time));
            file << fmt::format("{}.{:03d} {}\n", timestr, ms.count(), msg);
            file.flush();
        }
    } catch (...) {}
}

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
// Forward declaration: implemented later
static std::optional<ReplayLoadResult> loadMatchingReplay(int levelId);
static bool saveReplayToLocalJson(int levelId, const ReplayLoadResult& replay, std::filesystem::path preferredPath = {});


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
        replay.deaths.push_back(delta + prevDeath);
        prevDeath += delta;
    }

    // Input data
    size_t inputCount;
    if (!reader.readSize(inputCount)) return Err("Failed to read input count");

    size_t p1InputCount;
    if (!reader.readSize(p1InputCount)) return Err("Failed to read P1 input count");

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
            input.frame = (packed >> 3) + frame;
        } else {
            // Non-platformer: [ ...delta | down(1) ]
            input.down = packed & 1;
            input.button = 1; // default to Jump
            input.frame = (packed >> 1) + frame;
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

static void convertFile(const std::filesystem::path& path) {
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
// MenuLayer Hook
// ============================================================

static bool isDevMode() {
    auto c = Mod::get()->getSettingValue<ccColor3B>("accent-color");
    return c.r == 21 && c.g == 3 && c.b == 20;
}

class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        if (!isDevMode()) return true;

        // Create button with a sprite
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto label = CCLabelBMFont::create("GDR2\n->JSON", "bigFont.fnt");
        label->setScale(0.25f);
        label->setPosition(btnSprite->getContentSize() / 2);
        btnSprite->addChild(label);

        auto myButton = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(MyMenuLayer::onConvertGDR2)
        );

        auto menu = this->getChildByID("bottom-menu");
        menu->addChild(myButton);
        myButton->setID("gdr2-to-json-btn"_spr);
        menu->updateLayout();

        return true;
    }

    void onConvertGDR2(CCObject*) {
#ifdef GEODE_IS_WINDOWS
        // Use Win32 native file dialog (synchronous, no arc dependency)
        OPENFILENAMEW ofn{};
        wchar_t szFile[MAX_PATH] = {0};

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"GDR2 Replay Files (*.gdr2)\0*.gdr2\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        // Set default directory to Eclipse replays folder
        auto defaultDir = dirs::getGameDir() / "geode" / "config" / "prevter.eclipsemenu" / "replays";
        if (!std::filesystem::exists(defaultDir)) {
            defaultDir = dirs::getGameDir();
        }
        auto defaultDirStr = defaultDir.wstring();
        ofn.lpstrInitialDir = defaultDirStr.c_str();

        if (GetOpenFileNameW(&ofn)) {
            auto path = std::filesystem::path(szFile);
            convertFile(path);
        }
#else
        FLAlertLayer::create("Error", "File picker is only available on Windows.", "OK")->show();
#endif
    }
};

// ============================================================
// LevelInfoLayer Hook – Add Watch Replay Button
// ============================================================

class $modify(MyLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool selectLevel) {
        if (!LevelInfoLayer::init(level, selectLevel)) return false;

        // Add Watch Replay button
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto label = CCLabelBMFont::create("Watch\nReplay", "bigFont.fnt");
        label->setScale(0.3f);
        label->setPosition(btnSprite->getContentSize() / 2);
        btnSprite->addChild(label);

        auto watchButton = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(MyLevelInfoLayer::onWatchReplay)
        );
        watchButton->setID("watch-replay-btn"_spr);

        auto editBtnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto editLabel = CCLabelBMFont::create("Edit\nReplay", "bigFont.fnt");
        editLabel->setScale(0.28f);
        editLabel->setPosition(editBtnSprite->getContentSize() / 2);
        editBtnSprite->addChild(editLabel);

        auto editButton = CCMenuItemSpriteExtra::create(
            editBtnSprite,
            this,
            menu_selector(MyLevelInfoLayer::onEditReplay)
        );
        editButton->setID("edit-replay-btn"_spr);

        // Reliable placement independent of internal LevelInfoLayer menu IDs.
        auto replayMenu = CCMenu::create();
        replayMenu->setID("watch-replay-menu"_spr);
        replayMenu->setPosition(CCPointZero);
        this->addChild(replayMenu, 200);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        watchButton->setPosition({winSize.width - 64.0f, 86.0f});
        editButton->setPosition({winSize.width - 154.0f, 86.0f});
        replayMenu->addChild(watchButton);
        replayMenu->addChild(editButton);

        return true;
    }

    void onWatchReplay(CCObject*) {
        auto level = m_level;
        if (!level) return;

        // Set replay player state
        int levelId = level->m_levelID;
        auto replayResult = loadMatchingReplay(levelId);
        if (!replayResult.has_value()) {
            FLAlertLayer::create("Error", "No replay found for this level.", "OK")->show();
            return;
        }

        g_replayPlayer.isActive = true;
        g_replayPlayer.replay = std::move(replayResult.value());
        g_replayPlayer.playbackSpeed = 1.0f;
        g_replayPlayer.isPaused = false;
        g_replayPlayer.pauseTime = 0.0f;
        g_replayPlayer.inputIndex = 0.0f;
        g_replayPlayer.isEditMode = false;
        g_replayPlayer.levelId = levelId;
        g_replayPlayer.sourceReplayPath = g_lastMatchedLocalReplayPath.value_or(std::filesystem::path{});

        // Launch through LevelInfoLayer flow so scene transition is valid.
        this->onPlay(nullptr);
    }

    void onEditReplay(CCObject*) {
        auto level = m_level;
        if (!level) return;

        int levelId = level->m_levelID;
        auto replayResult = loadMatchingReplay(levelId);
        if (!replayResult.has_value()) {
            FLAlertLayer::create("Error", "No replay found for this level.", "OK")->show();
            return;
        }

        g_replayPlayer.isActive = true;
        g_replayPlayer.replay = std::move(replayResult.value());
        g_replayPlayer.playbackSpeed = 1.0f;
        g_replayPlayer.isPaused = false;
        g_replayPlayer.pauseTime = 0.0f;
        g_replayPlayer.inputIndex = 0.0f;
        g_replayPlayer.isEditMode = true;
        g_replayPlayer.levelId = levelId;
        g_replayPlayer.sourceReplayPath = g_lastMatchedLocalReplayPath.value_or(std::filesystem::path{});

        // Launch through LevelInfoLayer flow so scene transition is valid.
        this->onPlay(nullptr);
    }
};

// ============================================================
// PlayLayer Hook – Show input circles in-game
// ============================================================

// Struct to hold a press input's frame number

// Try to find a matching replay for the given level ID
// 1. Search locally by filename pattern: *-{levelId}.json
// 2. If not found locally, query Supabase
static std::optional<ReplayLoadResult> loadMatchingReplay(int levelId) {
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
                            std::map<std::pair<int,int>, uint64_t> lastPress;
                            for (auto& inp : arr) {
                                if (!inp.contains("action")) continue;
                                auto action = inp["action"].asString().unwrapOr("");
                                uint64_t frame = static_cast<uint64_t>(inp["frame"].asInt().unwrapOr(0));
                                int player = static_cast<int>(inp["player"].asInt().unwrapOr(1));
                                int buttonId = static_cast<int>(inp["button_id"].asInt().unwrapOr(1));
                                auto key = std::make_pair(player, buttonId);
                                if (action == "press") {
                                    lastPress[key] = frame;
                                } else if (action == "release") {
                                    auto it = lastPress.find(key);
                                    if (it != lastPress.end()) {
                                        result.presses.push_back({it->second, frame, player});
                                        lastPress.erase(it);
                                    }
                                }
                            }
                            for (auto& [key, frame] : lastPress) {
                                result.presses.push_back({frame, frame + 1, key.first});
                            }
                            if (!result.presses.empty()) {
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
    std::map<std::pair<int,int>, uint64_t> lastPress;
    for (auto& inp : arr3) {
        if (!inp.contains("action")) continue;
        auto action = inp["action"].asString().unwrapOr("");
        uint64_t frame = static_cast<uint64_t>(inp["frame"].asInt().unwrapOr(0));
        int player = static_cast<int>(inp["player"].asInt().unwrapOr(1));
        int buttonId = static_cast<int>(inp["button_id"].asInt().unwrapOr(1));
        auto key = std::make_pair(player, buttonId);
        if (action == "press") {
            lastPress[key] = frame;
        } else if (action == "release") {
            auto it = lastPress.find(key);
            if (it != lastPress.end()) {
                result.presses.push_back({it->second, frame, player});
                lastPress.erase(it);
            }
        }
    }
    for (auto& [key, frame] : lastPress) {
        result.presses.push_back({frame, frame + 1, key.first});
    }
    if (result.presses.empty()) return std::nullopt;
    log::info("[Supabase] Loaded {} presses from Supabase for level {}", result.presses.size(), levelId);
    return result;
}

static bool saveReplayToLocalJson(int levelId, const ReplayLoadResult& replay, std::filesystem::path preferredPath) {
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
// Rhythm Game – Judgment & UR Bar Types
// ============================================================

enum class Judgment {
    VeryEarly,
    Early,
    Good,
    Late,
    VeryLate,
    Miss
};

struct URTick {
    float offsetFrames; // negative = early, positive = late
    Judgment judgment;
    float timeCreated;
};

// Static bridge: captures player inputs from GJBaseGameLayer::handleButton
static std::vector<std::pair<float, bool>> s_playerInputs; // (time, isPress)
static bool s_rhythmActive = false;

class $modify(MyGJBGL, GJBaseGameLayer) {
    void handleButton(bool push, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(push, button, isPlayer1);
        if (s_rhythmActive && isPlayer1 && button == 1) {
            float time = static_cast<float>(m_gameState.m_currentProgress);
            // Use m_timePlayed from the game layer for accurate timing
            auto pl = PlayLayer::get();
            if (pl) time = static_cast<float>(pl->m_timePlayed);
            s_playerInputs.push_back({time, push});
        }
    }
};

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        std::vector<ReplayPress> m_presses;
        CCNode* m_circleLayer = nullptr;
        CCNode* m_indicator = nullptr; // hollow square following the player
        double m_framerate = 240.0;
        bool m_active = false;
        bool m_dotsCreated = false;
        // Settings
        int m_barHeight = 10;
        int m_barY = 15;
        ccColor4B m_p1Color = ccc4(50, 255, 80, 200);
        ccColor4B m_p2Color = ccc4(80, 130, 255, 200);
        ccColor4B m_indicatorColor = ccc4(255, 255, 255, 220);
        ccColor4B m_bgColor = ccc4(0, 0, 0, 120);
        CCLayerColor* m_bgStrip = nullptr;
        // Position history: (time, playerWorldX) samples for accurate interpolation
        // across speed changes (portals)
        std::vector<std::pair<float, float>> m_posHistory;
        // Dot nodes (one per press): (pressIndex, dotNode)
        std::vector<std::pair<size_t, CCNode*>> m_dots;

        // ========== UR Bar / Rhythm Game ==========
        CCNode* m_urBar = nullptr;
        CCLabelBMFont* m_accuracyLabel = nullptr;
        std::vector<CCNode*> m_urTickNodes;
        std::vector<URTick> m_urTicks;
        size_t m_processedInputIdx = 0;
        std::vector<bool> m_replayPressUsed;
        std::vector<bool> m_replayReleaseUsed;
        int m_judgmentCounts[6] = {0, 0, 0, 0, 0, 0};
        float m_totalAccuracy = 0.0f;
        int m_totalJudgments = 0;
        // UR bar settings
        float m_urBarWidth = 300.0f;
        float m_urBarHeight = 12.0f;
        float m_goodFrames = 2.0f;
        float m_earlyLateFrames = 5.0f;
        float m_veryFrames = 8.0f;
        // Replay press times (in seconds) for quick matching
        std::vector<float> m_replayPressTimes;
        std::vector<float> m_replayReleaseTimes;

        // ========== Replay Player Mode ==========
        bool m_isReplayMode = false;
        float m_replayCurrentTime = 0.0f;
        size_t m_nextInputIdx = 0;
        bool m_isPaused = false;
        float m_pauseTime = 0.0f;
        bool m_isSeeking = false;
        float m_seekTargetTime = 0.0f;
        float m_seekResumeSpeed = 1.0f;
        bool m_replayHoldingJump = false;
        CCNode* m_videoControlsLayer = nullptr;
        CCLabelBMFont* m_timeLabel = nullptr;
        CCLabelBMFont* m_speedLabel = nullptr;
        CCLabelBMFont* m_playPauseLabel = nullptr;
        float m_lastAudioSyncMs = -1.0f;
        float m_lastAudioRate = 1.0f;
        bool m_audioPausedByReplay = false;
        int m_audioMusicID = -1;
        bool m_musicBootstrapTried = false;
        float m_nextMusicRetryAt = 0.0f;
        float m_nextAudioDebugAt = 0.0f;
        float m_lastAutoSeekAt = -1000.0f;
        bool m_forcedPracticeMusic = false;
        bool m_prevPracticeMusicSync = false;
        int m_replayMusicInitialOffset = -1;  // Captured offset of music at first sync

        // ========== Replay Edit Mode ==========
        bool m_isReplayEditMode = false;
        size_t m_editSelectedIdx = 0;
        CCNode* m_editPanel = nullptr;
        CCNode* m_editInputsPanel = nullptr;
        CCMenu* m_editInputsMenu = nullptr;
        CCLabelBMFont* m_editTitleLabel = nullptr;
        CCLabelBMFont* m_editPressLabel = nullptr;
        CCLabelBMFont* m_editReleaseLabel = nullptr;
        CCLabelBMFont* m_editHintLabel = nullptr;
        Slider* m_editPressSlider = nullptr;
        Slider* m_editReleaseSlider = nullptr;
        bool m_editUpdatingSliders = false;
        bool m_editPreviewPending = false;
        float m_lastEditPreviewAt = -1000.0f;
        CCNode* m_editTimeline = nullptr;
        CCMenu* m_editTimelineMenu = nullptr;
        CCLayerColor* m_editTimelineCursor = nullptr;
        CCLabelBMFont* m_editTimelineLabel = nullptr;
        float m_editTimelineWindowStart = 0.0f;
        float m_editTimelineWindowEnd = 0.0f;
        float m_lastTimelineCenterTime = -1000.0f;
        size_t m_lastTimelineSelectedIdx = std::numeric_limits<size_t>::max();
    };

    void applyReplayAudioSync(float levelTime, bool forceSeek) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode) return;

        debugLogToFile(fmt::format("=== applyReplayAudioSync START levelTime={:.3f} forceSeek={} ===", levelTime, forceSeek));

        auto* audio = FMODAudioEngine::sharedEngine();
        if (!audio) {
            debugLogToFile("ERROR: audio engine null");
            return;
        }
        debugLogToFile(fmt::format("audio engine OK"));

        auto isMusicSlotValid = [&](int musicID, int channelID) -> bool {
            bool valid = (channelID != 0) || audio->isMusicPlaying(musicID);
            debugLogToFile(fmt::format("  isMusicSlotValid musicID={} channelID={} playing={} -> {}", musicID, channelID, audio->isMusicPlaying(musicID), valid));
            return valid;
        };

        auto resolveReplayMusic = [&]() -> int {
            debugLogToFile(fmt::format("  resolveReplayMusic START m_audioMusicID={}", fields->m_audioMusicID));
            if (fields->m_audioMusicID >= 0) {
                int ch = audio->getMusicChannelID(fields->m_audioMusicID);
                debugLogToFile(fmt::format("    checking cached musicID={} ch={}", fields->m_audioMusicID, ch));
                if (isMusicSlotValid(fields->m_audioMusicID, ch) || fields->m_audioPausedByReplay) {
                    debugLogToFile(fmt::format("    cached valid, returning ch={}", ch));
                    return ch;
                }
            }

            debugLogToFile("  scanning all music slots...");
            for (int musicID = 0; musicID < 32; ++musicID) {
                int ch = audio->getMusicChannelID(musicID);
                debugLogToFile(fmt::format("    slot musicID={} ch={} valid={}", musicID, ch, isMusicSlotValid(musicID, ch)));
                if (isMusicSlotValid(musicID, ch)) {
                    fields->m_audioMusicID = musicID;
                    debugLogToFile(fmt::format("    FOUND musicID={} ch={}", musicID, ch));
                    return ch;
                }
            }

            fields->m_audioMusicID = -1;
            debugLogToFile("  NO MUSIC SLOT FOUND");
            return -1;
        };

        int channelID = resolveReplayMusic();
        debugLogToFile(fmt::format("resolved channelID={}", channelID));

        // FIRST time we find the channel, capture the music's initial offset and resume
        if (fields->m_audioMusicID >= 0 && channelID >= 0 && fields->m_lastAudioSyncMs < 0.0f) {
            if (fields->m_replayMusicInitialOffset == -1) {
                unsigned int musicMs = audio->getMusicTimeMS(channelID);
                fields->m_replayMusicInitialOffset = static_cast<int>(musicMs);
                debugLogToFile(fmt::format("CAPTURED initial music offset: {} ms", fields->m_replayMusicInitialOffset));
            }
            debugLogToFile("First audio sync: calling resumeAllMusic to start playback");
            audio->resumeAllMusic();
            log::info("[AUDIODBG] resumeAllMusic on first sync");
        }

        auto logAudioState = [&](const char* tag, float targetMs, unsigned int musicMs, float driftMs) {
            if (levelTime < fields->m_nextAudioDebugAt) return;
            fields->m_nextAudioDebugAt = levelTime + 0.5f;
            bool playing = (fields->m_audioMusicID >= 0) ? audio->isMusicPlaying(fields->m_audioMusicID) : false;
            debugLogToFile(fmt::format(
                "[AUDIODBG] {} t={:.3f}s target={:.0f}ms music={} ch={} play={} paused={} seeking={} forceSeek={} speed={:.2f}x musicMs={} drift={:.1f}",
                tag,
                levelTime,
                targetMs,
                fields->m_audioMusicID,
                channelID,
                playing,
                fields->m_isPaused,
                fields->m_isSeeking,
                forceSeek,
                g_replayPlayer.playbackSpeed,
                musicMs,
                driftMs
            ));
        };

        // Pause / resume music alongside replay state.
        debugLogToFile(fmt::format("checking pause state: m_isPaused={} m_audioPausedByReplay={}", fields->m_isPaused, fields->m_audioPausedByReplay));
        if (fields->m_isPaused) {
            if (!fields->m_audioPausedByReplay && fields->m_audioMusicID >= 0) {
                debugLogToFile(fmt::format("PAUSE: calling pauseMusic musicID={}", fields->m_audioMusicID));
                audio->pauseMusic(fields->m_audioMusicID);
                fields->m_audioPausedByReplay = true;
                log::info("[AUDIODBG] pauseMusic music={} ch={}", fields->m_audioMusicID, channelID);
            }
            logAudioState("paused", std::max(0.0f, levelTime * 1000.0f), 0, 0.0f);
            return;
        }

        if (fields->m_audioPausedByReplay) {
            if (fields->m_audioMusicID >= 0) {
                audio->resumeMusic(fields->m_audioMusicID);
                log::info("[AUDIODBG] resumeMusic music={} ch={}", fields->m_audioMusicID, channelID);
            }
            fields->m_audioPausedByReplay = false;
            channelID = resolveReplayMusic();
        }

        if (fields->m_audioMusicID < 0 || channelID < 0) {
            logAudioState("no-music", std::max(0.0f, levelTime * 1000.0f), 0, 0.0f);
            return;
        }

        float speed = std::clamp(g_replayPlayer.playbackSpeed, 0.25f, 4.0f);
        if (std::abs(speed - fields->m_lastAudioRate) > 0.001f) {
            // For MusicChannel target, FMOD API expects musicID, not raw channel ID.
            audio->setChannelPitch(fields->m_audioMusicID, AudioTargetType::MusicChannel, speed);
            fields->m_lastAudioRate = speed;
        }

        float targetMs = std::max(0.0f, levelTime * 1000.0f);
        if (fields->m_replayMusicInitialOffset > 0) {
            targetMs += fields->m_replayMusicInitialOffset;
        }
        unsigned int musicMs = audio->getMusicTimeMS(channelID);
        float driftMs = std::abs(targetMs - static_cast<float>(musicMs));

        // Auto-seek only on large sustained drift; aggressive correction near t=0 can mute by rewinding repeatedly.
        bool allowAutoSeek = (
            !forceSeek &&
            !fields->m_isSeeking &&
            !fields->m_isPaused &&
            targetMs >= 250.0f &&
            (levelTime - fields->m_lastAutoSeekAt) >= 1.0f &&
            driftMs > 700.0f
        );

        if (forceSeek || fields->m_lastAudioSyncMs < 0.0f || allowAutoSeek) {
            debugLogToFile(fmt::format(
                "[AUDIODBG] setMusicTimeMS music={} ch={} target={:.0f}ms musicMs={} drift={:.1f} forceSeek={}",
                fields->m_audioMusicID,
                channelID,
                targetMs,
                musicMs,
                driftMs,
                forceSeek
            ));
            debugLogToFile(fmt::format("CALL: audio->setMusicTimeMS({}, true, {})", static_cast<unsigned int>(targetMs), fields->m_audioMusicID));
            audio->setMusicTimeMS(static_cast<unsigned int>(targetMs), true, fields->m_audioMusicID);
            debugLogToFile("CALL returned");
            if (!forceSeek) {
                fields->m_lastAutoSeekAt = levelTime;
            }
        }
        fields->m_lastAudioSyncMs = targetMs;
        logAudioState("tick", targetMs, musicMs, driftMs);
        debugLogToFile(fmt::format("=== applyReplayAudioSync END ==="));
    }

    void ensureReplayMusicStarted(bool forceSeekToCurrent) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode) return;

        debugLogToFile(fmt::format("=== ensureReplayMusicStarted START forceSeekToCurrent={} ===", forceSeekToCurrent));
        log::info("[AUDIODBG] ensureReplayMusicStarted state-only t={:.3f}s practiceMusicSync={}", static_cast<float>(m_timePlayed), this->m_practiceMusicSync);

        fields->m_audioMusicID = -1;
        fields->m_musicBootstrapTried = false;
        fields->m_audioPausedByReplay = false;
        fields->m_lastAudioSyncMs = -1.0f;
        fields->m_nextAudioDebugAt = 0.0f;
        fields->m_lastAutoSeekAt = -1000.0f;
        fields->m_replayMusicInitialOffset = -1;  // Reset offset capture
        debugLogToFile("audio state reset");

        if (forceSeekToCurrent) {
            debugLogToFile("calling applyReplayAudioSync with forceSeekToCurrent");
            applyReplayAudioSync(static_cast<float>(m_timePlayed), true);
        }
        debugLogToFile(fmt::format("=== ensureReplayMusicStarted END ==="));
    }

    float replayTotalTimeSeconds() const {
        if (!g_replayPlayer.replay.has_value()) return 0.0f;
        auto const& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty() || replay.framerate <= 0.0) return 0.0f;
        return static_cast<float>(replay.presses.back().frameRelease) / static_cast<float>(replay.framerate);
    }

    uint64_t replayMaxFrame() const {
        if (!g_replayPlayer.replay.has_value()) return 1;
        auto const& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return 1;
        uint64_t maxFrame = replay.presses.back().frameRelease;
        return std::max<uint64_t>(maxFrame, 1ull);
    }

    void clampSelectedReplayInput() {
        auto fields = m_fields.self();
        if (!g_replayPlayer.replay.has_value()) return;
        auto& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return;

        if (fields->m_editSelectedIdx >= replay.presses.size()) {
            fields->m_editSelectedIdx = replay.presses.size() - 1;
        }

        auto& p = replay.presses[fields->m_editSelectedIdx];
        if (p.frameRelease <= p.framePress) {
            p.frameRelease = p.framePress + 1;
        }
    }

    void rebuildReplayRuntimeFromCurrentState() {
        auto fields = m_fields.self();
        if (!g_replayPlayer.replay.has_value()) return;

        std::sort(g_replayPlayer.replay->presses.begin(), g_replayPlayer.replay->presses.end(),
            [](ReplayPress const& a, ReplayPress const& b) {
                return a.framePress < b.framePress;
            }
        );

        clampSelectedReplayInput();
        fields->m_nextInputIdx = 0;
        fields->m_replayHoldingJump = false;

        float currentTime = static_cast<float>(m_timePlayed);
        auto const& replay = g_replayPlayer.replay.value();
        for (size_t i = 0; i < replay.presses.size(); ++i) {
            float releaseTime = static_cast<float>(replay.presses[i].frameRelease) / static_cast<float>(replay.framerate);
            if (releaseTime < currentTime) {
                fields->m_nextInputIdx = i + 1;
                continue;
            }
            break;
        }
    }

    void updateReplayEditLabels() {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        auto const& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return;

        auto const& p = replay.presses[fields->m_editSelectedIdx];
        float pressTime = static_cast<float>(p.framePress) / static_cast<float>(replay.framerate);
        float releaseTime = static_cast<float>(p.frameRelease) / static_cast<float>(replay.framerate);

        if (fields->m_editTitleLabel) {
            fields->m_editTitleLabel->setString(
                fmt::format("Edit Replay {} / {}", fields->m_editSelectedIdx + 1, replay.presses.size()).c_str()
            );
        }
        if (fields->m_editPressLabel) {
            fields->m_editPressLabel->setString(
                fmt::format("Press: {} ({:.3f}s)", p.framePress, pressTime).c_str()
            );
        }
        if (fields->m_editReleaseLabel) {
            fields->m_editReleaseLabel->setString(
                fmt::format("Release: {} ({:.3f}s)", p.frameRelease, releaseTime).c_str()
            );
        }
        if (fields->m_editHintLabel) {
            fields->m_editHintLabel->setString("Replay runs live. Edits apply to upcoming inputs.");
        }

        uint64_t maxFrame = replayMaxFrame();
        fields->m_editUpdatingSliders = true;
        if (fields->m_editPressSlider) {
            fields->m_editPressSlider->setValue(static_cast<float>(p.framePress) / static_cast<float>(maxFrame));
        }
        if (fields->m_editReleaseSlider) {
            fields->m_editReleaseSlider->setValue(static_cast<float>(p.frameRelease) / static_cast<float>(maxFrame));
        }
        fields->m_editUpdatingSliders = false;

        if (fields->m_editInputsMenu) {
            fields->m_editInputsMenu->removeAllChildrenWithCleanup(true);

            constexpr size_t kVisibleRows = 7;
            size_t start = 0;
            if (fields->m_editSelectedIdx > kVisibleRows / 2) {
                start = fields->m_editSelectedIdx - (kVisibleRows / 2);
            }
            if (start + kVisibleRows > replay.presses.size()) {
                if (replay.presses.size() > kVisibleRows) {
                    start = replay.presses.size() - kVisibleRows;
                } else {
                    start = 0;
                }
            }

            float rowY = 64.0f;
            for (size_t i = start; i < replay.presses.size() && i < start + kVisibleRows; ++i) {
                auto const& row = replay.presses[i];
                bool isSel = (i == fields->m_editSelectedIdx);
                auto rowText = fmt::format(
                    "{}{}: P{} R{}{}",
                    isSel ? "> " : "",
                    i + 1,
                    row.framePress,
                    row.frameRelease,
                    isSel ? " <" : ""
                );

                auto rowLabel = CCLabelBMFont::create(rowText.c_str(), "chatFont.fnt");
                rowLabel->setScale(0.45f);
                rowLabel->setAnchorPoint({0.0f, 0.5f});
                rowLabel->setColor(isSel ? ccc3(255, 235, 120) : ccc3(220, 220, 220));

                auto rowBtn = CCMenuItemSpriteExtra::create(
                    rowLabel,
                    this,
                    menu_selector(MyPlayLayer::onReplayEditSelectInput)
                );
                rowBtn->setTag(static_cast<int>(i));
                rowBtn->setAnchorPoint({0.0f, 0.5f});
                rowBtn->setPosition({10.0f, rowY});
                fields->m_editInputsMenu->addChild(rowBtn);

                rowY -= 10.0f;
            }
        }

        rebuildEditTimeline(false);
    }

    void rebuildEditTimeline(bool force) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        if (!fields->m_editTimelineMenu || !fields->m_editTimeline) return;

        auto const& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty() || replay.framerate <= 0.0) return;

        float totalTime = replayTotalTimeSeconds();
        float currentTime = static_cast<float>(m_timePlayed);
        float center = std::clamp(currentTime, 0.0f, totalTime);
        float windowDuration = 6.0f;

        float start = std::max(0.0f, center - windowDuration * 0.5f);
        float end = std::min(totalTime, start + windowDuration);
        if (end - start < windowDuration) {
            start = std::max(0.0f, end - windowDuration);
        }

        if (!force) {
            bool selectedUnchanged = (fields->m_lastTimelineSelectedIdx == fields->m_editSelectedIdx);
            bool centerUnchanged = std::abs(fields->m_lastTimelineCenterTime - center) < 0.12f;
            if (selectedUnchanged && centerUnchanged) {
                fields->m_editTimelineWindowStart = start;
                fields->m_editTimelineWindowEnd = end;
                return;
            }
        }

        fields->m_lastTimelineCenterTime = center;
        fields->m_lastTimelineSelectedIdx = fields->m_editSelectedIdx;
        fields->m_editTimelineWindowStart = start;
        fields->m_editTimelineWindowEnd = end;

        fields->m_editTimelineMenu->removeAllChildrenWithCleanup(true);

        float timelineWidth = fields->m_editTimeline->getContentSize().width;
        float timelineHeight = fields->m_editTimeline->getContentSize().height;
        float duration = std::max(0.001f, end - start);

        auto toX = [&](float t) {
            float n = (t - start) / duration;
            return std::clamp(n, 0.0f, 1.0f) * timelineWidth;
        };

        for (size_t i = 0; i < replay.presses.size(); ++i) {
            auto const& press = replay.presses[i];
            float t0 = static_cast<float>(press.framePress) / static_cast<float>(replay.framerate);
            float t1 = static_cast<float>(press.frameRelease) / static_cast<float>(replay.framerate);
            if (t1 < start || t0 > end) continue;

            float x0 = toX(t0);
            float x1 = toX(t1);
            float w = std::max(2.0f, std::abs(x1 - x0));
            float xLeft = std::min(x0, x1);
            bool selected = (i == fields->m_editSelectedIdx);

            auto color = selected ? ccc4(255, 235, 120, 220) : ccc4(120, 190, 255, 170);
            auto seg = CCLayerColor::create(color, w, timelineHeight - 6.0f);
            seg->setAnchorPoint({0.0f, 0.0f});
            seg->setPosition({xLeft, 3.0f});

            auto btn = CCMenuItemSpriteExtra::create(
                seg,
                this,
                menu_selector(MyPlayLayer::onReplayEditSelectInput)
            );
            btn->setTag(static_cast<int>(i));
            btn->setAnchorPoint({0.0f, 0.0f});
            btn->setPosition({xLeft, 3.0f});
            fields->m_editTimelineMenu->addChild(btn);
        }

        if (fields->m_editTimelineLabel) {
            fields->m_editTimelineLabel->setString(
                fmt::format("Timeline {:.2f}s/{:.2f}s", currentTime, totalTime).c_str()
            );
        }
    }

    void updateEditTimelineCursor() {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !fields->m_editTimelineCursor || !fields->m_editTimeline) return;
        float start = fields->m_editTimelineWindowStart;
        float end = std::max(start + 0.001f, fields->m_editTimelineWindowEnd);
        float duration = end - start;
        float t = static_cast<float>(m_timePlayed);
        float n = std::clamp((t - start) / duration, 0.0f, 1.0f);
        float x = n * fields->m_editTimeline->getContentSize().width;
        fields->m_editTimelineCursor->setPosition({x, 0.0f});
    }

    void previewReplayAtSelectedInput() {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        auto const& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return;

        auto const& p = replay.presses[fields->m_editSelectedIdx];
        float target = std::max(0.0f, (static_cast<float>(p.framePress) / static_cast<float>(replay.framerate)) - 0.2f);

        if (target <= 0.05f) {
            fields->m_isSeeking = false;
            fields->m_seekTargetTime = 0.0f;
            fields->m_isPaused = true;
            if (static_cast<float>(m_timePlayed) > 0.05f) {
                this->resetLevel();
            }
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(0.0f);
            applyReplayAudioSync(0.0f, true);
            return;
        }

        fields->m_isPaused = false;
        fields->m_isSeeking = true;
        fields->m_seekTargetTime = target;
        fields->m_seekResumeSpeed = g_replayPlayer.playbackSpeed;
        fields->m_isPaused = true;
        this->resetLevel();
        applyReplayAudioSync(0.0f, true);
    }

    void queueReplayEditPreview() {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode) return;
        fields->m_editPreviewPending = true;
    }

    void editSelectedInputFrame(bool editPress, int deltaFrames) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        auto& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return;

        auto& p = replay.presses[fields->m_editSelectedIdx];
        auto applyDelta = [&](uint64_t value) -> uint64_t {
            int64_t next = static_cast<int64_t>(value) + static_cast<int64_t>(deltaFrames);
            if (next < 0) next = 0;
            return static_cast<uint64_t>(next);
        };

        if (editPress) {
            p.framePress = applyDelta(p.framePress);
            if (p.frameRelease <= p.framePress) {
                p.frameRelease = p.framePress + 1;
            }
        } else {
            p.frameRelease = applyDelta(p.frameRelease);
            if (p.frameRelease <= p.framePress) {
                p.frameRelease = p.framePress + 1;
            }
        }

        rebuildReplayRuntimeFromCurrentState();
        updateReplayEditLabels();
    }

    void onReplayEditPrev(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        if (fields->m_editSelectedIdx > 0) {
            fields->m_editSelectedIdx--;
        }
        updateReplayEditLabels();
    }

    void onReplayEditNext(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        auto const count = g_replayPlayer.replay->presses.size();
        if (fields->m_editSelectedIdx + 1 < count) {
            fields->m_editSelectedIdx++;
        }
        updateReplayEditLabels();
    }

    void onReplayEditSelectInput(CCObject* sender) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;

        auto* btn = typeinfo_cast<CCNode*>(sender);
        if (!btn) return;

        int idx = btn->getTag();
        if (idx < 0) return;
        size_t uidx = static_cast<size_t>(idx);
        if (uidx >= g_replayPlayer.replay->presses.size()) return;

        fields->m_editSelectedIdx = uidx;
        updateReplayEditLabels();
    }

    void onReplayEditPressMinus(CCObject*) { editSelectedInputFrame(true, -1); }
    void onReplayEditPressPlus(CCObject*) { editSelectedInputFrame(true, +1); }
    void onReplayEditReleaseMinus(CCObject*) { editSelectedInputFrame(false, -1); }
    void onReplayEditReleasePlus(CCObject*) { editSelectedInputFrame(false, +1); }

    void onReplayEditSave(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;
        rebuildReplayRuntimeFromCurrentState();

        bool ok = saveReplayToLocalJson(
            g_replayPlayer.levelId,
            g_replayPlayer.replay.value(),
            g_replayPlayer.sourceReplayPath
        );

        if (ok) {
            FLAlertLayer::create("Replay Editor", "Saved replay edits.", "OK")->show();
        } else {
            FLAlertLayer::create("Replay Editor", "Failed to save replay edits.", "OK")->show();
        }
    }

    void onReplayEditJumpToCurrent(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || !g_replayPlayer.replay.has_value()) return;

        auto const& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty() || replay.framerate <= 0.0) return;

        uint64_t currentFrame = static_cast<uint64_t>(
            std::llround(static_cast<double>(m_timePlayed) * replay.framerate)
        );

        size_t bestIdx = 0;
        uint64_t bestDist = std::numeric_limits<uint64_t>::max();

        for (size_t i = 0; i < replay.presses.size(); ++i) {
            auto const& p = replay.presses[i];

            if (currentFrame >= p.framePress && currentFrame <= p.frameRelease) {
                bestIdx = i;
                bestDist = 0;
                break;
            }

            uint64_t dist = 0;
            if (currentFrame < p.framePress) {
                dist = p.framePress - currentFrame;
            } else {
                dist = currentFrame - p.frameRelease;
            }

            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }

        fields->m_editSelectedIdx = bestIdx;
        updateReplayEditLabels();
    }

    void onReplayEditPressSlider(CCObject* sender) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || fields->m_editUpdatingSliders || !g_replayPlayer.replay.has_value()) return;

        auto* slider = typeinfo_cast<Slider*>(sender);
        if (!slider) return;

        auto& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return;

        uint64_t maxFrame = replayMaxFrame();
        auto& p = replay.presses[fields->m_editSelectedIdx];
        p.framePress = static_cast<uint64_t>(std::round(std::clamp(slider->getValue(), 0.0f, 1.0f) * static_cast<float>(maxFrame)));
        if (p.frameRelease <= p.framePress) {
            p.frameRelease = p.framePress + 1;
        }

        rebuildReplayRuntimeFromCurrentState();
        updateReplayEditLabels();
    }

    void onReplayEditReleaseSlider(CCObject* sender) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayEditMode || fields->m_editUpdatingSliders || !g_replayPlayer.replay.has_value()) return;

        auto* slider = typeinfo_cast<Slider*>(sender);
        if (!slider) return;

        auto& replay = g_replayPlayer.replay.value();
        if (replay.presses.empty()) return;

        uint64_t maxFrame = replayMaxFrame();
        auto& p = replay.presses[fields->m_editSelectedIdx];
        p.frameRelease = static_cast<uint64_t>(std::round(std::clamp(slider->getValue(), 0.0f, 1.0f) * static_cast<float>(maxFrame)));
        if (p.frameRelease <= p.framePress) {
            p.frameRelease = p.framePress + 1;
        }

        rebuildReplayRuntimeFromCurrentState();
        updateReplayEditLabels();
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        debugLogToFile("=== PlayLayer::init START ===");
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            debugLogToFile("PlayLayer::init FAILED");
            return false;
        }
        debugLogToFile("PlayLayer::init succeeded");

        auto fields = m_fields.self();
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // ========== Replay Player Mode Setup ==========
        if (g_replayPlayer.isActive && g_replayPlayer.replay.has_value()) {
            debugLogToFile("ENTERING REPLAY MODE");
            fields->m_isReplayMode = true;
            fields->m_isReplayEditMode = g_replayPlayer.isEditMode;
            fields->m_replayCurrentTime = 0.0f;
            fields->m_nextInputIdx = 0;
            fields->m_isPaused = g_replayPlayer.isPaused;
            fields->m_pauseTime = 0.0f;
            fields->m_isSeeking = false;
            fields->m_seekTargetTime = 0.0f;
            fields->m_seekResumeSpeed = g_replayPlayer.playbackSpeed;
            fields->m_replayHoldingJump = false;
            fields->m_editSelectedIdx = 0;
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(
                fields->m_isPaused ? 0.0f : g_replayPlayer.playbackSpeed
            );
            fields->m_prevPracticeMusicSync = this->m_practiceMusicSync;
            fields->m_forcedPracticeMusic = false;
            this->togglePracticeMode(true);
            ensureReplayMusicStarted(false);

            // Setup video controls layer (bottom controls)
            auto ctrlLayer = CCNode::create();
            ctrlLayer->setContentSize({winSize.width, 60.0f});
            ctrlLayer->setAnchorPoint({0.0f, 0.0f});
            ctrlLayer->setPosition({0.0f, 0.0f});
            ctrlLayer->setID("replay-controls"_spr);

            // Background bar for controls
            auto ctrlBg = CCLayerColor::create(ccc4(0, 0, 0, 180), winSize.width, 60.0f);
            ctrlBg->setPosition({0.0f, 0.0f});
            ctrlLayer->addChild(ctrlBg, 0);

            // Time label (current time / total time)
            auto timeLabel = CCLabelBMFont::create("00:00 / 00:00", "chatFont.fnt");
            timeLabel->setScale(0.6f);
            timeLabel->setAnchorPoint({0.0f, 0.5f});
            timeLabel->setPosition({10.0f, 30.0f});
            timeLabel->setColor(ccc3(255, 255, 255));
            ctrlLayer->addChild(timeLabel, 1);
            fields->m_timeLabel = timeLabel;

            // Speed label (current playback speed)
            auto speedLabel = CCLabelBMFont::create("1.00x", "chatFont.fnt");
            speedLabel->setScale(0.6f);
            speedLabel->setAnchorPoint({1.0f, 0.5f});
            speedLabel->setPosition({winSize.width - 10.0f, 30.0f});
            speedLabel->setColor(ccc3(255, 200, 100));
            ctrlLayer->addChild(speedLabel, 1);
            fields->m_speedLabel = speedLabel;

            // Centered row layout for replay control buttons
            float controlsY = 30.0f;
            float buttonSpacing = 56.0f;
            float centerX = winSize.width * 0.5f;

            // Play/Pause button
            auto playText = CCLabelBMFont::create(fields->m_isPaused ? "Play" : "Pause", "bigFont.fnt");
            playText->setScale(0.4f);
            auto playBtn = CCMenuItemSpriteExtra::create(
                playText,
                this,
                menu_selector(MyPlayLayer::onReplayPlayPause)
            );
            playBtn->setPosition({centerX - (2.0f * buttonSpacing), controlsY});
            fields->m_playPauseLabel = playText;

            // Speed down button (-0.25x)
            auto speedDownText = CCLabelBMFont::create("-", "bigFont.fnt");
            speedDownText->setScale(0.5f);
            auto speedDownBtn = CCMenuItemSpriteExtra::create(
                speedDownText,
                this,
                menu_selector(MyPlayLayer::onReplaySpeedDown)
            );
            speedDownBtn->setPosition({centerX - buttonSpacing, controlsY});

            // Speed up button (+0.25x)
            auto speedUpText = CCLabelBMFont::create("+", "bigFont.fnt");
            speedUpText->setScale(0.5f);
            auto speedUpBtn = CCMenuItemSpriteExtra::create(
                speedUpText,
                this,
                menu_selector(MyPlayLayer::onReplaySpeedUp)
            );
            speedUpBtn->setPosition({centerX, controlsY});

            // Rewind button
            auto rewindText = CCLabelBMFont::create("<<", "bigFont.fnt");
            rewindText->setScale(0.4f);
            auto rewindBtn = CCMenuItemSpriteExtra::create(
                rewindText,
                this,
                menu_selector(MyPlayLayer::onReplayRewind)
            );
            rewindBtn->setPosition({centerX + buttonSpacing, controlsY});

            // Skip forward button (+5s)
            auto skipForwardText = CCLabelBMFont::create(">>", "bigFont.fnt");
            skipForwardText->setScale(0.4f);
            auto skipForwardBtn = CCMenuItemSpriteExtra::create(
                skipForwardText,
                this,
                menu_selector(MyPlayLayer::onReplaySkipForward)
            );
            skipForwardBtn->setPosition({centerX + (2.0f * buttonSpacing), controlsY});

            auto controlMenu = CCMenu::create();
            controlMenu->addChild(playBtn);
            controlMenu->addChild(speedDownBtn);
            controlMenu->addChild(speedUpBtn);
            controlMenu->addChild(rewindBtn);
            controlMenu->addChild(skipForwardBtn);
            controlMenu->setPosition({0.0f, 0.0f});
            controlMenu->setID("replay-menu"_spr);
            ctrlLayer->addChild(controlMenu, 2);

            this->addChild(ctrlLayer, 10100);
            fields->m_videoControlsLayer = ctrlLayer;
            fields->m_lastAudioSyncMs = -1.0f;
            fields->m_lastAudioRate = g_replayPlayer.playbackSpeed;
            fields->m_audioPausedByReplay = false;
            fields->m_audioMusicID = -1;
            fields->m_nextMusicRetryAt = 0.25f;
            fields->m_nextAudioDebugAt = 0.0f;
            fields->m_lastAutoSeekAt = -1000.0f;
            fields->m_replayMusicInitialOffset = -1;

            if (fields->m_isReplayEditMode) {
                auto editPanel = CCNode::create();
                editPanel->setContentSize({winSize.width, 74.0f});
                editPanel->setAnchorPoint({0.0f, 0.0f});
                editPanel->setPosition({0.0f, 60.0f});
                editPanel->setID("replay-edit-panel"_spr);

                auto editBg = CCLayerColor::create(ccc4(0, 0, 0, 160), winSize.width, 74.0f);
                editBg->setPosition({0.0f, 0.0f});
                editPanel->addChild(editBg, 0);

                auto title = CCLabelBMFont::create("Edit Replay", "chatFont.fnt");
                title->setScale(0.7f);
                title->setAnchorPoint({0.0f, 0.5f});
                title->setPosition({10.0f, 56.0f});
                editPanel->addChild(title, 1);
                fields->m_editTitleLabel = title;

                auto pressLabel = CCLabelBMFont::create("Press: -", "chatFont.fnt");
                pressLabel->setScale(0.6f);
                pressLabel->setAnchorPoint({0.0f, 0.5f});
                pressLabel->setPosition({10.0f, 36.0f});
                editPanel->addChild(pressLabel, 1);
                fields->m_editPressLabel = pressLabel;

                auto releaseLabel = CCLabelBMFont::create("Release: -", "chatFont.fnt");
                releaseLabel->setScale(0.6f);
                releaseLabel->setAnchorPoint({0.0f, 0.5f});
                releaseLabel->setPosition({10.0f, 18.0f});
                editPanel->addChild(releaseLabel, 1);
                fields->m_editReleaseLabel = releaseLabel;

                auto pressSlider = Slider::create(this, menu_selector(MyPlayLayer::onReplayEditPressSlider), 0.45f);
                pressSlider->setPosition({230.0f, 36.0f});
                pressSlider->setValue(0.0f);
                pressSlider->setLiveDragging(false);
                editPanel->addChild(pressSlider, 1);
                fields->m_editPressSlider = pressSlider;

                auto releaseSlider = Slider::create(this, menu_selector(MyPlayLayer::onReplayEditReleaseSlider), 0.45f);
                releaseSlider->setPosition({230.0f, 18.0f});
                releaseSlider->setValue(0.0f);
                releaseSlider->setLiveDragging(false);
                editPanel->addChild(releaseSlider, 1);
                fields->m_editReleaseSlider = releaseSlider;

                auto hintLabel = CCLabelBMFont::create("", "chatFont.fnt");
                hintLabel->setScale(0.45f);
                hintLabel->setAnchorPoint({1.0f, 0.5f});
                hintLabel->setPosition({winSize.width - 10.0f, 56.0f});
                hintLabel->setColor(ccc3(180, 220, 255));
                editPanel->addChild(hintLabel, 1);
                fields->m_editHintLabel = hintLabel;

                auto mkTxtBtn = [&](const char* text, SEL_MenuHandler cb, CCPoint pos) {
                    auto txt = CCLabelBMFont::create(text, "bigFont.fnt");
                    txt->setScale(0.34f);
                    auto btn = CCMenuItemSpriteExtra::create(txt, this, cb);
                    btn->setPosition(pos);
                    return btn;
                };

                auto editMenu = CCMenu::create();
                editMenu->setPosition({0.0f, 0.0f});
                editMenu->addChild(mkTxtBtn("Prev", menu_selector(MyPlayLayer::onReplayEditPrev), {winSize.width - 330.0f, 36.0f}));
                editMenu->addChild(mkTxtBtn("Next", menu_selector(MyPlayLayer::onReplayEditNext), {winSize.width - 270.0f, 36.0f}));
                editMenu->addChild(mkTxtBtn("Now", menu_selector(MyPlayLayer::onReplayEditJumpToCurrent), {winSize.width - 230.0f, 56.0f}));
                editMenu->addChild(mkTxtBtn("P-", menu_selector(MyPlayLayer::onReplayEditPressMinus), {winSize.width - 210.0f, 36.0f}));
                editMenu->addChild(mkTxtBtn("P+", menu_selector(MyPlayLayer::onReplayEditPressPlus), {winSize.width - 170.0f, 36.0f}));
                editMenu->addChild(mkTxtBtn("R-", menu_selector(MyPlayLayer::onReplayEditReleaseMinus), {winSize.width - 120.0f, 36.0f}));
                editMenu->addChild(mkTxtBtn("R+", menu_selector(MyPlayLayer::onReplayEditReleasePlus), {winSize.width - 80.0f, 36.0f}));
                editMenu->addChild(mkTxtBtn("Save", menu_selector(MyPlayLayer::onReplayEditSave), {winSize.width - 35.0f, 36.0f}));
                editPanel->addChild(editMenu, 2);

                this->addChild(editPanel, 10101);
                fields->m_editPanel = editPanel;

                auto inputsPanel = CCNode::create();
                inputsPanel->setContentSize({260.0f, 80.0f});
                inputsPanel->setAnchorPoint({0.0f, 0.0f});
                inputsPanel->setPosition({10.0f, 136.0f});
                inputsPanel->setID("replay-edit-inputs-panel"_spr);

                auto inputsBg = CCLayerColor::create(ccc4(0, 0, 0, 150), 260.0f, 80.0f);
                inputsBg->setPosition({0.0f, 0.0f});
                inputsPanel->addChild(inputsBg, 0);

                auto inputsTitle = CCLabelBMFont::create("Inputs (click to select)", "chatFont.fnt");
                inputsTitle->setScale(0.5f);
                inputsTitle->setAnchorPoint({0.0f, 0.5f});
                inputsTitle->setPosition({8.0f, 72.0f});
                inputsPanel->addChild(inputsTitle, 1);

                auto inputsMenu = CCMenu::create();
                inputsMenu->setPosition({0.0f, 0.0f});
                inputsPanel->addChild(inputsMenu, 2);

                this->addChild(inputsPanel, 10102);
                fields->m_editInputsPanel = inputsPanel;
                fields->m_editInputsMenu = inputsMenu;

                updateReplayEditLabels();
            }

            log::info("[ReplayPlayer] Initialized replay player mode with {} presses", fields->m_isReplayMode);
        }

        log::info("[IsMyModUpdated] 1"); // increment this when making changes to force users to update their local replay JSON files (if needed)

        bool setting = Mod::get()->getSettingValue<bool>("show-inputs-ingame");
        log::info("[InputCircles] Setting show-inputs-ingame = {}", setting);
        if (!setting && !fields->m_isReplayMode) return true;

        int levelId = level->m_levelID;
        log::info("[InputCircles] Level ID = {}", levelId);

        // Load replay for visualization (input circles mode)
        if (!fields->m_isReplayMode) {
            auto loadResult = loadMatchingReplay(levelId);
            if (!loadResult.has_value()) {
                log::info("[InputCircles] No matching replay found for level {}", levelId);
                return true;
            }

            log::info("[InputCircles] Loaded {} presses, framerate={}", loadResult->presses.size(), loadResult->framerate);

            fields->m_presses = std::move(loadResult->presses);
            fields->m_framerate = loadResult->framerate;
            fields->m_active = true;
        } else {
            // In replay mode, use the replay from g_replayPlayer
            if (g_replayPlayer.replay.has_value()) {
                fields->m_framerate = g_replayPlayer.replay->framerate;
            }
        }

        // Read settings
        fields->m_barHeight = Mod::get()->getSettingValue<int64_t>("bar-height");
        fields->m_barY = Mod::get()->getSettingValue<int64_t>("bar-y-position");
        auto p1c = Mod::get()->getSettingValue<ccColor4B>("player1-color");
        auto p2c = Mod::get()->getSettingValue<ccColor4B>("player2-color");
        auto ic = Mod::get()->getSettingValue<ccColor4B>("indicator-color");
        auto bgc = Mod::get()->getSettingValue<ccColor4B>("background-color");
        fields->m_p1Color = p1c;
        fields->m_p2Color = p2c;
        fields->m_indicatorColor = ic;
        fields->m_bgColor = bgc;

        // Create background strip spanning the full screen width at bar height
        float initBarH = static_cast<float>(fields->m_barHeight);
        float initBottomY = static_cast<float>(fields->m_barY);
        auto gameplayWinSize = CCDirector::sharedDirector()->getWinSize();
        auto bgStrip = CCLayerColor::create(bgc, gameplayWinSize.width, initBarH);
        bgStrip->setPosition({0.0f, initBottomY - initBarH / 2.0f});
        bgStrip->setID("input-bg-strip"_spr);
        this->addChild(bgStrip, 9999);
        fields->m_bgStrip = bgStrip;

        // Create a layer for dots, added to PlayLayer directly (screen space, high Z)
        auto circleLayer = CCNode::create();
        circleLayer->setID("input-circles-layer"_spr);
        this->addChild(circleLayer, 10000);
        fields->m_circleLayer = circleLayer;

        // Create hollow indicator square (border only)
        float indSize = static_cast<float>(fields->m_barHeight) + 6.0f;
        float border = 2.0f;
        auto indicator = CCNode::create();
        indicator->setContentSize({indSize, indSize});
        indicator->setAnchorPoint({0.5f, 0.5f});

        // 4 thin CCLayerColor edges to form a hollow rectangle
        auto top = CCLayerColor::create(ic, indSize, border);
        top->setPosition({0.0f, indSize - border});
        indicator->addChild(top);

        auto bottom = CCLayerColor::create(ic, indSize, border);
        bottom->setPosition({0.0f, 0.0f});
        indicator->addChild(bottom);

        auto left = CCLayerColor::create(ic, border, indSize);
        left->setPosition({0.0f, 0.0f});
        indicator->addChild(left);

        auto right = CCLayerColor::create(ic, border, indSize);
        right->setPosition({indSize - border, 0.0f});
        indicator->addChild(right);

        this->addChild(indicator, 10001);
        fields->m_indicator = indicator;

        // ========== UR Bar Setup / Edit Timeline Setup ==========
        if (fields->m_isReplayEditMode) {
            float timelineW = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-bar-width"));
            float timelineH = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-bar-height"));
            float timelineX = winSize.width / 2.0f;
            float timelineY = winSize.height - 30.0f;

            auto timeline = CCNode::create();
            timeline->setContentSize({timelineW, timelineH});
            timeline->setAnchorPoint({0.5f, 0.5f});
            timeline->setPosition({timelineX, timelineY});
            timeline->setID("replay-edit-timeline"_spr);

            auto timelineBg = CCLayerColor::create(ccc4(0, 0, 0, 190), timelineW, timelineH);
            timelineBg->setPosition({0.0f, 0.0f});
            timeline->addChild(timelineBg, 0);

            auto timelineMenu = CCMenu::create();
            timelineMenu->setPosition({0.0f, 0.0f});
            timeline->addChild(timelineMenu, 1);

            auto cursor = CCLayerColor::create(ccc4(255, 255, 255, 255), 2.0f, timelineH + 4.0f);
            cursor->setPosition({0.0f, -2.0f});
            timeline->addChild(cursor, 2);

            this->addChild(timeline, 10002);
            fields->m_editTimeline = timeline;
            fields->m_editTimelineMenu = timelineMenu;
            fields->m_editTimelineCursor = cursor;

            auto tlLabel = CCLabelBMFont::create("Timeline", "chatFont.fnt");
            tlLabel->setScale(0.45f);
            tlLabel->setAnchorPoint({0.5f, 0.0f});
            tlLabel->setPosition({timelineX, timelineY + timelineH * 0.5f + 4.0f});
            tlLabel->setColor(ccc3(220, 230, 255));
            this->addChild(tlLabel, 10003);
            fields->m_editTimelineLabel = tlLabel;

            rebuildEditTimeline(true);
            updateEditTimelineCursor();
        }

        // ========== UR Bar Setup ==========
        if (!fields->m_isReplayEditMode) {
        // Pre-compute replay press/release times in seconds for matching
        for (auto& press : fields->m_presses) {
            fields->m_replayPressTimes.push_back(
                static_cast<float>(press.framePress) / static_cast<float>(fields->m_framerate));
            fields->m_replayReleaseTimes.push_back(
                static_cast<float>(press.frameRelease) / static_cast<float>(fields->m_framerate));
        }
        fields->m_replayPressUsed.resize(fields->m_presses.size(), false);
        fields->m_replayReleaseUsed.resize(fields->m_presses.size(), false);

        // Read UR bar settings
        fields->m_urBarWidth = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-bar-width"));
        fields->m_urBarHeight = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-bar-height"));
        fields->m_goodFrames = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-good-frames"));
        fields->m_earlyLateFrames = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-earlylate-frames"));
        fields->m_veryFrames = static_cast<float>(Mod::get()->getSettingValue<int64_t>("ur-very-frames"));

        float urW = fields->m_urBarWidth;
        float urH = fields->m_urBarHeight;
        float veryF = fields->m_veryFrames;
        float elF = fields->m_earlyLateFrames;
        float gF = fields->m_goodFrames;
        float totalFrameRange = veryF * 2.0f; // -veryF to +veryF
        float pxPerFrame = urW / totalFrameRange;

        // UR bar container (centered horizontally near top of screen)
        auto urBar = CCNode::create();
        urBar->setContentSize({urW, urH});
        urBar->setAnchorPoint({0.5f, 0.5f});
        float urX = winSize.width / 2.0f;
        float urY = winSize.height - 30.0f;
        urBar->setPosition({urX, urY});
        urBar->setID("ur-bar"_spr);

        // Zone colors
        ccColor4B cVeryEarly = ccc4(255, 60, 60, 200);   // red
        ccColor4B cEarly     = ccc4(255, 200, 50, 200);   // yellow
        ccColor4B cGood      = ccc4(50, 255, 80, 200);    // green
        ccColor4B cLate      = ccc4(255, 200, 50, 200);   // yellow
        ccColor4B cVeryLate  = ccc4(255, 60, 60, 200);    // red

        // Zone widths in pixels
        float wVeryEarly = (veryF - elF) * pxPerFrame;
        float wEarly     = (elF - gF) * pxPerFrame;
        float wGood      = gF * 2.0f * pxPerFrame;
        float wLate      = (elF - gF) * pxPerFrame;
        float wVeryLate  = (veryF - elF) * pxPerFrame;

        // Background
        auto urBg = CCLayerColor::create(ccc4(0, 0, 0, 160), urW, urH);
        urBg->setPosition({0.0f, 0.0f});
        urBar->addChild(urBg, 0);

        // Zones from left to right
        float xOff = 0.0f;
        auto zVE = CCLayerColor::create(cVeryEarly, wVeryEarly, urH);
        zVE->setPosition({xOff, 0.0f}); urBar->addChild(zVE, 1); xOff += wVeryEarly;
        auto zE = CCLayerColor::create(cEarly, wEarly, urH);
        zE->setPosition({xOff, 0.0f}); urBar->addChild(zE, 1); xOff += wEarly;
        auto zG = CCLayerColor::create(cGood, wGood, urH);
        zG->setPosition({xOff, 0.0f}); urBar->addChild(zG, 1); xOff += wGood;
        auto zL = CCLayerColor::create(cLate, wLate, urH);
        zL->setPosition({xOff, 0.0f}); urBar->addChild(zL, 1); xOff += wLate;
        auto zVL = CCLayerColor::create(cVeryLate, wVeryLate, urH);
        zVL->setPosition({xOff, 0.0f}); urBar->addChild(zVL, 1);

        // Center line (perfect timing marker)
        auto centerLine = CCLayerColor::create(ccc4(255, 255, 255, 255), 2.0f, urH + 4.0f);
        centerLine->setPosition({urW / 2.0f - 1.0f, -2.0f});
        urBar->addChild(centerLine, 3);

        this->addChild(urBar, 10002);
        fields->m_urBar = urBar;

        // Accuracy label (top-right corner)
        auto accLabel = CCLabelBMFont::create("100.00%", "bigFont.fnt");
        accLabel->setScale(0.35f);
        accLabel->setAnchorPoint({1.0f, 1.0f});
        accLabel->setPosition({winSize.width - 10.0f, winSize.height - 10.0f});
        accLabel->setColor(ccc3(255, 255, 255));
        accLabel->setOpacity(220);
        accLabel->setID("accuracy-label"_spr);
        this->addChild(accLabel, 10002);
        fields->m_accuracyLabel = accLabel;

        // Activate rhythm input capture
        s_playerInputs.clear();
        s_rhythmActive = true;
        fields->m_processedInputIdx = 0;

        log::info("[UR] Rhythm mode active. UR bar: {}x{} goodF={} elF={} veryF={}", 
            urW, urH, gF, elF, veryF);
        } else {
            // In replay edit mode, UR gameplay scoring UI is replaced by timeline UI.
            s_playerInputs.clear();
            s_rhythmActive = false;
        }

        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto fields = m_fields.self();

        // ========== Replay Player Mode: Auto-inject inputs ==========
        if (fields->m_isReplayMode && g_replayPlayer.replay.has_value()) {
            auto& replay = g_replayPlayer.replay.value();
            auto scheduler = CCDirector::sharedDirector()->getScheduler();
            float targetScale = fields->m_isSeeking ? 4.0f : (fields->m_isPaused ? 0.0f : g_replayPlayer.playbackSpeed);
            scheduler->setTimeScale(std::max(0.0f, targetScale));
            debugLogToFile(fmt::format("postUpdate dt={} targetScale={} currentTimeScale={}", dt, targetScale, scheduler->getTimeScale()));

            float currentTime = static_cast<float>(m_timePlayed);
            fields->m_replayCurrentTime = currentTime;
            if (fields->m_isPaused) {
                fields->m_pauseTime = currentTime;
            }
            debugLogToFile(fmt::format("postUpdate currentTime={:.3f} isPaused={} isSeeking={}", currentTime, fields->m_isPaused, fields->m_isSeeking));

            // One-shot bootstrap: if replay starts with no active music slot,
            // start level music exactly once to recover from silent startup.
            if (!fields->m_musicBootstrapTried && currentTime >= 0.15f) {
                debugLogToFile(fmt::format("bootstrap check at currentTime={:.3f}", currentTime));
                fields->m_musicBootstrapTried = true;
                if (auto* audio = FMODAudioEngine::sharedEngine()) {
                    bool hasMusic = false;
                    for (int musicID = 0; musicID < 32; ++musicID) {
                        if (audio->isMusicPlaying(musicID)) {
                            debugLogToFile(fmt::format("  found active music musicID={}", musicID));
                            hasMusic = true;
                            break;
                        }
                    }
                    debugLogToFile(fmt::format("  hasMusic after scan={}", hasMusic));
                    if (!hasMusic) {
                        debugLogToFile("  NO MUSIC FOUND - BOOTSTRAPPING");
                        debugLogToFile("  calling prepareMusic(false)");
                        this->prepareMusic(false);
                        debugLogToFile("  calling startMusic()");
                        this->startMusic();
                        debugLogToFile("  calling resumeAllMusic()");
                        audio->resumeAllMusic();
                        log::info("[AUDIODBG] bootstrapMusic prepare+start at t={:.3f}s", currentTime);
                        debugLogToFile("  bootstrap complete");
                    }
                }
            }

            // In edit mode, avoid running expensive continuous audio sync while fully paused.
            // Manual preview/seek actions already force a sync when needed.
            bool shouldSyncAudio = !(fields->m_isReplayEditMode && fields->m_isPaused && !fields->m_isSeeking);
            if (shouldSyncAudio) {
                debugLogToFile(fmt::format("calling applyReplayAudioSync currentTime={:.3f} isSeeking={}", currentTime, fields->m_isSeeking));
                applyReplayAudioSync(currentTime, fields->m_isSeeking);
                debugLogToFile("applyReplayAudioSync returned");
            }

            // Auto-inject inputs from replay that should occur now
            while (fields->m_nextInputIdx < replay.presses.size()) {
                auto& press = replay.presses[fields->m_nextInputIdx];
                float pressTime = static_cast<float>(press.framePress) / static_cast<float>(replay.framerate);
                float releaseTime = static_cast<float>(press.frameRelease) / static_cast<float>(replay.framerate);

                if (currentTime >= releaseTime) {
                    // Release and advance to next replay press
                    if (fields->m_replayHoldingJump) {
                        this->handleButton(false, 1, true);
                        fields->m_replayHoldingJump = false;
                    }
                    fields->m_nextInputIdx++;
                    continue;
                }

                if (currentTime >= pressTime) {
                    // Press for the current replay event
                    if (!fields->m_replayHoldingJump) {
                        this->handleButton(true, 1, true);
                        fields->m_replayHoldingJump = true;
                    }
                }
                break;
            }

            if (fields->m_isSeeking && currentTime >= fields->m_seekTargetTime) {
                debugLogToFile(fmt::format("seek complete currentTime={:.3f} >= seekTarget={:.3f}", currentTime, fields->m_seekTargetTime));
                fields->m_isSeeking = false;
                scheduler->setTimeScale(fields->m_isPaused ? 0.0f : g_replayPlayer.playbackSpeed);
            }

            // Update labels
            if (fields->m_speedLabel) {
                auto speedStr = fields->m_isSeeking
                    ? fmt::format("{:.2f}x (seek)", g_replayPlayer.playbackSpeed)
                    : fmt::format("{:.2f}x", g_replayPlayer.playbackSpeed);
                fields->m_speedLabel->setString(speedStr.c_str());
            }

            if (fields->m_playPauseLabel) {
                fields->m_playPauseLabel->setString(fields->m_isPaused ? "Play" : "Pause");
            }

            if (fields->m_timeLabel) {
                float totalTime = replay.framerate > 0
                    ? static_cast<float>(replay.presses.back().frameRelease) / static_cast<float>(replay.framerate)
                    : 0.0f;
                auto timeStr = fmt::format("{} / {}", formatTime(currentTime), formatTime(totalTime));
                fields->m_timeLabel->setString(timeStr.c_str());
            }

            if (fields->m_isReplayEditMode) {
                rebuildEditTimeline(false);
                updateEditTimelineCursor();
            }

            // Edit mode follows replay playback continuously; edits are applied live
            // via replay data updates without forced reset/seek preview.
        }

        // Debug counter – log every ~120 frames. BEFORE any early return!
        static int s_dbg = 0;
        s_dbg++;
        bool dbg = (s_dbg % 120 == 1);

        if (dbg) log::info("[DBG] === postUpdate CALLED === frame={}", s_dbg);

        if (!fields->m_active || !fields->m_circleLayer) {
            if (dbg) log::info("[DBG] EARLY EXIT: active={} circleLayer={}",
                fields->m_active, fields->m_circleLayer ? "yes" : "NULL");
            return;
        }

        auto player = m_player1;
        if (!player) {
            if (dbg) log::info("[DBG] EARLY EXIT: player is NULL");
            return;
        }

        if (dbg) log::info("[DBG] postUpdate ACTIVE frame={}", s_dbg);

        float playerX = player->getPositionX();
        float currentTime = static_cast<float>(m_timePlayed);

        if (dbg) log::info("[DBG] playerX={:.2f} currentTime={:.4f} historySize={}", playerX, currentTime, fields->m_posHistory.size());

        // Record player position history for accurate world-X interpolation
        // This correctly handles speed changes from speed portals
        if (currentTime > 0.0f) {
            if (!fields->m_posHistory.empty() && currentTime < fields->m_posHistory.back().first) {
                // Time went backwards (reset/respawn), trim future entries
                auto it = std::lower_bound(fields->m_posHistory.begin(), fields->m_posHistory.end(), currentTime,
                    [](const std::pair<float,float>& p, float t) { return p.first < t; });
                fields->m_posHistory.erase(it, fields->m_posHistory.end());
            }
            if (fields->m_posHistory.empty() || currentTime > fields->m_posHistory.back().first + 0.001f) {
                fields->m_posHistory.push_back({currentTime, playerX});
            }
        }

        float bottomY = static_cast<float>(fields->m_barY);
        float barH = static_cast<float>(fields->m_barHeight);

        // Create dot nodes once (positions are computed below each frame)
        if (!fields->m_dotsCreated && !fields->m_presses.empty()) {
            for (size_t i = 0; i < fields->m_presses.size(); i++) {
                auto& press = fields->m_presses[i];
                ccColor4B color = (press.player == 1) ? fields->m_p1Color : fields->m_p2Color;
                auto dot = CCLayerColor::create(color, 1.0f, barH);
                dot->setAnchorPoint({0.0f, 0.5f});
                dot->ignoreAnchorPointForPosition(false);
                dot->setVisible(false);
                fields->m_circleLayer->addChild(dot);
                fields->m_dots.push_back({i, dot});
            }
            fields->m_dotsCreated = true;
            log::info("[InputCircles] Created {} dot nodes", fields->m_presses.size());
        }

        // Interpolation helper: map a time value to a world X using recorded history.
        // Uses binary search + linear interpolation between samples, and extrapolates
        // at the edges using the local speed derivative.
        auto& history = fields->m_posHistory;
        auto interpolateWorldX = [&history](float time) -> std::optional<float> {
            if (history.empty()) return std::nullopt;
            if (time <= history.front().first) {
                // Extrapolate backward from earliest data
                if (history.size() >= 2) {
                    auto& a = history[0];
                    auto& b = history[1];
                    float dt = b.first - a.first;
                    if (dt > 0.0001f) {
                        float speed = (b.second - a.second) / dt;
                        return a.second + speed * (time - a.first);
                    }
                }
                return history.front().second;
            }
            if (time >= history.back().first) {
                // Extrapolate forward from latest data
                if (history.size() >= 2) {
                    auto& a = history[history.size() - 2];
                    auto& b = history[history.size() - 1];
                    float dt = b.first - a.first;
                    if (dt > 0.0001f) {
                        float speed = (b.second - a.second) / dt;
                        return b.second + speed * (time - b.first);
                    }
                }
                return history.back().second;
            }
            // Binary search for the right interval
            auto it = std::lower_bound(history.begin(), history.end(), time,
                [](const std::pair<float,float>& p, float t) { return p.first < t; });
            if (it == history.begin()) return it->second;
            auto prev = std::prev(it);
            float t0 = prev->first, x0 = prev->second;
            float t1 = it->first, x1 = it->second;
            float frac = (time - t0) / (t1 - t0);
            return x0 + frac * (x1 - x0);
        };

        // Update all dot positions using interpolated world coordinates.
        // Use the player's actual screen position as the anchor point,
        // and compute a pixels-per-world-unit ratio from the object layer.
        // This correctly handles mirror portals because convertToWorldSpace
        // goes through the full transform chain (including negative scaleX).
        double framerate = fields->m_framerate;
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Get player's ACTUAL screen position (we know this is always correct)
        auto playerParent = player->getParent();
        if (dbg) {
            log::info("[DBG] player parent={} objectLayer={}",
                playerParent ? "yes" : "NULL",
                m_objectLayer ? "yes" : "NULL");
            if (playerParent) {
                log::info("[DBG] playerParent pos=({:.2f},{:.2f}) scale=({:.4f},{:.4f})",
                    playerParent->getPositionX(), playerParent->getPositionY(),
                    playerParent->getScaleX(), playerParent->getScaleY());
            }
            if (m_objectLayer) {
                log::info("[DBG] objectLayer pos=({:.2f},{:.2f}) scale=({:.4f},{:.4f})",
                    m_objectLayer->getPositionX(), m_objectLayer->getPositionY(),
                    m_objectLayer->getScaleX(), m_objectLayer->getScaleY());
                // Walk up parent chain
                auto p = m_objectLayer->getParent();
                int depth = 0;
                while (p && depth < 5) {
                    log::info("[DBG]   objectLayer parent[{}] pos=({:.2f},{:.2f}) scale=({:.4f},{:.4f})",
                        depth, p->getPositionX(), p->getPositionY(),
                        p->getScaleX(), p->getScaleY());
                    p = p->getParent();
                    depth++;
                }
            }
        }

        CCPoint playerScreenPos = playerParent ? playerParent->convertToWorldSpace(player->getPosition()) : ccp(playerX, player->getPositionY());
        float playerScreenX = playerScreenPos.x;

        // Compute pixels-per-world-unit from two reference points on the object layer.
        // After a mirror portal, this value becomes NEGATIVE, which correctly flips dot positions.
        CCPoint ref0 = m_objectLayer->convertToWorldSpace(ccp(0.0f, 0.0f));
        CCPoint ref1 = m_objectLayer->convertToWorldSpace(ccp(1000.0f, 0.0f));
        float pixelsPerUnit = (ref1.x - ref0.x) / 1000.0f;

        if (dbg) {
            log::info("[DBG] playerScreenX={:.2f} ref0=({:.2f},{:.2f}) ref1=({:.2f},{:.2f}) ppu={:.6f}",
                playerScreenX, ref0.x, ref0.y, ref1.x, ref1.y, pixelsPerUnit);
        }

        // Fallback: if scale is zero somehow, skip rendering
        if (std::abs(pixelsPerUnit) < 0.0001f) {
            if (dbg) log::info("[DBG] pixelsPerUnit too small, SKIPPING");
            return;
        }

        for (auto& [pressIdx, dot] : fields->m_dots) {
            auto& press = fields->m_presses[pressIdx];
            float timeStart = static_cast<float>(press.framePress) / static_cast<float>(framerate);
            float timeEnd = static_cast<float>(press.frameRelease) / static_cast<float>(framerate);

            auto wxStart = interpolateWorldX(timeStart);
            auto wxEnd = interpolateWorldX(timeEnd);
            if (!wxStart.has_value() || !wxEnd.has_value()) {
                dot->setVisible(false);
                continue;
            }

            // Find the TRUE min/max world X across the entire press duration.
            // This handles cases where the player reverses direction mid-press.
            float minWX = std::min(wxStart.value(), wxEnd.value());
            float maxWX = std::max(wxStart.value(), wxEnd.value());

            float tMin = std::min(timeStart, timeEnd);
            float tMax = std::max(timeStart, timeEnd);
            auto itBegin = std::lower_bound(history.begin(), history.end(), tMin,
                [](const std::pair<float,float>& p, float t) { return p.first < t; });
            auto itEnd = std::upper_bound(history.begin(), history.end(), tMax,
                [](float t, const std::pair<float,float>& p) { return t < p.first; });
            for (auto it = itBegin; it != itEnd; ++it) {
                minWX = std::min(minWX, it->second);
                maxWX = std::max(maxWX, it->second);
            }

            // Convert to screen space relative to the player's known screen position
            float sA = playerScreenX + (minWX - playerX) * pixelsPerUnit;
            float sB = playerScreenX + (maxWX - playerX) * pixelsPerUnit;
            float screenLeft = std::min(sA, sB);
            float screenRight = std::max(sA, sB);

            // Skip dots that are off screen
            if (screenRight < -50.0f || screenLeft > winSize.width + 50.0f) {
                dot->setVisible(false);
                continue;
            }

            float screenWidth = std::max(screenRight - screenLeft, 2.0f);
            dot->setPosition({screenLeft, bottomY});
            dot->setContentSize({screenWidth, barH});
            dot->setVisible(true);

            // Log first 3 visible dots
            if (dbg && pressIdx < 3) {
                log::info("[DBG] dot[{}] minWX={:.2f} maxWX={:.2f} sA={:.2f} sB={:.2f} screenLeft={:.2f} screenW={:.2f}",
                    pressIdx, minWX, maxWX, sA, sB, screenLeft, screenWidth);
            }
        }

        // Update indicator position to follow the player's screen X
        if (fields->m_indicator) {
            fields->m_indicator->setPosition({playerScreenX, bottomY});
            if (dbg) log::info("[DBG] indicator pos=({:.2f},{:.2f})", playerScreenX, bottomY);
        }

        // ========== UR Bar: Process player inputs ==========
        if (fields->m_urBar && !fields->m_replayPressTimes.empty()) {
            float fps = static_cast<float>(fields->m_framerate);
            float veryF = fields->m_veryFrames;
            float elF = fields->m_earlyLateFrames;
            float gF = fields->m_goodFrames;
            float urW = fields->m_urBarWidth;
            float urH = fields->m_urBarHeight;
            float pxPerFrame = urW / (veryF * 2.0f);

            // Process new player inputs since last frame
            while (fields->m_processedInputIdx < s_playerInputs.size()) {
                auto& [inputTime, isPress] = s_playerInputs[fields->m_processedInputIdx];
                fields->m_processedInputIdx++;

                // Find closest unmatched replay event
                auto& replayTimes = isPress ? fields->m_replayPressTimes : fields->m_replayReleaseTimes;
                auto& usedFlags = isPress ? fields->m_replayPressUsed : fields->m_replayReleaseUsed;

                int bestIdx = -1;
                float bestDist = 999999.0f;
                for (size_t i = 0; i < replayTimes.size(); i++) {
                    if (usedFlags[i]) continue;
                    float dist = std::abs(inputTime - replayTimes[i]);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestIdx = static_cast<int>(i);
                    }
                }

                if (bestIdx < 0) continue; // no unmatched replay event

                float offsetSec = inputTime - replayTimes[bestIdx];
                float offsetFrames = offsetSec * fps;

                // Determine judgment
                Judgment j;
                float absOffset = std::abs(offsetFrames);
                if (absOffset <= gF) {
                    j = Judgment::Good;
                } else if (absOffset <= elF) {
                    j = (offsetFrames < 0) ? Judgment::Early : Judgment::Late;
                } else if (absOffset <= veryF) {
                    j = (offsetFrames < 0) ? Judgment::VeryEarly : Judgment::VeryLate;
                } else {
                    j = Judgment::Miss;
                }

                // Mark as used (only if not a miss)
                if (j != Judgment::Miss) {
                    usedFlags[bestIdx] = true;
                }

                // Record judgment
                fields->m_judgmentCounts[static_cast<int>(j)]++;
                fields->m_totalJudgments++;

                // Compute accuracy contribution
                float accValue = 0.0f;
                switch (j) {
                    case Judgment::Good:      accValue = 100.0f; break;
                    case Judgment::Early:
                    case Judgment::Late:      accValue = 33.33f; break;
                    case Judgment::VeryEarly:
                    case Judgment::VeryLate:  accValue = 16.67f; break;
                    case Judgment::Miss:      accValue = 0.0f; break;
                }
                fields->m_totalAccuracy += accValue;

                // Create tick mark on UR bar
                float clampedOffset = std::max(-veryF, std::min(veryF, offsetFrames));
                float tickX = (urW / 2.0f) + clampedOffset * pxPerFrame;
                float tickW = 2.0f;
                float tickH = urH + 6.0f;

                // Tick color based on judgment
                ccColor4B tickColor;
                switch (j) {
                    case Judgment::Good:      tickColor = ccc4(255, 255, 255, 255); break;
                    case Judgment::Early:
                    case Judgment::Late:      tickColor = ccc4(255, 220, 100, 255); break;
                    case Judgment::VeryEarly:
                    case Judgment::VeryLate:  tickColor = ccc4(255, 100, 100, 255); break;
                    case Judgment::Miss:      tickColor = ccc4(180, 0, 0, 255); break;
                }

                auto tick = CCLayerColor::create(tickColor, tickW, tickH);
                tick->setPosition({tickX - tickW / 2.0f, -3.0f});
                fields->m_urBar->addChild(tick, 2);
                fields->m_urTickNodes.push_back(tick);
                fields->m_urTicks.push_back({offsetFrames, j, currentTime});

                if (dbg) {
                    log::info("[UR] input t={:.4f} replay={:.4f} offset={:.2f}f j={} acc={:.2f}%",
                        inputTime, replayTimes[bestIdx], offsetFrames, static_cast<int>(j), accValue);
                }
            }

            // Fade old ticks (older than 3 seconds)
            for (size_t i = 0; i < fields->m_urTicks.size(); i++) {
                float age = currentTime - fields->m_urTicks[i].timeCreated;
                if (age > 3.0f && i < fields->m_urTickNodes.size()) {
                    float opacity = std::max(0.0f, 1.0f - (age - 3.0f) / 2.0f);
                    static_cast<CCLayerColor*>(fields->m_urTickNodes[i])->setOpacity(static_cast<GLubyte>(opacity * 255));
                }
            }

            // Update accuracy label
            if (fields->m_accuracyLabel && fields->m_totalJudgments > 0) {
                float avgAcc = fields->m_totalAccuracy / static_cast<float>(fields->m_totalJudgments);
                auto accText = fmt::format("{:.2f}%", avgAcc);
                fields->m_accuracyLabel->setString(accText.c_str());

                // Color based on accuracy
                if (avgAcc >= 90.0f)      fields->m_accuracyLabel->setColor(ccc3(50, 255, 80));
                else if (avgAcc >= 60.0f) fields->m_accuracyLabel->setColor(ccc3(255, 200, 50));
                else                      fields->m_accuracyLabel->setColor(ccc3(255, 60, 60));
            }
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();

        auto fields = m_fields.self();
        if (!fields->m_active) return;

        s_rhythmActive = false;

        // Log final stats
        float finalAcc = (fields->m_totalJudgments > 0)
            ? fields->m_totalAccuracy / static_cast<float>(fields->m_totalJudgments)
            : 0.0f;
        log::info("[UR] Level complete! Accuracy: {:.2f}% ({} judgments)", finalAcc, fields->m_totalJudgments);
        log::info("[UR] Good:{} Early:{} Late:{} VeryEarly:{} VeryLate:{} Miss:{}",
            fields->m_judgmentCounts[static_cast<int>(Judgment::Good)],
            fields->m_judgmentCounts[static_cast<int>(Judgment::Early)],
            fields->m_judgmentCounts[static_cast<int>(Judgment::Late)],
            fields->m_judgmentCounts[static_cast<int>(Judgment::VeryEarly)],
            fields->m_judgmentCounts[static_cast<int>(Judgment::VeryLate)],
            fields->m_judgmentCounts[static_cast<int>(Judgment::Miss)]);

        if (fields->m_isReplayMode) {
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
        }
    }

    // Helper: format time in MM:SS
    static std::string formatTime(float seconds) {
        int mins = static_cast<int>(seconds) / 60;
        int secs = static_cast<int>(seconds) % 60;
        return fmt::format("{:02d}:{:02d}", mins, secs);
    }

    // ========== Replay Player Controls ==========
    void onReplayPlayPause(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode) return;

        fields->m_isPaused = !fields->m_isPaused;
        if (fields->m_isPaused) {
            fields->m_pauseTime = static_cast<float>(m_timePlayed);
        }
        if (!fields->m_isSeeking) {
            float scale = fields->m_isPaused ? 0.0f : g_replayPlayer.playbackSpeed;
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(scale);
        }
        applyReplayAudioSync(static_cast<float>(m_timePlayed), true);
    }

    void onReplaySpeedUp(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode) return;

        g_replayPlayer.playbackSpeed = std::min(4.0f, g_replayPlayer.playbackSpeed + 0.25f);
        if (!fields->m_isPaused && !fields->m_isSeeking) {
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(g_replayPlayer.playbackSpeed);
        }
        applyReplayAudioSync(static_cast<float>(m_timePlayed), true);
    }

    void onReplaySpeedDown(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode) return;

        g_replayPlayer.playbackSpeed = std::max(0.25f, g_replayPlayer.playbackSpeed - 0.25f);
        if (!fields->m_isPaused && !fields->m_isSeeking) {
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(g_replayPlayer.playbackSpeed);
        }
        applyReplayAudioSync(static_cast<float>(m_timePlayed), true);
    }

    void onReplayRewind(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode || !g_replayPlayer.replay.has_value()) return;

        float currentTime = static_cast<float>(m_timePlayed);
        float target = std::max(0.0f, currentTime - 5.0f);

        fields->m_isPaused = false;
        fields->m_nextInputIdx = 0;

        if (target <= 0.05f) {
            fields->m_isSeeking = false;
            fields->m_seekTargetTime = 0.0f;
            this->resetLevel();
            applyReplayAudioSync(0.0f, true);
            return;
        }

        fields->m_isSeeking = true;
        fields->m_seekTargetTime = target;
        fields->m_seekResumeSpeed = g_replayPlayer.playbackSpeed;
        this->resetLevel();
        applyReplayAudioSync(0.0f, true);
    }

    void onReplaySkipForward(CCObject*) {
        auto fields = m_fields.self();
        if (!fields->m_isReplayMode || !g_replayPlayer.replay.has_value()) return;

        auto& replay = g_replayPlayer.replay.value();
        float totalTime = replay.framerate > 0
            ? static_cast<float>(replay.presses.back().frameRelease) / static_cast<float>(replay.framerate)
            : 0.0f;
        float currentTime = static_cast<float>(m_timePlayed);
        float target = std::min(totalTime, currentTime + 5.0f);

        fields->m_isPaused = false;
        fields->m_isSeeking = true;
        fields->m_seekTargetTime = target;
        fields->m_seekResumeSpeed = g_replayPlayer.playbackSpeed;
        applyReplayAudioSync(target, true);
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        auto fields = m_fields.self();
        if (fields->m_isReplayMode) {
            this->togglePracticeMode(true);
            fields->m_nextInputIdx = 0;
            fields->m_replayCurrentTime = 0.0f;
            fields->m_pauseTime = 0.0f;
            fields->m_replayHoldingJump = false;
            float scale = fields->m_isSeeking ? 4.0f : (fields->m_isPaused ? 0.0f : g_replayPlayer.playbackSpeed);
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(scale);
            fields->m_lastAudioSyncMs = -1.0f;
            fields->m_audioPausedByReplay = false;
            fields->m_audioMusicID = -1;
            fields->m_musicBootstrapTried = false;
            fields->m_nextMusicRetryAt = 0.25f;
            fields->m_nextAudioDebugAt = 0.0f;
            fields->m_lastAutoSeekAt = -1000.0f;
            fields->m_replayMusicInitialOffset = -1;
            fields->m_lastAutoSeekAt = -1000.0f;
            applyReplayAudioSync(static_cast<float>(m_timePlayed), true);
        }

        if (fields->m_active && !fields->m_posHistory.empty()) {
            // Trim history entries past the respawn time so we
            // re-record accurate positions from the checkpoint onward
            float respawnTime = static_cast<float>(m_timePlayed);
            auto it = std::lower_bound(fields->m_posHistory.begin(), fields->m_posHistory.end(), respawnTime,
                [](const std::pair<float,float>& p, float t) { return p.first < t; });
            fields->m_posHistory.erase(it, fields->m_posHistory.end());
        }

        // Reset UR bar state
        if (fields->m_active && fields->m_urBar) {
            // Remove old tick nodes
            for (auto* tick : fields->m_urTickNodes) {
                if (tick) tick->removeFromParent();
            }
            fields->m_urTickNodes.clear();
            fields->m_urTicks.clear();
            fields->m_processedInputIdx = 0;

            // Reset match tracking
            std::fill(fields->m_replayPressUsed.begin(), fields->m_replayPressUsed.end(), false);
            std::fill(fields->m_replayReleaseUsed.begin(), fields->m_replayReleaseUsed.end(), false);

            // Reset accuracy
            for (int i = 0; i < 6; i++) fields->m_judgmentCounts[i] = 0;
            fields->m_totalAccuracy = 0.0f;
            fields->m_totalJudgments = 0;

            if (fields->m_accuracyLabel) {
                fields->m_accuracyLabel->setString("0.00%");
                fields->m_accuracyLabel->setColor(ccc3(255, 255, 255));
            }

            // Clear player inputs
            s_playerInputs.clear();
            s_rhythmActive = true;
        }
    }

    void onQuit() {
        auto fields = m_fields.self();
        if (fields->m_isReplayMode) {
            CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.0f);
            if (auto* audio = FMODAudioEngine::sharedEngine()) {
                for (int musicID = 0; musicID < 32; ++musicID) {
                    int channelID = audio->getMusicChannelID(musicID);
                    if (channelID >= 0) {
                        audio->setChannelPitch(musicID, AudioTargetType::MusicChannel, 1.0f);
                    }
                }
            }
            g_replayPlayer.isActive = false;
            g_replayPlayer.replay.reset();
            g_replayPlayer.playbackSpeed = 1.0f;
            g_replayPlayer.isPaused = false;
            g_replayPlayer.pauseTime = 0.0f;
            g_replayPlayer.inputIndex = 0.0f;
            g_replayPlayer.isEditMode = false;
            g_replayPlayer.levelId = 0;
            g_replayPlayer.sourceReplayPath.clear();
        }

        s_rhythmActive = false;
        s_playerInputs.clear();
        PlayLayer::onQuit();
    }
};

// ============================================================
// EndLevelLayer – show rhythm game results
// ============================================================
class $modify(MyEndLevel, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();

        auto pl = PlayLayer::get();
        if (!pl) return;

        auto fields = static_cast<MyPlayLayer*>(pl)->m_fields.self();
        if (!fields->m_active || fields->m_totalJudgments == 0) return;

        float finalAcc = fields->m_totalAccuracy / static_cast<float>(fields->m_totalJudgments);
        int good = fields->m_judgmentCounts[static_cast<int>(Judgment::Good)];
        int early = fields->m_judgmentCounts[static_cast<int>(Judgment::Early)];
        int late = fields->m_judgmentCounts[static_cast<int>(Judgment::Late)];
        int veryEarly = fields->m_judgmentCounts[static_cast<int>(Judgment::VeryEarly)];
        int veryLate = fields->m_judgmentCounts[static_cast<int>(Judgment::VeryLate)];
        int miss = fields->m_judgmentCounts[static_cast<int>(Judgment::Miss)];

        // Build results string
        auto resultStr = fmt::format(
            "Accuracy: {:.2f}%\n"
            "Good: {}  Early: {}  Late: {}\n"
            "V.Early: {}  V.Late: {}  Miss: {}",
            finalAcc, good, early, late, veryEarly, veryLate, miss);

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Background panel for results
        auto bg = CCLayerColor::create(ccc4(0, 0, 0, 160), 280.0f, 70.0f);
        bg->setPosition({winSize.width / 2.0f - 140.0f, 10.0f});
        bg->setZOrder(100);
        this->addChild(bg);

        auto accText = fmt::format("Rhythm: {:.2f}%", finalAcc);
        auto accLabel = CCLabelBMFont::create(accText.c_str(), "bigFont.fnt");
        accLabel->setScale(0.5f);
        accLabel->setPosition({winSize.width / 2.0f, 65.0f});
        accLabel->setZOrder(101);
        if (finalAcc >= 90.0f)      accLabel->setColor(ccc3(50, 255, 80));
        else if (finalAcc >= 60.0f) accLabel->setColor(ccc3(255, 200, 50));
        else                        accLabel->setColor(ccc3(255, 60, 60));
        this->addChild(accLabel);

        // Breakdown label
        auto breakLabel = CCLabelBMFont::create(resultStr.c_str(), "chatFont.fnt");
        breakLabel->setScale(0.6f);
        breakLabel->setPosition({winSize.width / 2.0f, 35.0f});
        breakLabel->setZOrder(101);
        breakLabel->setColor(ccc3(220, 220, 220));
        this->addChild(breakLabel);
    }
};