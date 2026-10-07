#include "App.hpp"
#include "AppInternal.hpp"
#include "DarkMode.hpp"
#include "Diagnostics.hpp"
#include "FilePersistence.hpp"
#include "resource.h"

#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <utility>

namespace quaddeck {

using namespace app_internal;

namespace {
unsigned windowDpi(HWND window);
int resizeFrameThickness(unsigned dpi);
void leaveAutoHideTaskbarEdges(HWND window, RECT& client);
}  // namespace

int App::run(HINSTANCE instance, int showCommand, const std::vector<std::wstring>& initialFiles,
             SingleInstance* singleInstance) {
    instance_ = instance;
    singleInstance_ = singleInstance;
    externalOpenReady_ = false;
    if (!initializeWindow(instance, showCommand)) {
        if (singleInstance_) singleInstance_->stopAccepting();
        singleInstance_ = nullptr;
        return 1;
    }
    // From here every frame beats; renderFrame also runs inside menus,
    // dialogs and window drags, so only a window thread that is waiting on
    // something goes silent.
    hangWatchdog_.start(GetCurrentThreadId(), [](const std::string& report) { appendDiagnostic(report); });
    loadAppSettings();
    embyLoadAuth();
    if (!initialFiles.empty()) {
        if (initialFiles.size() == 1 &&
            _wcsicmp(std::filesystem::path(initialFiles[0]).extension().c_str(), L".qdeck") == 0) {
            if (!loadSession(initialFiles[0])) {
                MessageBoxW(window_, L"Could not open this QuadDeck session.", L"Session error", MB_ICONERROR);
            }
        } else {
            enqueueExternalFiles(initialFiles, {}, true);
        }
    }
    if (singleInstance_) singleInstance_->setWindow(window_);
    externalOpenReady_ = true;
    // Frames are paced by the swap chain rather than by WM_TIMER. A timer
    // message is low priority, coalesces, and cannot resolve finer than the
    // ~15.6ms system tick, so it can never line up with a 60Hz refresh; the
    // frame-latency object signals exactly when the swap chain wants another
    // frame. Waiting on it here, outside the device lock, is what lets the
    // renderer present with vsync without stalling the decoders.
    MSG message{};
    bool running = true;
    while (running) {
        const HANDLE waitable = renderer_.frameLatencyWaitableObject();
        const DWORD handleCount = waitable ? 1u : 0u;
        const auto waitStarted = std::chrono::steady_clock::now();
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            handleCount, &waitable, kFrameWaitTimeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        // Idle time is headroom: when it collapses, the frame loop has stopped
        // keeping up with the display rather than waiting on it.
        frameWaitMs_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStarted).count();
        // WAIT_TIMEOUT also renders: without a waitable swap chain it is the
        // only pacing available, and with one it keeps the clock, the seek
        // barrier and the audio handoff advancing if presentation ever stalls.
        if ((handleCount > 0 && waitResult == WAIT_OBJECT_0) || waitResult == WAIT_TIMEOUT) {
            renderFrame();
        }
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            // Route these application-wide gestures here so they work
            // whichever HWND has focus.
            if (message.message == WM_SYSKEYDOWN && message.wParam == VK_RETURN &&
                (message.lParam & (1LL << 29)) && !(message.lParam & (1LL << 30))) {
                toggleFullscreen();
                continue;
            }
            if (message.message == WM_MOUSEWHEEL) {
                // The wheel scrolls the settings sheet while the pointer is on
                // it and is the volume control everywhere else on the player.
                POINT local = message.pt;
                ScreenToClient(videoWindow_, &local);
                const PanelHit hit = panelHitAt(local);
                if (hit.kind != PanelHitKind::None && hit.kind != PanelHitKind::Outside) {
                    scrollSettingsPanel(GET_WHEEL_DELTA_WPARAM(message.wParam));
                } else {
                    adjustVolumeFromWheel(GET_WHEEL_DELTA_WPARAM(message.wParam));
                }
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    hangWatchdog_.stop();
    externalOpenReady_ = false;
    if (singleInstance_) {
        singleInstance_->setWindow(nullptr);
        singleInstance_->stopAccepting();
        singleInstance_ = nullptr;
    }
    return static_cast<int>(message.wParam);
}

void App::renderFrame() {
    lastRenderTick_ = GetTickCount64();
    hangWatchdog_.beat();
    tick();
    updateAutoHideControls();
    updateDockAnimation();
}

bool App::initializeWindow(HINSTANCE instance, int showCommand) {
    // Before any window or menu exists: the mode is read when a menu is created.
    enableDarkPopupMenus();
    HICON appIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_QUADDECK));
    if (!appIcon) appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &App::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = appIcon;
    // Let Windows select the small frame from hIcon's resource group.
    // Reusing appIcon here forces the large bitmap into the small slot.
    windowClass.hIconSm = nullptr;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    WNDCLASSEXW videoClass{};
    videoClass.cbSize = sizeof(videoClass);
    videoClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    videoClass.lpfnWndProc = &App::videoWindowProc;
    videoClass.hInstance = instance;
    videoClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    videoClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoClass.lpszClassName = kVideoWindowClass;
    if (!RegisterClassExW(&videoClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    // Created hidden on purpose: WS_VISIBLE paints the frame before the dark
    // attribute can be applied, and the light title bar it draws then survives
    // the change -- which is why the window wore a light frame over a black
    // client area.
    // WM_CREATE builds the controls, so the scale has to be known before
    // the window exists; the system DPI is the primary monitor's.
    uiScale_ = static_cast<float>(windowDpi(nullptr)) / 96.0F;
    window_ = CreateWindowExW(
        0, kWindowClass, L"QuadDeck",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, dp(1440), dp(900),
        nullptr, nullptr, instance, this);
    if (!window_) {
        return false;
    }
    // Have the frame worked out again through WM_NCCALCSIZE, so Windows and
    // DWM drop the caption before the window is first shown.
    SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const BOOL darkTitle = TRUE;
    const DWORD darkTitleAttribute = 20;
    DwmSetWindowAttribute(window_, darkTitleAttribute, &darkTitle, sizeof(darkTitle));
    // A secondary monitor with its own DPI: re-derive before the first paint.
    if (windowDpi(window_) != static_cast<unsigned>(std::lround(uiScale_ * 96.0F))) {
        applyDpi(windowDpi(window_));
    }
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

LRESULT CALLBACK App::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::videoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->videoWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleVideoMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCALCSIZE: {
        // No title bar. Windows keeps its own frame on the sides and the
        // bottom -- the resize borders outside the picture, the shadow,
        // snapping -- and the client reaches the window's top edge, where
        // the caption is drawn over the video instead. Both forms of the
        // message: CreateWindowExW sends the one with a bare RECT, and a
        // window that never changes size afterwards keeps what it said.
        RECT* client = wParam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
                              : reinterpret_cast<RECT*>(lParam);
        const LONG top = client->top;
        const LRESULT result = DefWindowProcW(window_, message, wParam, lParam);
        if (result != 0) return result;
        client->top = top;
        const auto style = GetWindowLongPtrW(window_, GWL_STYLE);
        if ((style & WS_CAPTION) == WS_CAPTION && IsZoomed(window_)) {
            // A maximized window hangs its frame off the monitor on every
            // side; the default pulled the other three in, not the top.
            client->top += resizeFrameThickness(windowDpi(window_));
            leaveAutoHideTaskbarEdges(window_, *client);
        }
        return 0;
    }
    case WM_NCHITTEST: {
        // The sides and the bottom are Windows' own frame; the top is ours.
        const LRESULT standard = DefWindowProcW(window_, message, wParam, lParam);
        if (standard != HTCLIENT) return standard;
        return frameHitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    }
    case WM_CREATE:
        DragAcceptFiles(window_, TRUE);
        videoWindow_ = CreateWindowExW(
            0, kVideoWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 16, 16, window_, nullptr, instance_, this);
        if (!videoWindow_) return -1;
        DragAcceptFiles(videoWindow_, TRUE);
        if (!renderer_.initialize(videoWindow_)) {
            MessageBoxA(window_, renderer_.error().c_str(), "D3D11 initialization failed", MB_ICONERROR);
            return -1;
        }
        audio_.initialize();
        createControls();
        controlsLastInteraction_ = GetTickCount64();
        pointerLastMoved_ = controlsLastInteraction_;
        GetCursorPos(&lastPointerScreen_);
        SetTimer(window_, kRenderTimer, 15, nullptr);
        return 0;
    case WM_DPICHANGED: {
        applyDpi(HIWORD(wParam));
        if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_SIZE:
        layoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == kSettingsSaveTimer) {
            flushScheduledAppSettingsSave();
            return 0;
        }
        // Popup menus, common dialogs and window drag/resize run their own
        // modal message loop, so the main loop stops pacing frames and video
        // would freeze for as long as a menu is open. This timer covers those
        // stretches and stands down as soon as the main loop is pumping again.
        if (wParam == kRenderTimer &&
            GetTickCount64() - lastRenderTick_ >= kModalRenderFallbackMs) {
            renderFrame();
        }
        return 0;
    case WM_DROPFILES:
        onDrop(reinterpret_cast<HDROP>(wParam), -1);
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wParam) {
        case VK_SPACE: togglePlayback(); break;
        case VK_LEFT: seekRelative(GetKeyState(VK_CONTROL) < 0 ? -30.0 : -5.0); break;
        case VK_RIGHT: seekRelative(GetKeyState(VK_CONTROL) < 0 ? 30.0 : 5.0); break;
        case VK_HOME:
        case 'R':
            if (perPaneTimelines()) {
                const auto pane = embyPlaybackTarget();
                if (embyPanes_[pane]) {
                    embyPanes_[pane]->fromBeginning = true;
                    embyPanes_[pane]->resumeTicks = 0;
                }
                seekPaneTo(pane, 0.0);
            } else {
                seekAbsolute(0.0);
            }
            break;
        case VK_ESCAPE:
            if (settingsOpen_) closeSettingsPanel();
            else if (soloPane_ >= 0) { soloPane_ = -1; layoutHoverControls(); }
            else if (fullscreen_) toggleFullscreen();
            break;
        case VK_F5: toggleSettingsPanel(); break;
        case VK_F6: toggleEmbyBrowser(); break;
        case 'E': if (ctrl) embyOpenBrowser(); break;
        case VK_PRIOR: openAdjacentFile(-1); break;
        case VK_NEXT: openAdjacentFile(1); break;
        case 'O':
            if (GetKeyState(VK_CONTROL) < 0) loadSessionDialog();
            else openFilesDialog();
            break;
        case 'S': if (GetKeyState(VK_CONTROL) < 0) saveSessionDialog(); break;
        case 'L':
        case VK_OEM_5:  // backslash, PotPlayer's A-B switch
            toggleMasterLoop();
            break;
        case VK_OEM_4:
            setMasterLoopA(clock_.position());
            break;
        case VK_OEM_6:
            setMasterLoopB(clock_.position());
            break;
        case 'M': toggleMute(); break;
        case 'U': toggleControls(); break;
        case '0': resetAllOffsets(); break;
        case '1': if (!ctrl) toggleAudioPane(paneForPosition(0)); break;
        case '2': if (!ctrl) toggleAudioPane(paneForPosition(1)); break;
        case '3': if (!ctrl) toggleAudioPane(paneForPosition(2)); break;
        // PotPlayer's playlist sorts on its keys (Ctrl+5 is "by extension"
        // there, which a library has no use for).
        case '4': if (ctrl) sortListBy(static_cast<int>(emby::SortKey::Name)); else toggleAudioPane(paneForPosition(3)); break;
        case '5': if (!ctrl) toggleAudioPane(paneForPosition(4)); break;
        case '6': if (ctrl) sortListBy(static_cast<int>(emby::SortKey::Size)); break;
        case '7': if (ctrl) sortListBy(static_cast<int>(emby::SortKey::DateAdded)); break;
        case '8': if (ctrl) sortListBy(static_cast<int>(emby::SortKey::Runtime)); break;
        case '9': if (ctrl) sortListBy(static_cast<int>(emby::SortKey::Random)); break;
        // PotPlayer's subtitle timing: . shows the lines earlier, , later,
        // / as their file says.
        case VK_OEM_PERIOD:
            if (const int pane = subtitlePane(); pane >= 0) nudgeSubtitleDelay(static_cast<std::size_t>(pane), -0.5);
            break;
        case VK_OEM_COMMA:
            if (const int pane = subtitlePane(); pane >= 0) nudgeSubtitleDelay(static_cast<std::size_t>(pane), 0.5);
            break;
        case VK_OEM_2:
            if (const int pane = subtitlePane(); pane >= 0) resetSubtitleDelay(static_cast<std::size_t>(pane));
            break;
        default: break;
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (lParam & (1LL << 29)) && !(lParam & (1LL << 30))) {
            toggleFullscreen();
            return 0;
        }
        // PotPlayer's subtitle keys are Alt keys: H, L, O, the size and the
        // place. Bit 29 says Alt is down with the key.
        if ((lParam & (1LL << 29)) && handleAltKey(wParam)) {
            // A letter's character message follows; whatever the keyboard
            // layout makes of the letter, it is this key's.
            altCharTaken_ = wParam >= 'A' && wParam <= 'Z';
            return 0;
        }
        altCharTaken_ = false;
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_SYSCHAR:
        // The character of an Alt key taken above: there is no menu bar to
        // look it up in, and the default would only beep.
        if (altCharTaken_) {
            altCharTaken_ = false;
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_SYSCOMMAND:
        // Alt let go on its own asks for the menu bar, and the window has
        // none: the next key would go to an invisible system menu instead
        // of the player. The default handler cannot tell an Alt that was
        // part of a subtitle key -- those never reach it -- from one
        // pressed alone. Alt+Space still opens the system menu: that
        // request carries the space.
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) return 0;
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_MOUSEWHEEL:
        adjustVolumeFromWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lParam)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE:
            togglePlayback();
            return TRUE;
        case APPCOMMAND_MEDIA_PLAY:
            if (!playbackIntended()) togglePlayback();
            return TRUE;
        case APPCOMMAND_MEDIA_PAUSE:
            if (playbackIntended()) togglePlayback();
            return TRUE;
        case APPCOMMAND_MEDIA_STOP:
            stopPlayback();
            return TRUE;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_ACTIVATE:
        // Back from another program -- the server's own client, where
        // something may just have been watched or changed.
        if (LOWORD(wParam) != WA_INACTIVE) embyWantRefresh();
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        externalOpenReady_ = false;
        if (singleInstance_) {
            singleInstance_->setWindow(nullptr);
            singleInstance_->stopAccepting();
        }
        externalOpenBatches_.clear();
        KillTimer(window_, kRenderTimer);
        KillTimer(window_, kSettingsSaveTimer);
        settingsSavePending_ = false;
        settingsSaveRetryCount_ = 0;
        if (!saveAppSettings()) appendDiagnostic("Could not save application settings");
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) embyReport(pane, "stopped");
        emby_.drain(1500);
        for (auto& source : sources_) {
            if (source) source->setAudioEnabled(false);
        }
        audio_.flush();
        for (auto& source : sources_) {
            source.reset();
        }
        if (cursorHidden_) { ShowCursor(TRUE); cursorHidden_ = false; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

namespace {

// The resize border Windows gives a window at this DPI, the band at the top
// of the client that resizes it, and what a maximized window hangs off the
// monitor.
int resizeFrameThickness(unsigned dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const auto forDpi = [] {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetSystemMetricsForDpiFn>(
                            GetProcAddress(user32, "GetSystemMetricsForDpi"))
                      : nullptr;
    }();
    if (forDpi && dpi > 0) {
        return forDpi(SM_CYSIZEFRAME, dpi) + forDpi(SM_CXPADDEDBORDER, dpi);
    }
    return GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

// An auto-hide taskbar comes up when the pointer reaches its edge of the
// monitor. A maximized window reaching that edge too would keep it hidden
// for good, so the client stops two pixels short of it.
void leaveAutoHideTaskbarEdges(HWND window, RECT& client) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    constexpr LONG kReveal = 2;
    for (const UINT edge : {ABE_TOP, ABE_LEFT, ABE_BOTTOM, ABE_RIGHT}) {
        APPBARDATA bar{sizeof(bar)};
        bar.uEdge = edge;
        bar.rc = monitor.rcMonitor;
        if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) continue;
        switch (edge) {
        case ABE_TOP: if (client.top <= monitor.rcMonitor.top) client.top += kReveal; break;
        case ABE_LEFT: if (client.left <= monitor.rcMonitor.left) client.left += kReveal; break;
        case ABE_BOTTOM: if (client.bottom >= monitor.rcMonitor.bottom) client.bottom -= kReveal; break;
        case ABE_RIGHT: if (client.right >= monitor.rcMonitor.right) client.right -= kReveal; break;
        default: break;
        }
    }
}

// Per-monitor DPI without a minimum SDK: both calls exist since Windows 10
// 1607, and a build without them reports 96, which keeps the 1x layout.
unsigned windowDpi(HWND window) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    using GetDpiForSystemFn = UINT(WINAPI*)();
    if (user32 && window) {
        if (const auto fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(user32, "GetDpiForWindow"))) {
            if (const UINT dpi = fn(window)) return dpi;
        }
    }
    if (user32) {
        if (const auto fn = reinterpret_cast<GetDpiForSystemFn>(
                GetProcAddress(user32, "GetDpiForSystem"))) {
            if (const UINT dpi = fn()) return dpi;
        }
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<unsigned>(dpi) : 96U;
}

}  // namespace

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif


