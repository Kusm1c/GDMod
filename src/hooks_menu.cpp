#include "replay_state.hpp"
#include "replay_generate.hpp"
#include "replay_levelview.hpp"
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/string.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
namespace {
    template <class... Args>
    static void traceDebug(fmt::format_string<Args...> format, Args&&... args) {
        (void)format;
        ((void)args, ...);
    }

    static std::atomic<bool> s_externalEditorRunning = false;
    static std::atomic<bool> s_generatorWindowRunning = false;
    static std::atomic<bool> s_levelViewWindowRunning = false;
    static std::atomic<bool> s_levelViewWarningOpen = false;
    static constexpr int kTimelineLeft = 16;
    static constexpr int kTimelineTop = 220;
    static constexpr int kTimelineMinWidth = 320;
    static constexpr int kTimelineHeight = 130;

    enum : int {
        IDC_LABEL_INDEX = 1001,
        IDC_EDIT_PRESS = 1002,
        IDC_EDIT_RELEASE = 1003,
        IDC_BTN_PREV = 1004,
        IDC_BTN_NEXT = 1005,
        IDC_BTN_P_MINUS = 1006,
        IDC_BTN_P_PLUS = 1007,
        IDC_BTN_R_MINUS = 1008,
        IDC_BTN_R_PLUS = 1009,
        IDC_BTN_SNAP_DOWN = 1010,
        IDC_BTN_SNAP_UP = 1011,
        IDC_LABEL_SNAP = 1012,
        IDC_BTN_APPLY = 1013,
        IDC_BTN_SAVE = 1014,
        IDC_BTN_CLOSE = 1015,
        IDC_BTN_TL_LEFT = 1016,
        IDC_BTN_TL_RIGHT = 1017,
        IDC_BTN_ZOOM_OUT = 1018,
        IDC_BTN_ZOOM_IN = 1019,
        IDC_LABEL_TL_INFO = 1020,
        IDC_BTN_CTRL_PAUSE = 1021,
        IDC_BTN_CTRL_STEP_BACK = 1022,
        IDC_BTN_CTRL_STEP_FWD = 1023,
        IDC_BTN_CTRL_RESTART = 1024,
        IDC_BTN_CTRL_FOLLOW = 1025,
        IDC_BTN_GENERATE = 1026,
        IDC_CHK_SHOW_TRAJECTORY = 2101
    };

    struct ExternalReplayEditor {
        enum class TimelineDragMode {
            None,
            Move,
            ResizeStart,
            ResizeEnd,
            Create
        };

        int levelId = 0;
        std::filesystem::path sourcePath;
        ReplayLoadResult replay;
        size_t index = 0;
        int snapFrames = 4;
        bool saved = false;
        HWND hwnd = nullptr;
        uint64_t timelineStartFrame = 0;
        int framesPerCell = 1;
        bool timelineDragging = false;
        bool dragTargetPressed = false;
        TimelineDragMode timelineDragMode = TimelineDragMode::None;
        size_t timelineDragIndex = std::numeric_limits<size_t>::max();
        uint64_t timelineDragStartFrame = 0;
        uint64_t timelineDragStartPress = 0;
        uint64_t timelineDragStartRelease = 0;
        uint64_t lastDragFrame = std::numeric_limits<uint64_t>::max();
        HWND gameWindow = nullptr;
        int timelineWidth = 700;
        bool followPlayhead = true;
    };

    struct ReplayGenerateWindowState {
        int levelId = 0;
        std::string levelString;
        bool hasRuntimeModel = false;
        GDLevelModel runtimeModel;
        int maxSearchSeconds = 600;
        HWND hwnd = nullptr;
        std::thread worker;

        std::atomic<uint64_t> currentFrame {0};
        std::atomic<uint64_t> bestFrame {0};
        std::atomic<uint64_t> totalFrames {1};
        std::atomic<size_t> clickCount {0};

        std::atomic<bool> done {false};
        std::atomic<bool> success {false};
        std::atomic<bool> stopRequested {false};
        bool notified = false;

        std::mutex dataMutex;
        std::vector<std::pair<float, float>> obstacleRanges;
        ReplayLoadResult replay;
        std::string failureReason;
    };

    struct GeneratorWindowRunningGuard {
        ~GeneratorWindowRunningGuard() {
            s_generatorWindowRunning.store(false);
        }
    };

    struct LevelViewWindowState {
        int levelId = 0;
        HWND hwnd = nullptr;
        GDLevelModel model;
        bool modelFromRuntime = false;
        bool showTrajectory = false;
        bool hasReplay = false;
        ReplayLoadResult replay;
        std::vector<std::pair<float, float>> trajectoryPoints;
        bool trajectoryDied = false;
        std::pair<float, float> trajectoryDeathPoint {0.0f, 0.0f};
        GDHitboxCategory trajectoryDeathCategory = GDHitboxCategory::Spike;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float zoom = 1.0f;
        bool panning = false;
        POINT panLast{};
    };

    struct LevelViewRunningGuard {
        ~LevelViewRunningGuard() {
            s_levelViewWindowRunning.store(false);
        }
    };

    static COLORREF colorForCategory(GDHitboxCategory c) {
        switch (c) {
            case GDHitboxCategory::Player: return RGB(116, 241, 160);
            case GDHitboxCategory::SolidBlock: return RGB(130, 170, 255);
            case GDHitboxCategory::Spike: return RGB(255, 102, 102);
            case GDHitboxCategory::Orb: return RGB(255, 225, 99);
            case GDHitboxCategory::JumpPad: return RGB(255, 165, 87);
            case GDHitboxCategory::Portal: return RGB(199, 129, 255);
            case GDHitboxCategory::Trigger: return RGB(122, 235, 235);
            case GDHitboxCategory::CollisionBlock:
            default: return RGB(230, 230, 230);
        }
    }

    static const wchar_t* nameForCategory(GDHitboxCategory c) {
        switch (c) {
            case GDHitboxCategory::Player: return L"Player";
            case GDHitboxCategory::SolidBlock: return L"Solid";
            case GDHitboxCategory::Spike: return L"Spike";
            case GDHitboxCategory::Orb: return L"Orb";
            case GDHitboxCategory::JumpPad: return L"JumpPad";
            case GDHitboxCategory::Portal: return L"Portal";
            case GDHitboxCategory::Trigger: return L"Trigger";
            case GDHitboxCategory::CollisionBlock:
            default: return L"Collision";
        }
    }

    static std::optional<GDLevelModel> buildLevelModelFromRuntimeSnapshot(int levelId) {
        auto snapshot = getRuntimeHitboxSnapshot(levelId);
        if (!snapshot.has_value() || snapshot->hitboxes.empty()) {
            return std::nullopt;
        }

        GDLevelModel model;
        model.minX = snapshot->minX;
        model.maxX = snapshot->maxX;
        model.minY = snapshot->minY;
        model.maxY = snapshot->maxY;
        model.rawLength = 0;
        model.objectTokenCount = snapshot->hitboxes.size();
        model.parsedObjectCount = snapshot->hitboxes.size();
        model.sourceSection = "runtime-collision";
        model.hitboxes.reserve(snapshot->hitboxes.size());

        for (auto const& rhb : snapshot->hitboxes) {
            GDHitboxPrimitive hb;
            switch (rhb.shape) {
                case ReplayRuntimeHitboxRect::Shape::Circle:
                    hb.shape = GDHitboxShape::Circle;
                    break;
                case ReplayRuntimeHitboxRect::Shape::OrientedQuad:
                    hb.shape = GDHitboxShape::OrientedQuad;
                    hb.corners.assign(rhb.corners.begin(), rhb.corners.end());
                    break;
                case ReplayRuntimeHitboxRect::Shape::Rectangle:
                default:
                    hb.shape = GDHitboxShape::Rectangle;
                    break;
            }
            if (rhb.isHazard) {
                hb.category = GDHitboxCategory::Spike;
            } else if (rhb.isSolid) {
                hb.category = GDHitboxCategory::SolidBlock;
            } else {
                hb.category = GDHitboxCategory::CollisionBlock;
            }
            hb.objectId = rhb.objectId;
            hb.runtimeObjectType = rhb.objectType;
            hb.x = rhb.x;
            hb.y = rhb.y;
            hb.width = std::max(1.0f, rhb.width);
            hb.height = std::max(1.0f, rhb.height);
            hb.radius = std::max(0.0f, rhb.radius);
            model.hitboxes.push_back(hb);
            model.interactionXs.push_back(hb.x);
        }

        model.recognizedHitboxCount = model.hitboxes.size();
        return model;
    }

    static uint64_t lastReplayFrame(const ReplayLoadResult& replay) {
        uint64_t out = 0;
        for (auto const& p : replay.presses) {
            out = std::max(out, p.frameRelease);
        }
        return out;
    }

    struct TrajectoryPreview {
        std::vector<std::pair<float, float>> points;
        bool died = false;
        std::pair<float, float> deathPoint {0.0f, 0.0f};
        GDHitboxCategory deathCategory = GDHitboxCategory::Spike;
    };

    static bool isLethalCategory(GDHitboxCategory cat) {
        return cat == GDHitboxCategory::SolidBlock || cat == GDHitboxCategory::Spike || cat == GDHitboxCategory::CollisionBlock;
    }

    static bool playerIntersectsHitbox(float px, float py, const GDHitboxPrimitive& hb) {
        constexpr float kHalfW = 14.0f; // 28x28 cube-style internal hitbox
        constexpr float kHalfH = 14.0f;

        float playerLeft = px - kHalfW;
        float playerRight = px + kHalfW;
        float playerBottom = py - kHalfH;
        float playerTop = py + kHalfH;

        if (hb.shape == GDHitboxShape::Circle) {
            float closestX = std::clamp(hb.x, playerLeft, playerRight);
            float closestY = std::clamp(hb.y, playerBottom, playerTop);
            float dx = hb.x - closestX;
            float dy = hb.y - closestY;
            return (dx * dx + dy * dy) <= (hb.radius * hb.radius);
        }

        float left = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        float top = 0.0f;

        if (hb.shape == GDHitboxShape::OrientedQuad) {
            left = hb.corners[0].x;
            right = hb.corners[0].x;
            bottom = hb.corners[0].y;
            top = hb.corners[0].y;
            for (auto const& c : hb.corners) {
                left = std::min(left, c.x);
                right = std::max(right, c.x);
                bottom = std::min(bottom, c.y);
                top = std::max(top, c.y);
            }
        } else {
            float halfW = hb.width * 0.5f;
            float halfH = hb.height * 0.5f;
            left = hb.x - halfW;
            right = hb.x + halfW;
            bottom = hb.y - halfH;
            top = hb.y + halfH;
        }

        return !(playerRight < left || playerLeft > right || playerTop < bottom || playerBottom > top);
    }

