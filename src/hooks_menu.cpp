#include "replay_state.hpp"
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/string.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

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
        IDC_BTN_CTRL_FOLLOW = 1025
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
        if (!editor->followPlayhead) return;
        bool active = g_replayExternalCommands.liveReplayActive.load();
        if (!active) return;

        int width = std::max(kTimelineMinWidth, editor->timelineWidth);
        uint64_t windowFrames = static_cast<uint64_t>((width / 8) * std::max(1, editor->framesPerCell));
        if (windowFrames < 2) windowFrames = 2;

        uint64_t liveFrame = g_replayExternalCommands.liveFrame.load();
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
        ensureSelectedInputVisible(editor);

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
                    16, 176, 90, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_APPLY), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE,
                    116, 176, 90, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_SAVE), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"<-", WS_CHILD | WS_VISIBLE,
                    216, 176, 45, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_TL_LEFT), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"->", WS_CHILD | WS_VISIBLE,
                    266, 176, 45, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_TL_RIGHT), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Zoom-", WS_CHILD | WS_VISIBLE,
                    316, 176, 70, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM_OUT), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Zoom+", WS_CHILD | WS_VISIBLE,
                    391, 176, 70, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM_IN), nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                    466, 176, 90, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CLOSE), nullptr, nullptr);
                CreateWindowW(L"STATIC", L"Timeline", WS_CHILD | WS_VISIBLE,
                    566, 182, 300, 20, hwnd, reinterpret_cast<HMENU>(IDC_LABEL_TL_INFO), nullptr, nullptr);

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
                } else if (id == IDC_BTN_SAVE) {
                    if (!applyCurrentEdit(editor, true)) return 0;
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
                    int width = std::max(kTimelineMinWidth, editor->timelineWidth);
                    uint64_t step = static_cast<uint64_t>((width / 8) * editor->framesPerCell / 2);
                    editor->timelineStartFrame = (editor->timelineStartFrame > step) ? editor->timelineStartFrame - step : 0;
                    refreshEditorUi(editor);
                } else if (id == IDC_BTN_TL_RIGHT) {
                    int width = std::max(kTimelineMinWidth, editor->timelineWidth);
                    uint64_t step = static_cast<uint64_t>((width / 8) * editor->framesPerCell / 2);
                    editor->timelineStartFrame += std::max<uint64_t>(step, 1);
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
                    editor->followPlayhead = !editor->followPlayhead;
                    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_CTRL_FOLLOW), editor->followPlayhead ? L"Auto-follow:On" : L"Auto-follow:Off");
                    refreshEditorUi(editor);
                }
                return 0;
            }

            case WM_TIMER:
                if (editor && wParam == 1) {
                    followLiveFrame(editor);
                    refreshEditorUi(editor);
                }
                return 0;

            case WM_MOUSEWHEEL: {
                if (!editor) return 0;
                POINT pt{ static_cast<LONG>(static_cast<short>(LOWORD(lParam))), static_cast<LONG>(static_cast<short>(HIWORD(lParam))) };
                ScreenToClient(hwnd, &pt);
                if (!isPointInTimeline(editor, pt.x, pt.y)) return 0;

                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                zoomTimelineAtX(editor, pt.x, delta > 0);
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

        std::thread(
            [levelId, replay = std::move(replay), sourcePath = std::move(sourcePath)]() mutable {
                runExternalReplayEditorWindow(levelId, std::move(replay), std::move(sourcePath));
            }
        ).detach();

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

        // Reliable placement independent of internal LevelInfoLayer menu IDs.
        auto replayMenu = CCMenu::create();
        replayMenu->setID("watch-replay-menu"_spr);
        replayMenu->setPosition(CCPointZero);
        this->addChild(replayMenu, 200);

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        watchButton->setPosition({winSize.width - 64.0f, 86.0f});
        editButton->setPosition({winSize.width - 154.0f, 86.0f});
        replayMenu->addChild(watchButton);
        replayMenu->addChild(editButton);

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
        if (!replayResult.has_value()) {
            FLAlertLayer::create("Error", "No replay found for this level.", "OK")->show();
            return;
        }

#ifdef GEODE_IS_WINDOWS
        // Launch level immediately and keep game active while external editor is open.
        g_replayPlayer.isActive = true;
        g_replayPlayer.replay = replayResult.value();
        g_replayPlayer.playbackSpeed = 1.0f;
        g_replayPlayer.isPaused = false;
        g_replayPlayer.pauseTime = 0.0f;
        g_replayPlayer.inputIndex = 0.0f;
        g_replayPlayer.isEditMode = false;
        g_replayPlayer.levelId = levelId;
        g_replayPlayer.sourceReplayPath = g_lastMatchedLocalReplayPath.value_or(std::filesystem::path{});
        this->onPlay(nullptr);

        auto source = g_lastMatchedLocalReplayPath.value_or(std::filesystem::path{});
        bool opened = openExternalReplayEditorWindow(levelId, std::move(replayResult.value()), std::move(source));
        if (opened) {
            FLAlertLayer::create("Replay Editor", "Level launched. Click timeline frames to toggle press/release.", "OK")->show();
        }
#else
        FLAlertLayer::create("Replay Editor", "External editor is only available on Windows.", "OK")->show();
#endif
    }
};

// ============================================================

