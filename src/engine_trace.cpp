// engine_trace.cpp — per-function engine state capture for the decoding program.
//
// WHAT: every in-scope PlayerObject gameplay function is wrapped; each call writes one
// record = (physics step, which function, nesting depth, which player, its arguments,
// the object it was called with, its return value, the COMPLETE physics state of the
// player just before the call, and just after).
//
// WHY: the lockstep bench (test/lockstep.cpp) proves a ported engine function exact by
// loading `before`, running the port with the same arguments, and comparing with `after`
// field by field. One call at a time: no trajectory, no drift, no two bugs cancelling.
// That is the missing instrument the plan calls "le programme d'apprentissage" — it tells
// us WHICH function and WHICH field our transcription gets wrong, with the input proving it.
//
// The state layout is src/sim/engine/SnapFields.inc, generated from the engine's own code
// by tools/gen_snapfields.py and shared with gdsim's EngineSnap, so what is captured and
// what is compared cannot drift apart. The file header carries its layoutHash() so a
// trace recorded against an older field list is rejected instead of misread.
//
// Gate: mod setting `engine-trace` (off by default — this is very high volume; use it for
// one short lab run). Output: testlevel/GDMod_enginetrace_<levelId>.bin
//
// Existing hooks on the same functions (re_scanner.cpp's updateJump probe, mech_recorder.cpp)
// only read state, and Geode chains $modify hooks, so there is no interaction.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "sim/engine/EngineSnap.hpp"
#include "sim/engine/EngineTrace.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace geode::prelude;
using gdsim::engine::EngineSnap;
using gdsim::engine::Fn;
using gdsim::engine::TraceRecord;

namespace {

// Function ids and the on-disk layout live in sim/engine/EngineTrace.hpp, shared with the
// reader (test/lockstep.cpp), so writer and reader cannot drift apart.

std::atomic<bool> s_enabled{false};
bool     s_listener = false;
std::FILE* g_out   = nullptr;
uint32_t g_step    = 0;             // physics steps of player 1 since the file was opened
int      g_depth   = 0;             // nesting of traced calls (updateJump -> flipGravity ...)

bool enabled() {
    if (!s_listener) {
        s_listener = true;
        s_enabled.store(Mod::get()->getSettingValue<bool>("engine-trace"));
        listenForSettingChanges<bool>("engine-trace", [](bool v) {
            s_enabled.store(v);
            log::info("[EngineTrace] {}", v ? "ENABLED" : "disabled");
        });
    }
    return s_enabled.load();
}

void closeOut() {
    if (g_out) { std::fclose(g_out); g_out = nullptr; }
}

void openOut(int levelId) {
    closeOut();
    g_step = 0;
    g_depth = 0;
    std::string dir = "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) dir = "C:/Users/Kusmic/Documents/";
    const std::string path = dir + "GDMod_enginetrace_" + std::to_string(levelId) + ".bin";
    g_out = std::fopen(path.c_str(), "wb");
    if (!g_out) { log::error("[EngineTrace] cannot open {}", path); return; }
    // Large buffer: a lab run writes several MB per second.
    std::setvbuf(g_out, nullptr, _IOFBF, 8 << 20);
    gdsim::engine::writeHeader(g_out, levelId);
    log::info("[EngineTrace] recording -> {} (layout {:08x})", path, gdsim::engine::layoutHash());
}

// Read the player's physics state through the SAME generated list gdsim uses.
void snap(PlayerObject* p, EngineSnap& s) {
#define SNAP_SCALAR(T, n, off) s.n = p->n;
#define SNAP_POINT(n, off)     s.n##_x = p->n.x; s.n##_y = p->n.y;
#define SNAP_OBJ(n, off)       s.n = p->n ? p->n->m_uniqueID : 0;
#define SNAP_SEED(n, off)      s.n = p->n.value();
// The live position is the CCNode's, reached through the virtual getPosition() exactly as the
// engine does — PlayerObject::m_position (0xa90) is a different, spider-only field.
#define SNAP_NODEPOS(n)        { const auto pt = p->getPosition(); s.n##_x = pt.x; s.n##_y = pt.y; }
#include "sim/engine/SnapFields.inc"
#undef SNAP_SCALAR
#undef SNAP_POINT
#undef SNAP_OBJ
#undef SNAP_SEED
#undef SNAP_NODEPOS
}

int playerIndex(PlayerObject* p) {
    auto* pl = PlayLayer::get();
    if (!pl) return 0;
    if (pl->m_player1 == p) return 1;
    if (pl->m_player2 == p) return 2;
    return 0;                                   // an editor / menu player: not traced
}

// One call. Built on entry, written on exit (RAII), so `after` is taken once the real
// function — and everything it called — has returned.
struct Rec {
    PlayerObject* p = nullptr;
    bool on = false;
    TraceRecord t;

