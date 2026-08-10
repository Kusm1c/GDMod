#include "leveldata.hpp"
#include "network.hpp"
#include "../vendor/miniz/miniz.h"
#include <vector>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace gdapp {

// On-disk cache: once a level's data is decoded, it's saved next to the exe
// so replaying it later (even across app restarts) never touches the network
// again. Format: first line is the level name, the rest is the raw level
// string (which never itself contains a literal newline).
static std::filesystem::path cacheFile(int levelId) {
    return std::filesystem::path("cache") / (std::to_string(levelId) + ".txt");
}

static bool loadFromCache(int levelId, LevelFetchResult& out) {
    std::ifstream f(cacheFile(levelId), std::ios::binary);
    if (!f) return false;
    std::string name;
    if (!std::getline(f, name)) return false;
    std::ostringstream rest;
    rest << f.rdbuf();
    std::string body = rest.str();
    if (body.empty()) return false;
    out.success = true;
    out.fromCache = true;
    out.name = name;
    out.levelString = body;
    return true;
}

static void saveToCache(int levelId, const std::string& name, const std::string& levelString) {
    std::error_code ec;
    std::filesystem::create_directories("cache", ec);
    std::ofstream f(cacheFile(levelId), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << (name.empty() ? "(untitled)" : name) << "\n" << levelString;
}

static std::vector<uint8_t> base64UrlDecode(const std::string& in) {
    // Accepts BOTH the URL-safe alphabet (-_, what RobTop's server normally
    // sends for field "4") and the standard one (+/), mapped to the same two
    // values — defensive against any level where the server's encoding isn't
    // consistent, at zero cost for the normal case.
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) table[i] = -1;
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) table[(uint8_t)alphabet[i]] = (int8_t)i;
        table[(uint8_t)'-'] = 62; // url-safe '+'
        table[(uint8_t)'_'] = 63; // url-safe '/'
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4 + 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        int8_t d = table[c];
        if (d < 0) continue; // skip anything unexpected rather than fail outright
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// RobTop's stored level data isn't consistently wrapped: depending on which
// client/version last saved the level, field "4" can come back as gzip
// (1f 8b, what GD's own client normally produces), raw zlib (78 xx, seen on
// at least one real level during testing — 78 9c is zlib's "default
// compression" header), or in principle bare/raw DEFLATE with no wrapper at
// all. Detect the actual format from the magic bytes instead of assuming one.
static bool gzipInflate(const std::vector<uint8_t>& d, std::string& out, std::string& reason) {
    if (d.size() < 6) { reason = "decoded data too short (" + std::to_string(d.size()) + " bytes)"; return false; }

    const uint8_t* deflateStart = nullptr;
    size_t deflateLen = 0;
    int tinflFlags = 0;

    if (d[0] == 0x1f && d[1] == 0x8b && d[2] == 0x08) {
        // gzip: 10-byte base header (+ optional FEXTRA/FNAME/FCOMMENT/FHCRC
        // fields per the FLG byte), then raw DEFLATE, then an 8-byte trailer.
        uint8_t flg = d[3];
        size_t pos = 10;
        if (flg & 0x04) { // FEXTRA
            if (pos + 2 > d.size()) { reason = "truncated FEXTRA field"; return false; }
            uint16_t xlen = d[pos] | (d[pos + 1] << 8);
            pos += 2 + xlen;
        }
        if (flg & 0x08) { while (pos < d.size() && d[pos] != 0) pos++; pos++; } // FNAME
        if (flg & 0x10) { while (pos < d.size() && d[pos] != 0) pos++; pos++; } // FCOMMENT
        if (flg & 0x02) pos += 2;                                               // FHCRC
        if (pos >= d.size() - 8) { reason = "gzip header consumed the whole buffer (flg=" + std::to_string(flg) + ")"; return false; }
        deflateStart = d.data() + pos;
        deflateLen = d.size() - 8 - pos;
    } else if (d[0] == 0x78) {
        // zlib: 2-byte header (CMF/FLG), raw DEFLATE, 4-byte Adler-32 trailer.
        // tinfl can parse the header itself and stop cleanly at the deflate
        // stream's own end marker, so just hand it the whole thing.
        deflateStart = d.data();
        deflateLen = d.size();
        tinflFlags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    } else {
        // Last resort: assume it's already a bare DEFLATE stream.
        deflateStart = d.data();
        deflateLen = d.size();
    }

    size_t outLen = 0;
    void* decompressed = tinfl_decompress_mem_to_heap(deflateStart, deflateLen, &outLen, tinflFlags);
    if (!decompressed) {
        char buf[96];
        snprintf(buf, sizeof(buf), "tinfl_decompress_mem_to_heap failed (magic %02x %02x, stream corrupt/truncated)", d[0], d[1]);
        reason = buf;
        return false;
    }
    out.assign((const char*)decompressed, outLen);
    mz_free(decompressed);
    return true;
}