    static TrajectoryPreview buildApproxTrajectory(
        const ReplayLoadResult& replay,
        const GDLevelModel& model
    ) {
        TrajectoryPreview out;
        if (replay.presses.empty()) return out;

        uint64_t maxFrame = std::max<uint64_t>(120, lastReplayFrame(replay));
        float minX = model.minX;
        float maxX = std::max(model.maxX, minX + 300.0f);
        float width = std::max(1.0f, maxX - minX);

        float groundY = model.minY + 18.0f;
        float y = groundY;
        float vy = 0.0f;
        bool wasPressed = false;

        size_t idx = 0;
        out.points.reserve(static_cast<size_t>(maxFrame / 2) + 4);

        for (uint64_t frame = 1; frame <= maxFrame; ++frame) {
            while (idx < replay.presses.size() && replay.presses[idx].frameRelease < frame) {
                idx++;
            }

            bool pressed = false;
            if (idx < replay.presses.size()) {
                auto const& p = replay.presses[idx];
                pressed = (p.framePress <= frame && frame <= p.frameRelease);
            }

            bool risingEdge = pressed && !wasPressed;
            wasPressed = pressed;

            if (risingEdge && y <= groundY + 0.5f) {
                vy = 7.6f;
            }

            vy -= 0.36f;
            y += vy;
            if (y < groundY) {
                y = groundY;
                vy = 0.0f;
            }

            float t = static_cast<float>(frame) / static_cast<float>(maxFrame);
            float x = minX + t * width;

            for (auto const& hb : model.hitboxes) {
                if (!isLethalCategory(hb.category)) continue;
                if (playerIntersectsHitbox(x, y, hb)) {
                    out.died = true;
                    out.deathPoint = {x, y};
                    out.deathCategory = hb.category;
                    if ((frame % 2) == 0 || frame == maxFrame) {
                        out.points.emplace_back(x, y);
                    }
                    return out;
                }
            }

            if ((frame % 2) == 0 || frame == maxFrame) {
                out.points.emplace_back(x, y);
            }
        }

        return out;
    }

    static void paintLevelViewWindow(LevelViewWindowState* state, HDC hdc) {
        if (!state || !state->hwnd) return;

        RECT client{};
        GetClientRect(state->hwnd, &client);
        int width = std::max(1, static_cast<int>(client.right - client.left));
        int height = std::max(1, static_cast<int>(client.bottom - client.top));

        HBRUSH back = CreateSolidBrush(RGB(15, 18, 24));
        FillRect(hdc, &client, back);
        DeleteObject(back);

        RECT header{8, 8, width - 8, 32};
        HBRUSH headBrush = CreateSolidBrush(RGB(30, 36, 49));
        FillRect(hdc, &header, headBrush);

        auto title = fmt::format("Level View Hitboxes - L{} | Source: {} | Hitboxes: {} | Parsed: {}/{} | Zoom: {:.2f}",
            state->levelId,
            state->modelFromRuntime ? "runtime" : "parsed",
            state->model.hitboxes.size(),
            state->model.parsedObjectCount,
            state->model.objectTokenCount,
            state->zoom);
        std::wstring wTitle(title.begin(), title.end());
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(236, 241, 252));
        DrawTextW(hdc, wTitle.c_str(), static_cast<int>(wTitle.size()), &header, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT arena{8, 40, width - 8, height - 70};
        HBRUSH arenaBrush = CreateSolidBrush(RGB(22, 27, 36));
        FillRect(hdc, &arena, arenaBrush);

        float worldMinX = state->model.minX;
        float worldMaxX = state->model.maxX;
        float worldMinY = state->model.minY;
        float worldMaxY = state->model.maxY;
        float worldW = std::max(1.0f, worldMaxX - worldMinX);
        float worldH = std::max(1.0f, worldMaxY - worldMinY);

        float baseScaleX = static_cast<float>(arena.right - arena.left) / worldW;
        float baseScaleY = static_cast<float>(arena.bottom - arena.top) / std::max(120.0f, worldH);
        float scale = std::max(0.02f, std::min(baseScaleX, baseScaleY) * state->zoom);

        auto worldToScreen = [&](float wx, float wy, float& sx, float& sy) {
            float x = (wx - worldMinX) * scale + static_cast<float>(arena.left) + state->offsetX;
            float y = static_cast<float>(arena.bottom) - ((wy - worldMinY) * scale + state->offsetY);
            sx = x;
            sy = y;
        };

        for (auto const& hb : state->model.hitboxes) {
            COLORREF c = colorForCategory(hb.category);
            HBRUSH fill = CreateSolidBrush(RGB(GetRValue(c) / 3, GetGValue(c) / 3, GetBValue(c) / 3));
            HPEN pen = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, fill);

            float sx = 0.0f;
            float sy = 0.0f;
            worldToScreen(hb.x, hb.y, sx, sy);

            if (hb.shape == GDHitboxShape::Circle) {
                int r = std::max(2, static_cast<int>(hb.radius * scale));
                Ellipse(hdc, static_cast<int>(sx) - r, static_cast<int>(sy) - r, static_cast<int>(sx) + r, static_cast<int>(sy) + r);
            } else if (hb.shape == GDHitboxShape::OrientedQuad) {
                POINT poly[4]{};
                for (size_t i = 0; i < 4; ++i) {
                    float px = 0.0f;
                    float py = 0.0f;
                    worldToScreen(hb.corners[i].x, hb.corners[i].y, px, py);
                    poly[i] = POINT{
                        static_cast<LONG>(std::lround(px)),
                        static_cast<LONG>(std::lround(py))
                    };
                }
                Polygon(hdc, poly, 4);
            } else {
                int w = std::max(2, static_cast<int>(hb.width * scale));
                int h = std::max(2, static_cast<int>(hb.height * scale));
                Rectangle(hdc,
                    static_cast<int>(sx - w * 0.5f),
                    static_cast<int>(sy - h * 0.5f),
                    static_cast<int>(sx + w * 0.5f),
                    static_cast<int>(sy + h * 0.5f));
            }

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(fill);
            DeleteObject(pen);
        }

