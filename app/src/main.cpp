// GD Level Player — fetches a real Geometry Dash level by ID directly from
// RobTop's servers and plays it through gdsim (the same physics engine
// developed/validated in this repo) with a minimalist vector-shape renderer.
#include "raylib.h"
#include "leveldata.hpp"
#include "render.hpp"
#include "exportdialog.hpp"
#include "../../src/sim/Level.hpp"
#include "../../src/sim/Solver.hpp"
#include "../../src/sim/Gdr2Export.hpp"
#include <memory>
#include <string>
#include <cmath>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <ctime>

using namespace gdsim;

static constexpr float FIXED_DT = 1.f / 240.f;
static constexpr size_t TRAIL_MAX = 200; // ~0.83s of history at 240Hz

enum class AppState { Menu, LevelSelect, Loading, Playing, Paused, Dead, Cleared, Solving, Replaying };

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "GD Level Player");
    SetTargetFPS(144);

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
    bool replayPaused = false;
    std::deque<gdapp::TrailPoint> replayTrail;
    gdapp::Camera2DState replayCam;
    std::string exportStatusMsg;
    double exportStatusUntil = 0.0;

    // ========== Hitbox-mismatch flagging (debug tool) ==========
    // gdsim's physics don't always match real GD exactly, and screenshots
    // alone haven't been enough to pin down where. This lets the user, at the
    // moment of death/clear, hover the object they believe was wrongly
    // passable (or wrongly solid) and press J to record a precise, per-object
    // report — level, frame, sim's own death cause, and the flagged object's
    // typeId/pos/size/rotation — to hitbox_flags.txt for later investigation.
    std::string flagStatusMsg;
    double flagStatusUntil = 0.0;

    auto startLoad = [&](int id) {
        pendingLevelId = id;
        state = AppState::Loading;
    };

    auto resetPlayback = [&]() {
        level = std::make_unique<Level>(decodedLevelString);
        accumulator = 0.f;
        cam.x = 0.f; cam.y = 0.f;
        trailDeque.clear();
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
        replayPaused = false;
        replayTrail.clear();
        replayCam.x = 0.f; replayCam.y = 105.f; replayCam.pixelsPerUnit = 3.f;
        state = AppState::Replaying;
    };

    auto exportSolve = [&]() {
        if (!solverResult.solved) return;
        std::string safeName = levelName;
        for (auto& c : safeName)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (safeName.empty()) safeName = "replay";
        std::string defaultFileName = safeName + "-" + std::to_string(pendingLevelId) + ".gdr2";

        auto picked = gdapp::pickGdr2SavePath(defaultFileName, gdapp::loadLastExportFolder());
        if (!picked) return;

        auto bytes = gdsim::exportClicksToGdr2(solverResult.clicks, 240.0, pendingLevelId, levelName);
        std::ofstream f(*picked, std::ios::binary | std::ios::trunc);
        bool ok = f.is_open();
        if (ok) { f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()); ok = f.good(); }
        if (ok) gdapp::saveLastExportFolder(std::filesystem::path(*picked).parent_path().string());

        exportStatusMsg  = ok ? ("Exported to " + std::filesystem::path(*picked).filename().string())
                               : "Export failed";
        exportStatusUntil = GetTime() + 3.0;
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
        } else if (state == AppState::LevelSelect) {
            if (IsKeyPressed(KEY_ESCAPE)) { state = AppState::Menu; }
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
            DrawText("Loading...", screenW / 2 - 60, screenH / 2 - 10, 24, RAYWHITE);
            EndDrawing();

            gdapp::LevelFetchResult fetched = gdapp::fetchLevel(pendingLevelId);
            if (fetched.success) {
                decodedLevelString = std::move(fetched.levelString);
                levelName = fetched.name.empty() ? ("Level " + std::to_string(pendingLevelId)) : fetched.name;
                if (fetched.fromCache) levelName += "  [cached]";
                statusMsg.clear();
                resetPlayback();
            } else {
                statusMsg = "Failed: " + fetched.error;
                // A rate-limit response means the server itself is telling us to
                // back off — lock out further attempts for a while instead of
                // letting the next Enter press (or an impatient retry) extend it.
                if (fetched.rateLimited) cooldownUntil = GetTime() + 90.0;
                state = AppState::Menu;
            }
        } else if (state == AppState::Playing) {
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

            bool pressed = !solveBtnHover && !humanLimitHover &&
                           (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP) ||
                           IsMouseButtonDown(MOUSE_BUTTON_LEFT));

            if (IsKeyPressed(KEY_R)) { resetPlayback(); }
            if (IsKeyPressed(KEY_ESCAPE)) { state = AppState::Paused; }
            // Flagging works at any moment now, not just after death/clear — hover
            // an object mid-run and press J the instant something looks wrong.
            if (IsKeyPressed(KEY_J) && level)
                flagHitboxAt(level.get(), level->latestState(), cam, "Playing");

            if (level && state == AppState::Playing) {
                accumulator += GetFrameTime();
                if (accumulator > 0.25f) accumulator = 0.25f; // avoid spiral of death
                while (accumulator >= FIXED_DT) {
                    Player& p = level->runFrame(pressed, FIXED_DT);
                    accumulator -= FIXED_DT;

                    trailDeque.push_back({p.pos, p.size, p.small});
                    if (trailDeque.size() > TRAIL_MAX) trailDeque.pop_front();

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
            if (IsKeyPressed(KEY_ESCAPE)) exitRequested = true;
            if (IsKeyPressed(KEY_J) && level)
                flagHitboxAt(level.get(), level->latestState(), cam, "Paused");
        } else if (state == AppState::Dead || state == AppState::Cleared) {
            if (IsKeyPressed(KEY_R)) resetPlayback();
            if (IsKeyPressed(KEY_ESCAPE)) { state = AppState::Menu; level.reset(); idInput.clear(); }
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
                if (IsKeyPressed(KEY_R)) startSolve();
                if (IsKeyPressed(KEY_ESCAPE)) state = AppState::Menu;

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
            if (wheel != 0.f) solverCam.pixelsPerUnit = std::clamp(solverCam.pixelsPerUnit * (1.f + wheel * 0.1f), 0.3f, 14.f);
            if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))      solverCam.pixelsPerUnit = std::min(14.f, solverCam.pixelsPerUnit * 1.02f);
            if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) solverCam.pixelsPerUnit = std::max(0.3f, solverCam.pixelsPerUnit / 1.02f);
        } else if (state == AppState::Replaying) {
            if (IsKeyPressed(KEY_SPACE)) replayPaused = !replayPaused;
            if (IsKeyPressed(KEY_R)) startReplay();
            if (IsKeyPressed(KEY_ESCAPE)) state = AppState::Solving;
            // Works at any moment now (not just once the replay has ended) —
            // hover an object mid-playback and press J the instant something
            // looks wrong, no need to wait for death/clear or pause first.
            if (IsKeyPressed(KEY_J) && replayLevel) {
                const char* src = !replayAlive ? (replayLevel->latestState().dead ? "Replay-Died" : "Replay-Cleared")
                                                : "Replay-Live";
                flagHitboxAt(replayLevel.get(), replayLevel->latestState(), replayCam, src);
            }

            // Same free camera as Solving — kept decoupled here too, per request.
            float dt = GetFrameTime();
            float panSpeed = 400.f / replayCam.pixelsPerUnit * dt;
            if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) replayCam.x -= panSpeed;
            if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) replayCam.x += panSpeed;
            if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) replayCam.y += panSpeed;
            if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) replayCam.y -= panSpeed;
            float wheel = GetMouseWheelMove();
            if (wheel != 0.f) replayCam.pixelsPerUnit = std::clamp(replayCam.pixelsPerUnit * (1.f + wheel * 0.1f), 0.3f, 14.f);
            if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))      replayCam.pixelsPerUnit = std::min(14.f, replayCam.pixelsPerUnit * 1.02f);
            if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) replayCam.pixelsPerUnit = std::max(0.3f, replayCam.pixelsPerUnit / 1.02f);

            if (replayLevel && replayAlive && !replayPaused) {
                replayAccumulator += GetFrameTime();
                if (replayAccumulator > 0.25f) replayAccumulator = 0.25f;
                while (replayAccumulator >= FIXED_DT) {
                    replayFrameCounter++;
                    bool press = replayFrameCounter < replayInputAt.size() && replayInputAt[replayFrameCounter];
                    Player& p = replayLevel->runFrame(press, FIXED_DT);
                    replayAccumulator -= FIXED_DT;

                    replayTrail.push_back({p.pos, p.size, p.small});
                    if (replayTrail.size() > TRAIL_MAX) replayTrail.pop_front();

                    if (p.dead || p.pos.x >= replayLevel->length - 5.f) {
                        replayAlive = false;
                        break;
                    }
                }
            }
        }

        if (exitRequested) break;

        BeginDrawing();
        ClearBackground(Color{18, 18, 24, 255});

        if (state == AppState::Menu) {
            DrawText("GD Level Player", screenW / 2 - 140, 80, 28, RAYWHITE);
            DrawText("Enter a level ID and press Enter:", screenW / 2 - 160, 160, 18, GRAY);
            std::string display = idInput + "_";
            DrawText(display.c_str(), screenW / 2 - 60, 200, 30, YELLOW);
            DrawText("TAB - browse already-downloaded levels", screenW / 2 - 150, 320, 16, Color{140,140,150,255});
            if (!statusMsg.empty())
                DrawText(statusMsg.c_str(), screenW / 2 - (int)statusMsg.size() * 4, 250, 16, RED);
            double remaining = cooldownUntil - GetTime();
            if (remaining > 0.0) {
                const char* txt = TextFormat("Rate limited - retry available in %.0fs", remaining);
                DrawText(txt, screenW / 2 - MeasureText(txt, 16) / 2, 280, 16, ORANGE);
            }
        } else if (state == AppState::LevelSelect) {
            DrawText("Downloaded levels", screenW / 2 - 100, 90, 24, RAYWHITE);
            if (cachedLevels.empty()) {
                DrawText("(nothing cached yet - load a level by ID first)", screenW / 2 - 190, 160, 16, GRAY);
            } else {
                int rowH = 28, listTop = 140;
                for (int i = 0; i < (int)cachedLevels.size(); i++) {
                    bool sel = (i == selectedCacheIdx);
                    Rectangle row{ (float)(screenW / 2 - 260), (float)(listTop + i * rowH), 520.f, (float)(rowH - 4) };
                    if (sel) DrawRectangleRec(row, Color{50, 50, 65, 255});
                    std::string line = cachedLevels[i].name + "  (" + std::to_string(cachedLevels[i].id) + ")";
                    DrawText(line.c_str(), (int)row.x + 10, (int)row.y + 5, 16, sel ? YELLOW : RAYWHITE);
                }
            }
            DrawText("UP/DOWN + Enter, or click - ESC back", screenW / 2 - 140, screenH - 40, 14, Color{140,140,150,255});
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
                DrawText(levelName.c_str(), 12, 10, 20, RAYWHITE);
                DrawText(TextFormat("Beam search: %s", statusTxt), 12, 34, 18, statusColor);
                DrawText(TextFormat("X = %.0f / %.0f  (%.1f%%)   clicks = %d",
                                     bestX, endX, endX > 0.f ? bestX * 100.f / endX : 0.f, clicksFound),
                         12, 58, 16, GRAY);

                {
                    std::lock_guard<std::mutex> lk(solverProgress->logMtx);
                    int y = 84;
                    int startIdx = std::max(0, (int)solverProgress->log.size() - 14);
                    for (int i = startIdx; i < (int)solverProgress->log.size(); i++) {
                        DrawText(solverProgress->log[i].c_str(), 12, y, 13, Color{170, 170, 180, 220});
                        y += 16;
                    }
                }

                if (!solverThreadDone) {
                    DrawText("P pause/resume  -  O stop  -  WASD/arrows pan  -  wheel zoom",
                             12, screenH - 26, 14, Color{140, 140, 150, 255});
                } else {
                    DrawText("R re-solve  -  ESC menu  -  WASD/arrows pan  -  wheel zoom",
                             12, screenH - 26, 14, Color{140, 140, 150, 255});
                    if (solverResult.solved) {
                        const char* txt = "SOLVED!";
                        DrawText(txt, screenW / 2 - MeasureText(txt, 34) / 2, 20, 34, GREEN);

                        Rectangle replayBtn{(float)(screenW - 190), 84.f, 170.f, 30.f};
                        Rectangle exportBtn{(float)(screenW - 190), 118.f, 170.f, 30.f};
                        auto drawBtn = [&](Rectangle r, const char* txt2) {
                            bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                            DrawRectangleRec(r, hover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                            DrawRectangleLinesEx(r, 1.5f, Color{130, 190, 230, 255});
                            int tw = MeasureText(txt2, 13);
                            DrawText(txt2, (int)(r.x + (r.width - tw) * 0.5f), (int)(r.y + 8), 13, RAYWHITE);
                        };
                        drawBtn(replayBtn, "Replay");
                        drawBtn(exportBtn, "Export .gdr2");

                        if (GetTime() < exportStatusUntil && !exportStatusMsg.empty()) {
                            int tw = MeasureText(exportStatusMsg.c_str(), 14);
                            DrawText(exportStatusMsg.c_str(), screenW - 190 + (170 - tw) / 2, 152, 14,
                                     Color{170, 230, 170, 255});
                        }
                    }
                }
            }
        } else if (state == AppState::Replaying) {
            if (replayLevel) {
                std::vector<gdapp::TrailPoint> trailVec(replayTrail.begin(), replayTrail.end());
                gdapp::drawHitboxTrail(trailVec, replayCam, screenW, screenH);
                gdapp::drawLevel(*replayLevel, replayLevel->latestState(), replayCam, screenW, screenH);

                float pct = 100.f * replayLevel->latestState().pos.x / replayLevel->length;
                bool died = replayLevel->latestState().dead;
                DrawText(levelName.c_str(), 12, 10, 20, RAYWHITE);
                DrawText(TextFormat("Replay: %.1f%%", pct), 12, 34, 18, GRAY);
                const char* status = replayAlive ? (replayPaused ? "PAUSED" : "PLAYING")
                                    : (died ? "DIED (unexpected!)" : "CLEARED");
                Color statusColor = replayAlive ? (replayPaused ? YELLOW : Color{130, 210, 255, 255})
                                  : (died ? RED : GREEN);
                DrawText(status, 12, 58, 16, statusColor);
                DrawText("SPACE pause/resume  -  R restart  -  ESC back  -  WASD/arrows pan  -  wheel zoom",
                         12, screenH - 26, 14, Color{140, 140, 150, 255});

                // Hitbox-mismatch flagging: available at any moment (live or
                // ended). Live preview highlight (before J is pressed) plus the
                // confirmation message after.
                {
                    Vector2 mouse = GetMousePosition();
                    Vec2D worldPos = gdapp::screenToWorld(mouse.x, mouse.y, replayCam, screenW, screenH);
                    auto picked = gdapp::pickObjectNear(*replayLevel, worldPos, replayLevel->latestState().frame);
                    if (picked.found) {
                        Vector2 c{ screenW * 0.5f + (picked.pos.x - replayCam.x) * replayCam.pixelsPerUnit,
                                   screenH * 0.5f - (picked.pos.y - replayCam.y) * replayCam.pixelsPerUnit };
                        float hw = picked.size.x * 0.5f * replayCam.pixelsPerUnit + 3.f;
                        float hh = picked.size.y * 0.5f * replayCam.pixelsPerUnit + 3.f;
                        DrawRectangleLinesEx({c.x - hw, c.y - hh, hw * 2, hh * 2}, 2.5f, Color{255, 255, 90, 230});
                        DrawText(TextFormat("typeId=%d - press J to flag", picked.typeId),
                                 (int)(c.x - hw), (int)(c.y - hh - 18), 14, Color{255, 255, 140, 255});
                    }
                    DrawText("Hover an object + press J to flag its hitbox as wrong",
                             12, 84, 14, Color{160, 200, 255, 255});
                    if (GetTime() < flagStatusUntil && !flagStatusMsg.empty())
                        DrawText(flagStatusMsg.c_str(), 12, 104, 15, Color{170, 230, 170, 255});
                }
            }
        } else if (level) {
            std::vector<gdapp::TrailPoint> trailVec(trailDeque.begin(), trailDeque.end());
            gdapp::drawHitboxTrail(trailVec, cam, screenW, screenH);
            gdapp::drawLevel(*level, level->latestState(), cam, screenW, screenH);

            float pct = 100.f * level->latestState().pos.x / level->length;
            DrawText(levelName.c_str(), 12, 10, 20, RAYWHITE);
            DrawText(TextFormat("%.1f%%", pct), 12, 34, 18, GRAY);
            DrawText("SPACE/click to jump  -  R restart  -  ESC pause", 12, screenH - 26, 14, Color{140,140,150,255});

            if (state == AppState::Playing) {
                Rectangle solveBtnRect{(float)(screenW - 190), 10.f, 170.f, 32.f};
                bool hover = CheckCollisionPointRec(GetMousePosition(), solveBtnRect);
                DrawRectangleRec(solveBtnRect, hover ? Color{70, 110, 150, 230} : Color{45, 55, 70, 210});
                DrawRectangleLinesEx(solveBtnRect, 1.5f, Color{130, 190, 230, 255});
                const char* btnTxt = "SOLVE (Beam Search)";
                int tw = MeasureText(btnTxt, 14);
                DrawText(btnTxt, (int)(solveBtnRect.x + (solveBtnRect.width - tw) * 0.5f),
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
                DrawText("Human click limit (14/s)", (int)(checkBox.x + 20), (int)(humanLimitRect.y + 6), 12,
                         Color{200, 200, 210, 255});
            }

            if (state == AppState::Paused) {
                DrawText("PAUSED", screenW / 2 - 70, screenH / 2 - 40, 40, YELLOW);
                DrawText("SPACE to resume  -  ESC to quit", screenW / 2 - 120, screenH / 2 + 10, 16, GRAY);
            } else if (state == AppState::Dead) {
                DrawText("DIED", screenW / 2 - 40, screenH / 2 - 40, 40, RED);
                DrawText(TextFormat("%.2f%% of the level", finalPercent), screenW / 2 - 90, screenH / 2 + 10, 18, RAYWHITE);
                DrawText("R to retry  -  ESC for menu", screenW / 2 - 110, screenH / 2 + 40, 16, GRAY);
            } else if (state == AppState::Cleared) {
                DrawText("CLEARED!", screenW / 2 - 80, screenH / 2 - 40, 40, GREEN);
                DrawText("R to replay  -  ESC for menu", screenW / 2 - 110, screenH / 2 + 10, 16, GRAY);
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
                    DrawText(TextFormat("typeId=%d - press J to flag", picked.typeId),
                             (int)(c.x - hw), (int)(c.y - hh - 18), 14, Color{255, 255, 140, 255});
                }
                DrawText("Hover an object + press J to flag its hitbox as wrong",
                         screenW / 2 - 190, screenH / 2 + 64, 14, Color{160, 200, 255, 255});
                if (GetTime() < flagStatusUntil && !flagStatusMsg.empty()) {
                    int tw = MeasureText(flagStatusMsg.c_str(), 15);
                    DrawText(flagStatusMsg.c_str(), screenW / 2 - tw / 2, screenH / 2 + 84, 15,
                             Color{170, 230, 170, 255});
                }
            }
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