// RobTop's raw protocol response: "1:<id>:2:<name>:3:...:4:<data>:5:...#hash1#hash2"
// Split on ':' and pull out the value following key "4" (the level data blob).
static bool extractField(const std::string& resp, const std::string& key, std::string& value) {
    size_t hashPos = resp.find('#');
    std::string body = (hashPos == std::string::npos) ? resp : resp.substr(0, hashPos);

    std::vector<std::string> tokens;
    size_t start = 0;
    for (size_t i = 0; i <= body.size(); i++) {
        if (i == body.size() || body[i] == ':') {
            tokens.push_back(body.substr(start, i - start));
            start = i + 1;
        }
    }
    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == key) { value = tokens[i + 1]; return true; }
    }
    return false;
}

LevelFetchResult fetchLevel(int levelId) {
    LevelFetchResult result;

    if (loadFromCache(levelId, result)) return result;

    std::string body = "levelID=" + std::to_string(levelId) + "&secret=Wmfd2893gb7";
    HttpResult http = httpPost(L"www.boomlings.com", 80, L"/database/downloadGJLevel22.php",
                                body, L"application/x-www-form-urlencoded", false);
    if (!http.ok) { result.error = "network request failed: " + http.error; return result; }

    if (http.body == "-1") { result.error = "level not found (server returned -1)"; return result; }
    if (http.body.empty()) { result.error = "empty server response"; return result; }
    if (http.body.rfind("error code:", 0) == 0) {
        result.error = "rate limited by RobTop's servers (Cloudflare " + http.body.substr(11)
                      + ") - wait a few minutes before trying again";
        result.rateLimited = true;
        return result;
    }

    std::string dataField;
    if (!extractField(http.body, "4", dataField)) {
        result.error = "couldn't find level data field in server response (response was "
                      + std::to_string(http.body.size()) + " bytes, starts with \""
                      + http.body.substr(0, 40) + "\")";
        return result;
    }
    extractField(http.body, "2", result.name);

    std::vector<uint8_t> gz = base64UrlDecode(dataField);
    std::string reason;
    if (!gzipInflate(gz, result.levelString, reason)) {
        char hexPrefix[3 * 8 + 1] = {0};
        for (size_t i = 0; i < gz.size() && i < 8; i++)
            snprintf(hexPrefix + i * 3, 4, "%02x ", gz[i]);
        result.error = "gzip decompression failed: " + reason
                      + " | field4 chars=" + std::to_string(dataField.size())
                      + " decoded bytes=" + std::to_string(gz.size())
                      + " first bytes=" + hexPrefix;
        return result;
    }

    result.success = true;
    saveToCache(levelId, result.name, result.levelString);
    return result;
}

std::vector<CachedLevelEntry> listCachedLevels() {
    std::vector<std::pair<std::filesystem::file_time_type, CachedLevelEntry>> withTime;

    std::error_code ec;
    if (!std::filesystem::exists("cache", ec)) return {};

    for (auto& entry : std::filesystem::directory_iterator("cache", ec)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".txt") continue;

        int id = 0;
        try { id = std::stoi(path.stem().string()); } catch (...) { continue; }

        std::ifstream f(path, std::ios::binary);
        std::string name;
        if (!f || !std::getline(f, name)) continue;

        withTime.push_back({entry.last_write_time(ec), CachedLevelEntry{id, name}});
    }

    std::sort(withTime.begin(), withTime.end(),
              [](auto& a, auto& b) { return a.first > b.first; }); // newest first

    std::vector<CachedLevelEntry> out;
    out.reserve(withTime.size());
    for (auto& [t, e] : withTime) out.push_back(e);
    return out;
}

} // namespace gdapp