void App::onDrop(HDROP drop, int targetPane, int pointerPane) {
    const UINT count = std::min<UINT>(
        static_cast<UINT>(kMaxPanes), DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0));
    std::vector<std::wstring> files;
    // A subtitle file is not another video: dropped on a video it becomes
    // that video's subtitles.
    std::wstring subtitle;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        if (isSubtitleFile(path)) {
            if (subtitle.empty()) subtitle = std::move(path);
        } else {
            files.push_back(std::move(path));
        }
    }
    DragFinish(drop);
    if (!files.empty()) {
        // Dropped with its video, a subtitle of the same name is found
        // beside it as any other would be.
        addFiles(files, targetPane);
    } else if (!subtitle.empty()) {
        const int pane = pointerPane >= 0 && paneLogicallyLoaded(static_cast<std::size_t>(pointerPane))
            ? pointerPane : subtitlePane();
        if (pane >= 0) addSubtitleFile(static_cast<std::size_t>(pane), subtitle);
        else showNotice(L"Open a video first, then drop its subtitles on it");
    }
}

// Periodic performance report.
//
// The question this exists to answer is which of three things limits playback:
// the decoders, the per-frame format conversion, or the single mutex the
// decoders and the renderer share. Guessing between them from the outside is
// how the seek stall was misdiagnosed twice, so each is measured separately.
//
// Reading the numbers: presented/s well below decoded/s means frames are being
// produced and thrown away, so the renderer is the limit. decoded/s below the
// source frame rate with the queue sitting empty means the decoder is the
// limit. A queue sitting at capacity means the decoder has headroom and is
// being throttled on purpose. Large lockwait means the renderer spends its
// time queued behind decoders rather than working, which points at the shared
// context rather than at either party.
void App::reportPerformance() {
    const auto now = std::chrono::steady_clock::now();
    if (lastPerfReport_.time_since_epoch().count() == 0) {
        lastPerfReport_ = now;
        renderer_.takeStats();
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - lastPerfReport_).count();
    if (elapsed < kPerformanceReportSeconds || !clock_.isPlaying()) return;
    lastPerfReport_ = now;

    const auto stats = renderer_.takeStats();
    const auto frames = static_cast<double>(std::max<std::uint64_t>(1, stats.frames));
    const double waited = frameWaitMs_;
    frameWaitMs_ = 0.0;

    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2)
            << "Perf window=" << elapsed << "s presented=" << stats.frames / elapsed
            << "/s idle=" << waited / frames
            << "ms lockwait=" << stats.lockWaitMs / frames
            << "ms convert=" << stats.convertMs / frames
            << "ms draw=" << stats.drawMs / frames
            << "ms present=" << stats.presentMs / frames
            << "ms gpuwait=" << stats.gpuWaitMs / frames
            << "ms held=" << stats.lockHeldMs / frames << "ms";
    appendDiagnostic(summary.str());

    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane] || !sources_[pane]->ready()) continue;
        const std::uint64_t decoded = sources_[pane]->videoFramesDecoded();
        const std::uint64_t waits = sources_[pane]->videoCapacityWaits();
        // A source replaced since the last report starts its counts over;
        // the old figures must not turn the difference into nonsense.
        const std::uint64_t decodedDelta = decoded >= lastDecodedFrames_[pane] ? decoded - lastDecodedFrames_[pane] : decoded;
        const std::uint64_t waitsDelta = waits >= lastCapacityWaits_[pane] ? waits - lastCapacityWaits_[pane] : waits;
        std::ostringstream line;
        line << std::fixed << std::setprecision(2)
             << "Perf pane=" << (pane + 1)
             << " decoded=" << static_cast<double>(decodedDelta) / elapsed
             << "/s shown=" << (presentedFrames_[pane]) / elapsed
             << "/s queue=" << sources_[pane]->queuedVideoFrames()
             << "/" << sources_[pane]->videoQueueCapacity()
             << " throttled=" << static_cast<double>(waitsDelta) / elapsed << "/s";
        const auto cache = sources_[pane]->cacheStats();
        if (cache.enabled) {
            line << " cache_mb=" << cache.residentBytes / (1024.0 * 1024)
                 << " ahead_s=" << cache.aheadSeconds << " hits=" << cache.hits
                 << " misses=" << cache.misses << " retries=" << cache.retries;
        }
        appendDiagnostic(line.str());
        lastDecodedFrames_[pane] = decoded;
        lastCapacityWaits_[pane] = waits;
        presentedFrames_[pane] = 0;
    }
}