        if (state->showTrajectory) {
            if (state->hasReplay && state->trajectoryPoints.size() >= 2) {
                HPEN trajPen = CreatePen(PS_SOLID, 2, RGB(255, 240, 118));
                HGDIOBJ oldPen = SelectObject(hdc, trajPen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

                POINT prev{};
                bool havePrev = false;
                for (auto const& p : state->trajectoryPoints) {
                    float sx = 0.0f;
                    float sy = 0.0f;
                    worldToScreen(p.first, p.second, sx, sy);
                    POINT cur{static_cast<LONG>(std::lround(sx)), static_cast<LONG>(std::lround(sy))};
                    if (havePrev) {
                        MoveToEx(hdc, prev.x, prev.y, nullptr);
                        LineTo(hdc, cur.x, cur.y);
                    }
                    prev = cur;
                    havePrev = true;
                }

                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(trajPen);

                if (state->trajectoryDied) {
                    float sx = 0.0f;
                    float sy = 0.0f;
                    worldToScreen(state->trajectoryDeathPoint.first, state->trajectoryDeathPoint.second, sx, sy);

                    HPEN deathPen = CreatePen(PS_SOLID, 2, RGB(255, 92, 92));
                    HGDIOBJ oldDeathPen = SelectObject(hdc, deathPen);
                    MoveToEx(hdc, static_cast<int>(sx) - 6, static_cast<int>(sy) - 6, nullptr);
                    LineTo(hdc, static_cast<int>(sx) + 6, static_cast<int>(sy) + 6);
                    MoveToEx(hdc, static_cast<int>(sx) + 6, static_cast<int>(sy) - 6, nullptr);
                    LineTo(hdc, static_cast<int>(sx) - 6, static_cast<int>(sy) + 6);
                    SelectObject(hdc, oldDeathPen);
                    DeleteObject(deathPen);

                    RECT deathNote{arena.left + 10, arena.top + 30, arena.right - 10, arena.top + 48};
                    SetTextColor(hdc, RGB(255, 140, 140));
                    DrawTextW(hdc, L"Trajectory collides with lethal hitbox (red marker)", -1, &deathNote, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
            } else {
                RECT note{arena.left + 10, arena.top + 10, arena.right - 10, arena.top + 28};
                SetTextColor(hdc, RGB(255, 206, 117));
                DrawTextW(hdc, L"Show trajectory: no replay found for this level", -1, &note, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        RECT legend{8, height - 62, width - 8, height - 8};
        FillRect(hdc, &legend, headBrush);
        int lx = legend.left + 8;
        int ly = legend.top + 10;
        GDHitboxCategory cats[] = {
            GDHitboxCategory::SolidBlock,
            GDHitboxCategory::Spike,
            GDHitboxCategory::Orb,
            GDHitboxCategory::JumpPad,
            GDHitboxCategory::Portal,
            GDHitboxCategory::Trigger,
            GDHitboxCategory::CollisionBlock
        };
        for (auto c : cats) {
            HBRUSH sw = CreateSolidBrush(colorForCategory(c));
            RECT swRc{lx, ly, lx + 14, ly + 14};
            FillRect(hdc, &swRc, sw);
            DeleteObject(sw);

            RECT txt{lx + 18, ly - 2, lx + 125, ly + 16};
            auto* name = nameForCategory(c);
            SetTextColor(hdc, RGB(236, 241, 252));
            DrawTextW(hdc, name, -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            lx += 120;
        }

        RECT help{legend.left + 840, legend.top + 8, legend.right - 10, legend.bottom - 8};
        SetTextColor(hdc, RGB(220, 226, 240));
        DrawTextW(hdc, L"Trajectory: yellow line (toggle with checkbox)", -1, &help, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        DeleteObject(headBrush);
        DeleteObject(arenaBrush);
    }

    static LRESULT CALLBACK ReplayLevelViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* state = reinterpret_cast<LevelViewWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
            case WM_CREATE: {
                auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
                auto* st = reinterpret_cast<LevelViewWindowState*>(create->lpCreateParams);
                st->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

                HWND chk = CreateWindowW(
                    L"BUTTON",
                    L"Show trajectory",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    12,
                    10,
                    150,
                    20,
                    hwnd,
                    reinterpret_cast<HMENU>(IDC_CHK_SHOW_TRAJECTORY),
                    nullptr,
                    nullptr
                );
                if (st->showTrajectory) {
                    SendMessageW(chk, BM_SETCHECK, BST_CHECKED, 0);
                }
                return 0;
            }
            case WM_COMMAND:
                if (state && LOWORD(wParam) == IDC_CHK_SHOW_TRAJECTORY) {
                    HWND chk = GetDlgItem(hwnd, IDC_CHK_SHOW_TRAJECTORY);
                    state->showTrajectory = (SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_MOUSEWHEEL:
                if (state) {
                    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    if (delta > 0) state->zoom = std::min(8.0f, state->zoom * 1.12f);
                    else state->zoom = std::max(0.08f, state->zoom / 1.12f);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_MBUTTONDOWN:
                if (state) {
                    state->panning = true;
                    state->panLast.x = static_cast<LONG>(static_cast<short>(LOWORD(lParam)));
                    state->panLast.y = static_cast<LONG>(static_cast<short>(HIWORD(lParam)));
                    SetCapture(hwnd);
                }
                return 0;
            case WM_MOUSEMOVE:
                if (state && state->panning) {
                    POINT pt{
                        static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                        static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
                    };
                    state->offsetX += static_cast<float>(pt.x - state->panLast.x);
                    state->offsetY += static_cast<float>(state->panLast.y - pt.y);
                    state->panLast = pt;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_MBUTTONUP:
                if (state && state->panning) {
                    state->panning = false;
                    ReleaseCapture();
                }
                return 0;
            case WM_RBUTTONDOWN:
                if (state) {
                    state->zoom = 1.0f;
                    state->offsetX = 0.0f;
                    state->offsetY = 0.0f;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                paintLevelViewWindow(state, hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static bool openReplayLevelViewWindow(int levelId, const std::string& levelString) {
        auto runtimeModel = buildLevelModelFromRuntimeSnapshot(levelId);
        
        if (!runtimeModel.has_value() || runtimeModel->hitboxes.empty()) {
            bool expectedWarning = false;
            if (!s_levelViewWarningOpen.compare_exchange_strong(expectedWarning, true)) {
                return false;  // Warning already shown in this session
            }
            MessageBoxW(nullptr, L"Runtime hitboxes not available for this level.\n\nEnter/play the level once in-game first, then try View Level again.", L"View Level", MB_OK | MB_ICONWARNING);
            return false;
        }

        GDLevelModel model = runtimeModel.value();

        bool expected = false;
        if (!s_levelViewWindowRunning.compare_exchange_strong(expected, true)) {
            MessageBoxW(nullptr, L"A View Level window is already open.", L"View Level", MB_OK | MB_ICONINFORMATION);
            return false;
        }

        std::thread([levelId, model = std::move(model)]() mutable {
            LevelViewRunningGuard guard;

            static bool s_registered = false;
            static constexpr wchar_t kClassName[] = L"GDModReplayLevelViewWindow";
            if (!s_registered) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = ReplayLevelViewWndProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = kClassName;
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
                if (!RegisterClassW(&wc)) {
                    MessageBoxW(nullptr, L"Unable to initialize level view window.", L"View Level", MB_OK | MB_ICONERROR);
                    return;
                }
                s_registered = true;
            }

            auto state = std::make_unique<LevelViewWindowState>();
            state->levelId = levelId;
            state->model = std::move(model);
            state->modelFromRuntime = true;
            if (auto replayResult = loadMatchingReplay(levelId); replayResult.has_value() && !replayResult->presses.empty()) {
                state->hasReplay = true;
                state->replay = replayResult.value();
                auto preview = buildApproxTrajectory(state->replay, state->model);
                state->trajectoryPoints = std::move(preview.points);
                state->trajectoryDied = preview.died;
                state->trajectoryDeathPoint = preview.deathPoint;
                state->trajectoryDeathCategory = preview.deathCategory;
                state->showTrajectory = true;  // Auto-show trajectory if replay exists
            }

            auto title = fmt::format("View Level - L{}", levelId);
            std::wstring wTitle(title.begin(), title.end());
            HWND hwnd = CreateWindowExW(
                WS_EX_APPWINDOW | WS_EX_TOPMOST,
                kClassName,
                wTitle.c_str(),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1200,
                720,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                state.get()
            );

            if (!hwnd) {
                MessageBoxW(nullptr, L"Unable to create View Level window.", L"View Level", MB_OK | MB_ICONERROR);
                return;
            }

            ShowWindow(hwnd, SW_SHOWNORMAL);
            UpdateWindow(hwnd);

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }).detach();

        return true;
    }

    #if 0  // Generator replay code disabled - requires pathfinding infrastructure
    static void paintGeneratorWindow(ReplayGenerateWindowState* state, HDC hdc) {
        if (!state || !state->hwnd) return;

        RECT client{};
        GetClientRect(state->hwnd, &client);
        int width = std::max(1, static_cast<int>(client.right - client.left));
        int height = std::max(1, static_cast<int>(client.bottom - client.top));

        HBRUSH backBrush = CreateSolidBrush(RGB(18, 22, 30));
        FillRect(hdc, &client, backBrush);
        DeleteObject(backBrush);

        RECT header{12, 10, width - 12, 40};
        HBRUSH panelBrush = CreateSolidBrush(RGB(30, 36, 49));
        FillRect(hdc, &header, panelBrush);

        uint64_t current = state->currentFrame.load();
        uint64_t best = state->bestFrame.load();
        uint64_t total = std::max<uint64_t>(1, state->totalFrames.load());
        size_t clicks = state->clickCount.load();
        bool done = state->done.load();
        bool success = state->success.load();

        std::string status = done ? (success ? "Completed" : "Failed") : "Searching";
        auto title = fmt::format("Replay Generate - L{}  |  {}", state->levelId, status);
        std::wstring wTitle(title.begin(), title.end());
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(233, 237, 246));
        DrawTextW(hdc, wTitle.c_str(), static_cast<int>(wTitle.size()), &header, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT info{12, 48, width - 12, 88};
        FillRect(hdc, &info, panelBrush);
        auto stats = fmt::format(
            "bot frame: {} / {}   |   player(best): {} / {}   |   clicks: {}",
            current,
            total,
            best,
            total,
            clicks
        );
        std::wstring wStats(stats.begin(), stats.end());
        SetTextColor(hdc, RGB(201, 214, 232));
        DrawTextW(hdc, wStats.c_str(), static_cast<int>(wStats.size()), &info, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT arena{12, 96, width - 12, height - 56};
        HBRUSH arenaBrush = CreateSolidBrush(RGB(24, 29, 39));
        FillRect(hdc, &arena, arenaBrush);

        std::vector<std::pair<float, float>> obstacles;
        {
            std::scoped_lock lock(state->dataMutex);
            obstacles = state->obstacleRanges;
        }

        HBRUSH obstacleBrush = CreateSolidBrush(RGB(237, 96, 87));
        for (auto const& range : obstacles) {
            float a = std::clamp(range.first, 0.0f, 1.0f);
            float b = std::clamp(range.second, 0.0f, 1.0f);
            if (b <= a) continue;
            int x0 = arena.left + static_cast<int>(a * static_cast<float>(arena.right - arena.left));
            int x1 = arena.left + static_cast<int>(b * static_cast<float>(arena.right - arena.left));
            RECT obRc{x0, arena.top + 20, std::max(x0 + 1, x1), arena.bottom - 14};
            FillRect(hdc, &obRc, obstacleBrush);
        }

        float botPct = static_cast<float>(std::clamp<double>(static_cast<double>(current) / static_cast<double>(total), 0.0, 1.0));
        float playerPct = static_cast<float>(std::clamp<double>(static_cast<double>(best) / static_cast<double>(total), 0.0, 1.0));
        int botX = arena.left + static_cast<int>(botPct * static_cast<float>(arena.right - arena.left));
        int playerX = arena.left + static_cast<int>(playerPct * static_cast<float>(arena.right - arena.left));

        HBRUSH botBrush = CreateSolidBrush(RGB(117, 177, 255));
        HBRUSH playerBrush = CreateSolidBrush(RGB(117, 242, 158));
        RECT botRc{botX - 4, arena.top + 4, botX + 4, arena.bottom - 4};
        RECT playerRc{playerX - 4, arena.top + 8, playerX + 4, arena.bottom - 8};
        FillRect(hdc, &botRc, botBrush);
        FillRect(hdc, &playerRc, playerBrush);

        RECT footer{12, height - 44, width - 12, height - 12};
        FillRect(hdc, &footer, panelBrush);
        std::string hint = done
            ? (success ? "Replay generated and saved. Close this window." : "Generation failed. Close this window.")
            : "Live view: red=hazards, blue=bot exploration, green=best player progress";
        std::wstring wHint(hint.begin(), hint.end());
        SetTextColor(hdc, RGB(190, 203, 223));
        DrawTextW(hdc, wHint.c_str(), static_cast<int>(wHint.size()), &footer, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        DeleteObject(panelBrush);
        DeleteObject(arenaBrush);
        DeleteObject(obstacleBrush);
        DeleteObject(botBrush);
        DeleteObject(playerBrush);
    }

    static DWORD WINAPI generatorWorkerEntry(LPVOID param) {
        auto* state = reinterpret_cast<ReplayGenerateWindowState*>(param);
        if (!state) return 0;

        try {
            auto build = buildSimplifiedPathfinderReplay(
                state->levelString,
                state->stopRequested,
                [state](PathfinderTelemetry const& t) {
                    state->currentFrame.store(t.currentFrame);
                    state->bestFrame.store(t.bestFrame);
                    state->totalFrames.store(std::max<uint64_t>(1, t.totalFrames));
                    state->clickCount.store(t.clickCount);
                },
                state->hasRuntimeModel ? &state->runtimeModel : nullptr
            );

            if (state->stopRequested.load()) {
                state->failureReason = "Cancelled.";
                state->done.store(true);
                state->success.store(false);
                return 0;
            }

            {
                std::scoped_lock lock(state->dataMutex);
                state->obstacleRanges = build.obstacleRanges;
                state->replay = build.replay;
            }
            state->totalFrames.store(std::max<uint64_t>(1, build.totalFrames));
            state->clickCount.store(replayClickCount(build.replay));

            bool ok = !build.replay.presses.empty() && saveReplayToLocalJson(state->levelId, build.replay, {});
            state->success.store(ok);
            if (!ok) {
                state->failureReason = build.replay.presses.empty()
                    ? fmt::format("No replay found in {}s budget.", state->maxSearchSeconds)
                    : "Unable to save replay to local JSON.";
            }
        } catch (std::exception const& e) {
            state->success.store(false);
            state->failureReason = e.what();
        } catch (...) {
            state->success.store(false);
            state->failureReason = "Unknown error.";
        }

        state->done.store(true);
        return 0;
    }

    static LRESULT CALLBACK ReplayGenerateWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* state = reinterpret_cast<ReplayGenerateWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
            case WM_CREATE: {
                auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
                auto* st = reinterpret_cast<ReplayGenerateWindowState*>(create->lpCreateParams);
                st->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

                CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                    12, 12, 80, 26, hwnd, reinterpret_cast<HMENU>(2001), nullptr, nullptr);
                SetTimer(hwnd, 1, 33, nullptr);
                return 0;
            }
            case WM_COMMAND: {
                if (!state) return 0;
                int id = LOWORD(wParam);
                if (id == 2001) {
                    if (!state->done.load()) {
                        state->stopRequested.store(true);
                    } else {
                        DestroyWindow(hwnd);
                    }
                }
                return 0;
            }
            case WM_TIMER:
                if (state && wParam == 1) {
                    if (state->done.load() && !state->notified) {
                        state->notified = true;
                        if (state->success.load()) {
                            MessageBoxW(hwnd, L"Replay generated and saved successfully.", L"Generate Replay", MB_OK | MB_ICONINFORMATION);
                        } else {
                            std::wstring msgW;
                            {
                                std::string text = state->failureReason.empty() ? "Generation failed." : state->failureReason;
                                msgW.assign(text.begin(), text.end());
                            }
                            MessageBoxW(hwnd, msgW.c_str(), L"Generate Replay", MB_OK | MB_ICONWARNING);
                        }
                        SetWindowTextW(GetDlgItem(hwnd, 2001), L"Close");
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                paintGeneratorWindow(state, hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CLOSE:
                if (state && !state->done.load()) {
                    state->stopRequested.store(true);
                    return 0;
                }
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, 1);
                if (state && state->worker.joinable()) {
                    state->worker.join();
                }
                PostQuitMessage(0);
                return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    #endif  // End of generator window code (paintGeneratorWindow, generatorWorkerEntry, ReplayGenerateWndProc)

    #if 0  // openGenerateReplayWindow disabled - requires pathfinding
    static bool openGenerateReplayWindow(int levelId, std::string levelString) {
        if (levelString.empty()) {
            MessageBoxW(nullptr, L"Level data not available yet. Open the level details once online data is loaded.", L"Generate Replay", MB_OK | MB_ICONWARNING);
            return false;
        }

        bool expected = false;
        if (!s_generatorWindowRunning.compare_exchange_strong(expected, true)) {
            MessageBoxW(nullptr, L"A generation window is already open.", L"Generate Replay", MB_OK | MB_ICONINFORMATION);
            return false;
        }

        std::thread([
            levelId,
            levelString = std::move(levelString)
        ]() mutable {
            GeneratorWindowRunningGuard guard;

            static bool s_registered = false;
            static constexpr wchar_t kClassName[] = L"GDModReplayGenerateWindow";
            if (!s_registered) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = ReplayGenerateWndProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = kClassName;
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
                if (!RegisterClassW(&wc)) {
                    MessageBoxW(nullptr, L"Unable to initialize generator window.", L"Generate Replay", MB_OK | MB_ICONERROR);
                    return;
                }
                s_registered = true;
            }

            auto state = std::make_unique<ReplayGenerateWindowState>();
            state->levelId = levelId;
            state->levelString = std::move(levelString);
            if (auto runtimeModel = buildLevelModelFromRuntimeSnapshot(levelId); runtimeModel.has_value()) {
                state->hasRuntimeModel = true;
                state->runtimeModel = std::move(runtimeModel.value());
                // Complex levels may need up to 10 minutes of search.
                state->maxSearchSeconds = static_cast<int>(std::clamp(
                    60 + static_cast<int>(state->runtimeModel.hitboxes.size() / 6),
                    60,
                    600
                ));
                log::info("[GenerateReplay] using runtime hitboxes level={} count={}", levelId, state->runtimeModel.hitboxes.size());
            }
            state->worker = std::thread([st = state.get()] {
                generatorWorkerEntry(st);
            });

            auto title = fmt::format("Generate Replay - Level {}", levelId);
            std::wstring wTitle(title.begin(), title.end());
            HWND hwnd = CreateWindowExW(
                WS_EX_APPWINDOW | WS_EX_TOPMOST,
                kClassName,
                wTitle.c_str(),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                900,
                520,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                state.get()
            );

            if (!hwnd) {
                state->stopRequested.store(true);
                if (state->worker.joinable()) state->worker.join();
                MessageBoxW(nullptr, L"Unable to create generator window.", L"Generate Replay", MB_OK | MB_ICONERROR);
                return;
            }

            ShowWindow(hwnd, SW_SHOWNORMAL);
            UpdateWindow(hwnd);

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }).detach();

        return true;
    }

    #endif  // End of generator replay code

    static int currentTimelineWidth(ExternalReplayEditor* editor) {
        if (!editor || !editor->hwnd) return 700;
        RECT rc{};
        GetClientRect(editor->hwnd, &rc);
        int clientWidth = static_cast<int>(rc.right - rc.left);
        int width = std::max(kTimelineMinWidth, clientWidth - (kTimelineLeft * 2));
        return width;
    }

    static void refocusGameWindow(ExternalReplayEditor* editor) {
        if (!editor || !editor->gameWindow || !IsWindow(editor->gameWindow)) return;
        SetForegroundWindow(editor->gameWindow);
        SetActiveWindow(editor->gameWindow);
    }

    struct ExternalEditorRunningGuard {
        ~ExternalEditorRunningGuard() {
            s_externalEditorRunning.store(false);
        }
    };

    static void sortReplayPresses(ReplayLoadResult& replay) {
        for (auto& p : replay.presses) {
            if (p.frameRelease <= p.framePress) {
                p.frameRelease = p.framePress + 1;
            }
        }
        std::sort(replay.presses.begin(), replay.presses.end(), [](auto const& a, auto const& b) {
            if (a.framePress != b.framePress) return a.framePress < b.framePress;
            return a.frameRelease < b.frameRelease;
        });
    }

    static ReplayLoadResult makeSeedReplayForLevel() {
        ReplayLoadResult seeded;
        seeded.framerate = 480.0;
        // One tiny placeholder press so the editor can always open and save a first replay file.
        seeded.presses.push_back(ReplayPress{0, 2, 1});
        return seeded;
    }

    static int activePlayerForEditor(const ExternalReplayEditor* editor) {
        if (!editor || editor->replay.presses.empty()) return 1;
        size_t idx = std::min(editor->index, editor->replay.presses.size() - 1);
        int player = editor->replay.presses[idx].player;
        return (player == 2) ? 2 : 1;
    }

    static uint64_t frameFromTimelineX(ExternalReplayEditor* editor, int x) {
        int width = std::max(kTimelineMinWidth, editor ? editor->timelineWidth : 700);
        int localX = std::clamp(x - kTimelineLeft, 0, width - 1);
        int cellIndex = localX / 8;
        return editor->timelineStartFrame + static_cast<uint64_t>(cellIndex * editor->framesPerCell);
    }

    static int xFromFrame(const ExternalReplayEditor* editor, uint64_t frame) {
        int width = std::max(kTimelineMinWidth, editor ? editor->timelineWidth : 700);
        uint64_t start = editor ? editor->timelineStartFrame : 0;
        int fpc = editor ? std::max(1, editor->framesPerCell) : 1;
        if (frame <= start) return kTimelineLeft;
        uint64_t rel = frame - start;
        uint64_t px = (rel * 8ull) / static_cast<uint64_t>(fpc);
        if (px >= static_cast<uint64_t>(width)) return kTimelineLeft + width;
        return kTimelineLeft + static_cast<int>(px);
    }

    static bool hasPlayerOverlapInFrameRange(const ReplayLoadResult& replay, int player, uint64_t startFrame, uint64_t endFrameExclusive) {
        for (auto const& p : replay.presses) {
            if (p.player != player) continue;
            uint64_t a = p.framePress;
            uint64_t b = p.frameRelease;
            if (b <= startFrame) continue;
            if (a >= endFrameExclusive) continue;
            return true;
        }
        return false;
    }

    static uint64_t timelineWindowFrames(const ExternalReplayEditor* editor);

    static void updateTimelineInfoLabel(ExternalReplayEditor* editor) {
        if (!editor || !editor->hwnd) return;
        uint64_t endFrame = editor->timelineStartFrame + timelineWindowFrames(editor);
        int activePlayer = activePlayerForEditor(editor);
        int activeVisible = 0;
        int otherVisible = 0;
        for (auto const& p : editor->replay.presses) {
            if (p.frameRelease <= editor->timelineStartFrame || p.framePress >= endFrame) continue;
            if (p.player == activePlayer) activeVisible++;
            else otherVisible++;
        }
        uint64_t liveFrame = g_replayExternalCommands.liveFrame.load();
        bool livePaused = g_replayExternalCommands.livePaused.load();
        auto text = fmt::format(
            "Window {}..{} | {}f/cell | P{} vis:{} other:{} | live:{} {} | follow:{}",
            editor->timelineStartFrame,
            endFrame,
            editor->framesPerCell,
            activePlayer,
            activeVisible,
            otherVisible,
            liveFrame,
            livePaused ? "paused" : "play",
            editor->followPlayhead ? "on" : "off"
        );
        std::wstring wText(text.begin(), text.end());
        SetWindowTextW(GetDlgItem(editor->hwnd, IDC_LABEL_TL_INFO), wText.c_str());
    }

    static void followLiveFrame(ExternalReplayEditor* editor) {
        if (!editor) return;
        bool active = g_replayExternalCommands.liveReplayActive.load();
        if (!active) return;
        bool livePaused = g_replayExternalCommands.livePaused.load();

        int width = std::max(kTimelineMinWidth, editor->timelineWidth);
        uint64_t windowFrames = static_cast<uint64_t>((width / 8) * std::max(1, editor->framesPerCell));
        if (windowFrames < 2) windowFrames = 2;

        uint64_t liveFrame = g_replayExternalCommands.liveFrame.load();
        if (!livePaused) {
            editor->timelineStartFrame = (liveFrame > windowFrames / 3) ? (liveFrame - windowFrames / 3) : 0;
            return;
        }

        if (!editor->followPlayhead) return;

        uint64_t leftBound = editor->timelineStartFrame + windowFrames / 8;
        uint64_t rightBound = editor->timelineStartFrame + (windowFrames * 7) / 8;
        
        static uint64_t s_lastLogFrame = 0;
        if (liveFrame != s_lastLogFrame) {
            s_lastLogFrame = liveFrame;
            traceDebug("[TLDBG] followLiveFrame liveFrame={} bounds=[{}..{}] start={} -> {}",
                liveFrame, leftBound, rightBound, editor->timelineStartFrame,
                (liveFrame < leftBound || liveFrame > rightBound) ? "ADJUST" : "OK");
        }

        if (liveFrame < leftBound || liveFrame > rightBound) {
            editor->timelineStartFrame = (liveFrame > windowFrames / 3) ? (liveFrame - windowFrames / 3) : 0;
        }
    }

    static void ensureSelectedInputVisible(ExternalReplayEditor* editor) {
        if (!editor || editor->replay.presses.empty()) return;
        int width = std::max(kTimelineMinWidth, editor->timelineWidth);
        uint64_t windowFrames = static_cast<uint64_t>((width / 8) * std::max(1, editor->framesPerCell));
        if (windowFrames < 2) windowFrames = 2;

        auto const& sel = editor->replay.presses[editor->index];
        uint64_t start = editor->timelineStartFrame;
        uint64_t end = start + windowFrames;
        bool visible = !(sel.frameRelease <= start || sel.framePress >= end);
        if (visible) return;

        uint64_t target = (sel.framePress > windowFrames / 3) ? (sel.framePress - windowFrames / 3) : 0;
        editor->timelineStartFrame = target;
    }

    static uint64_t timelineWindowFrames(const ExternalReplayEditor* editor) {
        int width = std::max(kTimelineMinWidth, editor ? editor->timelineWidth : 700);
        int framesPerCell = editor ? std::max(1, editor->framesPerCell) : 1;
        return static_cast<uint64_t>((width / 8) * framesPerCell);
    }

    static bool isPointInTimeline(const ExternalReplayEditor* editor, int x, int y) {
        if (!editor) return false;
        int width = std::max(kTimelineMinWidth, editor->timelineWidth);
        return x >= kTimelineLeft && x < kTimelineLeft + width &&
            y >= kTimelineTop && y < kTimelineTop + kTimelineHeight;
    }

    static void zoomTimelineAtX(ExternalReplayEditor* editor, int clientX, bool zoomIn) {
        if (!editor) return;

        int width = std::max(kTimelineMinWidth, editor->timelineWidth);
        clientX = std::clamp(clientX, kTimelineLeft, kTimelineLeft + width - 1);
        uint64_t anchorFrame = frameFromTimelineX(editor, clientX);

        if (zoomIn) {
            editor->framesPerCell = std::max(1, editor->framesPerCell / 2);
        } else {
            editor->framesPerCell = std::min(32, editor->framesPerCell * 2);
        }

        int localX = std::clamp(clientX - kTimelineLeft, 0, width - 1);
        uint64_t cellFrame = static_cast<uint64_t>((localX / 8) * editor->framesPerCell);
        editor->timelineStartFrame = (anchorFrame > cellFrame) ? (anchorFrame - cellFrame) : 0;
    }

    static void setTimelineAutoFollow(ExternalReplayEditor* editor, bool enabled) {
        if (!editor) return;
        editor->followPlayhead = enabled;
        if (editor->hwnd) {
            SetWindowTextW(
                GetDlgItem(editor->hwnd, IDC_BTN_CTRL_FOLLOW),
                enabled ? L"Auto-follow:On" : L"Auto-follow:Off"
            );
        }
    }

    static uint64_t clampNonNegativeFrame(int64_t frame);

    static void panTimelineByFrames(ExternalReplayEditor* editor, int64_t deltaFrames) {
        if (!editor) return;

        if (deltaFrames == 0) return;
        setTimelineAutoFollow(editor, false);

        int64_t next = static_cast<int64_t>(editor->timelineStartFrame) + deltaFrames;
        editor->timelineStartFrame = clampNonNegativeFrame(next);
    }

    static uint64_t clampNonNegativeFrame(int64_t frame) {
        return static_cast<uint64_t>(std::max<int64_t>(0, frame));
    }

    static uint64_t snapFrameToGrid(const ExternalReplayEditor* editor, uint64_t frame) {
        int snap = editor ? std::max(1, editor->snapFrames) : 1;
        return static_cast<uint64_t>(((frame + static_cast<uint64_t>(snap / 2)) / static_cast<uint64_t>(snap)) * static_cast<uint64_t>(snap));
    }

    static size_t findPressIndexNearFrame(const ReplayLoadResult& replay, int player, uint64_t frame, uint64_t tolerance) {
        for (size_t i = 0; i < replay.presses.size(); ++i) {
            auto const& p = replay.presses[i];
            if (p.player != player) continue;
            if (frame + tolerance < p.framePress) continue;
            if (frame > p.frameRelease + tolerance) continue;
            return i;
        }
        return std::numeric_limits<size_t>::max();
    }

    static void commitTimelineDragState(ExternalReplayEditor* editor) {
        if (!editor || editor->replay.presses.empty() || editor->timelineDragIndex >= editor->replay.presses.size()) return;
        auto dragged = editor->replay.presses[editor->timelineDragIndex];
        sortReplayPresses(editor->replay);
        size_t selected = editor->timelineDragIndex;
        for (size_t i = 0; i < editor->replay.presses.size(); ++i) {
            auto const& p = editor->replay.presses[i];
            if (p.player == dragged.player && p.framePress == dragged.framePress && p.frameRelease == dragged.frameRelease) {
                selected = i;
                break;
            }
        }
        editor->index = selected;
        editor->timelineDragMode = ExternalReplayEditor::TimelineDragMode::None;
        editor->timelineDragIndex = std::numeric_limits<size_t>::max();
    }

    static void updateTimelineDrag(ExternalReplayEditor* editor, uint64_t frame) {
        if (!editor || editor->timelineDragIndex >= editor->replay.presses.size()) return;

        auto& press = editor->replay.presses[editor->timelineDragIndex];
        uint64_t snapped = snapFrameToGrid(editor, frame);
        uint64_t snap = static_cast<uint64_t>(std::max(1, editor->snapFrames));

        switch (editor->timelineDragMode) {
            case ExternalReplayEditor::TimelineDragMode::Move: {
                int64_t delta = static_cast<int64_t>(snapped) - static_cast<int64_t>(editor->timelineDragStartFrame);
                int64_t newPress = static_cast<int64_t>(editor->timelineDragStartPress) + delta;
                int64_t newRelease = static_cast<int64_t>(editor->timelineDragStartRelease) + delta;
                press.framePress = snapFrameToGrid(editor, clampNonNegativeFrame(newPress));
                press.frameRelease = snapFrameToGrid(editor, clampNonNegativeFrame(newRelease));
                if (press.frameRelease <= press.framePress) {
                    press.frameRelease = press.framePress + snap;
                }
                break;
            }
            case ExternalReplayEditor::TimelineDragMode::ResizeStart: {
                uint64_t newPress = snapFrameToGrid(editor, snapped);
                if (newPress + 1 >= press.frameRelease) {
                    newPress = press.frameRelease - 1;
                }
                press.framePress = newPress;
                break;
            }
            case ExternalReplayEditor::TimelineDragMode::ResizeEnd: {
                uint64_t newRelease = snapFrameToGrid(editor, snapped);
                if (newRelease <= press.framePress) {
                    newRelease = press.framePress + 1;
                }
                press.frameRelease = newRelease;
                break;
            }
            case ExternalReplayEditor::TimelineDragMode::Create: {
                uint64_t start = std::min(editor->timelineDragStartFrame, snapped);
                uint64_t end = std::max(editor->timelineDragStartFrame, snapped);
                start = snapFrameToGrid(editor, start);
                end = snapFrameToGrid(editor, end);
                if (end <= start) end = start + snap;
                press.framePress = start;
                press.frameRelease = end;
                break;
            }
            case ExternalReplayEditor::TimelineDragMode::None:
                break;
        }

        if (press.frameRelease <= press.framePress) {
            press.frameRelease = press.framePress + 1;
        }
    }

    static bool parseFrameFromEdit(HWND hwndEdit, uint64_t& outFrame) {
        wchar_t buf[64] = {0};
        GetWindowTextW(hwndEdit, buf, static_cast<int>(std::size(buf)));
        std::wstring text(buf);
        if (text.empty()) return false;
        std::string narrow(text.begin(), text.end());
        auto parsed = geode::utils::numFromString<uint64_t>(narrow);
        if (!parsed) {
            return false;
        }
        outFrame = *parsed;
        return true;
    }

    static void setEditFrame(HWND parent, int controlId, uint64_t frame) {
        auto text = std::to_wstring(frame);
        SetWindowTextW(GetDlgItem(parent, controlId), text.c_str());
    }

    static void refreshEditorUi(ExternalReplayEditor* editor) {
        if (!editor || !editor->hwnd || editor->replay.presses.empty()) return;

        editor->timelineWidth = currentTimelineWidth(editor);

        auto const count = editor->replay.presses.size();
        editor->index = std::min(editor->index, count - 1);
        bool liveActive = g_replayExternalCommands.liveReplayActive.load();
        if (!liveActive) {
            ensureSelectedInputVisible(editor);
        }

        auto const& press = editor->replay.presses[editor->index];
        auto label = fmt::format("Input {} / {} | Player {}", editor->index + 1, count, press.player);
        std::wstring wLabel(label.begin(), label.end());
        SetWindowTextW(GetDlgItem(editor->hwnd, IDC_LABEL_INDEX), wLabel.c_str());

        setEditFrame(editor->hwnd, IDC_EDIT_PRESS, press.framePress);
        setEditFrame(editor->hwnd, IDC_EDIT_RELEASE, press.frameRelease);

        auto snapLabel = fmt::format("Snap: {}f", editor->snapFrames);
        std::wstring wSnap(snapLabel.begin(), snapLabel.end());
        SetWindowTextW(GetDlgItem(editor->hwnd, IDC_LABEL_SNAP), wSnap.c_str());

        updateTimelineInfoLabel(editor);
        RECT timelineRect {
            kTimelineLeft,
            kTimelineTop,
            kTimelineLeft + std::max(kTimelineMinWidth, editor->timelineWidth),
            kTimelineTop + kTimelineHeight
        };
        InvalidateRect(editor->hwnd, &timelineRect, FALSE);
    }

    static bool applyCurrentEdit(ExternalReplayEditor* editor, bool showError) {
        if (!editor || editor->replay.presses.empty()) return false;

        uint64_t pressFrame = 0;
        uint64_t releaseFrame = 0;
        if (!parseFrameFromEdit(GetDlgItem(editor->hwnd, IDC_EDIT_PRESS), pressFrame) ||
            !parseFrameFromEdit(GetDlgItem(editor->hwnd, IDC_EDIT_RELEASE), releaseFrame)) {
            if (showError) {
                MessageBoxW(editor->hwnd, L"Valeur de frame invalide.", L"Replay Editor", MB_ICONWARNING | MB_OK);
            }
            return false;
        }

        if (releaseFrame <= pressFrame) {
            releaseFrame = pressFrame + 1;
        }

        auto& current = editor->replay.presses[editor->index];
        current.framePress = pressFrame;
        current.frameRelease = releaseFrame;
        return true;
    }

    static void nudgeCurrentFrame(ExternalReplayEditor* editor, bool press, int delta) {
        if (!editor || editor->replay.presses.empty()) return;
        if (!applyCurrentEdit(editor, true)) return;

        auto& p = editor->replay.presses[editor->index];
        int64_t step = static_cast<int64_t>(editor->snapFrames) * static_cast<int64_t>(delta);
        if (press) {
            p.framePress = clampNonNegativeFrame(static_cast<int64_t>(p.framePress) + step);
            if (p.frameRelease <= p.framePress) p.frameRelease = p.framePress + 1;
        } else {
            p.frameRelease = clampNonNegativeFrame(static_cast<int64_t>(p.frameRelease) + step);
            if (p.frameRelease <= p.framePress) p.frameRelease = p.framePress + 1;
        }

        refreshEditorUi(editor);
    }

    static void paintTimeline(ExternalReplayEditor* editor, HDC hdc) {
        if (!editor || editor->replay.presses.empty()) return;

        int width = std::max(kTimelineMinWidth, editor->timelineWidth);
        RECT rc{ kTimelineLeft, kTimelineTop, kTimelineLeft + width, kTimelineTop + kTimelineHeight };
        RECT header{ rc.left + 1, rc.top + 1, rc.right - 1, rc.top + 19 };
        RECT track{ rc.left + 1, rc.top + 20, rc.right - 1, rc.bottom - 1 };
        RECT footer{ rc.left + 1, rc.bottom - 16, rc.right - 1, rc.bottom - 1 };

        HBRUSH bgBrush = CreateSolidBrush(RGB(19, 22, 29));
        HBRUSH panelBrush = CreateSolidBrush(RGB(28, 32, 42));
        HBRUSH headerBrush = CreateSolidBrush(RGB(36, 41, 55));
        HBRUSH railBrush = CreateSolidBrush(RGB(24, 28, 37));
        HBRUSH laneBrush = CreateSolidBrush(RGB(31, 36, 48));
        HBRUSH p1Brush = CreateSolidBrush(RGB(84, 216, 133));
        HBRUSH p1DimBrush = CreateSolidBrush(RGB(52, 134, 84));
        HBRUSH p2Brush = CreateSolidBrush(RGB(104, 163, 255));
        HBRUSH p2DimBrush = CreateSolidBrush(RGB(60, 106, 185));
        HBRUSH liveBrush = CreateSolidBrush(RGB(255, 208, 88));

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(74, 87, 111));
        HPEN gridMinorPen = CreatePen(PS_SOLID, 1, RGB(48, 56, 72));
        HPEN gridMajorPen = CreatePen(PS_SOLID, 1, RGB(82, 93, 118));
        HPEN pressPen = CreatePen(PS_SOLID, 1, RGB(114, 206, 255));
        HPEN releasePen = CreatePen(PS_SOLID, 1, RGB(255, 130, 130));
        HPEN playheadPen = CreatePen(PS_SOLID, 2, RGB(255, 208, 88));
        HPEN playheadMutedPen = CreatePen(PS_SOLID, 2, RGB(145, 149, 160));
        HPEN selectPen = CreatePen(PS_SOLID, 2, RGB(255, 235, 152));
        HFONT timelineFont = CreateFontW(
            -11,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );
        HGDIOBJ oldFont = SelectObject(hdc, timelineFont);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(236, 240, 248));

        FillRect(hdc, &rc, bgBrush);
        FillRect(hdc, &rc, panelBrush);
        FillRect(hdc, &header, headerBrush);
        FillRect(hdc, &track, railBrush);
        FillRect(hdc, &footer, panelBrush);

        auto drawBadge = [&](int x, const std::wstring& text, COLORREF bg, COLORREF fg) {
            RECT badge{ x, rc.top + 3, x + 1, rc.top + 16 };
            DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &badge, DT_SINGLELINE | DT_CALCRECT);
            InflateRect(&badge, 8, 2);
            HBRUSH badgeBrush = CreateSolidBrush(bg);
            FillRect(hdc, &badge, badgeBrush);
            DeleteObject(badgeBrush);
            SetTextColor(hdc, fg);
            DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &badge, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            return badge.right + 6;
        };

        auto const& presses = editor->replay.presses;
        int activePlayer = activePlayerForEditor(editor);
        uint64_t liveFrame = g_replayExternalCommands.liveFrame.load();
        bool livePaused = g_replayExternalCommands.livePaused.load();
        bool liveActive = g_replayExternalCommands.liveReplayActive.load();
        uint64_t windowStart = editor->timelineStartFrame;
        uint64_t windowEnd = windowStart + timelineWindowFrames(editor);

        SetTextColor(hdc, RGB(244, 247, 255));
        std::wstring title = L"Timeline";
        TextOutW(hdc, rc.left + 8, rc.top + 2, title.c_str(), static_cast<int>(title.size()));

        int badgeX = rc.left + 92;
        badgeX = drawBadge(badgeX, std::wstring(L"P") + std::to_wstring(activePlayer), activePlayer == 1 ? RGB(84, 216, 133) : RGB(104, 163, 255), RGB(16, 18, 24));
        badgeX = drawBadge(badgeX, std::wstring(L"Zoom x") + std::to_wstring(editor->framesPerCell), RGB(53, 59, 76), RGB(236, 240, 248));
        badgeX = drawBadge(badgeX, livePaused ? L"Paused" : L"Playing", livePaused ? RGB(120, 123, 133) : RGB(82, 214, 129), RGB(16, 18, 24));
        badgeX = drawBadge(badgeX, liveActive ? L"Live" : L"Idle", liveActive ? RGB(255, 208, 88) : RGB(72, 79, 95), RGB(16, 18, 24));

        int cells = std::max(1, width / 8);
        int gridTop = rc.top + 20;
        int otherLaneTop = rc.top + 25;
        int otherLaneBottom = rc.top + 34;
        int mainLaneTop = rc.top + 40;
        int mainLaneBottom = rc.bottom - 18;

        for (int i = 0; i <= cells; ++i) {
            int x = kTimelineLeft + i * 8;
            bool major = (i % 4) == 0;
            SelectObject(hdc, major ? gridMajorPen : gridMinorPen);
            MoveToEx(hdc, x, gridTop, nullptr);
            LineTo(hdc, x, rc.bottom - 18);

            if (major) {
                uint64_t frame = windowStart + static_cast<uint64_t>(i * std::max(1, editor->framesPerCell));
                std::wstring frameText = std::to_wstring(frame);
                SetTextColor(hdc, RGB(155, 165, 186));
                TextOutW(hdc, x + 2, rc.top + 15, frameText.c_str(), static_cast<int>(frameText.size()));
            }
        }

        for (int i = 0; i < cells; ++i) {
            int x0 = kTimelineLeft + i * 8;
            int x1 = x0 + 8;
            uint64_t frame = editor->timelineStartFrame + static_cast<uint64_t>(i * std::max(1, editor->framesPerCell));
            uint64_t nextFrame = frame + static_cast<uint64_t>(std::max(1, editor->framesPerCell));
            bool p1Active = hasPlayerOverlapInFrameRange(editor->replay, 1, frame, nextFrame);
            bool p2Active = hasPlayerOverlapInFrameRange(editor->replay, 2, frame, nextFrame);

            RECT bandTop{ x0, otherLaneTop, x1, otherLaneBottom };
            RECT bandMain{ x0, mainLaneTop, x1, mainLaneBottom };
            FillRect(hdc, &bandTop, p2Active ? p2DimBrush : laneBrush);
            FillRect(hdc, &bandMain, p1Active ? p1DimBrush : laneBrush);
        }

        for (auto const& p : presses) {
            int x0 = xFromFrame(editor, p.framePress);
            int x1 = xFromFrame(editor, p.frameRelease);
            if (x1 <= rc.left || x0 >= rc.right) continue;
            x0 = std::max(x0, static_cast<int>(rc.left));
            x1 = std::min(x1, static_cast<int>(rc.right));
            if (x1 <= x0) x1 = x0 + 1;

            RECT seg{};
            if (p.player == 1) {
                seg = { x0, mainLaneTop + 3, x1, mainLaneBottom - 2 };
                FillRect(hdc, &seg, p1Brush);
            } else {
                seg = { x0, otherLaneTop + 1, x1, otherLaneBottom - 1 };
                FillRect(hdc, &seg, p2Brush);
            }
        }

        for (auto const& p : presses) {
            if (p.player != activePlayer) continue;
            auto drawEvent = [&](uint64_t frame, HPEN pen) {
                if (frame < editor->timelineStartFrame) return;
                uint64_t rel = frame - editor->timelineStartFrame;
                uint64_t idx = rel / static_cast<uint64_t>(std::max(1, editor->framesPerCell));
                if (idx >= static_cast<uint64_t>(cells)) return;
                int x = kTimelineLeft + static_cast<int>(idx * 8);
                SelectObject(hdc, pen);
                MoveToEx(hdc, x, mainLaneTop - 2, nullptr);
                LineTo(hdc, x, mainLaneBottom + 1);
            };
            drawEvent(p.framePress, pressPen);
            drawEvent(p.frameRelease, releasePen);
        }

        if (editor->index < presses.size()) {
            auto const& sel = presses[editor->index];
            int x0 = xFromFrame(editor, sel.framePress);
            int x1 = xFromFrame(editor, sel.frameRelease);
            if (x1 > rc.left && x0 < rc.right) {
                x0 = std::max(x0, static_cast<int>(rc.left));
                x1 = std::min(x1, static_cast<int>(rc.right));
                if (x1 <= x0) x1 = x0 + 1;
                RECT selRect{};
                if (sel.player == 1) {
                    selRect = { x0, mainLaneTop + 1, x1, mainLaneBottom - 1 };
                } else {
                    selRect = { x0, otherLaneTop, x1, otherLaneBottom };
                }
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                HGDIOBJ oldPen = SelectObject(hdc, selectPen);
                RoundRect(hdc, selRect.left, selRect.top, selRect.right, selRect.bottom, 8, 8);
                RECT leftHandle{ selRect.left - 2, selRect.top, selRect.left + 4, selRect.bottom };
                RECT rightHandle{ selRect.right - 4, selRect.top, selRect.right + 2, selRect.bottom };
                FillRect(hdc, &leftHandle, liveBrush);
                FillRect(hdc, &rightHandle, liveBrush);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
            }
        }

        int xPlayhead = xFromFrame(editor, liveFrame);
        if (xPlayhead >= rc.left && xPlayhead <= rc.right) {
            SelectObject(hdc, livePaused ? playheadMutedPen : playheadPen);
            MoveToEx(hdc, xPlayhead, gridTop, nullptr);
            LineTo(hdc, xPlayhead, mainLaneBottom + 1);

            int liveBadgeX = std::clamp(xPlayhead + 6, static_cast<int>(rc.left) + 4, static_cast<int>(rc.right) - 60);
            RECT liveBadge{ liveBadgeX, rc.top + 3, liveBadgeX + 56, rc.top + 16 };
            FillRect(hdc, &liveBadge, liveBrush);
            SetTextColor(hdc, RGB(16, 18, 24));
            std::wstring liveText = livePaused ? L"PAUSED" : L"LIVE";
            DrawTextW(hdc, liveText.c_str(), static_cast<int>(liveText.size()), &liveBadge, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }

        SetTextColor(hdc, RGB(195, 204, 218));
        std::wstring hint = L"Drag a block to move, drag its ends to resize, drag empty space to create   Wheel = zoom";
        TextOutW(hdc, rc.left + 8, rc.bottom - 15, hint.c_str(), static_cast<int>(hint.size()));

        int totalForPlayer = 0;
        int visibleForPlayer = 0;
        for (auto const& p : presses) {
            if (p.player != activePlayer) continue;
            totalForPlayer++;
            if (!(p.frameRelease <= windowStart || p.framePress >= windowEnd)) {
                visibleForPlayer++;
            }
        }

        auto const& sel = presses[std::min(editor->index, presses.size() - 1)];
        auto status = fmt::format(
            "P{} visible {} / {} | window {}..{} | sel {}..{}",
            activePlayer,
            visibleForPlayer,
            totalForPlayer,
            windowStart,
            windowEnd,
            sel.framePress,
            sel.frameRelease
        );
        std::wstring wStatus(status.begin(), status.end());
        SetTextColor(hdc, RGB(240, 243, 248));
        TextOutW(hdc, rc.left + 8, rc.top + 50, wStatus.c_str(), static_cast<int>(wStatus.size()));

        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldFont);

        DeleteObject(bgBrush);
        DeleteObject(panelBrush);
        DeleteObject(headerBrush);
        DeleteObject(railBrush);
        DeleteObject(laneBrush);
        DeleteObject(p1Brush);
        DeleteObject(p1DimBrush);
        DeleteObject(p2Brush);
        DeleteObject(p2DimBrush);
        DeleteObject(liveBrush);
        DeleteObject(borderPen);
        DeleteObject(gridMinorPen);
        DeleteObject(gridMajorPen);
        DeleteObject(pressPen);
        DeleteObject(releasePen);
        DeleteObject(playheadPen);
        DeleteObject(playheadMutedPen);
        DeleteObject(selectPen);
        DeleteObject(timelineFont);
    }

    static LRESULT CALLBACK ReplayEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* editor = reinterpret_cast<ExternalReplayEditor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_CREATE: {
                auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
                auto* state = reinterpret_cast<ExternalReplayEditor*>(create->lpCreateParams);
                state->hwnd = hwnd;
                state->timelineWidth = currentTimelineWidth(state);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

                CreateWindowW(L"STATIC", L"Input 1 / 1", WS_CHILD | WS_VISIBLE,
                    16, 16, 220, 20, hwnd, reinterpret_cast<HMENU>(IDC_LABEL_INDEX), nullptr, nullptr);

                CreateWindowW(L"STATIC", L"Press frame", WS_CHILD | WS_VISIBLE,
                    16, 46, 100, 20, hwnd, nullptr, nullptr, nullptr);
                CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
                    16, 66, 150, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_PRESS), nullptr, nullptr);

                CreateWindowW(L"STATIC", L"Release frame", WS_CHILD | WS_VISIBLE,
                    196, 46, 110, 20, hwnd, nullptr, nullptr, nullptr);
                CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
                    196, 66, 150, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_RELEASE), nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"Prev", WS_CHILD | WS_VISIBLE,
                    16, 102, 70, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_PREV), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Next", WS_CHILD | WS_VISIBLE,
                    96, 102, 70, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_NEXT), nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"P-", WS_CHILD | WS_VISIBLE,
                    196, 102, 45, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_P_MINUS), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"P+", WS_CHILD | WS_VISIBLE,
                    246, 102, 45, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_P_PLUS), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"R-", WS_CHILD | WS_VISIBLE,
                    296, 102, 45, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_R_MINUS), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"R+", WS_CHILD | WS_VISIBLE,
                    346, 102, 45, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_R_PLUS), nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"Snap-", WS_CHILD | WS_VISIBLE,
                    16, 138, 70, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_SNAP_DOWN), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Snap+", WS_CHILD | WS_VISIBLE,
                    96, 138, 70, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_SNAP_UP), nullptr, nullptr);
                CreateWindowW(L"STATIC", L"Snap: 4f", WS_CHILD | WS_VISIBLE,
                    196, 143, 120, 20, hwnd, reinterpret_cast<HMENU>(IDC_LABEL_SNAP), nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE,
                    16, 176, 80, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_APPLY), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Generate", WS_CHILD | WS_VISIBLE,
                    101, 176, 100, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_GENERATE), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE,
                    206, 176, 80, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_SAVE), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"<-", WS_CHILD | WS_VISIBLE,
                    291, 176, 40, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_TL_LEFT), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"->", WS_CHILD | WS_VISIBLE,
                    336, 176, 40, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_TL_RIGHT), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Zoom-", WS_CHILD | WS_VISIBLE,
                    381, 176, 65, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM_OUT), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Zoom+", WS_CHILD | WS_VISIBLE,
                    451, 176, 65, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM_IN), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                    521, 176, 70, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CLOSE), nullptr, nullptr);
                CreateWindowW(L"STATIC", L"Timeline", WS_CHILD | WS_VISIBLE,
                    596, 182, 300, 20, hwnd, reinterpret_cast<HMENU>(IDC_LABEL_TL_INFO), nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"Pause/Play", WS_CHILD | WS_VISIBLE,
                    16, 360, 100, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CTRL_PAUSE), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Frame -", WS_CHILD | WS_VISIBLE,
                    126, 360, 80, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CTRL_STEP_BACK), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Frame +", WS_CHILD | WS_VISIBLE,
                    216, 360, 80, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CTRL_STEP_FWD), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Restart", WS_CHILD | WS_VISIBLE,
                    306, 360, 80, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CTRL_RESTART), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Auto-follow:On", WS_CHILD | WS_VISIBLE,
                    396, 360, 110, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CTRL_FOLLOW), nullptr, nullptr);

                SetTimer(hwnd, 1, 33, nullptr);

                refreshEditorUi(state);
                return 0;
            }

            case WM_SIZE:
                if (editor) {
                    editor->timelineWidth = currentTimelineWidth(editor);
                    refreshEditorUi(editor);
                }
                return 0;

            case WM_COMMAND: {
                if (!editor) return 0;
                auto id = LOWORD(wParam);

                if (id == IDC_BTN_PREV) {
                    applyCurrentEdit(editor, false);
                    if (editor->index > 0) editor->index--;
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_NEXT) {
                    applyCurrentEdit(editor, false);
                    if (editor->index + 1 < editor->replay.presses.size()) editor->index++;
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_P_MINUS) {
                    nudgeCurrentFrame(editor, true, -1);
                } else if (id == IDC_BTN_P_PLUS) {
                    nudgeCurrentFrame(editor, true, 1);
                } else if (id == IDC_BTN_R_MINUS) {
                    nudgeCurrentFrame(editor, false, -1);
                } else if (id == IDC_BTN_R_PLUS) {
                    nudgeCurrentFrame(editor, false, 1);
                } else if (id == IDC_BTN_SNAP_DOWN) {
                    editor->snapFrames = std::max(1, editor->snapFrames - 1);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_SNAP_UP) {
                    editor->snapFrames = std::min(16, editor->snapFrames + 1);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_APPLY) {
                    if (applyCurrentEdit(editor, true)) refreshEditorUi(editor);
                } else if (id == IDC_BTN_GENERATE) {
                    applyCurrentEdit(editor, false);

                    size_t beforeClicks = replayClickCount(editor->replay);
                    ReplayGeneratorConfig cfg;
                    cfg.targetFramerate = std::max(480.0, editor->replay.framerate);
                    cfg.minHoldFrames = static_cast<uint64_t>(std::max(1, editor->snapFrames / 2));
                    cfg.mergeGapFrames = 1;

                    editor->replay = generateUniversalLowClickReplay(editor->replay, cfg);
                    sortReplayPresses(editor->replay);
                    if (editor->replay.presses.empty()) {
                        MessageBoxW(hwnd, L"Generation impossible: replay vide apres optimisation.", L"Replay Editor", MB_OK | MB_ICONWARNING);
                        return 0;
                    }

                    editor->index = std::min(editor->index, editor->replay.presses.size() - 1);
                    size_t afterClicks = replayClickCount(editor->replay);
                    auto msg = fmt::format(
                        "Replay genere.\nClicks: {} -> {}\nFPS canonique: {:.0f}\n\nSauvegarde si le resultat te convient.",
                        beforeClicks,
                        afterClicks,
                        editor->replay.framerate
                    );
                    std::wstring wmsg(msg.begin(), msg.end());
                    MessageBoxW(hwnd, wmsg.c_str(), L"Replay Generator", MB_OK | MB_ICONINFORMATION);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_SAVE) {
                    applyCurrentEdit(editor, false);
                    sortReplayPresses(editor->replay);
                    bool ok = saveReplayToLocalJson(editor->levelId, editor->replay, editor->sourcePath);
                    if (ok) {
                        editor->saved = true;
                        MessageBoxW(hwnd, L"Replay sauvegarde avec succes.", L"Replay Editor", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(hwnd, L"Echec de la sauvegarde du replay.", L"Replay Editor", MB_OK | MB_ICONERROR);
                    }
                } else if (id == IDC_BTN_CLOSE) {
                    DestroyWindow(hwnd);
                } else if (id == IDC_BTN_TL_LEFT) {
                    int64_t step = static_cast<int64_t>(std::max<uint64_t>(1, timelineWindowFrames(editor) / 2));
                    panTimelineByFrames(editor, -step);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_TL_RIGHT) {
                    int64_t step = static_cast<int64_t>(std::max<uint64_t>(1, timelineWindowFrames(editor) / 2));
                    panTimelineByFrames(editor, step);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_ZOOM_OUT) {
                    editor->framesPerCell = std::min(32, editor->framesPerCell * 2);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_ZOOM_IN) {
                    editor->framesPerCell = std::max(1, editor->framesPerCell / 2);
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_CTRL_PAUSE) {
                    traceDebug("[UIDBG] Pause/Play clicked livePaused={}", g_replayExternalCommands.livePaused.load());
                    bool paused = g_replayExternalCommands.livePaused.load();
                    g_replayExternalCommands.setPausedState.store(paused ? 0 : 1);
                    traceDebug("[UIDBG] -> setPausedState={}", paused ? 0 : 1);
                    refocusGameWindow(editor);
                } else if (id == IDC_BTN_CTRL_STEP_BACK) {
                    traceDebug("[UIDBG] Frame- clicked");
                    g_replayExternalCommands.setPausedState.store(1);
                    g_replayExternalCommands.stepFrameRequests.fetch_sub(1);
                    traceDebug("[UIDBG] -> setPausedState=1 stepFrameRequests--");
                    refocusGameWindow(editor);
                } else if (id == IDC_BTN_CTRL_STEP_FWD) {
                    traceDebug("[UIDBG] Frame+ clicked");
                    g_replayExternalCommands.setPausedState.store(1);
                    g_replayExternalCommands.stepFrameRequests.fetch_add(1);
                    traceDebug("[UIDBG] -> setPausedState=1 stepFrameRequests++");
                    refocusGameWindow(editor);
                } else if (id == IDC_BTN_CTRL_RESTART) {
                    traceDebug("[UIDBG] Restart clicked");
                    g_replayExternalCommands.setPausedState.store(1);
                    g_replayExternalCommands.restartRequested.store(true);
                    traceDebug("[UIDBG] -> setPausedState=1 restartRequested=true");
                    refocusGameWindow(editor);
                } else if (id == IDC_BTN_CTRL_FOLLOW) {
                    setTimelineAutoFollow(editor, !editor->followPlayhead);
                    refreshEditorUi(editor);
                }
                return 0;
            }

            case WM_TIMER:
                if (editor && wParam == 1) {
                    followLiveFrame(editor);
                    if (editor->followPlayhead) {
                        SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_CTRL_FOLLOW), L"Auto-follow:On");
                    }
                    refreshEditorUi(editor);
                }
                return 0;

            case WM_MOUSEWHEEL: {
                if (!editor) return 0;
                POINT pt{ static_cast<LONG>(static_cast<short>(LOWORD(lParam))), static_cast<LONG>(static_cast<short>(HIWORD(lParam))) };
                ScreenToClient(hwnd, &pt);
                if (!isPointInTimeline(editor, pt.x, pt.y)) return 0;

                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                bool ctrlDown = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
                if (ctrlDown) {
                    zoomTimelineAtX(editor, pt.x, delta > 0);
                } else {
                    int64_t window = static_cast<int64_t>(std::max<uint64_t>(1, timelineWindowFrames(editor)));
                    int64_t step = std::max<int64_t>(1, window / 4);
                    panTimelineByFrames(editor, delta > 0 ? -step : step);
                }
                refreshEditorUi(editor);
                refocusGameWindow(editor);
                return 0;
            }

            case WM_LBUTTONDOWN: {
                if (!editor) return 0;
                int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
                int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
                int width = std::max(kTimelineMinWidth, editor->timelineWidth);
                if (x >= kTimelineLeft && x < kTimelineLeft + width &&
                    y >= kTimelineTop && y < kTimelineTop + kTimelineHeight) {
                    int activePlayer = activePlayerForEditor(editor);
                    uint64_t frame = frameFromTimelineX(editor, x);
                    uint64_t snappedFrame = snapFrameToGrid(editor, frame);
                    uint64_t tolerance = static_cast<uint64_t>(std::max(1, editor->snapFrames / 2));
                    size_t pressIndex = findPressIndexNearFrame(editor->replay, activePlayer, frame, tolerance);
                    editor->timelineDragging = true;
                    editor->lastDragFrame = frame;
                    editor->timelineDragStartFrame = snappedFrame;

                    if (pressIndex != std::numeric_limits<size_t>::max()) {
                        editor->index = pressIndex;
                        editor->timelineDragIndex = pressIndex;
                        auto const& press = editor->replay.presses[pressIndex];
                        editor->timelineDragStartPress = press.framePress;
                        editor->timelineDragStartRelease = press.frameRelease;
                        if (frame <= press.framePress + tolerance) {
                            editor->timelineDragMode = ExternalReplayEditor::TimelineDragMode::ResizeStart;
                        } else if (frame + tolerance >= press.frameRelease) {
                            editor->timelineDragMode = ExternalReplayEditor::TimelineDragMode::ResizeEnd;
                        } else {
                            editor->timelineDragMode = ExternalReplayEditor::TimelineDragMode::Move;
                        }
                    } else {
                        ReplayPress created{snappedFrame, snappedFrame + static_cast<uint64_t>(std::max(1, editor->snapFrames)), activePlayer};
                        editor->replay.presses.push_back(created);
                        editor->timelineDragIndex = editor->replay.presses.size() - 1;
                        editor->index = editor->timelineDragIndex;
                        editor->timelineDragMode = ExternalReplayEditor::TimelineDragMode::Create;
                        editor->timelineDragStartPress = created.framePress;
                        editor->timelineDragStartRelease = created.frameRelease;
                    }

                    SetCapture(hwnd);
                    refreshEditorUi(editor);
                    refocusGameWindow(editor);
                    return 0;
                }
                break;
            }

            case WM_MOUSEMOVE: {
                if (!editor || !editor->timelineDragging) break;
                int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
                int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
                (void)y;
                uint64_t frame = frameFromTimelineX(editor, x);
                if (frame != editor->lastDragFrame) {
                    editor->lastDragFrame = frame;
                    updateTimelineDrag(editor, frame);
                    refreshEditorUi(editor);
                    refocusGameWindow(editor);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (editor && editor->timelineDragging) {
                    editor->timelineDragging = false;
                    editor->lastDragFrame = std::numeric_limits<uint64_t>::max();
                    commitTimelineDragState(editor);
                    ReleaseCapture();
                    refreshEditorUi(editor);
                    refocusGameWindow(editor);
                    return 0;
                }
                break;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT client{};
                GetClientRect(hwnd, &client);
                int clientWidth = std::max(1, static_cast<int>(client.right - client.left));
                int clientHeight = std::max(1, static_cast<int>(client.bottom - client.top));

                HDC memDc = CreateCompatibleDC(hdc);
                HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientWidth, clientHeight);
                HGDIOBJ oldBitmap = SelectObject(memDc, memBitmap);
                HBRUSH backBrush = CreateSolidBrush(RGB(23, 26, 34));
                RECT fillRc{0, 0, clientWidth, clientHeight};
                FillRect(memDc, &fillRc, backBrush);
                DeleteObject(backBrush);

                paintTimeline(editor, memDc);
                BitBlt(hdc, 0, 0, clientWidth, clientHeight, memDc, 0, 0, SRCCOPY);

                SelectObject(memDc, oldBitmap);
                DeleteObject(memBitmap);
                DeleteDC(memDc);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                if (editor) {
                    editor->timelineDragging = false;
                    editor->lastDragFrame = std::numeric_limits<uint64_t>::max();
                }
                KillTimer(hwnd, 1);
                return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static void runExternalReplayEditorWindow(int levelId, ReplayLoadResult replay, std::filesystem::path sourcePath) {
        if (replay.presses.empty()) {
            MessageBoxW(nullptr, L"Replay vide.", L"Replay Editor", MB_ICONWARNING | MB_OK);
            s_externalEditorRunning = false;
            return;
        }

        static bool s_registered = false;
        static constexpr wchar_t kClassName[] = L"GDModReplayEditorWindow";
        if (!s_registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = ReplayEditorWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClassName;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            if (!RegisterClassW(&wc)) {
                MessageBoxW(nullptr, L"Impossible d'initialiser la fenetre d'edition.", L"Replay Editor", MB_ICONERROR | MB_OK);
                s_externalEditorRunning = false;
                return;
            }
            s_registered = true;
        }

        ExternalReplayEditor editor;
        editor.levelId = levelId;
        editor.sourcePath = std::move(sourcePath);
        editor.replay = std::move(replay);
        editor.gameWindow = GetForegroundWindow();
        sortReplayPresses(editor.replay);
        for (auto const& p : editor.replay.presses) {
            if (p.player == 1) {
                editor.timelineStartFrame = (p.framePress > 40) ? (p.framePress - 40) : 0;
                break;
            }
        }

        auto title = fmt::format("Replay Editor v3 - Level {}", levelId);
        std::wstring wTitle(title.begin(), title.end());
        HWND hwnd = CreateWindowExW(
            WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kClassName,
            wTitle.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            760,
            450,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            &editor
        );

        if (!hwnd) {
            MessageBoxW(nullptr, L"Creation de la fenetre impossible.", L"Replay Editor", MB_ICONERROR | MB_OK);
            s_externalEditorRunning = false;
            return;
        }

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        MSG msg;
        while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (!IsWindow(hwnd)) break;
        }

        s_externalEditorRunning = false;
    }

    static bool openExternalReplayEditorWindow(int levelId, ReplayLoadResult replay, std::filesystem::path sourcePath) {
        bool expected = false;
        if (!s_externalEditorRunning.compare_exchange_strong(expected, true)) {
            MessageBoxW(nullptr, L"Une fenetre d'edition est deja ouverte.", L"Replay Editor", MB_ICONINFORMATION | MB_OK);
            return false;
        }

        try {
            std::thread(
                [levelId, replay = std::move(replay), sourcePath = std::move(sourcePath)]() mutable {
                    ExternalEditorRunningGuard guard;
                    try {
                        runExternalReplayEditorWindow(levelId, std::move(replay), std::move(sourcePath));
                    } catch (std::exception const& e) {
                        log::error("[Replay Editor] External editor crashed: {}", e.what());
                    } catch (...) {
                        log::error("[Replay Editor] External editor crashed with an unknown exception");
                    }
                }
            ).detach();
        } catch (std::exception const& e) {
            s_externalEditorRunning.store(false);
            auto utf8ToWide = [](std::string const& text) {
                int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
                if (required <= 0) {
                    return std::wstring(L"Impossible d'ouvrir la fenetre d'edition.");
                }

                std::wstring result(required - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
                return result;
            };

            auto message = utf8ToWide(fmt::format("Impossible d'ouvrir la fenetre d'edition:\n{}", e.what()));
            MessageBoxW(nullptr, message.c_str(), L"Replay Editor", MB_ICONERROR | MB_OK);
            return false;
        }

        return true;
    }
}
#endif
// MenuLayer Hook
// ============================================================

