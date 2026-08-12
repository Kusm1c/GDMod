#pragma once
#include <cmath>
#include <iostream>
#include <utility>
#include <string>
#include <vector>
#include <unordered_set>
#include <variant>
#include <memory>
#include <unordered_map>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <type_traits>

// src/sim is a standalone physics library: the mod links it into the Geode target
// (GDSIM_USE_GEODE, set by CMakeLists) and routes numeric parsing / logging through
// Geode's APIs, while the offline validation tools (test/divergehunt, trace,
// calibrate, …) build it with a bare compiler and fall back to the C runtime.
#ifdef GDSIM_USE_GEODE
    #include <Geode/utils/general.hpp>   // geode::utils::numFromString
    #include <Geode/loader/Log.hpp>      // geode::log
#endif

namespace gdsim {

class hash_tuple {
    template<class T>
    struct component {
        const T& value;
        component(const T& value) : value(value) {}
        uintmax_t operator,(uintmax_t n) const {
            n ^= std::hash<T>()(value);
            n ^= n << (sizeof(uintmax_t) * 4 - 1);
            return n ^ std::hash<uintmax_t>()(n);
        }
    };
public:
    template<class Tuple>
    size_t operator()(const Tuple& tuple) const {
        return std::hash<uintmax_t>()(
            std::apply([](const auto& ... xs) { return (component(xs), ..., 0); }, tuple));
    }
};

template <typename ...Args>
struct velocity_map : public std::unordered_map<std::tuple<Args...>, std::vector<double>, hash_tuple> {
    using std::unordered_map<std::tuple<Args...>, std::vector<double>, hash_tuple>::unordered_map;
    double get(Args... args, int sec) const {
        // Safety net: an unknown (vehicle/type/size) combo must never throw — an
        // uncaught std::out_of_range here calls std::terminate and closes the game.
        // Callers collapse to a covered key (see orbPadVehicle); if one still slips
        // through, treat it as no boost rather than crashing.
        auto it = this->find(std::tuple(args...));
        if (it == this->end() || sec < 0 || sec >= (int)it->second.size())
            return 0.0;
        return it->second[sec];
    }
};

// Copy-on-write set: cheap to copy (shares the buffer), copies lazily on first
// mutation of a shared buffer. Shared ownership is decided by the shared_ptr's
// refcount, NOT a per-instance flag.
//
// The previous design used a `written` bool that was set true on the first
// mutation but NEVER reset when the object was copied FROM. So after
//   O.insert(a);           // O.written = true, owns buffer B
//   cow_set C = O;         // C shares B (C.written=false), but O.written STAYS true
//   O.insert(c);           // O.written==true -> mutates the SHARED B in place
// O silently corrupted C's contents, and the in-place insert could rehash B while
// another sharing copy was reading it — heap corruption that only surfaced under
// heavy copying (the beam search at large width copies thousands of Players),
// layout-sensitively (invisible to gdb / _GLIBCXX_ASSERTIONS). Deciding "am I
// shared?" from use_count() fixes it: we copy exactly when the buffer is aliased.
// (Single-threaded use only — the solver runs on one thread; use_count is not a
// synchronization primitive.)
template <typename T>
class cow_set {
    std::shared_ptr<std::unordered_set<T>> data;
    void detach() {                       // ensure we hold an unshared buffer before mutating
        if (data.use_count() > 1)
            data = std::make_shared<std::unordered_set<T>>(*data);
    }
public:
    cow_set() : data(std::make_shared<std::unordered_set<T>>()) {}
    cow_set(cow_set const& other) = default;
    cow_set(cow_set&& other) noexcept = default;
    cow_set(std::unordered_set<T> const& other) : data(std::make_shared<std::unordered_set<T>>(other)) {}

    cow_set& operator=(cow_set const& other) = default;
    cow_set& operator=(cow_set&& other) noexcept = default;

