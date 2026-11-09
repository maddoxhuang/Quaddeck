// App: the window that asks to confirm deleting a video from the Emby
// server (AppEmbyDelete.cpp asks for it once the server has named the
// files). A window of its own, owned by the player's so it stays above it
// and goes with it, a message box's size in the settings window's colours
// (DeleteDialog.hpp lays it out, Overlay::drawDeleteDialog paints it): a
// caption strip with the question and a square close, the item and its
// type, the pane it plays in, the files on a box of their own, the warning,
// Cancel and Delete. It is as tall as what it says, up to a share of the
// screen, past which its list of files scrolls; it is moved by its caption
// and not sized. While it is up the player is disabled, as under a Windows
// dialog, and goes on playing; the wheel scrolls only this window. Esc, its
// close and Cancel give the deletion up; only Delete, pressed with the
// pointer, goes on with it -- no key does. With no renderer it is state
// alone: open or not, what it says, its layout and its hits, but no window.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace quaddeck {

using namespace app_internal;

namespace {

constexpr wchar_t kEmbyDeleteWindowClass[] = L"QuadDeckEmbyDeleteWindow";
// The share of the work area's height it grows to before its list scrolls.
constexpr float kEmbyDeleteWindowTallest = 0.8F;

OverlayRect overlayRect(const RECT& rect) {
    return {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
            static_cast<float>(rect.bottom - rect.top)};
}

}  // namespace

bool App::openEmbyDeleteWindow() {
    auto& dialog = embyDeleteWindow_;
    if (dialog.open) return true;
    // Another dialog holds the player already.
    if (window_ && !IsWindowEnabled(window_)) return false;
    // With a renderer it is a window or nothing: a player disabled with no
    // window to answer would be stuck.
    if (renderer_.device() && !createEmbyDeleteWindow()) return false;
    dialog.open = true;
    dialog.scroll = 0.0F;
    dialog.hot = DeleteDialogHit::None;
    dialog.pointerInside = false;
    embyDeleteWindowCancelPress();
    // A menu left open on the player goes; the player is held still behind
    // the window, its own press ended by Windows as it is disabled.
    closePanelMenu();
    if (window_) EnableWindow(window_, FALSE);
    if (const HWND window = dialog.window) {
        placeEmbyDeleteWindow();
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
    return true;
}

void App::closeEmbyDeleteWindow() {
    auto& dialog = embyDeleteWindow_;
    embyDeleteWindowCancelPress();
    dialog.pointerInside = false;
    dialog.hot = DeleteDialogHit::None;
    if (!dialog.open) return;
    dialog.open = false;
    // The player first, as a Windows dialog hands it back, so the keys go
    // to it and not to whatever window Windows would pick.
    if (window_) EnableWindow(window_, TRUE);
    const HWND window = dialog.window;
    if (!window || !IsWindowVisible(window)) return;
    const bool active = GetActiveWindow() == window;
    // Kept, hidden, for the next one.
    ShowWindow(window, SW_HIDE);
    if (active && window_) SetForegroundWindow(window_);
}

bool App::createEmbyDeleteWindow() {
    auto& dialog = embyDeleteWindow_;
    if (dialog.window) return true;
    // Painted on the renderer's device.
    if (!window_ || !renderer_.device()) return false;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &App::embyDeleteWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kEmbyDeleteWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        appendDiagnostic("Emby delete window class could not be registered");
        return false;
    }
    // Created hidden, sized and placed on opening (placeEmbyDeleteWindow);
    // a caption for Windows to move it by, though none is drawn
    // (WM_NCCALCSIZE), and no frame to size it with.
    const HWND window = CreateWindowExW(0, kEmbyDeleteWindowClass, L"Delete from Emby server",
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, 1, 1, window_, nullptr,
                                        instance_, this);
    if (!window) {
        appendDiagnostic("Emby delete window could not be created");
        return false;
    }
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const BOOL darkTitle = TRUE;
    const DWORD darkTitleAttribute = 20;
    DwmSetWindowAttribute(window, darkTitleAttribute, &darkTitle, sizeof(darkTitle));
    // Nothing in it is typed into.
    ImmAssociateContextEx(window, nullptr, 0);
    dialog.scale = static_cast<float>(windowDpi(window)) / 96.0F;
    return true;
}

