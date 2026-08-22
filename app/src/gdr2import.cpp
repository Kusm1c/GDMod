#include "gdr2import.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <utility>

namespace gdapp {

namespace {

// ---- byte reader (identical logic to src/replay_core.cpp's GDR2Reader) ----
class Gdr2Reader {
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;

public:
    Gdr2Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool empty() const { return m_pos >= m_size; }
    size_t remaining() const { return m_size > m_pos ? m_size - m_pos : 0; }

    bool readRaw(void* out, size_t size) {
        if (remaining() < size) return false;
        std::memcpy(out, m_data + m_pos, size);
        m_pos += size;
        return true;
    }

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

    bool readString(std::string& result) {
        size_t len;
        if (!readSize(len)) return false;
        if (len > 0xFFFF || remaining() < len) return false;
        result = std::string(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return true;
    }

    bool readFloat(float& result) {
        if (remaining() < 4) return false;
        uint8_t bytes[4];
        readRaw(bytes, 4);
        std::reverse(bytes, bytes + 4);
        std::memcpy(&result, bytes, 4);
        return true;
    }

    bool readDouble(double& result) {
        if (remaining() < 8) return false;
        uint8_t bytes[8];
        readRaw(bytes, 8);
        std::reverse(bytes, bytes + 8);
        std::memcpy(&result, bytes, 8);
        return true;
    }

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

struct Gdr2Input {
    uint64_t frame;
    uint8_t button; // 1=Jump, 2=Left, 3=Right
    bool player2;
    bool down;
};

struct Gdr2Raw {
    int version = 0;
    std::string inputTag;
    bool platformer = false;
    uint32_t levelId = 0;
    std::string levelName;
    double framerate = 240.0;
    std::vector<Gdr2Input> inputs;
};

// Ported 1:1 from src/replay_core.cpp's parseGDR2 (see that file for the
// annotated original) — only the return type changed (bool + error string
// instead of Geode's Result<T>).
bool parseGdr2Bytes(const std::vector<uint8_t>& data, Gdr2Raw& out, std::string& err) {
    Gdr2Reader reader(data.data(), data.size());

    char magic[3];
    if (!reader.readRaw(magic, 3)) { err = "Failed to read magic bytes"; return false; }
    if (std::string_view(magic, 3) != "GDR") { err = "Not a GDR2 file (invalid magic header)"; return false; }

    if (!reader.readInt(out.version)) { err = "Failed to read format version"; return false; }
    if (!reader.readString(out.inputTag)) { err = "Failed to read input tag"; return false; }

    std::string author, description, botName;
    float duration; int gameVersion; int seed; int coins; bool ldm; int botVersion;
    if (!reader.readString(author)) { err = "Failed to read author"; return false; }
    if (!reader.readString(description)) { err = "Failed to read description"; return false; }
    if (!reader.readFloat(duration)) { err = "Failed to read duration"; return false; }
    if (!reader.readInt(gameVersion)) { err = "Failed to read game version"; return false; }
    if (!reader.readDouble(out.framerate)) { err = "Failed to read framerate"; return false; }
    if (!reader.readInt(seed)) { err = "Failed to read seed"; return false; }
    if (!reader.readInt(coins)) { err = "Failed to read coins"; return false; }
    if (!reader.readBool(ldm)) { err = "Failed to read LDM flag"; return false; }
    if (!reader.readBool(out.platformer)) { err = "Failed to read platformer flag"; return false; }
    if (!reader.readString(botName)) { err = "Failed to read bot name"; return false; }
    if (!reader.readInt(botVersion)) { err = "Failed to read bot version"; return false; }
    if (!reader.readUint32(out.levelId)) { err = "Failed to read level ID"; return false; }
    if (!reader.readString(out.levelName)) { err = "Failed to read level name"; return false; }

    size_t extSize;
    if (!reader.readSize(extSize)) { err = "Failed to read extension size"; return false; }
    if (!reader.skip(extSize)) { err = "Failed to skip extension data"; return false; }

    size_t deathCount;
    if (!reader.readSize(deathCount)) { err = "Failed to read death count"; return false; }
    uint64_t prevDeath = 0;
    for (size_t i = 0; i < deathCount; i++) {
        uint64_t delta;
        if (!reader.readVarint(delta)) { err = "Failed to read death frame delta"; return false; }
        prevDeath += delta;
    }

    size_t inputCount;
    if (!reader.readSize(inputCount)) { err = "Failed to read input count"; return false; }
    size_t p1InputCount;
    if (!reader.readSize(p1InputCount)) { err = "Failed to read P1 input count"; return false; }
    if (p1InputCount > inputCount) { err = "P1 input count exceeds total input count"; return false; }

    bool hasInputExt = !out.inputTag.empty();
    uint64_t frame = 0;
    size_t p1Read = 0;

    for (size_t i = 0; i < inputCount && !reader.empty(); i++) {
        uint64_t packed;
        if (!reader.readVarint(packed)) { err = "Failed to read packed input"; return false; }

        Gdr2Input input{};
        if (out.platformer) {
            input.down = packed & 1;
            input.button = (uint8_t)((packed >> 1) & 3);
            uint64_t delta = packed >> 3;
            input.frame = delta + frame;
        } else {
            input.down = packed & 1;
            input.button = 1;
            uint64_t delta = packed >> 1;
            input.frame = delta + frame;
        }
        input.player2 = (p1Read >= p1InputCount);

        if (hasInputExt) {
            size_t inputExtSize;
            if (!reader.readSize(inputExtSize)) { err = "Failed to read input extension size"; return false; }
            if (!reader.skip(inputExtSize)) { err = "Failed to skip input extension data"; return false; }
        }

        out.inputs.push_back(input);
        frame = input.frame;

        if (p1Read < p1InputCount) {
            p1Read++;
            if (p1Read == p1InputCount) frame = 0; // reset delta for P2 inputs
        }
    }

    std::sort(out.inputs.begin(), out.inputs.end(),
              [](const Gdr2Input& a, const Gdr2Input& b) { return a.frame < b.frame; });
    return true;
}

} // namespace

Gdr2ImportResult importGdr2(const std::string& path) {
    Gdr2ImportResult result;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) { result.error = "Could not open file"; return result; }
    std::streamsize size = f.tellg();
    if (size <= 0) { result.error = "Empty file"; return result; }
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) {
        result.error = "Failed to read file";
        return result;
    }

