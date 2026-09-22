#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GMB — a compact binary container for GDMod's per-frame captures.
//
// The move-trigger capture reached 36 GB on this machine (two single levels at
// 17 and 16 GB). As plain text each row costs ~60 bytes to carry values that are
// almost entirely redundant: the frame number repeats, the object id repeats,
// and the positions change by a fraction of a unit between consecutive rows.
//
// This encodes the same rows as:
//   * fixed-point integers at a per-column scale (positions to 1/1024 of a unit
//     is already finer than the 3-decimal quantisation the engine itself uses)
//   * DELTA against that column's previous value, so a slow-moving object costs
//     one or two bytes per field instead of eight characters
//   * zig-zag + LEB128 varints, so small deltas — the overwhelming majority —
//     occupy a single byte
//
// Deliberately not human-readable; `test/gmbdecode.exe` prints the original text
// form, and Reader below is what tooling should use directly.
//
// A column may also be declared RAW (scale 0), which stores the full IEEE double
// unchanged. That exists so a capture needing exact round-trips — the jump probe
// depends on 17-digit precision — can use this container without silently losing
// the precision it was built to preserve.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gdsim::gmb {

// A capture kind, so a decoder can label columns without being told.
enum Kind : uint8_t { KIND_MOVETRIGGERS = 1, KIND_PHYSICS = 2, KIND_JUMP = 3 };

struct Column {
    std::string name;
    // Fixed-point divisor: the stored integer is round(value * scale). 0 = store
    // the raw double instead (no quantisation, 8 bytes).
    double scale = 1.0;
};