// A driver reset, a GPU hang or a TDR invalidates the device and everything
// built on it. Rebuilding the renderer alone is not enough: FFmpeg's D3D11VA
// contexts and every decoded surface belong to the dead device too, so the
// sources have to be released before it and reopened after.
void App::recoverLostDevice() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastDeviceRecovery_ < std::chrono::seconds(1)) return;
    lastDeviceRecovery_ = now;
    if (deviceRecoveryAttempts_ >= kMaximumDeviceRecoveries) return;
    ++deviceRecoveryAttempts_;
    appendDiagnostic("Device lost; rebuilding renderer and sources, attempt " +
                     std::to_string(deviceRecoveryAttempts_));

    if (!deviceRecoveryPending_) {
        // A failed first rebuild leaves the renderer latched as lost and will
        // re-enter here. Preserve the state from before that first attempt;
        // sampling the already parked clock on a retry would lose the user's
        // playing intent and make a successful recovery look like a pause.
        if (const int reference = linkedBrowserReferencePane(); reference >= 0) {
            deviceRecoveryBrowserPosition_ = currentSourceTime(static_cast<std::size_t>(reference));
        } else {
            deviceRecoveryBrowserPosition_ = clock_.position();
        }
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            deviceRecoverySourceDurations_[pane] = sources_[pane] ? sources_[pane]->duration() : 0.0;
        }
        deviceRecoveryPending_ = true;
        deviceRecoveryPosition_ = clock_.position();
        deviceRecoveryResume_ =
            clock_.isPlaying() || (seekBarrier_.active() && seekBarrier_.resume());
        cancelSeekBarrier();
        clock_.pause();
        clock_.setSlew(1.0);
        audio_.pause();
        for (auto& source : sources_) {
            if (source) source->setAudioEnabled(false);
        }
        audio_.flush();
        for (auto& source : sources_) source.reset();
        sourceInitialAlignmentPending_.fill(false);
        sourceProvisionalTargets_.fill(0.0);
    }

    if (!renderer_.recover(videoWindow_)) {
        appendDiagnostic("Device recovery failed: " + renderer_.error());
        if (deviceRecoveryAttempts_ >= kMaximumDeviceRecoveries) {
            appendDiagnostic("Device recovery stopped after " +
                             std::to_string(kMaximumDeviceRecoveries) +
                             " failed attempts");
        }
        updateTitle();
        return;
    }
    const double position = deviceRecoveryPosition_;
    const bool wasPlaying = deviceRecoveryResume_;
    deviceRecoveryPending_ = false;
    deviceRecoveryResume_ = false;
    deviceRecoveryPosition_ = 0.0;
    deviceRecoveryAttempts_ = 0;

    // The compiled shader and its constant buffers went with the old device.
    if (customShaderPath_.empty() || !renderer_.loadPixelShader(customShaderPath_)) {
        applyShaderPreset(shaderPreset_);
    }
    renderer_.setSmartVibranceSettings(smartVibrance_);
    if (!overlay_.initialize(renderer_.device())) {
        appendDiagnostic("Direct2D overlay unavailable after device recovery");
    }

    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        if (!paths_[pane].empty()) openSource(pane, paths_[pane], false);
    }
    seekAbsolute(position);
    if (wasPlaying) togglePlayback();
    updateTitle();
    updateControls();
    appendDiagnostic("Device recovery complete");
}

