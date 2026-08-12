#include "Gdr2Export.hpp"
#include <algorithm>
#include <cstring>

namespace gdsim {

namespace {

// Ported byte-for-byte from src/replay_core.cpp's GDR2Writer (see
// Gdr2Export.hpp for why this is a separate copy, not a shared refactor).
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

} // namespace

std::vector<uint8_t> exportClicksToGdr2(const std::vector<SolverClick>& clicks,
                                         double framerate, int levelId,
                                         const std::string& levelName) {
    struct Event { uint64_t frame; bool down; };
    std::vector<Event> p1Events;
    p1Events.reserve(clicks.size() * 2);
    for (auto& c : clicks) {
        p1Events.push_back({c.pressFrame,   true});
        p1Events.push_back({c.releaseFrame, false});
    }
    std::sort(p1Events.begin(), p1Events.end(),
              [](const Event& a, const Event& b) { return a.frame < b.frame; });

    GDR2Writer w;
    w.writeRaw("GDR", 3);          // magic header
    w.writeVarint(2);              // format version 2
    w.writeString("");             // input tag (empty = non-platformer)
    w.writeString("");             // author
    w.writeString("");             // description
    w.writeFloat(0.0f);            // duration
    w.writeVarint(22);             // GD game version 2.2
    w.writeDouble(framerate);
    w.writeVarint(0);              // seed
    w.writeVarint(0);              // coins
    w.writeBool(false);            // ldm
    w.writeBool(false);            // platformer
    w.writeString("kusmic.pathfinder"); // bot name
    w.writeVarint(1);              // bot version
    w.writeVarint(static_cast<uint64_t>(levelId));
    w.writeString(levelName);
    w.writeVarint(0);              // extension block (empty)
    w.writeVarint(0);              // deaths (none)
    w.writeVarint(p1Events.size()); // total input count (no P2 events)
    w.writeVarint(p1Events.size()); // P1 event count

    uint64_t prevFrame = 0;
    for (auto& ev : p1Events) {
        uint64_t delta = ev.frame - prevFrame;
        w.writeVarint((delta << 1) | (ev.down ? 1 : 0));
        prevFrame = ev.frame;
    }
    // P2 event count = 0, no P2 events follow.

    return w.take();
}

} // namespace gdsim