    Gdr2Raw raw;
    if (!parseGdr2Bytes(bytes, raw, result.error)) return result;

    // Frame numbers in the file are ticks at the FILE's own declared
    // framerate, not necessarily 240 — the real mod converts via elapsed
    // wall-clock time * framerate (hooks_play.cpp's processCommands: `f =
    // round(stepTime * fr)`), not a bare frame-index copy. gdsim/this app's
    // replay is always fixed-240fps (FIXED_DT), so rescale every frame
    // number to 240fps-equivalent up front — skipping this silently
    // corrupts timing throughout the whole replay for any file recorded at
    // a framerate other than 240 (found 2026-08-18: a real .gdr2 that
    // cleared in-game didn't clear on import — this was why).
    const double toFps240 = (raw.framerate > 0.0) ? (240.0 / raw.framerate) : 1.0;
    auto rescale = [&](uint64_t f) -> uint64_t {
        return (uint64_t)std::llround((double)f * toFps240);
    };

    // Press/release pairing — ported from replay_core.cpp's
    // parseReplayEventsFromJsonArray: a "press" while the button is already
    // held is a redundant no-op in GD's input model, not a new hold (a real
    // GDR2 can contain such duplicates; overwriting the start frame here
    // would shorten the hold and desync frame-perfect sections). Only Jump
    // (button==1), player 1 — this app's replay model has no concept of
    // Left/Right or a second player.
    uint64_t heldSince = 0;
    bool held = false;
    for (const auto& in : raw.inputs) {
        if (in.button != 1 || in.player2) continue;
        uint64_t frame = rescale(in.frame);
        if (in.down) {
            if (!held) { heldSince = frame; held = true; }
        } else if (held) {
            uint64_t releaseFrame = std::max(frame, heldSince + 1);
            result.presses.push_back({heldSince, releaseFrame});
            held = false;
        }
    }
    if (held) result.presses.push_back({heldSince, heldSince + 1});

    std::sort(result.presses.begin(), result.presses.end(),
              [](const Gdr2Press& a, const Gdr2Press& b) { return a.framePress < b.framePress; });

    result.levelId = raw.levelId;
    result.levelName = raw.levelName;
    result.framerate = raw.framerate;
    result.success = true;
    return result;
}

} // namespace gdapp