// Trim the master clock so it follows the audio hardware rather than
// steady_clock. The first usable selected pane is the reference, but every
// selected voice still has its playback timeline advanced here. Otherwise the
// non-reference voices in a multi-audio mix would retain one bookkeeping span
// per submitted buffer for the entire playback session.
void App::updateAudioDriftCorrection(double timelinePosition) {
    if (seekBarrier_.active() || !clock_.isPlaying()) {
        clock_.setSlew(1.0);
        return;
    }
    bool haveReference = false;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!audioPaneEnabled(pane)) continue;
        const double audioSeconds = audio_.playbackPosition(pane);
        const bool queuedAudio = audio_.hasQueuedAudio(pane);
        if (haveReference || !sources_[pane] || !sources_[pane]->ready() ||
            sourcePaused_[pane] || !sources_[pane]->hasAudioStream() ||
            !queuedAudio || audioSeconds < 0.0 ||
            !shouldOutputAudio(pane, timelinePosition, true)) continue;
        const double expected = mappedSourceTime(pane, timelinePosition);
        const auto correction =
            audioDriftSlew(audioSeconds, expected, playbackRates_[pane]);
        clock_.setSlew(correction.factor);
        haveReference = true;

        // Drift is only observable over long playback, so leave a trail that
        // makes it checkable after the fact instead of by eye.
        const auto now = std::chrono::steady_clock::now();
        if (correction.corrected &&
            now - lastDriftReport_ >= std::chrono::seconds(30)) {
            lastDriftReport_ = now;
            std::ostringstream message;
            message << "Audio drift pane=" << (pane + 1) << std::fixed
                    << std::setprecision(4)
                    << " audio=" << audioSeconds << " clock=" << expected
                    << " error_ms=" << (audioSeconds - expected) * 1000.0
                    << " slew=" << correction.factor;
            appendDiagnostic(message.str());
        }
    }
    if (!haveReference) clock_.setSlew(1.0);
}

