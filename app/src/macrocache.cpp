#include "macrocache.hpp"
#include "../vendor/miniz/miniz.h"
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstring>

namespace gdapp {

static std::filesystem::path macrosDir() {
    return std::filesystem::path("cache") / "macros";
}

static void writeU32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
static void writeU64(std::ofstream& f, uint64_t v) { f.write(reinterpret_cast<const char*>(&v), 8); }
static void writeStr(std::ofstream& f, const std::string& s) {
    writeU32(f, (uint32_t)s.size());
    if (!s.empty()) f.write(s.data(), (std::streamsize)s.size());
}
static bool readU32(std::ifstream& f, uint32_t& v) { f.read(reinterpret_cast<char*>(&v), 4); return (bool)f; }
static bool readU64(std::ifstream& f, uint64_t& v) { f.read(reinterpret_cast<char*>(&v), 8); return (bool)f; }
static bool readStr(std::ifstream& f, std::string& s) {
    uint32_t len;
    if (!readU32(f, len)) return false;
    s.resize(len);
    if (len) f.read(s.data(), len);
    return (bool)f;
}

static constexpr char kMagic[4] = {'M', 'A', 'C', '1'};

bool saveMacroToDisk(const MacroEntry& m) {
    std::error_code ec;
    std::filesystem::create_directories(macrosDir(), ec);
    if (ec) return false;

    // Compress the level string (by far the largest field — a big level's
    // decoded string can be 300KB+, clicks/metadata are negligible by
    // comparison) via miniz's single-call zlib-compatible API — the same
    // library already vendored/linked for decoding fetched levels
    // (leveldata.cpp), just used for writing here instead of reading.
    mz_ulong boundLen = mz_compressBound((mz_ulong)m.levelString.size());
    std::vector<unsigned char> compressed(boundLen);
    mz_ulong compressedLen = boundLen;
    int rc = mz_compress2(compressed.data(), &compressedLen,
                           reinterpret_cast<const unsigned char*>(m.levelString.data()),
                           (mz_ulong)m.levelString.size(), MZ_BEST_COMPRESSION);
    if (rc != MZ_OK) return false;

    std::string safeTime = m.recordedAt;
    for (auto& c : safeTime) if (!isalnum((unsigned char)c)) c = '_';
    std::filesystem::path path = macrosDir() / (std::to_string(m.levelId) + "_" + safeTime + ".macro");

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(kMagic, 4);
    writeU32(f, (uint32_t)m.levelId);
    writeStr(f, m.levelName);
    writeStr(f, m.recordedAt);
    f.write(reinterpret_cast<const char*>(&m.levelEndEstimate), 4);
    writeU32(f, (uint32_t)m.levelString.size()); // uncompressed length, needed to size the inflate buffer
    writeU32(f, (uint32_t)compressedLen);
    f.write(reinterpret_cast<const char*>(compressed.data()), (std::streamsize)compressedLen);
    writeU32(f, (uint32_t)m.clicks.size());
    for (auto& c : m.clicks) { writeU64(f, c.pressFrame); writeU64(f, c.releaseFrame); }
    return f.good();
}

static bool loadOneMacro(const std::filesystem::path& path, MacroEntry& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char magic[4];
    f.read(magic, 4);
    if (!f || std::memcmp(magic, kMagic, 4) != 0) return false;

    uint32_t levelIdU;
    if (!readU32(f, levelIdU)) return false;
    out.levelId = (int)levelIdU;
    if (!readStr(f, out.levelName)) return false;
    if (!readStr(f, out.recordedAt)) return false;
    f.read(reinterpret_cast<char*>(&out.levelEndEstimate), 4);
    if (!f) return false;

    uint32_t uncompressedLen, compressedLen;
    if (!readU32(f, uncompressedLen) || !readU32(f, compressedLen)) return false;
    std::vector<unsigned char> compressed(compressedLen);
    if (compressedLen) f.read(reinterpret_cast<char*>(compressed.data()), compressedLen);
    if (!f) return false;

    out.levelString.resize(uncompressedLen);
    mz_ulong destLen = uncompressedLen;
    int rc = mz_uncompress(reinterpret_cast<unsigned char*>(out.levelString.data()), &destLen,
                            compressed.data(), compressedLen);
    if (rc != MZ_OK || destLen != uncompressedLen) return false;

    uint32_t clickCount;
    if (!readU32(f, clickCount)) return false;
    out.clicks.clear();
    out.clicks.reserve(clickCount);
    for (uint32_t i = 0; i < clickCount; i++) {
        uint64_t pf, rf;
        if (!readU64(f, pf) || !readU64(f, rf)) return false;
        out.clicks.push_back({pf, rf});
    }
    return true;
}

std::vector<MacroEntry> loadMacrosFromDisk() {
    std::vector<MacroEntry> result;
    std::error_code ec;
    if (!std::filesystem::exists(macrosDir(), ec)) return result;
    for (auto& entry : std::filesystem::directory_iterator(macrosDir(), ec)) {
        if (ec) break;
        if (entry.path().extension() != ".macro") continue;
        MacroEntry m{};
        if (loadOneMacro(entry.path(), m)) result.push_back(std::move(m));
    }
    return result;
}

} // namespace gdapp