static bool isDevMode() {
    auto c = Mod::get()->getSettingValue<ccColor3B>("accent-color");
    return c.r == 21 && c.g == 3 && c.b == 20;
}

class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        if (!isDevMode()) return true;

        // Create button with a sprite
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto label = CCLabelBMFont::create("GDR2\n->JSON", "bigFont.fnt");
        label->setScale(0.25f);
        label->setPosition(btnSprite->getContentSize() / 2);
        btnSprite->addChild(label);

        auto myButton = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(MyMenuLayer::onConvertGDR2)
        );

        auto menu = this->getChildByID("bottom-menu");
        menu->addChild(myButton);
        myButton->setID("gdr2-to-json-btn"_spr);
        menu->updateLayout();

        return true;
    }

    void onConvertGDR2(CCObject*) {
#ifdef GEODE_IS_WINDOWS
        // Use Win32 native file dialog (synchronous, no arc dependency)
        OPENFILENAMEW ofn{};
        wchar_t szFile[MAX_PATH] = {0};

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"GDR2 Replay Files (*.gdr2)\0*.gdr2\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        // Set default directory to Eclipse replays folder
        auto defaultDir = dirs::getGameDir() / "geode" / "config" / "prevter.eclipsemenu" / "replays";
        if (!std::filesystem::exists(defaultDir)) {
            defaultDir = dirs::getGameDir();
        }
        auto defaultDirStr = defaultDir.wstring();
        ofn.lpstrInitialDir = defaultDirStr.c_str();

        if (GetOpenFileNameW(&ofn)) {
            auto path = std::filesystem::path(szFile);
            convertGdr2File(path);
        }
#else
        FLAlertLayer::create("Error", "File picker is only available on Windows.", "OK")->show();
#endif
    }
};