// Software decoding scales frames on the CPU before upload, so each decoder
// needs to know how large its pane currently is. The hardware path derives the
// same figure inside the renderer, where the cell geometry already lives.
void App::publishPreferredSizes(const PaneArray<float>& aspects) {
    RECT client{};
    if (!videoWindow_ || !GetClientRect(videoWindow_, &client)) return;
    const float width = static_cast<float>(std::max(1L, client.right));
    const float height = static_cast<float>(std::min<LONG>(
        std::max(1L, client.bottom), std::max(1U, contentHeight_)));
    const auto cells = currentLayoutCells(width, height, aspects);
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane] || cells[pane].width <= 0 || cells[pane].height <= 0) continue;
        const float zoom = std::clamp(paneViews_[pane].zoom, 1.0F, 4.0F);
        sources_[pane]->setPreferredSize(
            static_cast<int>(std::lround(cells[pane].width * zoom)),
            static_cast<int>(std::lround(cells[pane].height * zoom)));
    }
}

void App::tick() {
    expireNotice(GetTickCount64());
    embyTick();
    mainQueue_->drain();
    if (externalOpenReady_) {
        drainExternalRequests();
        processExternalFiles();
    }
    adoptEmbeddedSubtitles();
    double readyDuration = 0.0;
    bool sourceOpening = false;
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        const auto& source = sources_[index];
        if (source && source->ready()) {
            readyDuration = std::max(readyDuration, timelineDuration(
                source->duration(), startDelays_[index], syncAdjustments_[index], playbackRates_[index]));
        } else if (paneOpening(index)) {
            sourceOpening = true;
        }
    }
    duration_ = stableTimelineDuration(duration_, readyDuration, sourceOpening);

    // openSource must issue a provisional seek before FFmpeg has discovered
    // the duration. Once that duration arrives, Independent auto-repeat may
    // map the same master position to a different local cycle. Correct it
    // exactly once, including while globally paused. A synchronized barrier
    // owns its recorded generations, so never replace just one of them:
    // rebuild the whole barrier as each duration-dependent mapping settles.
    // This can restart it a few times during a multi-file open, but prevents an
    // older raw-target generation from releasing the clock in between.
    const double alignmentTimeline = clock_.position();
    bool refreshOpeningBarrier = false;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sourceInitialAlignmentPending_[pane]) continue;
        const bool exists = sources_[pane] != nullptr;
        const bool ready = exists && sources_[pane]->ready();
        const bool failed = exists && !sources_[pane]->error().empty();
        const double provisional = sourceProvisionalTargets_[pane];
        const double mapped = ready && !failed
            ? mappedSourceTime(pane, alignmentTimeline) : provisional;
        const auto action = readyAlignmentAction(
            true, exists, ready, failed,
            seekBarrier_.active() && (!perPaneTimelines() || seekBarrier_.waiting(pane)),
            provisional, mapped);
        if (action == ReadyAlignmentAction::Wait) continue;
        if (action == ReadyAlignmentAction::Clear) {
            sourceInitialAlignmentPending_[pane] = false;
            if (ready && !failed) {
                sourceProvisionalTargets_[pane] = mapped;
                lastVideoMappedTimes_[pane] = mapped;
            }
            continue;
        }
        if (action == ReadyAlignmentAction::RefreshBarrier) {
            refreshOpeningBarrier = true;
            std::ostringstream message;
            message << "Source ready requires barrier refresh pane=" << (pane + 1)
                    << " provisional=" << std::fixed << std::setprecision(3)
                    << provisional << " final=" << mapped;
            appendDiagnostic(message.str());
            continue;
        }

        sourceInitialAlignmentPending_[pane] = false;
        sourceProvisionalTargets_[pane] = mapped;
        lastVideoMappedTimes_[pane] = mapped;
        sources_[pane]->setAudioEnabled(false);
        sources_[pane]->requestSeek(mapped);
        const auto audioStatus = sources_[pane]->audioDecodeStatus();
        audio_.flush(pane, audioStatus.generation);
        const bool useAudio = shouldOutputAudio(
            pane, alignmentTimeline, clock_.isPlaying());
        sources_[pane]->setAudioEnabled(useAudio);
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
        if (useAudio) audio_.play(pane);
        std::ostringstream message;
        message << "Source ready alignment pane=" << (pane + 1)
                << " provisional=" << std::fixed << std::setprecision(3)
                << provisional << " final=" << mapped;
        appendDiagnostic(message.str());
    }
    if (refreshOpeningBarrier && seekBarrier_.active()) {
        const double target = seekBarrier_.target();
        const bool resume = seekBarrier_.resume();
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (sourceInitialAlignmentPending_[pane] && sources_[pane] &&
                (sources_[pane]->ready() || !sources_[pane]->error().empty())) {
                sourceInitialAlignmentPending_[pane] = false;
            }
        }
        appendDiagnostic("Refreshing seek barrier after source metadata settled");
        beginSynchronizedSeek(target, false, resume);
    }

    bool hasRepeatingSource = false;
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        hasRepeatingSource = hasRepeatingSource ||
            (sources_[index] && sources_[index]->ready() &&
             sources_[index]->error().empty() &&
            (sourceLoopEnabled_[index] ||
             (activeSeekMode() == SeekMode::Independent && paneTiming(index).autoRepeat)));
    }
    // Emby sources own their EOF and queue independently. A shortest source
    // may finish or resolve its successor while every other pane keeps moving.
    finishEmbyPanes();
    finishLocalPanes();
    const auto endAction = perPaneTimelines() ? MasterTimelineEndAction::None
        : masterTimelineEndAction(
            clock_.position(), duration_, clock_.isPlaying(), sourceOpening,
            masterLoopEnabled_ && loopArmed(masterLoopA_, masterLoopB_, duration_),
            hasRepeatingSource, repeatAll_);
    if (endAction != MasterTimelineEndAction::None && finishSingleVideo()) {
        // The only video ended and the playback order said what comes next.
    } else if (endAction == MasterTimelineEndAction::Restart) {
        seekAbsolute(0.0);
    } else if (endAction == MasterTimelineEndAction::Pause) {
        pausePlaybackAtEnd();
    }
    // A keyframe seek landed near its target, not on it: the timeline
    // follows the picture, and the audio, re-aimed by the source, starts
    // over from there under its new generation.
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        if (!sources_[index]) continue;
        const auto landing = sources_[index]->takeSeekLanding();
        if (!landing) continue;
        if (perPaneTimelines()) {
            // A keyframe request made before an Emby pane was added may land
            // afterwards. Its decoder's result cannot move the new deck.
            syncAdjustments_[index] = landing->seconds -
                (clock_.position() - startDelays_[index]) * playbackRates_[index];
            if (sourcePaused_[index]) sourcePausedTimes_[index] = landing->seconds;
            if (embyPanes_[index]) embyPanes_[index]->endHandled = false;
            if (localPanes_[index]) localPanes_[index]->endHandled = false;
        } else {
            const double timeline = timelineForSourceTime(
                landing->seconds, startDelays_[index], syncAdjustments_[index], playbackRates_[index]);
            clock_.seek(timeline);
        }
        lastVideoMappedTimes_[index] = landing->seconds;
        audio_.flush(index, landing->audioGeneration);
        std::ostringstream message;
        message << "Seek landed on keyframe pane=" << (index + 1) << " at=" << std::fixed
                << std::setprecision(3) << landing->seconds;
        appendDiagnostic(message.str());
    }
    double position = clock_.position();
    bool playing = clock_.isPlaying();
    const double masterLoopEnd = loopEnd(masterLoopA_, masterLoopB_, duration_);
    if (playing && masterLoopEnabled_ && loopArmed(masterLoopA_, masterLoopB_, duration_) &&
        position >= masterLoopEnd) {
        seekAbsolute(loopedTime(position, masterLoopA_, masterLoopEnd, true));
        position = clock_.position();
        playing = clock_.isPlaying();
    }
    if (playing) selectNextUnfinishedAudio(position);
    updateAudioDriftCorrection(position);
    PaneArray<std::shared_ptr<const HardwareVideoFrame>> frames{};
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        if (sources_[index]) {
            const bool started = sourceIsActive(position, startDelays_[index],
                                                syncAdjustments_[index], playbackRates_[index]);
            if (started) {
                const double mapped = mappedSourceTime(index, position);
                if (playing && !sourcePaused_[index] && lastVideoMappedTimes_[index] >= 0.0 &&
                    mapped + 0.020 < lastVideoMappedTimes_[index]) {
                    sources_[index]->setAudioEnabled(false);
                    sources_[index]->requestSeek(mapped);
                    const auto audioStatus = sources_[index]->audioDecodeStatus();
                    audio_.flush(index, audioStatus.generation);
                    if (audioPaneEnabled(index)) {
                        audio_.setRate(index, static_cast<float>(playbackRates_[index]));
                    }
                }
                lastVideoMappedTimes_[index] = mapped;
                frames[index] = sources_[index]->frameForTime(
                    mapped, playing && !sourcePaused_[index]);
            }
            const bool primeSeekAudio = seekBarrier_.active() &&
                                        seekBarrier_.audioWaiting(index);
            const bool liveAudio = shouldOutputAudio(index, position, playing);
            sources_[index]->setAudioEnabled(primeSeekAudio || liveAudio);
            if (liveAudio) {
                audio_.setRate(index, static_cast<float>(playbackRates_[index]));
                audio_.play(index);
            }
        }
    }
    bool completeBarrierAfterRender = false;
    if (seekBarrier_.active()) {
        const auto elapsed = seekBarrierElapsedMs();
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (!sources_[pane]) continue;
            const auto event = seekBarrier_.observeFrame(
                pane, frames[pane] != nullptr,
                frames[pane] ? frames[pane]->seekGeneration : 0,
                frames[pane] && frames[pane]->exactSeekFrame);
            if (event.firstFrame) {
                std::ostringstream message;
                message << "Seek pane=" << (pane + 1) << " target=" << std::fixed
                        << std::setprecision(3) << seekBarrier_.paneTarget(pane)
                        << " first_frame_ms=" << elapsed;
                appendDiagnostic(message.str());
            }
            if (event.exactFrame) {
                std::ostringstream message;
                message << "Seek pane=" << (pane + 1) << " target=" << std::fixed
                        << std::setprecision(3) << seekBarrier_.paneTarget(pane)
                        << " exact_frame_ms=" << elapsed;
                appendDiagnostic(message.str());
            }
        }
        seekBarrier_.noteElapsed(elapsed);
        completeBarrierAfterRender = seekBarrier_.ready(elapsed, paneStatuses());
    }
    for (std::size_t index = 0; index < frames.size(); ++index) {
        // Only a change of serial is a frame the viewer actually saw; anything
        // else is the same picture presented again.
        if (!frames[index] || frames[index]->serial == lastShownSerial_[index]) continue;
        lastShownSerial_[index] = frames[index]->serial;
        ++presentedFrames_[index];
    }
    const auto aspects = paneAspectRatios();
    refreshAutoLayoutFocus(aspects);
    if (aspects != lastLayoutAspects_) {
        lastLayoutAspects_ = aspects;
        layoutHoverControls();
    }
    publishPreferredSizes(aspects);
    buildOverlayScene();
    renderer_.render(frames, paneViews_, activePanes(), layoutMode_,
                     expandedPane_, soloPane_, contentHeight_, aspects,
                     autoLayoutFocus_, autoLayoutFocusPane_);
    if (renderer_.deviceLost()) {
        recoverLostDevice();
        return;
    }
    if (completeBarrierAfterRender) completeSeekBarrier();
    reportPerformance();
    updateHoverControls();
    static unsigned controlsCounter = 0;
    if (++controlsCounter % 6 == 0) updateControls();
    static unsigned titleCounter = 0;
    if (++titleCounter % 20 == 0) {
        updateTitle();
    }
}

