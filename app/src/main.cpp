// GD Level Player — fetches a real Geometry Dash level by ID directly from
// RobTop's servers and plays it through gdsim (the same physics engine
// developed/validated in this repo) with a minimalist vector-shape renderer.
#include "raylib.h"
#include "leveldata.hpp"
#include "render.hpp"
#include "exportdialog.hpp"
#include "macroentry.hpp"
#include "macrocache.hpp"
#include "gdr2import.hpp"
#include "tunerui.hpp"
#include "transport.hpp"
#include "fitui.hpp"
#include "uifont.hpp"
#include "../../src/sim/Level.hpp"
#include "../../src/sim/Solver.hpp"
#include "../../src/sim/Gdr2Export.hpp"
#include "../../src/sim/DebugPaths.hpp"
#include "../../src/sim/Tunables.hpp"
#include <memory>
#include <string>
#include <cmath>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>

using namespace gdsim;

static constexpr float FIXED_DT = 1.f / 240.f;

enum class AppState { Menu, LevelSelect, Loading, Playing, Paused, Dead, Cleared, Solving, Replaying, MacroList, Options };

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "GD Level Player");
    SetTargetFPS(144);
    gdapp::LoadUIFont();   // must come after InitWindow (needs a GL context for the atlas texture)

    AppState state = AppState::Menu;
    std::string idInput;
    std::string statusMsg;
    std::unique_ptr<Level> level;
    std::string levelName, decodedLevelString;
    float accumulator = 0.f;
    gdapp::Camera2DState cam;
    float finalPercent = 0.f;
    // Set after a Cloudflare rate-limit response so a stray Enter press can't
    // immediately re-trigger another request and extend the block.
    double cooldownUntil = 0.0;
    bool exitRequested = false;

    std::vector<gdapp::CachedLevelEntry> cachedLevels;
    int selectedCacheIdx = 0;
    int pendingLevelId = 0; // set by both the ID-entry menu and the cache picker

    std::deque<gdapp::TrailPoint> trailDeque;

    // ========== Physics Lab + replay transport ==========
    // The Lab (TAB) edits any gdsim constant live; the transport owns replay
    // playback rate, pause, frame-stepping and seeking. Both are pure UI - the
    // resim they imply is done here, because only this loop owns the Level and
    // the input track.
    gdapp::TunerUI tuner;
    gdapp::TransportUI transport;
    // Keyboard-half transport actions, collected during the input pass and merged
    // with the bar's own (click) actions after it is drawn — the bar has to be on
    // screen before it can be clicked, so its half necessarily runs later.
    gdapp::TransportResult pendingTransport;
    // Trajectory correction: pin a trail point, drag it (or snap it onto the real
    // capture), then ask which physics constant explains the difference.
    gdapp::FitUIState fitUI;
    // Set briefly after a resim, so the HUD can say what just happened instead of
    // the trail silently jumping under the user.
    double resimFlashUntil = 0.0;
    int    resimFrames = 0;

    // ========== Physics trail (real capture overlay) ==========
    // Overlays a REAL re-scanner capture (testlevel/GDMod_physics_<id>.txt)
    // as a static reference line alongside the live gdsim trail, so a real
    // playthrough and gdsim's own simulation of the same level can be
    // compared visually. Off by default (an options-menu toggle); loaded
    // fresh whenever a level is (re)loaded, since the file is per-level-ID.
    bool showPhysicsTrail = false;
    std::vector<gdapp::RealTrailPoint> physicsTrail;
    // Thin centre-of-player path line (no hitbox boxes) — a lighter-weight
    // alternative/addition to the full swept-hitbox trail, toggled separately.
    bool showCenterPath = false;
    AppState optionsReturnState = AppState::Menu;

    // Real Y in a GDMod_physics_<id>.txt capture is ~105 world-units above
    // gdsim's own Y for the same instant (established convention, matches
    // this app's own solver-camera default of y=105 as "ground level") — but
    // "~" matters: it isn't exact for every level, so both axes are live-
    // nudgeable in-game (numpad 4/6 = X, numpad 8/2 = Y) instead of a single
    // hardcoded constant, so the orange/green trails can be eyeballed into
    // exact alignment per level rather than trusting one guessed number.
    float physicsTrailOffsetX = 0.f;
    // Exactly 90 — confirmed 2026-08-20 via 181 X-matched grounded samples on
    // DeCode (level 2997354), zero stddev to 6 decimals. Not a guess/nudge value.
    float physicsTrailOffsetY = 90.f;

    auto loadPhysicsTrail = [&](int id) {
        physicsTrail.clear();
        if (id <= 0) return;
        std::string path = gdsim::gdmodBaseDir() + "GDMod_physics_" + std::to_string(id) + ".txt";
        std::ifstream in(path);
        if (!in.is_open()) return;

        // The file appends one "# --- ATTEMPT ---"-delimited block per real
        // play session; only the LAST (most recent) attempt is relevant.
        std::vector<std::string> lines;
        {
            std::string line;
            while (std::getline(in, line)) lines.push_back(std::move(line));
        }
        size_t lastAttemptStart = 0;
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i].rfind("# --- ATTEMPT", 0) == 0) lastAttemptStart = i + 1;

        // The "# --- ATTEMPT ---" marker is only written once per level LOAD
        // (PlayLayer::init), not per in-level death/retry (resetLevel doesn't
        // re-fire init, so g_frame and the file just keep appending across
        // every retry within that same session). So one marked block can
        // silently contain SEVERAL real sub-attempts, each snapping the
        // recorded X back toward the spawn point — connecting them naively
        // draws one long diagonal line straight across the level (exactly
        // the "why is there a straight line through everything" report).
        // Detect this the same way a restart shows up in the data: X
        // dropping instead of increasing. Keep only the LAST such segment.
        std::vector<gdapp::RealTrailPoint> pts;
        size_t segmentStart = lastAttemptStart;
        float prevX = -1e9f;
        for (size_t i = lastAttemptStart; i < lines.size(); i++) {
            const std::string& line = lines[i];
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            long frame; float x, y, yVel, speed; int veh, upsideDown, mini, grounded;
            if (!(ss >> frame >> x >> y >> yVel >> speed >> veh >> upsideDown >> mini >> grounded))
                continue;
            if (x < prevX - 5.f) segmentStart = i;   // backward jump = a fresh retry started here
            prevX = x;
        }

        prevX = -1e9f;
        for (size_t i = segmentStart; i < lines.size(); i++) {
            const std::string& line = lines[i];
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            long frame; float x, y, yVel, speed; int veh, upsideDown, mini, grounded;
            if (!(ss >> frame >> x >> y >> yVel >> speed >> veh >> upsideDown >> mini >> grounded))
                continue;
            pts.push_back({gdsim::Vec2D{x, y}, veh, mini != 0, frame, yVel});   // raw — offset applied at draw time
        }
        physicsTrail = std::move(pts);
    };

    // Live nudge for the physics trail's alignment offset — CTRL + arrow keys
    // (not numpad: not every keyboard has one). Held-key continuous
    // adjustment (not a single step per press), so it can be eyeballed into
    // alignment quickly. Shared between the Playing and Replaying states so
    // both behave identically. Replaying's own free camera also uses plain
    // arrow keys to pan — held Ctrl there suppresses that pan (see its own
    // comment) so the two controls never fire off the same keypress.
    auto nudgePhysicsTrailOffset = [&]() {
        if (!showPhysicsTrail) return;
        if (!IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL)) return;
        float step = 30.f * GetFrameTime(); // world-units/sec
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) step *= 5.f; // fast nudge
        if (IsKeyDown(KEY_RIGHT)) physicsTrailOffsetX += step;
        if (IsKeyDown(KEY_LEFT))  physicsTrailOffsetX -= step;
        if (IsKeyDown(KEY_UP))    physicsTrailOffsetY += step;
        if (IsKeyDown(KEY_DOWN))  physicsTrailOffsetY -= step;
    };

    std::string deviationStatusMsg;
    double deviationStatusUntil = 0.0;

    // Writes testlevel/debug/GDMod_deviation_<id>.txt: for every sim trail
    // point, the nearest-by-X real physics-trail point (offset-corrected) and
    // the resulting Y deviation — the exact by-hand comparison this session
    // kept doing manually via scratchpad tools, now a one-click export. Two-
    // pointer walk (both trails are ~monotonic increasing in X for normal
    // forward-scrolling levels; a local X dip just costs a few wasted steps,
    // never wrong output) instead of an O(N*M) nearest-neighbour scan.
    auto writeDeviationFile = [&](const std::vector<gdapp::TrailPoint>& simTrail) {
        if (simTrail.empty()) { deviationStatusMsg = "Deviation: no sim trail yet"; deviationStatusUntil = GetTime() + 3.0; return; }
        if (physicsTrail.empty()) { deviationStatusMsg = "Deviation: no physics trail loaded (enable it in Options + play a level with a capture)"; deviationStatusUntil = GetTime() + 4.0; return; }

        std::string path = gdsim::debugPath("GDMod_deviation_" + std::to_string(pendingLevelId) + ".txt");
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) { deviationStatusMsg = "Deviation: failed to open " + path; deviationStatusUntil = GetTime() + 4.0; return; }

        out << "# simStep simX simY | realFrame realX realY realYVel | diffY (real-sim, offset X=" << physicsTrailOffsetX
            << " Y=" << physicsTrailOffsetY << " already applied)\n";
        size_t ri = 0;
        for (size_t si = 0; si < simTrail.size(); si++) {
            float sx = simTrail[si].pos.x, sy = simTrail[si].pos.y;
            // Advance ri while the NEXT real point is as close (or closer) an X match
            // than the current one. Must be <=, not <: the real capture starts with a
            // long flat run of identical points (the known ~459-frame re-scanner
            // startup freeze at x=0 — see loadPhysicsTrail's own segment-cut comment),
            // and a strict < can never see past a run of EQUAL distances to reach the
            // genuinely closer point on the far side of it, leaving ri stuck at index 0
            // for the whole file.
            while (ri + 1 < physicsTrail.size() &&
                   std::fabs((physicsTrail[ri + 1].pos.x - physicsTrailOffsetX) - sx) <=
                   std::fabs((physicsTrail[ri].pos.x - physicsTrailOffsetX) - sx))
                ri++;
            const auto& rp = physicsTrail[ri];
            float rx = rp.pos.x - physicsTrailOffsetX;
            float ry = rp.pos.y - physicsTrailOffsetY;
            out << si << " " << sx << " " << sy << " | " << rp.frame << " " << rx << " " << ry << " " << rp.yVel
                << " | " << (ry - sy) << "\n";
        }
        deviationStatusMsg = "Deviation written: " + path;
        deviationStatusUntil = GetTime() + 5.0;
    };

    // ========== Beam search (solver) ==========
    // Runs on a background thread (solveLevel can take tens of seconds) and
    // reports progress into solverProgress, which the render thread polls
    // every frame — the exact same live-progress contract test/
    // solveview_standalone.cpp already uses for its own Win32 viewer.
    std::unique_ptr<Level> solveDrawLevel;               // static geometry only, for the viz
    std::unique_ptr<SolverProgressReport> solverProgress; // fresh instance per solve (deleted copy/move)
    std::atomic<bool> solverCancelled{false};
    std::atomic<bool> solverPaused{false};
    std::thread solverThread;
    SolverResult solverResult;
    bool solverThreadDone = false; // true once the background thread has been join()ed
    gdapp::Camera2DState solverCam;
    // OFF by default: a "human clicks/sec" cap (cfg.minClickGap) rejects a beam
    // solve that's physically valid (passes the zero-margin re-check) purely
    // because some click pair is faster than a human could tap, discarding it and
    // falling through to the much slower pathseeker/greedy/GA fallbacks — which
    // is exactly the "it clears then keeps retrying from scratch" behaviour a real
    // playtest hit on DeCode. This app is a debug/search tool first, not a replay-
    // recorder, so default to accepting whatever the beam actually finds; toggle
    // on if a humanly-clickable result specifically matters.
    bool solverHumanClickLimit = false;
    // DELIBERATELY SEPARATE from SolverProgressReport::done: that flag is set true
    // by SEVERAL internal Solver.cpp phases (beam success, greedy success, greedy
    // "Timeout" give-up, GA, ...), not just once when solveLevel() truly returns —
    // e.g. a greedy attempt can set it, then the outer pipeline still falls through
    // to more restarts / the GA fallback / the adaptive-margin retry loop for a long
    // time afterward, with `done` never reset back to false. Joining the thread as
    // soon as `done` first flips true (as this used to) blocks the render thread —
    // freezing the WHOLE app — for however much longer the solve actually keeps
    // running, reading as a hang/crash. This flag is set ONLY by the thread lambda
    // itself, as its truly-last statement after solveLevel() has genuinely returned.
    std::atomic<bool> solverActuallyFinished{false};

    // ========== Replay (watch the solve play out) ==========
    // Deterministic playback of solverResult.clicks through a FRESH Level — no
    // live input. Camera is free (same WASD/arrows/wheel controls as Solving),
    // deliberately NOT auto-following, per request.
    std::unique_ptr<Level> replayLevel;
    std::vector<bool> replayInputAt;   // precomputed press state per frame index
    uint64_t replayFrameCounter = 0;   // external counter; matches SolverClick's own
                                        // frame numbering exactly (see test/gdrcheck.cpp)
    float replayAccumulator = 0.f;
    bool replayAlive = true;
    std::deque<gdapp::TrailPoint> replayTrail;
    gdapp::Camera2DState replayCam;
    std::string exportStatusMsg;
    double exportStatusUntil = 0.0;

    // ========== .gdr2 import (replay a real recording, compare vs physics trail) ==========
    // Picking a file sets pendingLevelId to the .gdr2's OWN level ID and kicks
    // off the normal async Loading flow; once that level finishes fetching,
    // the Loading-state handler below checks importPending and — instead of
    // the usual resetPlayback() into free Playing — loads the imported
    // presses into solverResult.clicks and starts a Replay with them, so this
    // reuses all of Replaying's existing rendering (including the physics
    // trail overlay) for free.
    bool importPending = false;
    std::vector<gdapp::Gdr2Press> importedPresses;
    std::string importStatusMsg;
    double importStatusUntil = 0.0;

    // ========== Hitbox-mismatch flagging (debug tool) ==========
    // gdsim's physics don't always match real GD exactly, and screenshots
    // alone haven't been enough to pin down where. This lets the user, at the
    // moment of death/clear, hover the object they believe was wrongly
    // passable (or wrongly solid) and press J to record a precise, per-object
    // report — level, frame, sim's own death cause, and the flagged object's
    // typeId/pos/size/rotation — to hitbox_flags.txt for later investigation.
    std::string flagStatusMsg;
    double flagStatusUntil = 0.0;

    // ========== Macro list (cleared replays kept, compressed, on disk) ==========
    // Every time a Replay actually reaches the end (not a death), it's kept
    // in memory AND appended to cache/macros/ (gzip-compressed level string,
    // see macrocache.cpp) so it survives across app launches — loaded back
    // here on startup.
    std::vector<MacroEntry> macroList = gdapp::loadMacrosFromDisk();
    int selectedMacroIdx = 0;

    auto startLoad = [&](int id) {
        pendingLevelId = id;
        state = AppState::Loading;
    };

    auto startGdr2Import = [&]() {
        auto path = gdapp::pickGdr2OpenPath(gdapp::loadLastExportFolder());
        if (!path) return;
        gdapp::saveLastExportFolder(std::filesystem::path(*path).parent_path().string());

        auto result = gdapp::importGdr2(*path);
        if (!result.success) {
            importStatusMsg = "Import failed: " + result.error;
            importStatusUntil = GetTime() + 4.0;
            return;
        }
        if (result.presses.empty()) {
            importStatusMsg = "Import failed: no Jump inputs found in file";
            importStatusUntil = GetTime() + 4.0;
            return;
        }
        importStatusMsg = TextFormat("Imported: level %u, %d fps, %d clicks",
                                      result.levelId, (int)result.framerate, (int)result.presses.size());
        importStatusUntil = GetTime() + 6.0;
        importedPresses = std::move(result.presses);
        importPending = true;
        startLoad((int)result.levelId);
    };

    auto resetPlayback = [&]() {
        level = std::make_unique<Level>(decodedLevelString);
        accumulator = 0.f;
        cam.x = 0.f; cam.y = 0.f;
        trailDeque.clear();
        loadPhysicsTrail(pendingLevelId);
        state = AppState::Playing;
    };

    auto startSolve = [&]() {
        if (solverThread.joinable()) {
            solverCancelled.store(true);
            solverThread.join();
        }
        solverCancelled.store(false);
        solverPaused.store(false);
        solverThreadDone = false;
        solverActuallyFinished.store(false);
        solverProgress = std::make_unique<SolverProgressReport>();
        solveDrawLevel = std::make_unique<Level>(decodedLevelString);
        solverCam.x = 0.f; solverCam.y = 105.f; solverCam.pixelsPerUnit = 3.f;

        std::string levelCopy = decodedLevelString;
        SolverProgressReport* progPtr = solverProgress.get();
        std::atomic<bool>* cancelPtr = &solverCancelled;
        std::atomic<bool>* pausePtr = &solverPaused;
        SolverResult* resultPtr = &solverResult;
        std::atomic<bool>* finishedPtr = &solverActuallyFinished;
        int minClickGap = solverHumanClickLimit ? (int)std::lround(240.0 / 14.0) : 0;
        solverThread = std::thread([levelCopy, progPtr, cancelPtr, pausePtr, resultPtr, finishedPtr, minClickGap]() {
            SolverConfig cfg;
            cfg.progress = progPtr;
            cfg.paused = pausePtr;
            cfg.minClickGap = minClickGap;
            *resultPtr = solveLevel(levelCopy, cfg, cancelPtr);

            // SECOND, fully independent re-verification — same pattern as the real
            // mod's doSimulate (hooks_menu.cpp): a FRESH Level, zero margin, replaying
            // the exact same clicks from scratch. solveLevel() already runs its own
            // zero-margin check internally (Solver.cpp's finalizeSolved/
            // verifyZeroMargin) before ever reporting solved=true — but that check is
            // itself DOCUMENTED as occasionally giving a false OK (see Solver.cpp's
            // verifyZeroMargin comment, "OPEN 2026-08-09"): touch-triggered toggle-group
            // state is path-dependent, so two independently-fresh replays of the
            // identical click list can still disagree on whether a given block is
            // solid at a given frame. This won't catch every case (the mod's own copy
            // of this check has the same fundamental limit), but it catches the same
            // class of case the mod's does, instead of the app having NO safety net at
            // all and just showing a false "SOLVED!" the way it did here.
            if (resultPtr->solved && !resultPtr->clicks.empty()) {
                Level verifySim(levelCopy);
                verifySim.hazardInflate = 0.f;
                verifySim.flightHazardInflate = 0.f;
                std::vector<bool> inputAt(cfg.maxFrames + 4, false);
                for (auto& c : resultPtr->clicks)
                    for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < inputAt.size(); ++f)
                        inputAt[f] = true;
                float end = resultPtr->levelEndEstimate;
                bool diverged = false;
                uint64_t deathF = 0;
                float deathX = 0.f;
                for (uint64_t f = 1; f <= cfg.maxFrames; ++f) {
                    bool p = f < inputAt.size() && inputAt[f];
                    auto& s = verifySim.runFrame(p, cfg.dt);
                    if (s.dead) { diverged = true; deathF = f; deathX = s.pos.x; break; }
                    if (end > 0.f && s.pos.x >= end) break;
                }
                if (diverged) {
                    resultPtr->solved = false;
                    resultPtr->message = "Solver said solved, but an independent re-verify "
                        "dies at frame " + std::to_string(deathF) + " x=" + std::to_string((int)deathX)
                        + " (known gdsim limitation: touch-triggered toggle state can be "
                        "path-dependent between runs — see Solver.cpp verifyZeroMargin)";
                }
            }

            progPtr->addLog(resultPtr->solved
                ? ("FINAL: solved, " + std::to_string(resultPtr->clicks.size()) + " clicks")
                : ("FINAL: " + resultPtr->message));
            // Truly last: solveLevel() has genuinely returned at this point (unlike
            // progPtr->done, which Solver.cpp can set true from several earlier
            // internal phases — see solverActuallyFinished's own declaration comment).
            finishedPtr->store(true);
        });
        state = AppState::Solving;
    };

    auto startReplay = [&]() {
        replayLevel = std::make_unique<Level>(decodedLevelString);
        uint64_t maxF = solverResult.clicks.empty() ? 100 : solverResult.clicks.back().releaseFrame + 500;
        replayInputAt.assign(maxF + 4, false);
        for (auto& c : solverResult.clicks)
            for (uint64_t f = c.pressFrame; f < c.releaseFrame && f < replayInputAt.size(); f++)
                replayInputAt[f] = true;
        replayFrameCounter = 0;
        replayAccumulator = 0.f;
        replayAlive = true;
        transport.paused = false;
        replayTrail.clear();
        replayCam.x = 0.f; replayCam.y = 105.f; replayCam.pixelsPerUnit = 3.f;
        loadPhysicsTrail(pendingLevelId);
        state = AppState::Replaying;
    };

    // Deterministic seek: gdsim has no reverse step, so "go to frame N" means
    // rebuild the Level and replay the recorded input track up to N. That is also
    // exactly what a physics-constant edit needs (a changed constant only shows
    // its true effect from frame 0), so both paths share this one function.
    // Fast enough to run every frame while scrubbing or dragging a value: the
    // step loop is the same code the solver runs at ~1M frames/sec.
    auto rebuildReplayTo = [&](int targetFrame) {
        if (decodedLevelString.empty()) return;
        replayLevel = std::make_unique<Level>(decodedLevelString);
        replayTrail.clear();
        replayFrameCounter = 0;
        replayAccumulator = 0.f;
        replayAlive = true;
        for (int f = 0; f < targetFrame; f++) {
            replayFrameCounter++;
            bool press = replayFrameCounter < replayInputAt.size() && replayInputAt[replayFrameCounter];
            Player& p = replayLevel->runFrame(press, FIXED_DT);
            replayTrail.push_back({p.pos, p.size, p.small});
            // Stop at the run's real end rather than pretending to seek past it -
            // seeking beyond a death would otherwise silently keep stepping a dead
            // player and draw a trail the run never had.
            if (p.dead || p.pos.x >= replayLevel->length - 5.f) { replayAlive = false; break; }
        }
        resimFrames = (int)replayFrameCounter;
        resimFlashUntil = GetTime() + 0.35;
    };

    // Shared by the Solving-done Export button and the Macro List's Export
    // action — takes explicit clicks/id/name instead of always reading the
    // (single, overwritable) solverResult so either source can use it.
    auto exportClicks = [&](const std::vector<SolverClick>& clicks, int levelId, const std::string& lvlName) {
        std::string safeName = lvlName;
        for (auto& c : safeName)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (safeName.empty()) safeName = "replay";
        std::string defaultFileName = safeName + "-" + std::to_string(levelId) + ".gdr2";

        auto picked = gdapp::pickGdr2SavePath(defaultFileName, gdapp::loadLastExportFolder());
        if (!picked) return;

        auto bytes = gdsim::exportClicksToGdr2(clicks, 240.0, levelId, lvlName);
        std::ofstream f(*picked, std::ios::binary | std::ios::trunc);
        bool ok = f.is_open();
        if (ok) { f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()); ok = f.good(); }
        if (ok) gdapp::saveLastExportFolder(std::filesystem::path(*picked).parent_path().string());

        exportStatusMsg  = ok ? ("Exported to " + std::filesystem::path(*picked).filename().string())
                               : "Export failed";
        exportStatusUntil = GetTime() + 3.0;
    };
    auto exportSolve = [&]() {
        if (!solverResult.solved) return;
        exportClicks(solverResult.clicks, pendingLevelId, levelName);
    };

    // Loads a previously-cleared macro's own level/clicks into the "current"
    // solve context and re-runs startReplay() — lets the Macro List reuse
    // Replaying's existing state/rendering unchanged.
    auto startReplayFromMacro = [&](const MacroEntry& m) {
        decodedLevelString = m.levelString;
        levelName = m.levelName;
        pendingLevelId = m.levelId;
        solverResult.solved = true;
        solverResult.clicks = m.clicks;
        solverResult.levelEndEstimate = m.levelEndEstimate;
        solverResult.message.clear();
        startReplay();
    };

    // `lvl`/`state` are the level being inspected and its final Player state
    // (the death/clear frame — sim's own verdict, for cross-reference against
    // whatever the user flags); `useCam` is whichever camera that view is
    // using (cam for live play, replayCam for Replaying); `source` tags which
    // view the report came from.
    auto flagHitboxAt = [&](Level* lvl, const Player& deathState,
                             const gdapp::Camera2DState& useCam, const char* source) {
        if (!lvl) return;
        Vector2 mouse = GetMousePosition();
        Vec2D worldPos = gdapp::screenToWorld(mouse.x, mouse.y, useCam, GetScreenWidth(), GetScreenHeight());
        auto picked = gdapp::pickObjectNear(*lvl, worldPos, deathState.frame);
        if (!picked.found) {
            flagStatusMsg = "No object near cursor";
            flagStatusUntil = GetTime() + 2.0;
            return;
        }
        std::ofstream f("hitbox_flags.txt", std::ios::app);
        if (f.is_open()) {
            std::time_t t = std::time(nullptr);
            char timeBuf[32];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            f << "[" << timeBuf << "] " << source
              << " level=\"" << levelName << "\" (" << pendingLevelId << ")"
              << " frame=" << deathState.frame
              << " playerPos=(" << deathState.pos.x << "," << deathState.pos.y << ")"
              << " simDead=" << (deathState.dead ? "1" : "0")
              << " simDeathCause=" << (deathState.deathCause ? deathState.deathCause : "none")
              << " simDeathObjType=" << deathState.deathObjType
              << " simDeathObjPos=(" << deathState.deathObjPos.x << "," << deathState.deathObjPos.y << ")"
              << " || FLAGGED typeId=" << picked.typeId
              << " prio=" << picked.prio
              << " pos=(" << picked.pos.x << "," << picked.pos.y << ")"
              << " size=(" << picked.size.x << "," << picked.size.y << ")"
              << " rot=" << picked.rotation
              << "\n";
        }
        flagStatusMsg = "Flagged typeId=" + std::to_string(picked.typeId) + " at ("
                       + std::to_string((int)picked.pos.x) + "," + std::to_string((int)picked.pos.y) + ")";
        flagStatusUntil = GetTime() + 3.0;
    };

    while (!WindowShouldClose() && !exitRequested) {
        int screenW = GetScreenWidth(), screenH = GetScreenHeight();

        // TAB opens the Physics Lab from any in-level state. Excluded from the
        // menu states, where TAB already means "browse the level cache", and
        // suppressed while one of the Lab's own text fields has focus.
        if (state != AppState::Menu && state != AppState::LevelSelect &&
            state != AppState::MacroList && state != AppState::Options &&
            !gdapp::tunerWantsKeyboard(tuner) && IsKeyPressed(KEY_TAB)) {
            tuner.open = !tuner.open;
            if (tuner.open) gdapp::refreshTunerPresets(tuner);
        }

        if (state == AppState::Menu) {
            int ch;
            while ((ch = GetCharPressed()) != 0) {
                if (ch >= '0' && ch <= '9' && idInput.size() < 10) idInput += (char)ch;
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !idInput.empty()) idInput.pop_back();
            if (IsKeyPressed(KEY_ENTER) && !idInput.empty() && GetTime() >= cooldownUntil) {
                int id = 0;
                try { id = std::stoi(idInput); } catch (...) {}
                startLoad(id);
            }
            if (IsKeyPressed(KEY_TAB)) {
                cachedLevels = gdapp::listCachedLevels();
                selectedCacheIdx = 0;
                state = AppState::LevelSelect;
            }
            if (IsKeyPressed(KEY_M)) {
                selectedMacroIdx = 0;
                state = AppState::MacroList;
            }
            if (IsKeyPressed(KEY_O)) {
                optionsReturnState = AppState::Menu;
                state = AppState::Options;
            }
            if (IsKeyPressed(KEY_I) && GetTime() >= cooldownUntil) startGdr2Import();
        } else if (state == AppState::Options) {
            if (IsKeyPressed(KEY_A)) state = optionsReturnState;
            Rectangle trailToggleRect{(float)(screenW / 2 - 160), 160.f, 320.f, 34.f};
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), trailToggleRect))
                showPhysicsTrail = !showPhysicsTrail;
            Rectangle centerPathToggleRect{(float)(screenW / 2 - 160), 250.f, 320.f, 34.f};
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), centerPathToggleRect))
                showCenterPath = !showCenterPath;
        } else if (state == AppState::LevelSelect) {
            if (IsKeyPressed(KEY_A)) { state = AppState::Menu; }
            if (!cachedLevels.empty()) {
                if (IsKeyPressed(KEY_DOWN)) selectedCacheIdx = (selectedCacheIdx + 1) % (int)cachedLevels.size();
                if (IsKeyPressed(KEY_UP))
                    selectedCacheIdx = (selectedCacheIdx - 1 + (int)cachedLevels.size()) % (int)cachedLevels.size();
                if (IsKeyPressed(KEY_ENTER))
                    startLoad(cachedLevels[selectedCacheIdx].id);

                // Mouse: click a row to select+load it directly.
                int rowH = 28, listTop = 140;
                for (int i = 0; i < (int)cachedLevels.size(); i++) {
                    Rectangle row{ (float)(screenW / 2 - 260), (float)(listTop + i * rowH), 520.f, (float)rowH };
                    if (CheckCollisionPointRec(GetMousePosition(), row)) {
                        selectedCacheIdx = i;
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) startLoad(cachedLevels[i].id);
                    }
                }
            }
        } else if (state == AppState::Loading) {
            // Blocking fetch — simplest correct behaviour for a first version;
            // the UI just shows "loading" for the (typically sub-second) request
            // (or is instant if this ID is already cached on disk).
            BeginDrawing();
            ClearBackground(Color{18, 18, 24, 255});
            gdapp::UIText("Loading...", screenW / 2 - 60, screenH / 2 - 10, 24, RAYWHITE);
            EndDrawing();

            gdapp::LevelFetchResult fetched = gdapp::fetchLevel(pendingLevelId);
            if (fetched.success) {
                decodedLevelString = std::move(fetched.levelString);
                levelName = fetched.name.empty() ? ("Level " + std::to_string(pendingLevelId)) : fetched.name;
                if (fetched.fromCache) levelName += "  [cached]";
                statusMsg.clear();
                if (importPending) {
                    importPending = false;
                    solverResult = SolverResult{};
                    solverResult.solved = true;
                    solverResult.clicks.reserve(importedPresses.size());
                    for (auto& p : importedPresses)
                        solverResult.clicks.push_back({p.framePress, p.frameRelease});
                    startReplay();
                } else {
                    resetPlayback();
                }
            } else {
                importPending = false;
                statusMsg = "Failed: " + fetched.error;
                // A rate-limit response means the server itself is telling us to
                // back off — lock out further attempts for a while instead of
                // letting the next Enter press (or an impatient retry) extend it.
                if (fetched.rateLimited) cooldownUntil = GetTime() + 90.0;
                state = AppState::Menu;
            }
        } else if (state == AppState::Playing) {
            nudgePhysicsTrailOffset();
            Rectangle solveBtnRect{(float)(screenW - 190), 10.f, 170.f, 32.f};
            bool solveBtnHover = CheckCollisionPointRec(GetMousePosition(), solveBtnRect);
            if (solveBtnHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                startSolve();
            }
            Rectangle humanLimitRect{(float)(screenW - 190), 46.f, 170.f, 24.f};
            bool humanLimitHover = CheckCollisionPointRec(GetMousePosition(), humanLimitRect);
            if (humanLimitHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                solverHumanClickLimit = !solverHumanClickLimit;
            }
            Rectangle deviationBtn{(float)(screenW - 190), 76.f, 170.f, 28.f};
            bool deviationHover = CheckCollisionPointRec(GetMousePosition(), deviationBtn);
            if (deviationHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                std::vector<gdapp::TrailPoint> simVec(trailDeque.begin(), trailDeque.end());
                writeDeviationFile(simVec);
            }

            // While a Physics Lab field has focus, typing must not also drive the
            // player — SPACE is both "jump" here and "space" in a text field.
            const bool labKeys = gdapp::tunerWantsKeyboard(tuner);
            const bool labMouse = gdapp::tunerWantsMouse(tuner, screenW);

            bool pressed = !labKeys && !labMouse &&
                           !solveBtnHover && !humanLimitHover && !deviationHover &&
                           (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP) ||
                           IsMouseButtonDown(MOUSE_BUTTON_LEFT));

            if (!labKeys && IsKeyPressed(KEY_R)) { resetPlayback(); }
            if (!labKeys && IsKeyPressed(KEY_A)) { state = AppState::Paused; }
            // Flagging works at any moment now, not just after death/clear — hover
            // an object mid-run and press J the instant something looks wrong.
            if (!labKeys && IsKeyPressed(KEY_J) && level)
                flagHitboxAt(level.get(), level->latestState(), cam, "Playing");

            if (level && state == AppState::Playing) {
                accumulator += GetFrameTime();
                if (accumulator > 0.25f) accumulator = 0.25f; // avoid spiral of death
                while (accumulator >= FIXED_DT) {
                    Player& p = level->runFrame(pressed, FIXED_DT);
                    accumulator -= FIXED_DT;

                    // Permanent for the whole attempt (not capped to a short
                    // rolling window) — on death, the full path taken is still
                    // visible to review, not just the last fraction of a second.
                    trailDeque.push_back({p.pos, p.size, p.small});

                    if (p.dead) {
                        finalPercent = 100.f * p.pos.x / level->length;
                        state = AppState::Dead;
                        break;
                    }
                    if (p.pos.x >= level->length - 5.f) {
                        finalPercent = 100.f;
                        state = AppState::Cleared;
                        break;
                    }
                }
                cam.x = level->latestState().pos.x;
                cam.y = level->latestState().pos.y;
            }
        } else if (state == AppState::Paused) {
            if (IsKeyPressed(KEY_SPACE)) state = AppState::Playing;
            if (IsKeyPressed(KEY_A)) exitRequested = true;
            if (IsKeyPressed(KEY_O)) { optionsReturnState = AppState::Paused; state = AppState::Options; }
            if (IsKeyPressed(KEY_J) && level)
                flagHitboxAt(level.get(), level->latestState(), cam, "Paused");
        } else if (state == AppState::Dead || state == AppState::Cleared) {
            if (IsKeyPressed(KEY_R)) resetPlayback();
            if (IsKeyPressed(KEY_A)) { state = AppState::Menu; level.reset(); idInput.clear(); }
            if (IsKeyPressed(KEY_J) && level)
                flagHitboxAt(level.get(), level->latestState(), cam,
                             state == AppState::Dead ? "Dead" : "Cleared");
        } else if (state == AppState::Solving) {
            // Gate the join on solverActuallyFinished (set ONLY by the thread lambda
            // after solveLevel() truly returns) — NOT solverProgress->done, which
            // Solver.cpp can set true well before the call actually returns (see that
            // field's own declaration comment). By the time solverActuallyFinished is
            // observed true the thread is already exiting, so this join() is
            // effectively instant rather than a multi-second/-minute freeze.
            if (solverActuallyFinished.load() && !solverThreadDone && solverThread.joinable()) {
                solverThread.join();
                solverThreadDone = true;
            }

            if (!solverThreadDone) {
                if (IsKeyPressed(KEY_P)) solverPaused.store(!solverPaused.load());
                if (IsKeyPressed(KEY_O)) solverCancelled.store(true);
            } else {
                if (!gdapp::tunerWantsKeyboard(tuner) && IsKeyPressed(KEY_R)) startSolve();
                // BACKSPACE, not A: this state's free camera already uses A for
                // "pan left" (WASD-style) — see below — so A would double as an
                // unwanted "back to menu" every time the camera is panned left.
                if (!gdapp::tunerWantsKeyboard(tuner) && IsKeyPressed(KEY_BACKSPACE)) state = AppState::Menu;

                if (solverResult.solved) {
                    Rectangle replayBtn{(float)(screenW - 190), 84.f, 170.f, 30.f};
                    Rectangle exportBtn{(float)(screenW - 190), 118.f, 170.f, 30.f};
                    if (CheckCollisionPointRec(GetMousePosition(), replayBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                        startReplay();
                    if (CheckCollisionPointRec(GetMousePosition(), exportBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                        exportSolve();
                }
            }

            // Free camera: fully decoupled from the search — pans/zooms only on
            // direct input, never auto-follows the beam frontier.
            float dt = GetFrameTime();
            float panSpeed = 400.f / solverCam.pixelsPerUnit * dt;
            if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) solverCam.x -= panSpeed;
            if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) solverCam.x += panSpeed;
            if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) solverCam.y += panSpeed;
            if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) solverCam.y -= panSpeed;
            float wheel = GetMouseWheelMove();
            // Same zoom model as the replay view (see render.hpp for the limits).
            if (wheel != 0.f) gdapp::zoomAt(solverCam, 1.f + wheel * 0.12f, GetMousePosition(), screenW, screenH);
            if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))      solverCam.pixelsPerUnit = std::min(gdapp::kMaxZoom, solverCam.pixelsPerUnit * 1.02f);
            if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) solverCam.pixelsPerUnit = std::max(gdapp::kMinZoom, solverCam.pixelsPerUnit / 1.02f);
        } else if (state == AppState::Replaying) {
            // The Physics Lab owns the keyboard whenever one of its fields has
            // focus, and owns the mouse whenever the cursor is over the panel —
            // otherwise typing "2" into a value would also fire a level shortcut,
            // and the wheel would zoom the world behind an open panel.
            const bool labKeys  = gdapp::tunerWantsKeyboard(tuner);
            // Correction mode owns the cursor while a pin is being dragged or the
            // cursor is over its panel, so the camera must not also pan/zoom.
            const bool labMouse = gdapp::tunerWantsMouse(tuner, screenW) ||
                                  gdapp::fitWantsMouse(fitUI, [&]{
                                      gdapp::FitUIHost h; h.screenW = screenW; h.screenH = screenH; return h; }());

            if (!labKeys && IsKeyPressed(KEY_C)) {
                fitUI.enabled = !fitUI.enabled;
                if (!fitUI.enabled) { gdapp::cancelFit(fitUI); fitUI.selectedIdx = -1; }
                // A pinned point is only meaningful against a still frame, and the
                // fit itself must be the only thing simulating while it runs.
                if (fitUI.enabled) transport.paused = true;
            }

            if (!labKeys) nudgePhysicsTrailOffset();
            // BACKSPACE, not A: same WASD-pan conflict as Solving above.
            if (!labKeys && IsKeyPressed(KEY_BACKSPACE)) state = AppState::Solving;

            // ── transport: keyboard half (the bar's own half runs at draw time,
            //    since it needs to be drawn to be clicked) ─────────────────────
            const int replayTotalFrames =
                std::max<int>(1, (int)std::max<size_t>(replayInputAt.size(),
                                                        (size_t)solverResult.levelEndEstimate));
            gdapp::TransportResult act;
            if (!labKeys) act = gdapp::transportKeys(transport, (int)replayFrameCounter, replayTotalFrames);
            pendingTransport = act;   // merged with the bar's result after drawing

            if (!labMouse) {
                Rectangle deviationBtn{(float)(screenW - 190), 10.f, 170.f, 30.f};
                if (CheckCollisionPointRec(GetMousePosition(), deviationBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    std::vector<gdapp::TrailPoint> simVec(replayTrail.begin(), replayTrail.end());
                    writeDeviationFile(simVec);
                }
            }
            // Works at any moment now (not just once the replay has ended) —
            // hover an object mid-playback and press J the instant something
            // looks wrong, no need to wait for death/clear or pause first.
            if (!labKeys && IsKeyPressed(KEY_J) && replayLevel) {
                const char* src = !replayAlive ? (replayLevel->latestState().dead ? "Replay-Died" : "Replay-Cleared")
                                                : "Replay-Live";
                flagHitboxAt(replayLevel.get(), replayLevel->latestState(), replayCam, src);
            }

            // Same free camera as Solving — kept decoupled here too, per request.
            // Ctrl+arrow is reserved for nudging the physics trail offset (see
            // nudgePhysicsTrailOffset) — held Ctrl suppresses plain-arrow pan
            // so the two controls can't both fire off the same keypress.
            bool ctrlHeld = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            float dt = GetFrameTime();
            float panSpeed = labKeys ? 0.f : 400.f / replayCam.pixelsPerUnit * dt;
            if (IsKeyDown(KEY_A)) replayCam.x -= panSpeed;
            if (IsKeyDown(KEY_D)) replayCam.x += panSpeed;
            if (IsKeyDown(KEY_W)) replayCam.y += panSpeed;
            if (IsKeyDown(KEY_S)) replayCam.y -= panSpeed;
            if (!ctrlHeld) {
                if (IsKeyDown(KEY_LEFT))  replayCam.x -= panSpeed;
                if (IsKeyDown(KEY_RIGHT)) replayCam.x += panSpeed;
                if (IsKeyDown(KEY_UP))    replayCam.y += panSpeed;
                if (IsKeyDown(KEY_DOWN))  replayCam.y -= panSpeed;
            }
            float wheel = labMouse ? 0.f : GetMouseWheelMove();
            // ── REPLAY ZOOM (this is the one that matters for trajectory work) ──
            // Cursor-anchored: at high zoom, keeping what you point at in place is
            // the difference between inspecting a point and chasing it off screen.
            // Hold SHIFT for fine steps when lining a pin up on a single frame.
            // Limits live in render.hpp (kMinZoom / kMaxZoom).
            if (wheel != 0.f) {
                float rate = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.03f : 0.12f;
                gdapp::zoomAt(replayCam, 1.f + wheel * rate, GetMousePosition(), screenW, screenH);
            }
            if (!labKeys && (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD)))      replayCam.pixelsPerUnit = std::min(gdapp::kMaxZoom, replayCam.pixelsPerUnit * 1.02f);
            if (!labKeys && (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT))) replayCam.pixelsPerUnit = std::max(gdapp::kMinZoom, replayCam.pixelsPerUnit / 1.02f);

            // Optional camera follow (transport bar toggle / F). Off by default so
            // the long-standing "camera stays where I put it" behaviour is intact.
            if (transport.followCam && replayLevel) {
                const auto& lp = replayLevel->latestState();
                replayCam.x = lp.pos.x;
                replayCam.y = lp.pos.y;
            }

            // speed 0 = frozen (frame-stepping only); scrubbing owns the frame
            // cursor while the user drags, so the sim must not also advance.
            // A running fit is stepping its OWN Level on a worker thread while
            // mutating the shared physics globals — simulating here at the same
            // time would be a data race and would also corrupt the fit's own
            // measurements, so playback stops for its duration.
            if (replayLevel && replayAlive && !transport.paused && !transport.scrubbing
                && !fitUI.running && transport.speed > 0.f) {
                replayAccumulator += GetFrameTime() * transport.speed;
                // Cap scales with the rate so a 32x pass is not silently throttled
                // back to real time, while a stall still cannot spiral.
                const float accCap = 0.25f * std::max(1.f, transport.speed);
                if (replayAccumulator > accCap) replayAccumulator = accCap;
                while (replayAccumulator >= FIXED_DT) {
                    replayFrameCounter++;
                    bool press = replayFrameCounter < replayInputAt.size() && replayInputAt[replayFrameCounter];
                    Player& p = replayLevel->runFrame(press, FIXED_DT);
                    replayAccumulator -= FIXED_DT;

                    // Permanent for the whole replay — see trailDeque's own
                    // comment in the Playing state.
                    replayTrail.push_back({p.pos, p.size, p.small});

                    if (p.dead || p.pos.x >= replayLevel->length - 5.f) {
                        replayAlive = false;
                        if (!p.dead) {
                            // Cleared, not died: keep it in the macro list. Dedupe on
                            // (levelId, click count, last click's release frame) — cheap
                            // but enough to skip re-adding the identical solve after a
                            // plain R restart of the same replay.
                            bool already = std::any_of(macroList.begin(), macroList.end(), [&](const MacroEntry& m) {
                                return m.levelId == pendingLevelId && m.clicks.size() == solverResult.clicks.size()
                                    && (solverResult.clicks.empty()
                                        || m.clicks.back().releaseFrame == solverResult.clicks.back().releaseFrame);
                            });
                            if (!already) {
                                std::time_t t = std::time(nullptr);
                                char timeBuf[32];
                                std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                                MacroEntry entry{
                                    pendingLevelId, levelName, decodedLevelString,
                                    solverResult.clicks, solverResult.levelEndEstimate, timeBuf
                                };
                                gdapp::saveMacroToDisk(entry); // persisted (compressed) even if this fails
                                macroList.push_back(std::move(entry));
                            }
                        }
                        break;
                    }
                }
            }
        } else if (state == AppState::MacroList) {
            if (IsKeyPressed(KEY_A)) state = AppState::Menu;
            if (!macroList.empty()) {
                if (IsKeyPressed(KEY_DOWN)) selectedMacroIdx = (selectedMacroIdx + 1) % (int)macroList.size();
                if (IsKeyPressed(KEY_UP))
                    selectedMacroIdx = (selectedMacroIdx - 1 + (int)macroList.size()) % (int)macroList.size();
                if (IsKeyPressed(KEY_ENTER)) startReplayFromMacro(macroList[selectedMacroIdx]);
                if (IsKeyPressed(KEY_E)) exportClicks(macroList[selectedMacroIdx].clicks,
                                                       macroList[selectedMacroIdx].levelId,
                                                       macroList[selectedMacroIdx].levelName);

                int rowH = 28, listTop = 140;
                for (int i = 0; i < (int)macroList.size(); i++) {
                    Rectangle row{ (float)(screenW / 2 - 300), (float)(listTop + i * rowH), 600.f, (float)rowH };
                    if (CheckCollisionPointRec(GetMousePosition(), row)) {
                        selectedMacroIdx = i;
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) startReplayFromMacro(macroList[i]);
                    }
                }

                Rectangle replayBtn{(float)(screenW / 2 + 310), (float)(listTop), 90.f, 26.f};
                Rectangle exportBtn{(float)(screenW / 2 + 310), (float)(listTop + 32), 90.f, 26.f};
                if (CheckCollisionPointRec(GetMousePosition(), replayBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    startReplayFromMacro(macroList[selectedMacroIdx]);
                if (CheckCollisionPointRec(GetMousePosition(), exportBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    exportClicks(macroList[selectedMacroIdx].clicks, macroList[selectedMacroIdx].levelId,
                                 macroList[selectedMacroIdx].levelName);
            }
        }

        if (exitRequested) break;

        BeginDrawing();
        ClearBackground(Color{18, 18, 24, 255});

        if (state == AppState::Menu) {
            gdapp::UIText("GD Level Player", screenW / 2 - 140, 80, 28, RAYWHITE);
            gdapp::UIText("Enter a level ID and press Enter:", screenW / 2 - 160, 160, 18, GRAY);
            std::string display = idInput + "_";
            gdapp::UIText(display.c_str(), screenW / 2 - 60, 200, 30, YELLOW);
            gdapp::UIText("TAB - browse already-downloaded levels", screenW / 2 - 150, 320, 16, Color{140,140,150,255});
            gdapp::UIText("M - macro list (cleared replays kept this session)", screenW / 2 - 190, 344, 16, Color{140,140,150,255});
            gdapp::UIText("O - options", screenW / 2 - 150, 368, 16, Color{140,140,150,255});
            gdapp::UIText("I - import a .gdr2 and replay it (compare vs the physics trail)",
                     screenW / 2 - 260, 392, 16, Color{140,140,150,255});
            if (!statusMsg.empty())
                gdapp::UIText(statusMsg.c_str(), screenW / 2 - (int)statusMsg.size() * 4, 250, 16, RED);
            if (GetTime() < importStatusUntil && !importStatusMsg.empty())
                gdapp::UIText(importStatusMsg.c_str(), screenW / 2 - (int)importStatusMsg.size() * 3, 420, 15,
                         Color{255, 170, 90, 255});
            double remaining = cooldownUntil - GetTime();
            if (remaining > 0.0) {
                const char* txt = TextFormat("Rate limited - retry available in %.0fs", remaining);
                gdapp::UIText(txt, screenW / 2 - gdapp::UITextWidth(txt, 16) / 2, 280, 16, ORANGE);
            }
        } else if (state == AppState::Options) {
            gdapp::UIText("Options", screenW / 2 - 60, 90, 28, RAYWHITE);

            Rectangle trailToggleRect{(float)(screenW / 2 - 160), 160.f, 320.f, 34.f};
            bool trailHover = CheckCollisionPointRec(GetMousePosition(), trailToggleRect);
            DrawRectangleRec(trailToggleRect, trailHover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
            DrawRectangleLinesEx(trailToggleRect, 1.5f, Color{130, 190, 230, 255});
            std::string trailLabel = std::string("Physics trail: ") + (showPhysicsTrail ? "ON" : "OFF");
            int tw = gdapp::UITextWidth(trailLabel.c_str(), 16);
            gdapp::UIText(trailLabel.c_str(), (int)(trailToggleRect.x + (trailToggleRect.width - tw) * 0.5f),
                     (int)(trailToggleRect.y + 9), 16, RAYWHITE);
            gdapp::UIText("Overlays a real GDMod_physics_<id>.txt capture (orange line) next to",
                     screenW / 2 - 260, 210, 14, Color{140, 140, 150, 255});
            gdapp::UIText("the live gdsim trail, when one exists for the loaded level.",
                     screenW / 2 - 260, 228, 14, Color{140, 140, 150, 255});

            Rectangle centerPathToggleRect{(float)(screenW / 2 - 160), 250.f, 320.f, 34.f};
            bool centerPathHover = CheckCollisionPointRec(GetMousePosition(), centerPathToggleRect);
            DrawRectangleRec(centerPathToggleRect, centerPathHover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
            DrawRectangleLinesEx(centerPathToggleRect, 1.5f, Color{130, 190, 230, 255});
            std::string centerPathLabel = std::string("Show middle of player path: ") + (showCenterPath ? "ON" : "OFF");
            int cptw = gdapp::UITextWidth(centerPathLabel.c_str(), 16);
            gdapp::UIText(centerPathLabel.c_str(), (int)(centerPathToggleRect.x + (centerPathToggleRect.width - cptw) * 0.5f),
                     (int)(centerPathToggleRect.y + 9), 16, RAYWHITE);
            gdapp::UIText("A thin cyan line through the player's centre, instead of/alongside",
                     screenW / 2 - 260, 300, 14, Color{140, 140, 150, 255});
            gdapp::UIText("the full swept-hitbox trail — easier to read as a pure path curve.",
                     screenW / 2 - 260, 318, 14, Color{140, 140, 150, 255});

            gdapp::UIText("A back", screenW / 2 - 40, screenH - 40, 14, Color{140,140,150,255});
        } else if (state == AppState::LevelSelect) {
            gdapp::UIText("Downloaded levels", screenW / 2 - 100, 90, 24, RAYWHITE);
            if (cachedLevels.empty()) {
                gdapp::UIText("(nothing cached yet - load a level by ID first)", screenW / 2 - 190, 160, 16, GRAY);
            } else {
                int rowH = 28, listTop = 140;
                for (int i = 0; i < (int)cachedLevels.size(); i++) {
                    bool sel = (i == selectedCacheIdx);
                    Rectangle row{ (float)(screenW / 2 - 260), (float)(listTop + i * rowH), 520.f, (float)(rowH - 4) };
                    if (sel) DrawRectangleRec(row, Color{50, 50, 65, 255});
                    std::string line = cachedLevels[i].name + "  (" + std::to_string(cachedLevels[i].id) + ")";
                    gdapp::UIText(line.c_str(), (int)row.x + 10, (int)row.y + 5, 16, sel ? YELLOW : RAYWHITE);
                }
            }
            gdapp::UIText("UP/DOWN + Enter, or click - A back", screenW / 2 - 140, screenH - 40, 14, Color{140,140,150,255});
        } else if (state == AppState::MacroList) {
            gdapp::UIText("Macro list (this session)", screenW / 2 - 130, 90, 24, RAYWHITE);
            if (macroList.empty()) {
                gdapp::UIText("(nothing yet - a Replay that reaches the end gets kept here)",
                         screenW / 2 - 230, 160, 16, GRAY);
            } else {
                int rowH = 28, listTop = 140;
                for (int i = 0; i < (int)macroList.size(); i++) {
                    bool sel = (i == selectedMacroIdx);
                    Rectangle row{ (float)(screenW / 2 - 300), (float)(listTop + i * rowH), 600.f, (float)(rowH - 4) };
                    if (sel) DrawRectangleRec(row, Color{50, 50, 65, 255});
                    const MacroEntry& m = macroList[i];
                    std::string line = m.levelName + "  (" + std::to_string(m.levelId) + ")  "
                                      + std::to_string(m.clicks.size()) + " clicks  " + m.recordedAt;
                    gdapp::UIText(line.c_str(), (int)row.x + 10, (int)row.y + 5, 16, sel ? YELLOW : RAYWHITE);
                }

                Rectangle replayBtn{(float)(screenW / 2 + 310), (float)(listTop), 90.f, 26.f};
                Rectangle exportBtn{(float)(screenW / 2 + 310), (float)(listTop + 32), 90.f, 26.f};
                auto drawBtn = [&](Rectangle r, const char* txt) {
                    bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                    DrawRectangleRec(r, hover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                    DrawRectangleLinesEx(r, 1.5f, Color{130, 190, 230, 255});
                    int tw = gdapp::UITextWidth(txt, 13);
                    gdapp::UIText(txt, (int)(r.x + (r.width - tw) * 0.5f), (int)(r.y + 7), 13, RAYWHITE);
                };
                drawBtn(replayBtn, "Replay");
                drawBtn(exportBtn, "Export");

                if (GetTime() < exportStatusUntil && !exportStatusMsg.empty())
                    gdapp::UIText(exportStatusMsg.c_str(), (int)replayBtn.x, (int)(listTop + 66), 13,
                             Color{170, 230, 170, 255});
            }
            gdapp::UIText("UP/DOWN select - ENTER/click replay - E export - A back",
                     screenW / 2 - 200, screenH - 40, 14, Color{140,140,150,255});
        } else if (state == AppState::Solving) {
            if (solveDrawLevel) gdapp::drawLevelGeometry(*solveDrawLevel, solverCam, screenW, screenH);

            if (solverProgress) {
                SolverProgressReport::VizSnapshot snap;
                {
                    std::lock_guard<std::mutex> lk(solverProgress->vizMtx);
                    snap = solverProgress->viz;
                }
                gdapp::drawSolverViz(snap, solverCam, screenW, screenH);

                float bestX = solverProgress->bestX.load();
                float endX  = solverProgress->levelEndX.load();
                int   clicksFound = solverProgress->clicksFound.load();
                bool  paused = solverPaused.load();

                const char* statusTxt = !solverThreadDone ? (paused ? "PAUSED" : "SEARCHING...")
                                       : (solverResult.solved ? "SOLVED" : "STOPPED");
                Color statusColor = !solverThreadDone ? (paused ? YELLOW : Color{130, 210, 255, 255})
                                  : (solverResult.solved ? GREEN : ORANGE);
                gdapp::UIText(levelName.c_str(), 12, 10, 20, RAYWHITE);
                gdapp::UIText(TextFormat("Beam search: %s", statusTxt), 12, 34, 18, statusColor);
                gdapp::UIText(TextFormat("X = %.0f / %.0f  (%.1f%%)   clicks = %d",
                                     bestX, endX, endX > 0.f ? bestX * 100.f / endX : 0.f, clicksFound),
                         12, 58, 16, GRAY);

                {
                    std::lock_guard<std::mutex> lk(solverProgress->logMtx);
                    int y = 84;
                    int startIdx = std::max(0, (int)solverProgress->log.size() - 14);
                    for (int i = startIdx; i < (int)solverProgress->log.size(); i++) {
                        gdapp::UIText(solverProgress->log[i].c_str(), 12, y, 13, Color{170, 170, 180, 220});
                        y += 16;
                    }
                }

                if (!solverThreadDone) {
                    gdapp::UIText("P pause/resume  -  O stop  -  WASD/arrows pan  -  wheel zoom",
                             12, screenH - 26, 14, Color{140, 140, 150, 255});
                } else {
                    gdapp::UIText("R re-solve  -  Backspace menu  -  WASD/arrows pan  -  wheel zoom",
                             12, screenH - 26, 14, Color{140, 140, 150, 255});
                    if (solverResult.solved) {
                        const char* txt = "SOLVED!";
                        gdapp::UIText(txt, screenW / 2 - gdapp::UITextWidth(txt, 34) / 2, 20, 34, GREEN);

                        Rectangle replayBtn{(float)(screenW - 190), 84.f, 170.f, 30.f};
                        Rectangle exportBtn{(float)(screenW - 190), 118.f, 170.f, 30.f};
                        auto drawBtn = [&](Rectangle r, const char* txt2) {
                            bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                            DrawRectangleRec(r, hover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                            DrawRectangleLinesEx(r, 1.5f, Color{130, 190, 230, 255});
                            int tw = gdapp::UITextWidth(txt2, 13);
                            gdapp::UIText(txt2, (int)(r.x + (r.width - tw) * 0.5f), (int)(r.y + 8), 13, RAYWHITE);
                        };
                        drawBtn(replayBtn, "Replay");
                        drawBtn(exportBtn, "Export .gdr2");

                        if (GetTime() < exportStatusUntil && !exportStatusMsg.empty()) {
                            int tw = gdapp::UITextWidth(exportStatusMsg.c_str(), 14);
                            gdapp::UIText(exportStatusMsg.c_str(), screenW - 190 + (170 - tw) / 2, 152, 14,
                                     Color{170, 230, 170, 255});
                        }
                    }
                }
            }
        } else if (state == AppState::Replaying) {
            if (replayLevel) {
                std::vector<gdapp::TrailPoint> trailVec(replayTrail.begin(), replayTrail.end());
                if (showPhysicsTrail) gdapp::drawPhysicsTrail(physicsTrail, physicsTrailOffsetX, physicsTrailOffsetY, replayCam, screenW, screenH);
                gdapp::drawHitboxTrail(trailVec, replayCam, screenW, screenH);
                if (showCenterPath) gdapp::drawCenterPath(trailVec, replayCam, screenW, screenH);
                gdapp::drawLevel(*replayLevel, replayLevel->latestState(), replayCam, screenW, screenH);

                // ── trajectory correction (C) ─────────────────────────────────
                // Drawn over the world so its pin sits on top of the trail, and
                // before the HUD so the panel does not cover the status text.
                {
                    gdapp::FitUIHost fh;
                    fh.trail       = &trailVec;
                    fh.realTrail   = &physicsTrail;
                    fh.realOffsetX = physicsTrailOffsetX;
                    fh.realOffsetY = physicsTrailOffsetY;
                    fh.levelString = &decodedLevelString;
                    fh.inputAt     = &replayInputAt;
                    fh.cam         = replayCam;
                    fh.screenW     = screenW;
                    fh.screenH     = screenH;
                    fh.blockMouse  = gdapp::tunerWantsMouse(tuner, screenW);
                    fh.blockKeys   = gdapp::tunerWantsKeyboard(tuner);
                    if (gdapp::updateFitUI(fitUI, fh)) {
                        // A candidate was applied — re-run to the same frame so the
                        // trail immediately shows whether it actually landed on the
                        // target, which is the only honest confirmation.
                        rebuildReplayTo((int)replayFrameCounter);
                    }
                }

                float pct = 100.f * replayLevel->latestState().pos.x / replayLevel->length;
                bool died = replayLevel->latestState().dead;
                gdapp::UIText(levelName.c_str(), 12, 10, 20, RAYWHITE);
                gdapp::UIText(TextFormat("Replay: %.1f%%", pct), 12, 34, 18, GRAY);
                const char* status = replayAlive ? (transport.paused ? "PAUSED" : "PLAYING")
                                    : (died ? "DIED (unexpected!)" : "CLEARED");
                Color statusColor = replayAlive ? (transport.paused ? YELLOW : Color{130, 210, 255, 255})
                                  : (died ? RED : GREEN);
                gdapp::UIText(status, 12, 58, 16, statusColor);
                // Zoom readout: at 400 px/unit it is easy to lose track of scale,
                // and "how many pixels is one world unit" is the number that
                // actually matters when judging a sub-unit deviation.
                gdapp::UIText(TextFormat("zoom %.2f px/unit   (wheel, shift=fine)", replayCam.pixelsPerUnit),
                         12, 76, 13, Color{140, 140, 150, 255});
                // Lifted clear of the transport bar (56px tall, drawn at the very
                // bottom) so the two never overlap.
                gdapp::UIText("TAB physics lab  -  C correct trajectory  -  SPACE pause  -  , . step  -  [ ] speed  -  R restart  -  F follow",
                         12, screenH - 78, 14, Color{140, 140, 150, 255});

                {
                    Rectangle deviationBtn{(float)(screenW - 190), 10.f, 170.f, 30.f};
                    bool hover = CheckCollisionPointRec(GetMousePosition(), deviationBtn);
                    DrawRectangleRec(deviationBtn, hover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                    DrawRectangleLinesEx(deviationBtn, 1.5f, Color{130, 190, 230, 255});
                    const char* btnTxt = "Export deviation";
                    int tw = gdapp::UITextWidth(btnTxt, 14);
                    gdapp::UIText(btnTxt, (int)(deviationBtn.x + (deviationBtn.width - tw) * 0.5f),
                             (int)(deviationBtn.y + 8), 14, RAYWHITE);
                    if (GetTime() < deviationStatusUntil && !deviationStatusMsg.empty())
                        gdapp::UIText(deviationStatusMsg.c_str(), (int)deviationBtn.x, (int)(deviationBtn.y + 36), 13,
                                 Color{170, 230, 170, 255});
                }
                if (GetTime() < importStatusUntil && !importStatusMsg.empty())
                    gdapp::UIText(importStatusMsg.c_str(), 12, 128, 15, Color{255, 170, 90, 255});
                if (showPhysicsTrail) {
                    gdapp::UIText(TextFormat("Physics trail offset: X=%.1f Y=%.1f  (ctrl+arrows, shift=fast)",
                                        physicsTrailOffsetX, physicsTrailOffsetY),
                             12, screenH - 98, 14, Color{255, 170, 90, 255});
                }

                // Suppressed in correction mode: both tools highlight whatever is
                // under the cursor, and two overlapping highlights read as a bug.
                // Hitbox-mismatch flagging: available at any moment (live or
                // ended). Live preview highlight (before J is pressed) plus the
                // confirmation message after.
                if (!fitUI.enabled) {
                    Vector2 mouse = GetMousePosition();
                    Vec2D worldPos = gdapp::screenToWorld(mouse.x, mouse.y, replayCam, screenW, screenH);
                    auto picked = gdapp::pickObjectNear(*replayLevel, worldPos, replayLevel->latestState().frame);
                    if (picked.found) {
                        Vector2 c{ screenW * 0.5f + (picked.pos.x - replayCam.x) * replayCam.pixelsPerUnit,
                                   screenH * 0.5f - (picked.pos.y - replayCam.y) * replayCam.pixelsPerUnit };
                        float hw = picked.size.x * 0.5f * replayCam.pixelsPerUnit + 3.f;
                        float hh = picked.size.y * 0.5f * replayCam.pixelsPerUnit + 3.f;
                        DrawRectangleLinesEx({c.x - hw, c.y - hh, hw * 2, hh * 2}, 2.5f, Color{255, 255, 90, 230});
                        gdapp::UIText(TextFormat("typeId=%d - press J to flag", picked.typeId),
                                 (int)(c.x - hw), (int)(c.y - hh - 18), 14, Color{255, 255, 140, 255});
                    }
                    gdapp::UIText("Hover an object + press J to flag its hitbox as wrong",
                             12, 84, 14, Color{160, 200, 255, 255});
                    if (GetTime() < flagStatusUntil && !flagStatusMsg.empty())
                        gdapp::UIText(flagStatusMsg.c_str(), 12, 104, 15, Color{170, 230, 170, 255});
                }
            }
        } else if (level) {
            std::vector<gdapp::TrailPoint> trailVec(trailDeque.begin(), trailDeque.end());
            if (showPhysicsTrail) gdapp::drawPhysicsTrail(physicsTrail, physicsTrailOffsetX, physicsTrailOffsetY, cam, screenW, screenH);
            gdapp::drawHitboxTrail(trailVec, cam, screenW, screenH);
            if (showCenterPath) gdapp::drawCenterPath(trailVec, cam, screenW, screenH);
            gdapp::drawLevel(*level, level->latestState(), cam, screenW, screenH);

            float pct = 100.f * level->latestState().pos.x / level->length;
            gdapp::UIText(levelName.c_str(), 12, 10, 20, RAYWHITE);
            gdapp::UIText(TextFormat("%.1f%%", pct), 12, 34, 18, GRAY);
            gdapp::UIText("SPACE/click to jump  -  R restart  -  A pause  -  TAB physics lab", 12, screenH - 26, 14, Color{140,140,150,255});
            if (showPhysicsTrail) {
                gdapp::UIText(TextFormat("Physics trail offset: X=%.1f Y=%.1f  (ctrl+arrows, shift=fast)",
                                    physicsTrailOffsetX, physicsTrailOffsetY),
                         12, screenH - 46, 14, Color{255, 170, 90, 255});
            }

            if (state == AppState::Playing) {
                Rectangle solveBtnRect{(float)(screenW - 190), 10.f, 170.f, 32.f};
                bool hover = CheckCollisionPointRec(GetMousePosition(), solveBtnRect);
                DrawRectangleRec(solveBtnRect, hover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                DrawRectangleLinesEx(solveBtnRect, 1.5f, Color{130, 190, 230, 255});
                const char* btnTxt = "SOLVE (Beam Search)";
                int tw = gdapp::UITextWidth(btnTxt, 14);
                gdapp::UIText(btnTxt, (int)(solveBtnRect.x + (solveBtnRect.width - tw) * 0.5f),
                         (int)(solveBtnRect.y + 9), 14, RAYWHITE);

                // Toggle: OFF by default (see solverHumanClickLimit's declaration) —
                // a valid beam solve that just needs a faster-than-human click gets
                // discarded and the search restarts from scratch through the much
                // slower fallbacks otherwise, which reads as "it clears then keeps
                // retrying forever" with no visible explanation.
                Rectangle humanLimitRect{(float)(screenW - 190), 46.f, 170.f, 24.f};
                bool hlHover = CheckCollisionPointRec(GetMousePosition(), humanLimitRect);
                DrawRectangleRec(humanLimitRect, hlHover ? Color{60, 60, 75, 220} : Color{35, 35, 45, 190});
                DrawRectangleLinesEx(humanLimitRect, 1.f, Color{110, 110, 130, 200});
                Rectangle checkBox{humanLimitRect.x + 6, humanLimitRect.y + 5, 14, 14};
                DrawRectangleLinesEx(checkBox, 1.5f, Color{170, 170, 190, 255});
                if (solverHumanClickLimit)
                    DrawRectangle((int)checkBox.x + 3, (int)checkBox.y + 3, 8, 8, Color{130, 200, 255, 255});
                gdapp::UIText("Human click limit (14/s)", (int)(checkBox.x + 20), (int)(humanLimitRect.y + 6), 12,
                         Color{200, 200, 210, 255});

                Rectangle deviationBtn{(float)(screenW - 190), 76.f, 170.f, 28.f};
                bool devHover = CheckCollisionPointRec(GetMousePosition(), deviationBtn);
                DrawRectangleRec(deviationBtn, devHover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                DrawRectangleLinesEx(deviationBtn, 1.5f, Color{130, 190, 230, 255});
                const char* devTxt = "Export deviation";
                int devTw = gdapp::UITextWidth(devTxt, 13);
                gdapp::UIText(devTxt, (int)(deviationBtn.x + (deviationBtn.width - devTw) * 0.5f),
                         (int)(deviationBtn.y + 7), 13, RAYWHITE);
                if (GetTime() < deviationStatusUntil && !deviationStatusMsg.empty())
                    gdapp::UIText(deviationStatusMsg.c_str(), (int)deviationBtn.x, (int)(deviationBtn.y + 34), 12,
                             Color{170, 230, 170, 255});
            }

            if (state == AppState::Paused) {
                gdapp::UIText("PAUSED", screenW / 2 - 70, screenH / 2 - 40, 40, YELLOW);
                gdapp::UIText("SPACE to resume  -  A to quit  -  O options", screenW / 2 - 150, screenH / 2 + 10, 16, GRAY);
            } else if (state == AppState::Dead) {
                gdapp::UIText("DIED", screenW / 2 - 40, screenH / 2 - 40, 40, RED);
                gdapp::UIText(TextFormat("%.2f%% of the level", finalPercent), screenW / 2 - 90, screenH / 2 + 10, 18, RAYWHITE);
                gdapp::UIText("R to retry  -  A for menu", screenW / 2 - 110, screenH / 2 + 40, 16, GRAY);
            } else if (state == AppState::Cleared) {
                gdapp::UIText("CLEARED!", screenW / 2 - 80, screenH / 2 - 40, 40, GREEN);
                gdapp::UIText("R to replay  -  A for menu", screenW / 2 - 110, screenH / 2 + 10, 16, GRAY);
            }

            // Hitbox-mismatch flagging: available at any moment (Playing,
            // Paused, Dead, Cleared) — live preview highlight of whichever
            // object the cursor is over (before J is pressed) plus the
            // confirmation message after — see flagHitboxAt's own comment.
            {
                Vector2 mouse = GetMousePosition();
                Vec2D worldPos = gdapp::screenToWorld(mouse.x, mouse.y, cam, screenW, screenH);
                auto picked = gdapp::pickObjectNear(*level, worldPos, level->latestState().frame);
                if (picked.found) {
                    Vector2 c{ screenW * 0.5f + (picked.pos.x - cam.x) * cam.pixelsPerUnit,
                               screenH * 0.5f - (picked.pos.y - cam.y) * cam.pixelsPerUnit };
                    float hw = picked.size.x * 0.5f * cam.pixelsPerUnit + 3.f;
                    float hh = picked.size.y * 0.5f * cam.pixelsPerUnit + 3.f;
                    DrawRectangleLinesEx({c.x - hw, c.y - hh, hw * 2, hh * 2}, 2.5f, Color{255, 255, 90, 230});
                    gdapp::UIText(TextFormat("typeId=%d - press J to flag", picked.typeId),
                             (int)(c.x - hw), (int)(c.y - hh - 18), 14, Color{255, 255, 140, 255});
                }
                gdapp::UIText("Hover an object + press J to flag its hitbox as wrong",
                         screenW / 2 - 190, screenH / 2 + 64, 14, Color{160, 200, 255, 255});
                if (GetTime() < flagStatusUntil && !flagStatusMsg.empty()) {
                    int tw = gdapp::UITextWidth(flagStatusMsg.c_str(), 15);
                    gdapp::UIText(flagStatusMsg.c_str(), screenW / 2 - tw / 2, screenH / 2 + 84, 15,
                             Color{170, 230, 170, 255});
                }
            }
        }

        // ── Replay transport bar ─────────────────────────────────────────────
        // Drawn after the world so it sits on top, and before the Lab panel so
        // the panel overlaps it rather than the other way round.
        if (state == AppState::Replaying && replayLevel) {
            const auto& lp = replayLevel->latestState();
            const int total = std::max<int>(1, (int)std::max<size_t>(
                replayInputAt.size(), (size_t)solverResult.levelEndEstimate));
            std::string status = TextFormat("x=%.2f  y=%.2f  vel=%.2f  %s%s",
                                            lp.pos.x, lp.pos.y, lp.velocity,
                                            lp.small ? "mini " : "",
                                            lp.upsideDown ? "flipped" : "");
            auto barAct = gdapp::drawTransport(transport, (int)replayFrameCounter, total,
                                                status, replayAlive, screenW, screenH,
                                                gdapp::tunerWantsMouse(tuner, screenW));

            // Merge the keyboard half collected during the input pass.
            gdapp::TransportResult a = pendingTransport;
            a.restart    |= barAct.restart;
            a.stepFrames += barAct.stepFrames;
            if (barAct.seek) { a.seek = true; a.seekFrame = barAct.seekFrame; }

            if (a.restart) {
                startReplay();
            } else if (a.seek) {
                rebuildReplayTo(std::max(0, a.seekFrame));
            } else if (a.stepFrames != 0) {
                if (a.stepFrames > 0 && replayAlive) {
                    // Forward is just more simulation — no rebuild needed.
                    for (int i = 0; i < a.stepFrames && replayAlive; i++) {
                        replayFrameCounter++;
                        bool press = replayFrameCounter < replayInputAt.size()
                                  && replayInputAt[replayFrameCounter];
                        Player& p = replayLevel->runFrame(press, FIXED_DT);
                        replayTrail.push_back({p.pos, p.size, p.small});
                        if (p.dead || p.pos.x >= replayLevel->length - 5.f) { replayAlive = false; break; }
                    }
                } else if (a.stepFrames < 0) {
                    // Backward needs a rebuild: gdsim integrates forward only.
                    rebuildReplayTo(std::max(0, (int)replayFrameCounter + a.stepFrames));
                }
                // Stepping implies "I want to look at this frame", so stop rolling.
                transport.paused = true;
            }
            pendingTransport = {};
        }

        // ── Physics Lab ──────────────────────────────────────────────────────
        // Available from every state that has a world on screen. Drawn last so it
        // is always on top of whatever HUD that state put up.
        if (state == AppState::Replaying || state == AppState::Playing ||
            state == AppState::Paused    || state == AppState::Dead    ||
            state == AppState::Cleared   || state == AppState::Solving) {
            const bool physChanged = gdapp::updateTunerPanel(tuner, screenW, screenH);
            // Same reason as the playback guard: never simulate on this thread
            // while the fitter is probing the shared physics globals.
            if (physChanged && tuner.resimOnChange && !fitUI.running) {
                // Re-run to the SAME frame so the effect of the edit is visible at
                // the point being studied, not from the level start. This is the
                // whole point of the panel: change a constant, watch the trail
                // move under the same input track.
                if (state == AppState::Replaying && replayLevel)
                    rebuildReplayTo((int)replayFrameCounter);
                else if (state == AppState::Playing && level)
                    resetPlayback();   // live input can't be replayed; start over
            }
            if (GetTime() < resimFlashUntil)
                gdapp::UIText(TextFormat("re-simulated %d frames", resimFrames), 12, 128, 14,
                         Color{130, 210, 255, 255});
        }

        EndDrawing();
    }

    // The window can close (WindowShouldClose / exitRequested) while a solve is
    // still running in the background — request cancellation and wait for it so
    // the thread never touches torn-down stack state (solverResult, the progress
    // report) after main() returns.
    if (solverThread.joinable()) {
        solverCancelled.store(true);
        solverThread.join();
    }

    CloseWindow();
    return 0;
}