    Rec(PlayerObject* self, Fn f) : p(self) {
        if (!g_out || !enabled()) return;
        t.player = (uint8_t)playerIndex(self);
        if (!t.player) return;
        on = true;
        t.step = g_step; t.fn = (uint16_t)f; t.depth = (uint8_t)g_depth;
        snap(self, t.before);
        ++g_depth;
    }
    Rec& f(float a, float b = 0) { t.argF[0] = a; t.argF[1] = b; return *this; }
    Rec& i(int a, int b = 0, int c = 0) { t.argI[0] = a; t.argI[1] = b; t.argI[2] = c; return *this; }
    Rec& obj(GameObject* o) {
        if (!o) return *this;
        t.objUid = o->m_uniqueID; t.objId = o->m_objectID; t.objType = (int32_t)o->m_objectType;
        t.objX = o->getPositionX(); t.objY = o->getPositionY(); t.objRot = o->getRotation();
        auto r = o->getObjectRect(); t.objW = r.size.width; t.objH = r.size.height;
        return *this;
    }
    Rec& r4(const cocos2d::CCRect& c) {
        t.rect[0] = c.origin.x; t.rect[1] = c.origin.y; t.rect[2] = c.size.width; t.rect[3] = c.size.height;
        return *this;
    }
    void result(bool v) { t.ret = v ? 1 : 0; }

    ~Rec() {
        if (!on) return;
        --g_depth;
        snap(p, t.after);
        if (g_out) gdsim::engine::writeRecord(g_out, t);   // closed mid-call (level exit): drop it
    }
};

} // namespace

class $modify(EngineTracePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (enabled()) openOut(level ? (int)level->m_levelID : 0);
        return true;
    }
    void resetLevel() {
        PlayLayer::resetLevel();
        if (!g_out) return;
        // Attempt marker: a record with fn = AttemptStart and no payload beyond the header
        // fields, so the bench can segment attempts (state jumps back to spawn here).
        g_depth = 0;
        gdsim::engine::writeAttemptMarker(g_out, g_step);
    }
    void onQuit() {
        closeOut();
        PlayLayer::onQuit();
    }
};