void App::updateTitle() {
    // The title used to carry per-source frame counters, offsets, rates and a
    // list of keyboard shortcuts. Diagnostics belong in QuadDeck.log, which now
    // records all of it, and the shortcuts are in the popup menu and the README;
    // what a title bar is for is saying which files these are.
    std::wostringstream title;
    title << L"QuadDeck";

    std::wstring names;
    int loaded = 0;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (paths_[pane].empty()) continue;
        ++loaded;
        if (!names.empty()) names += L"  ·  ";
        names += embyPanes_[pane] && !embyPanes_[pane]->title.empty()
            ? embyPanes_[pane]->title
            : std::filesystem::path(paths_[pane]).stem().wstring();
    }

    if (loaded > 0) {
        bool buffering = false;
        for (const auto& source : sources_) {
            if (source && source->cacheStats().priming) buffering = true;
        }
        const wchar_t* state = deviceRecoveryPending_ ? L"Recovering"
                             : buffering ? L"Buffering NAS"
                             : seekBarrier_.active() ? L"Seeking"
                             : playbackIntended() ? L"Playing" : L"Paused";
        title << L" — " << state;
        if (bottomTimelineVisible()) {
            // The same time as the bar: the only F6 or Emby video's own.
            const auto shown = barTime();
            title << L"  " << formatTime(shown.position);
            if (shown.duration > 0.0) title << L" / " << formatTime(shown.duration);
        }
        title << L" — " << names;
    }

    // Failures still have to reach the user, and this is the only surface that
    // is always visible.
    std::wstring problems;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane]) continue;
        const std::string error = sources_[pane]->error();
        if (error.empty()) continue;
        if (!problems.empty()) problems += L"; ";
        problems += L"V" + std::to_wstring(positionForPane(pane) + 1) + L": " +
                    utf8ToWide(error);
    }
    const std::string rendererError = renderer_.error();
    if (!rendererError.empty()) {
        if (!problems.empty()) problems += L"; ";
        problems += L"GPU: " + utf8ToWide(rendererError);
    }
    if (!problems.empty()) title << L"  ⚠ " << problems;

    windowTitle_ = title.str();
    SetWindowTextW(window_, windowTitle_.c_str());
}