    void insert(T const& value) { detach(); data->insert(value); }
    void erase(T const& value)  { detach(); data->erase(value); }
    bool contains(T const& value) const { return data->contains(value); }
    bool empty() const { return data->empty(); }
    size_t size() const { return data->size(); }
    void clear() {
        if (data.use_count() > 1) data = std::make_shared<std::unordered_set<T>>();
        else                      data->clear();
    }
    std::unordered_set<T> const& get() const { return *data; }
};

float slerp(float fromAngle, float toAngle, float t);

constexpr int case_and(auto a, auto b) {
    return static_cast<int>(a) | (static_cast<int>(b) << 4);
}

inline constexpr float deg2rad(float deg) { return deg * 3.141592653f / 180.f; }
inline constexpr float rad2deg(float rad) { return rad * 180.f / 3.141592653f; }

inline float stod_def(std::string const& str, float def = 0) {
    char const* begin = str.c_str();
    char* end = nullptr;
    errno = 0;
    double out = std::strtod(begin, &end);
    if (begin == end || *end != '\0' || errno == ERANGE) return def;
    return (float)out;
}

// Integer/float parse shim replacing std::stoi/std::atoi in the level parsers. In
// the Geode build it uses geode::utils::numFromString (project convention); the
// standalone tools use a lenient C-runtime parse that matches the old stoi/atoi
// behaviour (a leading numeric prefix wins; empty/invalid yields `def`).
template <class T>
inline T numFromString(std::string const& str, T def = T{}) {
#ifdef GDSIM_USE_GEODE
    return geode::utils::numFromString<T>(str).unwrapOr(def);
#else
    char const* begin = str.c_str();
    char* end = nullptr;
    errno = 0;
    if constexpr (std::is_integral_v<T>) {
        long long out = std::strtoll(begin, &end, 10);
        return (begin == end || errno == ERANGE) ? def : (T)out;
    } else {
        double out = std::strtod(begin, &end);
        return (begin == end || errno == ERANGE) ? def : (T)out;
    }
#endif
}

// Debug logging shim. std::cout/stderr is never used in the mod: the Geode build
// routes through geode::log; standalone tools print to stderr.
inline void logDebug(std::string const& msg) {
#ifdef GDSIM_USE_GEODE
    geode::log::debug("{}", msg);
#else
    std::fprintf(stderr, "%s\n", msg.c_str());
#endif
}

struct Vec2D {
    float x, y;
    inline Vec2D() : x(0), y(0) {}
    inline Vec2D(float x, float y) : x(x), y(y) {}
    inline bool operator==(Vec2D const& o) const { return x == o.x && y == o.y; }
    inline Vec2D operator-(Vec2D const& o) const { return {x-o.x, y-o.y}; }
    inline Vec2D operator+(Vec2D const& o) const { return {x+o.x, y+o.y}; }
    inline Vec2D operator*(float s) const { return {x*s, y*s}; }
    inline Vec2D operator/(float s) const { return {x/s, y/s}; }
    inline Vec2D& operator+=(Vec2D const& o) { x+=o.x; y+=o.y; return *this; }
    inline Vec2D& operator-=(Vec2D const& o) { x-=o.x; y-=o.y; return *this; }
    inline Vec2D& operator*=(float s) { x*=s; y*=s; return *this; }
    inline Vec2D& operator/=(float s) { x/=s; y/=s; return *this; }
    Vec2D rotate(float angle, Vec2D const& pivot = {0,0}) const;
};

struct Entity {
    Vec2D pos;
    Vec2D size;
    float rotation;
    bool intersects(Entity const& other) const;
    inline float getLeft()   const { return pos.x - size.x / 2; }
    inline float getRight()  const { return pos.x + size.x / 2; }
    inline float getTop()    const { return pos.y + size.y / 2; }
    inline float getBottom() const { return pos.y - size.y / 2; }
};

// NOTE (2026-08-11): a `subframeCheck` helper (4 discrete interpolated position
// samples between two frames) briefly lived here, built from GD Creator School's
// "Advanced Hitboxes" #2 prose ("4 additional checks between every 2 frames of
// processing"). Removed after reading the actual decompiled
// PlayerObject::collidedWithObjectInternal (src/gdp-2.2/PlayerObject/
// PlayerObject_collidedWithObjectInternal.cpp:616-619): real GD's death check is a
// single discrete test at the CURRENT position only, no sweep/interpolation term at
// all. The doc's "subframes" are GJBaseGameLayer::update's stepCount loop
// reconciling variable render FPS with the fixed 240Hz physics tick, which gdsim
// already has by construction (it simulates purely in 240Hz-tick space, no
// render-frame coarsening) — there was nothing missing here to begin with. See
// Block.cpp's collide() for the fuller writeup.

} // namespace gdsim
