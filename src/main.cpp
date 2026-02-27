#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <optional>
#include <map>

#ifdef GEODE_IS_WINDOWS
#include <commdlg.h>
#include <shlobj.h>
#endif

using namespace geode::prelude;

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
// PlayLayer Hook – Show input circles in-game
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

// Try to find a matching replay for the given level ID
// 1. Search locally by filename pattern: *-{levelId}.json
// 2. If not found locally, query Supabase
static std::optional<ReplayLoadResult> loadMatchingReplay(int levelId) {
    // --- Local search first ---
    auto replaysDir = Mod::get()->getSaveDir() / "replay";
    if (std::filesystem::exists(replaysDir)) {
        auto suffix = fmt::format("-{}.json", levelId);
        std::filesystem::path matchedFile;
        int matchCount = 0;

        for (auto& entry : std::filesystem::directory_iterator(replaysDir)) {
            if (!entry.is_regular_file()) continue;
            auto filename = entry.path().filename().string();
            if (filename.size() <= suffix.size()) continue;
            if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
                matchCount++;
                matchedFile = entry.path();
                if (matchCount > 1) break;
            }
        }

        if (matchCount == 1) {
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

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        std::vector<ReplayPress> m_presses;
        CCNode* m_circleLayer = nullptr;
        CCNode* m_indicator = nullptr; // hollow square following the player
        double m_framerate = 240.0;
        bool m_active = false;
        bool m_allPlaced = false;
        // Settings
        int m_barHeight = 10;
        int m_barY = 15;
        ccColor4B m_p1Color = ccc4(50, 255, 80, 200);
        ccColor4B m_p2Color = ccc4(80, 130, 255, 200);
        ccColor4B m_indicatorColor = ccc4(255, 255, 255, 220);
        ccColor4B m_bgColor = ccc4(0, 0, 0, 120);
        CCLayerColor* m_bgStrip = nullptr;
        // Store world X for each placed dot so we can update screen positions each frame
        std::vector<std::tuple<float, float, CCNode*>> m_dots; // (worldXStart, worldXEnd, dotNode)
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        bool setting = Mod::get()->getSettingValue<bool>("show-inputs-ingame");
        log::info("[InputCircles] Setting show-inputs-ingame = {}", setting);
        if (!setting) return true;

        int levelId = level->m_levelID;
        log::info("[InputCircles] Level ID = {}", levelId);

        auto loadResult = loadMatchingReplay(levelId);
        if (!loadResult.has_value()) {
            log::info("[InputCircles] No matching replay found for level {}", levelId);
            return true;
        }

        log::info("[InputCircles] Loaded {} presses, framerate={}", loadResult->presses.size(), loadResult->framerate);

        auto fields = m_fields.self();
        fields->m_presses = std::move(loadResult->presses);
        fields->m_framerate = loadResult->framerate;
        fields->m_active = true;

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
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto bgStrip = CCLayerColor::create(bgc, winSize.width, initBarH);
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

        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto fields = m_fields.self();
        if (!fields->m_active || !fields->m_circleLayer) return;

        auto player = m_player1;
        if (!player) return;

        // Current time and frame
        float playerX = player->getPositionX();

        // Settings values
        float bottomY = static_cast<float>(fields->m_barY);
        float barH = static_cast<float>(fields->m_barHeight);

        // Once we have real speed data, place ALL dots at once
        if (!fields->m_allPlaced && m_timePlayed > 0.1 && playerX > 10.0f) {
            // Estimate speed from current movement
            float speed = playerX / static_cast<float>(m_timePlayed);

            log::info("[InputCircles] Placing all {} dots at once, speed={:.1f} units/sec", 
                fields->m_presses.size(), speed);

            for (auto& press : fields->m_presses) {
                float worldXStart = static_cast<float>(press.framePress) / static_cast<float>(fields->m_framerate) * speed;
                float worldXEnd = static_cast<float>(press.frameRelease) / static_cast<float>(fields->m_framerate) * speed;

                ccColor4B color = (press.player == 1)
                    ? fields->m_p1Color
                    : fields->m_p2Color;

                auto dot = CCLayerColor::create(color, 1.0f, barH);
                dot->setAnchorPoint({0.0f, 0.5f});
                dot->ignoreAnchorPointForPosition(false);
                fields->m_circleLayer->addChild(dot);

                fields->m_dots.push_back({worldXStart, worldXEnd, dot});
            }
            fields->m_allPlaced = true;
        }

        // Update ALL dot positions every frame to follow the level scroll
        auto objLayerPos = m_objectLayer->getPosition();
        float objScale = m_objectLayer->getScale();

        for (auto& [worldXStart, worldXEnd, dot] : fields->m_dots) {
            float screenXStart = worldXStart * objScale + objLayerPos.x;
            float screenXEnd = worldXEnd * objScale + objLayerPos.x;
            float screenWidth = std::max(screenXEnd - screenXStart, 2.0f);
            dot->setPosition({screenXStart, bottomY});
            dot->setContentSize({screenWidth, barH});
        }

        // Update indicator position to follow the player's screen X
        if (fields->m_indicator) {
            float screenPlayerX = playerX * objScale + objLayerPos.x;
            fields->m_indicator->setPosition({screenPlayerX, bottomY});
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // Don't clear dots — they use world coordinates and are
        // repositioned every frame via m_objectLayer transform,
        // so they stay aligned after practice-mode respawns.
    }
};