LRESULT App::frameHitTest(POINT screen) const {
    if (!window_ || !captionShown()) return HTCLIENT;
    POINT local = screen;
    ScreenToClient(window_, &local);
    RECT client{};
    GetClientRect(window_, &client);
    const bool resizable = !IsZoomed(window_) &&
                           (GetWindowLongPtrW(window_, GWL_STYLE) & WS_THICKFRAME) != 0;
    const float band = resizable
        ? static_cast<float>(resizeFrameThickness(windowDpi(window_))) : 0.0F;
    switch (windowFrameHitTest(static_cast<float>(local.x), static_cast<float>(local.y),
                               static_cast<float>(client.right), band, 16.0F * uiScale_,
                               captionLayout_, captionAlpha() > 0.05F)) {
    case WindowFrameHit::Caption: return HTCAPTION;
    case WindowFrameHit::Top: return HTTOP;
    case WindowFrameHit::TopLeft: return HTTOPLEFT;
    case WindowFrameHit::TopRight: return HTTOPRIGHT;
    default: return HTCLIENT;
    }
}

void App::toggleFullscreen() {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    if (!fullscreen_) {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetWindowPlacement(window_, &previousPlacement_) &&
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            SetWindowLongPtrW(window_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window_, HWND_TOP,
                monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left,
                monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = true;
            RECT client{}; GetClientRect(window_, &client); layoutControls(client.right, client.bottom);
        }
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window_, &previousPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
        RECT client{}; GetClientRect(window_, &client); layoutControls(client.right, client.bottom);
    }
}

}  // namespace quaddeck