// ============================================================
// LevelInfoLayer Hook – Add Watch Replay Button
// ============================================================

class $modify(MyLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool selectLevel) {
        if (!LevelInfoLayer::init(level, selectLevel)) return false;

        // Add Watch Replay button
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto label = CCLabelBMFont::create("Watch\nReplay", "bigFont.fnt");
        label->setScale(0.3f);
        label->setPosition(btnSprite->getContentSize() / 2);
        btnSprite->addChild(label);

        auto watchButton = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(MyLevelInfoLayer::onWatchReplay)
        );
        watchButton->setID("watch-replay-btn"_spr);

        auto editBtnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto editLabel = CCLabelBMFont::create("Edit\nReplay", "bigFont.fnt");
        editLabel->setScale(0.28f);
        editLabel->setPosition(editBtnSprite->getContentSize() / 2);
        editBtnSprite->addChild(editLabel);

        auto editButton = CCMenuItemSpriteExtra::create(
            editBtnSprite,
            this,
            menu_selector(MyLevelInfoLayer::onEditReplay)
        );
        editButton->setID("edit-replay-btn"_spr);

        auto viewBtnSprite = CCSprite::createWithSpriteFrameName("GJ_plainBtn_001.png");
        auto viewLabel = CCLabelBMFont::create("View\nLevel", "bigFont.fnt");
        viewLabel->setScale(0.28f);
        viewLabel->setPosition(viewBtnSprite->getContentSize() / 2);
        viewBtnSprite->addChild(viewLabel);

        auto viewButton = CCMenuItemSpriteExtra::create(
            viewBtnSprite,
            this,
            menu_selector(MyLevelInfoLayer::onViewLevel)
        );
        viewButton->setID("view-level-btn"_spr);

        // Reliable placement independent of internal LevelInfoLayer menu IDs.
        auto replayMenu = CCMenu::create();
        replayMenu->setID("watch-replay-menu"_spr);
        replayMenu->setPosition(CCPointZero);
        this->addChild(replayMenu, 200);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        watchButton->setPosition({winSize.width - 64.0f, 86.0f});
        editButton->setPosition({winSize.width - 154.0f, 86.0f});
        viewButton->setPosition({winSize.width - 244.0f, 86.0f});
        replayMenu->addChild(watchButton);
        replayMenu->addChild(editButton);
        replayMenu->addChild(viewButton);

        return true;
    }

    void onWatchReplay(CCObject*) {
        auto level = m_level;
        if (!level) return;

        // Set replay player state
        int levelId = level->m_levelID;
        auto replayResult = loadMatchingReplay(levelId);
        if (!replayResult.has_value()) {
            FLAlertLayer::create("Error", "No replay found for this level.", "OK")->show();
            return;
        }
        if (replayResult->presses.empty() || replayResult->framerate <= 0.0) {
            FLAlertLayer::create("Error", "Replay invalide: donnees vides ou framerate incorrect.", "OK")->show();
            return;
        }

        g_replayPlayer.isActive = true;
        g_replayPlayer.replay = std::move(replayResult.value());
        g_replayPlayer.playbackSpeed = 1.0f;
        g_replayPlayer.isPaused = false;
        g_replayPlayer.pauseTime = 0.0f;
        g_replayPlayer.inputIndex = 0.0f;
        g_replayPlayer.isEditMode = false;
        g_replayPlayer.levelId = levelId;
        g_replayPlayer.sourceReplayPath = g_lastMatchedLocalReplayPath.value_or(std::filesystem::path{});

        // Launch through LevelInfoLayer flow so scene transition is valid.
        this->onPlay(nullptr);
    }

    void onEditReplay(CCObject*) {
        auto level = m_level;
        if (!level) return;

        int levelId = level->m_levelID;
        auto replayResult = loadMatchingReplay(levelId);
        bool hasExistingReplay = replayResult.has_value() && !replayResult->presses.empty() && replayResult->framerate > 0.0;

#ifdef GEODE_IS_WINDOWS
        ReplayLoadResult editorReplay = hasExistingReplay ? std::move(replayResult.value()) : makeSeedReplayForLevel();
        auto source = hasExistingReplay ? g_lastMatchedLocalReplayPath.value_or(std::filesystem::path{}) : std::filesystem::path{};

        if (hasExistingReplay) {
            // Existing replay: launch level and keep game active while external editor is open.
            g_replayPlayer.isActive = true;
            g_replayPlayer.replay = editorReplay;
            g_replayPlayer.playbackSpeed = 1.0f;
            g_replayPlayer.isPaused = false;
            g_replayPlayer.pauseTime = 0.0f;
            g_replayPlayer.inputIndex = 0.0f;
            g_replayPlayer.isEditMode = false;
            g_replayPlayer.levelId = levelId;
            g_replayPlayer.sourceReplayPath = source;
            this->onPlay(nullptr);
        }

        bool opened = openExternalReplayEditorWindow(levelId, std::move(editorReplay), std::move(source));
        if (opened) {
            if (hasExistingReplay) {
                FLAlertLayer::create("Replay Editor", "Level launched. Click timeline frames to toggle press/release.", "OK")->show();
            } else {
                FLAlertLayer::create("Replay Editor", "No replay found: opened editor with a new base replay. Edit it, then Save.", "OK")->show();
            }
        }
#else
        FLAlertLayer::create("Replay Editor", "External editor is only available on Windows.", "OK")->show();
#endif
    }

    #if 0  // Generator disabled - requires pathfinding
    void onGenerateReplay(CCObject*) {
        auto level = m_level;
        if (!level) return;

#ifdef GEODE_IS_WINDOWS
        int levelId = level->m_levelID;
        auto levelString = extractLevelStringForGeneration(level);
        bool opened = openGenerateReplayWindow(levelId, std::move(levelString));
        if (!opened) {
            return;
        }
        FLAlertLayer::create(
            "Generate Replay",
            "Pathfinder-style search started (clean-room implementation).\nYou can stay in menus while it runs.",
            "OK"
        )->show();
#else
        FLAlertLayer::create("Generate Replay", "Generator window is only available on Windows.", "OK")->show();
#endif
    }
    #endif

    void onViewLevel(CCObject*) {
        auto level = m_level;
        if (!level) return;

#ifdef GEODE_IS_WINDOWS
        int levelId = level->m_levelID;
        bool opened = openReplayLevelViewWindow(levelId, "");
        if (!opened) return;
        FLAlertLayer::create("View Level", "Opened level viewer.\nIf a replay exists, trajectory shown (yellow line).\nMouse wheel = zoom, middle mouse = pan, right click = reset.", "OK")->show();
#else
        FLAlertLayer::create("View Level", "Level viewer is only available on Windows.", "OK")->show();
#endif
    }
};

// ============================================================