namespace detail {

inline void putVarint(std::string& out, uint64_t v) {
    while (v >= 0x80) { out.push_back((char)(uint8_t)(v | 0x80)); v >>= 7; }
    out.push_back((char)(uint8_t)v);
}
inline uint64_t zig(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
inline int64_t  unzig(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

inline bool getVarint(const uint8_t* p, size_t n, size_t& i, uint64_t& out) {
    out = 0; int shift = 0;
    while (i < n) {
        uint8_t b = p[i++];
        out |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

} // namespace detail

// ── Writer ───────────────────────────────────────────────────────────────────
// Buffers into a string and flushes periodically; the caller controls when.
class Writer {
public:
    bool open(const std::string& path, Kind kind, uint32_t levelId,
              const std::vector<Column>& cols) {
        close();
        // Append, like the text captures did, so repeated attempts on one level
        // accumulate instead of the last run wiping the earlier ones. A file that
        // already exists keeps its header; only a fresh one gets a new one.
        bool fresh = true;
        if (FILE* probe = std::fopen(path.c_str(), "rb")) { fresh = false; std::fclose(probe); }
        f_ = std::fopen(path.c_str(), "ab");
        if (!f_) return false;
        cols_ = cols;
        prev_.assign(cols.size(), 0);
        havePrev_ = false;
        if (fresh) {
            std::string h;
            h.append("GMB1", 4);
            h.push_back((char)kind);
            for (int i = 0; i < 4; i++) h.push_back((char)((levelId >> (8 * i)) & 0xff));
            detail::putVarint(h, cols.size());
            for (auto& c : cols) {
                detail::putVarint(h, c.name.size());
                h += c.name;
                double s = c.scale;
                char buf[8]; std::memcpy(buf, &s, 8);
                h.append(buf, 8);
            }
            std::fwrite(h.data(), 1, h.size(), f_);
        }
        return true;
    }

    // Marks a new attempt AND resets the delta baseline: rows across an attempt
    // boundary are unrelated, so delta-coding across it would store large
    // meaningless jumps.
    void attempt() { if (!f_) return; buf_.push_back((char)0x00); havePrev_ = false; }

    void row(const double* vals) {
        if (!f_) return;
        buf_.push_back((char)0x01);
        for (size_t i = 0; i < cols_.size(); i++) {
            if (cols_[i].scale == 0.0) {
                char b[8]; std::memcpy(b, &vals[i], 8); buf_.append(b, 8);
            } else {
                const int64_t q = (int64_t)std::llround(vals[i] * cols_[i].scale);
                detail::putVarint(buf_, detail::zig(havePrev_ ? q - prev_[i] : q));
                prev_[i] = q;
            }
        }
        havePrev_ = true;
        if (buf_.size() >= (1u << 16)) flush();
    }

    void flush() {
        if (f_ && !buf_.empty()) { std::fwrite(buf_.data(), 1, buf_.size(), f_); buf_.clear(); }
        if (f_) std::fflush(f_);
    }
    void close() { if (f_) { flush(); std::fclose(f_); f_ = nullptr; } }
    bool isOpen() const { return f_ != nullptr; }
    ~Writer() { close(); }

private:
    FILE* f_ = nullptr;
    std::vector<Column> cols_;
    std::vector<int64_t> prev_;
    bool havePrev_ = false;
    std::string buf_;
};

// ── Reader ───────────────────────────────────────────────────────────────────
class Reader {
public:
    bool open(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        data_.resize((size_t)(n > 0 ? n : 0));
        if (n > 0 && std::fread(data_.data(), 1, (size_t)n, f) != (size_t)n) { std::fclose(f); return false; }
        std::fclose(f);

        if (data_.size() < 9 || std::memcmp(data_.data(), "GMB1", 4) != 0) return false;
        size_t i = 4;
        kind_ = (Kind)data_[i++];
        levelId_ = 0;
        for (int k = 0; k < 4; k++) levelId_ |= (uint32_t)data_[i++] << (8 * k);

        uint64_t nc = 0;
        if (!detail::getVarint(data_.data(), data_.size(), i, nc)) return false;
        cols_.clear();
        for (uint64_t c = 0; c < nc; c++) {
            uint64_t len = 0;
            if (!detail::getVarint(data_.data(), data_.size(), i, len)) return false;
            if (i + len + 8 > data_.size()) return false;
            Column col;
            col.name.assign((const char*)data_.data() + i, (size_t)len);
            i += (size_t)len;
            std::memcpy(&col.scale, data_.data() + i, 8);
            i += 8;
            cols_.push_back(std::move(col));
        }
        pos_ = i;
        prev_.assign(cols_.size(), 0);
        havePrev_ = false;
        return true;
    }

    // Returns false at end of file. `isAttempt` marks an attempt boundary; `out`
    // is untouched for those.
    bool next(std::vector<double>& out, bool& isAttempt) {
        isAttempt = false;
        if (pos_ >= data_.size()) return false;
        const uint8_t tag = data_[pos_++];
        if (tag == 0x00) { isAttempt = true; havePrev_ = false; return true; }
        if (tag != 0x01) return false;

        out.resize(cols_.size());
        for (size_t c = 0; c < cols_.size(); c++) {
            if (cols_[c].scale == 0.0) {
                if (pos_ + 8 > data_.size()) return false;
                std::memcpy(&out[c], data_.data() + pos_, 8);
                pos_ += 8;
            } else {
                uint64_t raw = 0;
                if (!detail::getVarint(data_.data(), data_.size(), pos_, raw)) return false;
                const int64_t d = detail::unzig(raw);
                const int64_t q = havePrev_ ? prev_[c] + d : d;
                prev_[c] = q;
                out[c] = (double)q / cols_[c].scale;
            }
        }
        havePrev_ = true;
        return true;
    }

    const std::vector<Column>& columns() const { return cols_; }
    Kind kind() const { return kind_; }
    uint32_t levelId() const { return levelId_; }

private:
    std::vector<uint8_t> data_;
    std::vector<Column>  cols_;
    std::vector<int64_t> prev_;
    size_t pos_ = 0;
    bool havePrev_ = false;
    Kind kind_ = KIND_MOVETRIGGERS;
    uint32_t levelId_ = 0;
};

} // namespace gdsim::gmb
