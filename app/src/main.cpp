// GD Level Player — fetches a real Geometry Dash level by ID directly from
// RobTop's servers and plays it through gdsim (the same physics engine
// developed/validated in this repo) with a minimalist vector-shape renderer.
#include "raylib.h"
#include "leveldata.hpp"
#include "render.hpp"
#include "../../src/sim/Level.hpp"
#include <memory>
#include <string>
#include <cmath>

using namespace gdsim;

static constexpr float FIXED_DT = 1.f / 240.f;

enum class AppState { Menu, LevelSelect, Loading, Playing, Dead, Cleared };

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

    std::vector<gdapp::CachedLevelEntry> cachedLevels;
    int selectedCacheIdx = 0;
    int pendingLevelId = 0; // set by both the ID-entry menu and the cache picker

    auto startLoad = [&](int id) {
        pendingLevelId = id;
        state = AppState::Loading;
    };

    auto resetPlayback = [&]() {
        level = std::make_unique<Level>(decodedLevelString);
        accumulator = 0.f;
        cam.x = 0.f; cam.y = 0.f;
        state = AppState::Playing;
    };

    while (!WindowShouldClose()) {
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
            bool pressed = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP) ||
                           IsMouseButtonDown(MOUSE_BUTTON_LEFT);

            if (IsKeyPressed(KEY_R)) { resetPlayback(); }
            if (IsKeyPressed(KEY_ESCAPE)) { state = AppState::Menu; level.reset(); idInput.clear(); }

            if (level) {
                accumulator += GetFrameTime();
                if (accumulator > 0.25f) accumulator = 0.25f; // avoid spiral of death
                while (accumulator >= FIXED_DT) {
                    Player& p = level->runFrame(pressed, FIXED_DT);
                    accumulator -= FIXED_DT;
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
        } else if (state == AppState::Dead || state == AppState::Cleared) {
            if (IsKeyPressed(KEY_R)) resetPlayback();
            if (IsKeyPressed(KEY_ESCAPE)) { state = AppState::Menu; level.reset(); idInput.clear(); }
        }

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
        } else if (level) {
            gdapp::drawLevel(*level, level->latestState(), cam, screenW, screenH);

            float pct = 100.f * level->latestState().pos.x / level->length;
            DrawText(levelName.c_str(), 12, 10, 20, RAYWHITE);
            DrawText(TextFormat("%.1f%%", pct), 12, 34, 18, GRAY);
            DrawText("SPACE/click to jump  -  R restart  -  ESC menu", 12, screenH - 26, 14, Color{140,140,150,255});

            if (state == AppState::Dead) {
                DrawText("DIED", screenW / 2 - 40, screenH / 2 - 40, 40, RED);
                DrawText(TextFormat("%.2f%% of the level", finalPercent), screenW / 2 - 90, screenH / 2 + 10, 18, RAYWHITE);
                DrawText("R to retry  -  ESC for menu", screenW / 2 - 110, screenH / 2 + 40, 16, GRAY);
            } else if (state == AppState::Cleared) {
                DrawText("CLEARED!", screenW / 2 - 80, screenH / 2 - 40, 40, GREEN);
                DrawText("R to replay  -  ESC for menu", screenW / 2 - 110, screenH / 2 + 10, 16, GRAY);
            }
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