class $modify(EngineTracePlayer, PlayerObject) {
    void update(float dt) {
        if (g_out && playerIndex(this) == 1) ++g_step;   // one physics step of player 1
        Rec r(this, Fn::update); r.f(dt);
        PlayerObject::update(dt);
    }
    void updateJump(float dt) {
        Rec r(this, Fn::updateJump); r.f(dt);
        PlayerObject::updateJump(dt);
    }
    void postCollision(float dt, bool betweenSteps) {
        Rec r(this, Fn::postCollision); r.f(dt).i(betweenSteps);
        PlayerObject::postCollision(dt, betweenSteps);
    }
    bool preSlopeCollision(float dt, GameObject* object) {
        Rec r(this, Fn::preSlopeCollision); r.f(dt).obj(object);
        bool v = PlayerObject::preSlopeCollision(dt, object);
        r.result(v);
        return v;
    }
    void collidedWithSlopeInternal(float dt, GameObject* object, bool forced) {
        Rec r(this, Fn::collidedWithSlopeInternal); r.f(dt).i(forced).obj(object);
        PlayerObject::collidedWithSlopeInternal(dt, object, forced);
    }
    bool collidedWithObjectInternal(float dt, GameObject* object, cocos2d::CCRect rect, bool skipCheck) {
        // `rect` is the object rect the caller (checkCollisions) computed for this pair — it
        // can differ from the object's own getObjectRect(), so it is recorded as-is.
        Rec r(this, Fn::collidedWithObjectInternal); r.f(dt).i(skipCheck).obj(object).r4(rect);
        bool v = PlayerObject::collidedWithObjectInternal(dt, object, rect, skipCheck);
        r.result(v);
        return v;
    }
    void didHitHead() {
        Rec r(this, Fn::didHitHead);
        PlayerObject::didHitHead();
    }
    void checkSnapJumpToObject(GameObject* object) {
        Rec r(this, Fn::checkSnapJumpToObject); r.obj(object);
        PlayerObject::checkSnapJumpToObject(object);
    }
    void updateCollide(PlayerCollisionDirection direction, GameObject* object) {
        Rec r(this, Fn::updateCollide); r.i((int)direction).obj(object);
        PlayerObject::updateCollide(direction, object);
    }
    void hitGround(GameObject* object, bool notFlipped) {
        Rec r(this, Fn::hitGround); r.i(notFlipped);
        if (object) r.obj(object);
        PlayerObject::hitGround(object, notFlipped);
    }
    void hitGroundNoJump(GameObject* object, bool notFlipped) {
        Rec r(this, Fn::hitGroundNoJump); r.i(notFlipped);
        if (object) r.obj(object);
        PlayerObject::hitGroundNoJump(object, notFlipped);
    }
    void flipGravity(bool flip, bool noEffects) {
        Rec r(this, Fn::flipGravity); r.i(flip, noEffects);
        PlayerObject::flipGravity(flip, noEffects);
    }
    void ringJump(RingObject* object, bool skipCheck) {
        Rec r(this, Fn::ringJump); r.i(skipCheck).obj(object);
        PlayerObject::ringJump(object, skipCheck);
    }
    void bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object) {
        Rec r(this, Fn::bumpPlayer); r.f(bumpMod).i(objectType, noEffects).obj(object);
        PlayerObject::bumpPlayer(bumpMod, objectType, noEffects, object);
    }
    void propellPlayer(float yVelocity, bool noEffects, int objectType) {
        Rec r(this, Fn::propellPlayer); r.f(yVelocity).i(noEffects, objectType);
        PlayerObject::propellPlayer(yVelocity, noEffects, objectType);
    }
    void boostPlayer(float yVelocity) {
        Rec r(this, Fn::boostPlayer); r.f(yVelocity);
        PlayerObject::boostPlayer(yVelocity);
    }
    void toggleFlyMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleFlyMode); r.i(enable, noEffects);
        PlayerObject::toggleFlyMode(enable, noEffects);
    }
    void toggleRollMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleRollMode); r.i(enable, noEffects);
        PlayerObject::toggleRollMode(enable, noEffects);
    }
    void toggleBirdMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleBirdMode); r.i(enable, noEffects);
        PlayerObject::toggleBirdMode(enable, noEffects);
    }
    void toggleDartMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleDartMode); r.i(enable, noEffects);
        PlayerObject::toggleDartMode(enable, noEffects);
    }
    void toggleRobotMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleRobotMode); r.i(enable, noEffects);
        PlayerObject::toggleRobotMode(enable, noEffects);
    }
    void toggleSpiderMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleSpiderMode); r.i(enable, noEffects);
        PlayerObject::toggleSpiderMode(enable, noEffects);
    }
    void toggleSwingMode(bool enable, bool noEffects) {
        Rec r(this, Fn::toggleSwingMode); r.i(enable, noEffects);
        PlayerObject::toggleSwingMode(enable, noEffects);
    }
    void togglePlayerScale(bool enable, bool noEffects) {
        Rec r(this, Fn::togglePlayerScale); r.i(enable, noEffects);
        PlayerObject::togglePlayerScale(enable, noEffects);
    }
    void spiderTestJumpInternal(bool dynamic) {
        Rec r(this, Fn::spiderTestJumpInternal); r.i(dynamic);
        PlayerObject::spiderTestJumpInternal(dynamic);
    }
    void updateTimeMod(float speed, bool noEffects) {
        // First in the port order (plan phase 2, #1): sets the per-tier speed state that
        // every later function reads.
        Rec r(this, Fn::updateTimeMod); r.f(speed).i(noEffects);
        PlayerObject::updateTimeMod(speed, noEffects);
    }
    bool playerIsFallingBugged() {
        Rec r(this, Fn::playerIsFallingBugged);
        bool v = PlayerObject::playerIsFallingBugged();
        r.result(v);
        return v;
    }
};