// As wide as DeleteDialogMetrics says and as tall as what it says, up to a
// share of the work area; over the middle of the player, below its caption,
// as the settings window first stands.
void App::placeEmbyDeleteWindow() {
    auto& dialog = embyDeleteWindow_;
    const HWND window = dialog.window;
    if (!window || !window_) return;
    RECT frame{};
    GetWindowRect(window_, &frame);
    // Onto the player's monitor first, so that it is sized in that
    // monitor's DPI, which Windows would otherwise change under it.
    SetWindowPos(window, nullptr, frame.left + (frame.right - frame.left) / 2,
                 frame.top + (frame.bottom - frame.top) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    dialog.scale = static_cast<float>(windowDpi(window)) / 96.0F;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) monitor.rcWork = frame;
    const float width = std::round(DeleteDialogMetrics{}.width * dialog.scale);
    const float most = static_cast<float>(monitor.rcWork.bottom - monitor.rcWork.top) * kEmbyDeleteWindowTallest;
    // Laid out once in the tallest it may be: what it asks is then its
    // height, the text measured as it will be painted.
    float wanted = most;
    if (ID3D11Device* device = renderer_.device()) {
        const std::lock_guard<std::recursive_mutex> lock(renderer_.deviceMutex());
        if (dialog.overlay.ready() || dialog.overlay.initialize(device)) {
            refreshEmbyDeleteWindowGeometry(RECT{0, 0, std::lround(width), std::lround(most)});
            wanted = dialog.layout.wanted;
        }
    }
    const float height = std::round(std::clamp(wanted, 1.0F, most));
    // The frame Windows keeps round the client: its sides and its bottom.
    RECT outer{};
    RECT client{};
    GetWindowRect(window, &outer);
    GetClientRect(window, &client);
    const float extraX = static_cast<float>((outer.right - outer.left) - client.right);
    const float extraY = static_cast<float>((outer.bottom - outer.top) - client.bottom);
    const OverlayRect place = settingsWindowPlacement(overlayRect(frame), overlayRect(monitor.rcWork), width + extraX,
                                                      height + extraY, captionInset());
    SetWindowPos(window, nullptr, std::lround(place.x), std::lround(place.y), std::lround(place.width),
                 std::lround(place.height), SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK App::embyDeleteWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->embyDeleteWindow_.window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleEmbyDeleteWindowMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleEmbyDeleteWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    auto& dialog = embyDeleteWindow_;
    const HWND window = dialog.window;
    const auto local = [&] { return POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; };
    switch (message) {
    case WM_NCCALCSIZE: {
        // No title bar, as the player and the settings window have none:
        // Windows keeps its frame on the sides and the bottom, and the
        // window's own strip is the caption.
        RECT* client = wParam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
                              : reinterpret_cast<RECT*>(lParam);
        const LONG top = client->top;
        const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        if (result != 0) return result;
        client->top = top;
        return 0;
    }
    case WM_NCHITTEST: {
        const LRESULT standard = DefWindowProcW(window, message, wParam, lParam);
        if (standard != HTCLIENT || !window) return standard;
        // The caption strip outside its close moves it; no edge sizes it.
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        return embyDeleteWindowHitAt(point) == DeleteDialogHit::Caption ? HTCAPTION : HTCLIENT;
    }
    case WM_DPICHANGED: {
        // Its own monitor's, once moved to another.
        dialog.scale = static_cast<float>(HIWORD(wParam)) / 96.0F;
        if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) renderEmbyDeleteWindow();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        renderEmbyDeleteWindow();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (!dialog.trackingLeave) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            dialog.trackingLeave = TrackMouseEvent(&track) != FALSE;
        }
        embyDeleteWindowMove(local());
        return 0;
    case WM_MOUSELEAVE:
        dialog.trackingLeave = false;
        dialog.pointerInside = false;
        embyDeleteWindowHover();
        return 0;
    case WM_LBUTTONDOWN:
        embyDeleteWindowPress(local());
        return 0;
    case WM_LBUTTONUP:
        embyDeleteWindowRelease(local());
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window) embyDeleteWindowCancelPress();
        return 0;
    case WM_MOUSEWHEEL:
        // The main loop scrolls it wherever the wheel is sent; this is for
        // one that reaches the window another way.
        scrollEmbyDeleteWindow(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_KEYDOWN:
        // Esc gives it up; no key deletes, and the player's keys wait.
        if (wParam == VK_ESCAPE && !(lParam & (1LL << 30))) embyCancelDelete();
        return 0;
    case WM_CHAR:
        return 0;
    case WM_CLOSE:
        // Alt+F4 and the window's system menu, as its close.
        if (embyDeleteAsking()) embyCancelDelete();
        else closeEmbyDeleteWindow();
        return 0;
    case WM_NCDESTROY:
        // With the player's window: an owned window goes with its owner.
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        dialog.window = nullptr;
        dialog.trackingLeave = false;
        dialog.surface.release();
        if (dialog.open) {
            dialog.open = false;
            embyPendingDelete_.reset();
            if (window_ && IsWindow(window_)) EnableWindow(window_, TRUE);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Layout and painting

void App::refreshEmbyDeleteWindowGeometry(const RECT& client) {
    auto& dialog = embyDeleteWindow_;
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    dialog.content = embyDeleteDialogContent();
    dialog.overlay.measureDeleteDialog(dialog.content, width, dialog.scale, dialog.measures);
    dialog.layout = deleteDialogLayout(dialog.content, dialog.measures, width, height, dialog.scale, dialog.scroll);
    dialog.scroll = dialog.layout.listScroll;
}

void App::renderEmbyDeleteWindow() {
    auto& dialog = embyDeleteWindow_;
    const HWND window = dialog.window;
    if (!dialog.open || dialog.painting || !window || !IsWindowVisible(window) || IsIconic(window)) return;
    ID3D11Device* device = renderer_.device();
    if (!device || renderer_.deviceLost()) return;
    RECT client{};
    GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    dialog.painting = true;
    {
        const std::lock_guard<std::recursive_mutex> lock(renderer_.deviceMutex());
        if (dialog.overlay.ready() || dialog.overlay.initialize(device)) {
            refreshEmbyDeleteWindowGeometry(client);
            embyDeleteWindowHover();
            OverlayDeleteDialogScene& scene = dialog.scene;
            scene.width = static_cast<float>(client.right);
            scene.height = static_cast<float>(client.bottom);
            scene.scale = dialog.scale;
            scene.content = &dialog.content;
            scene.layout = &dialog.layout;
            scene.hot = dialog.hot;
            scene.pressed = dialog.pressed;
            scene.thumbDragging = dialog.thumbDrag;
            if (const auto surface = dialog.surface.backBuffer(device, window, static_cast<UINT>(client.right),
                                                               static_cast<UINT>(client.bottom))) {
                dialog.overlay.drawDeleteDialog(surface.Get(), scene);
                dialog.surface.present();
            }
        }
    }
    dialog.painting = false;
}

// ---------------------------------------------------------------------------
// The pointer and the wheel

DeleteDialogHit App::embyDeleteWindowHitAt(POINT local) const {
    const auto& dialog = embyDeleteWindow_;
    if (!dialog.open) return DeleteDialogHit::None;
    return deleteDialogHitTest(dialog.layout, static_cast<float>(local.x), static_cast<float>(local.y));
}

void App::embyDeleteWindowHover() {
    auto& dialog = embyDeleteWindow_;
    const DeleteDialogHit hit = dialog.pointerInside ? embyDeleteWindowHitAt(dialog.pointer) : DeleteDialogHit::None;
    const bool control = hit == DeleteDialogHit::Close || hit == DeleteDialogHit::Cancel ||
                         hit == DeleteDialogHit::Confirm || hit == DeleteDialogHit::Scrollbar;
    // A held thumb stays lit wherever the pointer goes.
    dialog.hot = dialog.thumbDrag ? DeleteDialogHit::Scrollbar : control ? hit : DeleteDialogHit::None;
}

// A press on the close or a button is a click when it lets go where it
// began; on the list's strip it drags the thumb.
void App::embyDeleteWindowPress(POINT local) {
    auto& dialog = embyDeleteWindow_;
    embyDeleteWindowCancelPress();
    dialog.pointer = local;
    dialog.pointerInside = true;
    switch (const DeleteDialogHit hit = embyDeleteWindowHitAt(local)) {
    case DeleteDialogHit::Scrollbar: {
        // On the thumb it is grabbed where it is; on the strip the thumb
        // jumps under the pointer.
        const OverlayRect thumb = deleteDialogThumb(dialog.layout, dialog.scale);
        const float y = static_cast<float>(local.y);
        dialog.thumbGrab = thumb.visible() && y >= thumb.y && y < thumb.bottom() ? y - thumb.y : thumb.height * 0.5F;
        dialog.thumbDrag = true;
        break;
    }
    case DeleteDialogHit::Close:
    case DeleteDialogHit::Cancel:
    case DeleteDialogHit::Confirm:
        dialog.pressed = hit;
        break;
    default:
        return;
    }
    if (dialog.window) SetCapture(dialog.window);
    embyDeleteWindowMove(local);
}

void App::embyDeleteWindowMove(POINT local) {
    auto& dialog = embyDeleteWindow_;
    dialog.pointer = local;
    dialog.pointerInside = true;
    if (dialog.thumbDrag) {
        dialog.scroll = deleteDialogScrollForThumbTop(dialog.layout, dialog.scale,
                                                      static_cast<float>(local.y) - dialog.thumbGrab);
        dialog.layout.listScroll = dialog.scroll;
    }
    embyDeleteWindowHover();
}

void App::embyDeleteWindowRelease(POINT local) {
    auto& dialog = embyDeleteWindow_;
    if (dialog.thumbDrag) {
        embyDeleteWindowMove(local);
        embyDeleteWindowCancelPress();
        embyDeleteWindowHover();
        return;
    }
    const DeleteDialogHit pressed = dialog.pressed;
    embyDeleteWindowCancelPress();
    if (pressed == DeleteDialogHit::None || embyDeleteWindowHitAt(local) != pressed) return;
    if (pressed == DeleteDialogHit::Confirm) embyConfirmDelete();
    else embyCancelDelete();
}

void App::embyDeleteWindowCancelPress() {
    auto& dialog = embyDeleteWindow_;
    dialog.thumbDrag = false;
    dialog.pressed = DeleteDialogHit::None;
    if (dialog.window && GetCapture() == dialog.window) ReleaseCapture();
}

// Three lines a notch, as a list's.
void App::scrollEmbyDeleteWindow(int wheelDelta) {
    auto& dialog = embyDeleteWindow_;
    if (!dialog.open) return;
    const float line = DeleteDialogMetrics{}.smallLine * dialog.scale;
    dialog.scroll = std::clamp(dialog.scroll - static_cast<float>(wheelDelta) / WHEEL_DELTA * 3.0F * line, 0.0F,
                               dialog.layout.listMaxScroll);
    dialog.layout.listScroll = dialog.scroll;
}

bool App::pointerOverEmbyDeleteWindow(POINT screen) const {
    const HWND window = embyDeleteWindow_.window;
    if (!window || !embyDeleteWindow_.open || !IsWindowVisible(window)) return false;
    const HWND hit = WindowFromPoint(screen);
    return hit && GetAncestor(hit, GA_ROOT) == window;
}

}  // namespace quaddeck
