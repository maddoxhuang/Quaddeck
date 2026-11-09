// App: the settings window. The seven tabs of settings, which the sheet over
// the right of the picture showed until 2026-10-21, in a window of their
// own: owned by the player's, so it stays above it and minimises and
// closes with it, moved and sized by the user, and shown again where it
// was left. Its rows are buildSettingsTabRows', laid out and hit as the
// sheet's are (SettingsPanel.hpp) and acted on by activatePanelRow. An
// Overlay of its own paints them on the renderer's device into a swap
// chain of its own (WindowSurface), once a frame while it is up. With no
// renderer it is state alone: open or not, its rows, its layout and its
// hits, but no window. It is drawn as the player is: a
// caption strip of its own with its title and a square close, the sheet's
// colours and controls under it. Its header has no switches: the sheet's
// faces are reached from the sheet. A drop-down's list hangs from
// its box in a popup of its own, owned by it and painted by the same
// Overlay, free to reach past the window as a Windows combo box's is.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string_view>

namespace quaddeck {

using namespace app_internal;

namespace {

constexpr wchar_t kSettingsWindowClass[] = L"QuadDeckSettingsWindow";
constexpr wchar_t kSettingsListClass[] = L"QuadDeckSettingsList";
// Its first size and its least, in DIPs: a window of its own, wider than
// it is tall, with room for most of a tab beside the column of tabs; and
// still the whole column, with rows beside it as wide as the sheet's were.
constexpr float kSettingsWindowWidth = 800.0F;
constexpr float kSettingsWindowHeight = 560.0F;
constexpr float kSettingsWindowMinWidth = 620.0F;
constexpr float kSettingsWindowMinHeight = 400.0F;
// A spin box's arrow held down steps again after the first of these and
// then at each of the second, as a held key repeats.
constexpr UINT_PTR kSpinTimer = 1;
constexpr UINT kSpinRepeatDelayMs = 400;
constexpr UINT kSpinRepeatMs = 40;
// A spin box's number is typed in whole digits, three at most: the volume's
// limit runs to 200.
constexpr std::size_t kSpinBoxDigits = 3;

// The digits of `text` a spin box takes, as ASCII: a full-width digit, as an
// input method may give, is one too; anything else is left out.
std::wstring spinBoxDigits(std::wstring_view text) {
    std::wstring digits;
    for (const wchar_t c : text) {
        if (c >= L'0' && c <= L'9') digits += c;
        else if (c >= 0xFF10 && c <= 0xFF19) digits += static_cast<wchar_t>(L'0' + (c - 0xFF10));
    }
    return digits;
}

int spinBoxNumber(const std::wstring& digits) {
    int number = 0;
    for (const wchar_t c : digits) number = number * 10 + (c - L'0');
    return number;
}

OverlayRect overlayRect(const RECT& rect) {
    return {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
            static_cast<float>(rect.bottom - rect.top)};
}

// Where it first stands, in screen pixels: over the middle of the player,
// below its caption.
RECT settingsWindowStart(HWND player, float scale, float captionInset) {
    RECT frame{};
    GetWindowRect(player, &frame);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(player, MONITOR_DEFAULTTONEAREST), &monitor)) monitor.rcWork = frame;
    const OverlayRect place = settingsWindowPlacement(
        overlayRect(frame), overlayRect(monitor.rcWork), kSettingsWindowWidth * scale, kSettingsWindowHeight * scale,
        captionInset);
    return {std::lround(place.x), std::lround(place.y), std::lround(place.right()), std::lround(place.bottom())};
}

}  // namespace

void App::toggleSettingsPanel() {
    if (settingsWindow_.open) closeSettingsWindow();
    else openSettingsWindow();
}

// Already up, it only comes to the front: the menu asks to see it.
void App::openSettingsWindow() {
    if (!settingsWindow_.open) {
        settingsWindow_.open = true;
        // Whether the file types are QuadDeck's is looked up whenever the
        // General tab may come into view.
        refreshFileTypesStatus();
        // And the cards and displays the Video tab lists.
        refreshVideoDevices();
    }
    if (!settingsWindow_.window && !createSettingsWindow()) return;
    const HWND window = settingsWindow_.window;
    if (!IsWindowVisible(window)) {
        // Where it was left, unless the monitor it stood on has gone since.
        RECT rect{};
        GetWindowRect(window, &rect);
        if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) && window_) {
            const RECT start = settingsWindowStart(window_, uiScale_, captionInset());
            SetWindowPos(window, nullptr, start.left, start.top, start.right - start.left, start.bottom - start.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(window, SW_SHOW);
    }
    SetForegroundWindow(window);
}

void App::closeSettingsWindow() {
    auto& settings = settingsWindow_;
    keyCapture_.reset();
    closeSettingsDropDown();
    endSpinEdit(true);
    settingsWindowCancelPress();
    settings.released.reset();
    settings.pointerInside = false;
    settings.hotKind = PanelHitKind::None;
    settings.hotRow = settings.hotPart = -1;
    settings.tooltip.clear();
    settings.tooltipAnchor = {};
    if (!settings.open) return;
    settings.open = false;
    const HWND window = settings.window;
    if (!window || !IsWindowVisible(window)) return;
    // The window is kept, hidden, so it opens again where it was left. Put
    // away while it had the keys, it hands them to the player rather than
    // to whatever window Windows would pick.
    const bool active = GetActiveWindow() == window;
    ShowWindow(window, SW_HIDE);
    if (active && window_) SetForegroundWindow(window_);
}

bool App::createSettingsWindow() {
    if (settingsWindow_.window) return true;
    // Painted on the renderer's device; with none there is no window.
    if (!window_ || !renderer_.device()) return false;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = &App::settingsWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kSettingsWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        appendDiagnostic("Settings window class could not be registered");
        return false;
    }
    // Sized in the player's DPI: it starts on the player's monitor.
    const RECT start = settingsWindowStart(window_, uiScale_, captionInset());
    // Created hidden, for the dark frame to be set before it is first drawn;
    // a caption for Windows to move it by and size it with, though none is
    // drawn (WM_NCCALCSIZE), and no maximise box.
    const HWND window = CreateWindowExW(
        0, kSettingsWindowClass, L"QuadDeck Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        start.left, start.top, start.right - start.left, start.bottom - start.top, window_, nullptr, instance_,
        this);
    if (!window) {
        appendDiagnostic("Settings window could not be created");
        return false;
    }
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const BOOL darkTitle = TRUE;
    const DWORD darkTitleAttribute = 20;
    DwmSetWindowAttribute(window, darkTitleAttribute, &darkTitle, sizeof(darkTitle));
    // No input method, ever: its letters are shortcuts or keys for a key box,
    // and its only typed text, a spin box's number, is digits key by key. An
    // input method in its native mode would make them VK_PROCESSKEY.
    ImmAssociateContextEx(window, nullptr, 0);
    settingsWindow_.scale = static_cast<float>(windowDpi(window)) / 96.0F;
    // A modal dialog over the player holds this window too.
    if (!IsWindowEnabled(window_)) EnableWindow(window, FALSE);
    return true;
}

LRESULT CALLBACK App::settingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->settingsWindow_.window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleSettingsWindowMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleSettingsWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    const HWND window = settingsWindow_.window;
    const auto local = [&] { return POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; };
    switch (message) {
    case WM_NCCALCSIZE: {
        // No title bar, as the player has none: Windows keeps its frame on
        // the sides and the bottom, and its own strip is the caption.
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
        if (standard != HTCLIENT) return standard;
        return settingsWindowFrameHit({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = std::lround(kSettingsWindowMinWidth * settingsWindow_.scale);
        info->ptMinTrackSize.y = std::lround(kSettingsWindowMinHeight * settingsWindow_.scale);
        return 0;
    }
    case WM_DPICHANGED: {
        // Its own monitor's: it may stand on another than the player.
        settingsWindow_.scale = static_cast<float>(HIWORD(wParam)) / 96.0F;
        if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    // Sized by hand, it is painted at each new size at once rather than at
    // the player's next frame.
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) renderSettingsWindow();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        renderSettingsWindow();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (!settingsWindow_.trackingLeave) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            settingsWindow_.trackingLeave = TrackMouseEvent(&track) != FALSE;
        }
        settingsWindowMove(local(), (wParam & MK_LBUTTON) != 0);
        return 0;
    case WM_MOUSELEAVE:
        settingsWindow_.trackingLeave = false;
        settingsWindow_.pointerInside = false;
        settingsWindowHover();
        return 0;
    case WM_LBUTTONDOWN:
        settingsWindowPress(local());
        return 0;
    case WM_LBUTTONDBLCLK: {
        // The class asks for double-clicks, so the second of two quick
        // clicks comes here. With a drop-down's list open it only closes
        // it, as a press does; on a band of the equalizer it sets the band back
        // to 0 dB. On the stepper, rail or scrollbar the first let go on,
        // it is a second click like the first; elsewhere nothing.
        if (settingsWindow_.dropDown) {
            settingsWindowPress(local());
            return 0;
        }
        const PanelHit hit = settingsWindowHitAt(local());
        // On a spin box's number being typed, all of it chosen, as a word is
        // in a text box.
        if (hit.kind == PanelHitKind::Field && settingsWindow_.spinEdit) {
            textFieldSelectAll(settingsWindow_.spinEdit->field);
            settingsWindow_.spinEdit->caretTick = GetTickCount64();
            return 0;
        }
        // A colour control's slider goes back to 0 the same way.
        if (hit.kind == PanelHitKind::Slider && hit.row >= 0 && hit.row < static_cast<int>(settingsWindow_.rows.size()) &&
            (settingsWindow_.rows[static_cast<std::size_t>(hit.row)].kind == PanelRowKind::Equalizer ||
             settingsWindow_.rows[static_cast<std::size_t>(hit.row)].id == SettingId::VideoColor)) {
            const PanelRow row = settingsWindow_.rows[static_cast<std::size_t>(hit.row)];
            applyPanelSlider(row, 0.5F, hit.part);
            scheduleAppSettingsSave();
            updateTitle();
            return 0;
        }
        if (const auto control = panelRepeatControl(hit, settingsWindow_.rows);
            control && control == settingsWindow_.released) {
            settingsWindowPress(local());
        }
        return 0;
    }
    case WM_LBUTTONUP:
        settingsWindowRelease(local());
        return 0;
    case WM_RBUTTONUP:
        // A spin box's number has Windows' edit-box menu.
        settingsWindowRightClick(local());
        return 0;
    case WM_TIMER:
        if (wParam == kSpinTimer) settingsSpinStep();
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window) settingsWindowCancelPress();
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
        // A press on the header or the frame, to move or size the window,
        // closes a drop-down's list as one in the window does.
        closeSettingsDropDown();
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_MOUSEWHEEL:
        // The main loop scrolls it wherever the wheel is sent; this is for
        // one that reaches the window another way.
        scrollSettingsWindow(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_KEYDOWN: {
        // The media keys are the player's application commands, made by
        // Windows from the default handling (handleMessage, WM_KEYDOWN).
        if (wParam >= VK_BROWSER_BACK && wParam <= VK_LAUNCH_APP2) {
            if ((lParam & (1LL << 30)) && wParam >= VK_MEDIA_NEXT_TRACK && wParam <= VK_MEDIA_PLAY_PAUSE) return 0;
            return DefWindowProcW(window, message, wParam, lParam);
        }
        // An open list takes the keys: its arrows, Enter and Esc.
        if (settingsWindow_.dropDown) {
            settingsDropDownKey(wParam);
            return 0;
        }
        const bool alt = GetKeyState(VK_MENU) < 0;
        // A spin box's number being typed has every key before the
        // shortcuts but a function key of the settings or the list, as a
        // text box has (handleMessage).
        if (settingsWindow_.spinEdit && !keyChordLeavesTextBox(keyBindings_, keyChordOf(wParam, alt))) {
            spinEditKey(wParam, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        if (capturingKey()) captureKey(wParam, lParam, alt);
        else settingsWindowShortcut(wParam, lParam, alt);
        return 0;
    }
    case WM_SYSKEYDOWN: {
        // As the player's (handleMessage): a key taken drops its character;
        // Alt+F4 reaches Windows and closes this window, Alt+Space its menu.
        // An open list closes first.
        closeSettingsDropDown();
        const bool alt = (lParam & (1LL << 29)) != 0;
        if (capturingKey() ? captureKey(wParam, lParam, alt) : settingsWindowShortcut(wParam, lParam, alt)) {
            altCharTaken_ = true;
            return 0;
        }
        altCharTaken_ = false;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_SYSCHAR:
        if (altCharTaken_) {
            altCharTaken_ = false;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_SYSCOMMAND:
        // Alt let go on its own would put the keys in an invisible menu bar.
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) return 0;
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_APPCOMMAND:
        // Play, pause and the tracks act on the player, as from its window.
        return handleMessage(message, wParam, lParam);
    case WM_CHAR:
        // Not under the number's open menu, which has the keys.
        if (settingsWindow_.spinEdit && !settingsWindow_.dropDown) spinEditCharacter(static_cast<wchar_t>(wParam));
        return 0;
    case WM_SETCURSOR:
        // An I-beam over a spin box's number, which is typed into.
        if (LOWORD(lParam) == HTCLIENT && !settingsWindow_.dropDown) {
            POINT pointer{};
            GetCursorPos(&pointer);
            ScreenToClient(window, &pointer);
            if (settingsWindowHitAt(pointer).kind == PanelHitKind::Field) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_KILLFOCUS:
        // A key box stops waiting, a drop-down's list closes and a number
        // being typed is set, when the keyboard goes elsewhere.
        keyCapture_.reset();
        closeSettingsDropDown();
        endSpinEdit(true);
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_CLOSE:
        closeSettingsWindow();
        return 0;
    case WM_NCDESTROY:
        // With the player's window: an owned window goes with its owner.
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        settingsWindow_.window = nullptr;
        settingsWindow_.open = false;
        settingsWindow_.trackingLeave = false;
        settingsWindow_.surface.release();
        return DefWindowProcW(window, message, wParam, lParam);
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Rows, layout and painting

void App::refreshSettingsWindowGeometry(const RECT& client) {
    auto& settings = settingsWindow_;
    // A key box waits only while the Keys tab shows it.
    if (keyCapture_ && !capturingKey()) keyCapture_.reset();
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    // A window as wide as this has no use for segments and buttons stretched
    // across it: each is its caption's width, beside the label where it fits.
    // A caption strip of its own across its top, as the player has; the rows
    // on cards, as Windows' own settings.
    PanelMetrics metrics{};
    metrics.compactControls = true;
    metrics.windowCaption = true;
    metrics.cards = true;
    settings.rows = buildSettingsTabRows();
    // The spin box being typed shows what was typed; gone with its row --
    // another tab, or greyed -- what was typed is set.
    if (settings.spinEdit) {
        const auto typed = std::find_if(settings.rows.begin(), settings.rows.end(), [&](const PanelRow& row) {
            return row.spinBox && row.enabled && row.id == settings.spinEdit->id &&
                   row.param == settings.spinEdit->param;
        });
        if (typed == settings.rows.end()) {
            endSpinEdit(true);
        } else {
            typed->field.edit = settings.spinEdit->field;
            typed->field.focused = true;
            // Solid just after a key, then blinking at the system's rate, and
            // only while the window has the keyboard.
            const UINT blink = GetCaretBlinkTime();
            const ULONGLONG since = GetTickCount64() - settings.spinEdit->caretTick;
            typed->field.caretShown = settings.window && GetFocus() == settings.window &&
                                      (blink == 0 || blink == INFINITE || (since / blink) % 2 == 0);
        }
    }
    const float rowWidth = std::max(
        0.0F, width - panelTabColumnWidth(width, settings.scale, metrics) - 2.0F * metrics.pad * settings.scale);
    // A note on a card is narrower by the card's padding; a pinned one has
    // the content's width.
    const float cardRowWidth = std::max(0.0F, rowWidth - 2.0F * panelCardInset(settings.scale, metrics));
    for (auto& row : settings.rows) {
        if (row.kind == PanelRowKind::Note && row.wrapNote) {
            settings.overlay.measureWrappedNote(row, row.pinned ? rowWidth : cardRowWidth, settings.scale);
        }
        if (row.kind == PanelRowKind::Choice || row.kind == PanelRowKind::Buttons) {
            settings.overlay.measureCaptions(row, settings.scale);
        }
    }
    // The sheet the whole width of the window, the tabs down its left and
    // the header at the top: no caption strip above it, and nothing to
    // slide in from.
    float& scroll = settingsTabScroll_[static_cast<std::size_t>(settingsTab_)];
    settingsPanelLayoutInto(settings.layout, width, height, settings.scale, settings.rows, scroll, 1.0F, metrics,
                            width, 0.0F, kSettingsTabCount);
    scroll = std::clamp(scroll, 0.0F, settings.layout.maxScroll);
    layoutSettingsDropDown();
}

// What the painter draws: the rows, the layout and the pointer's state as
// they stand. The fixtures draw it too.
void App::buildSettingsWindowScene(const RECT& client) {
    auto& settings = settingsWindow_;
    OverlayScene& scene = settings.scene;
    scene.width = static_cast<float>(client.right);
    scene.height = static_cast<float>(client.bottom);
    scene.scale = settings.scale;
    scene.tooltip = settings.tooltip;
    scene.tooltipAnchor = settings.tooltipAnchor;
    OverlayPanelScene& panel = scene.panel;
    panel.alpha = 1.0F;
    panel.title = L"Settings";
    panel.sharedLayout = &settings.layout;
    panel.sharedRows = &settings.rows;
    panel.thumbnails = nullptr;
    panel.tabs.clear();
    panel.tabGlyphs.clear();
    for (int tab = 0; tab < kSettingsTabCount; ++tab) {
        panel.tabs.emplace_back(settingsTabName(static_cast<SettingsTab>(tab)));
        panel.tabGlyphs.push_back(settingsTabGlyph(static_cast<SettingsTab>(tab)));
    }
    panel.tab = static_cast<int>(settingsTab_);
    panel.switchCaptions.clear();
    panel.hotKind = settings.hotKind;
    panel.hotRow = settings.hotRow;
    panel.hotPart = settings.hotPart;
    panel.pressedKind = settings.pressedKind;
    panel.pressedRow = settings.pressedRow;
    panel.pressedPart = settings.pressedPart;
    panel.dragRow = settings.drag == SettingsWindow::Drag::Slider ? settings.dragRow : -1;
    panel.dragPart = settings.drag == SettingsWindow::Drag::Slider ? settings.dragPart : -1;
    panel.scrollbarHot = settings.hotKind == PanelHitKind::Scrollbar;
    panel.scrollbarDragging = settings.drag == SettingsWindow::Drag::Scrollbar;
    panel.scrollMarks.clear();
    // A drop-down's box is lit as pressed while its list is open (not a spin
    // box under its menu); the list is painted in its own window
    // (renderSettingsList).
    scene.panelMenu.items.clear();
    scene.panelMenu.submenus.clear();
    if (settings.dropDown && !settings.dropDown->editMenu) {
        panel.pressedKind = PanelHitKind::DropDown;
        panel.pressedRow = settings.dropDown->row;
        panel.pressedPart = -1;
    }
}

// What the list's window paints: its lines, the one lit and the one
// pressed, its ticks in the colour of the video its row is about.
void App::buildSettingsListScene() {
    auto& settings = settingsWindow_;
    OverlayScene& scene = settings.listScene;
    OverlayPanelMenuScene& menu = scene.panelMenu;
    menu.submenus.clear();
    if (!settings.dropDown) {
        menu.items.clear();
        return;
    }
    const auto& open = *settings.dropDown;
    scene.width = open.frame.width;
    scene.height = open.frame.height;
    scene.scale = settings.scale;
    menu.items = open.items;
    menu.layout = open.layout;
    menu.highlighted = open.highlighted;
    menu.pressed = open.pressed;
    menu.alpha = 1.0F;
    menu.offset = 0.0F;
    menu.position = open.row >= 0 && open.row < static_cast<int>(settings.rows.size())
        ? settings.rows[static_cast<std::size_t>(open.row)].video : -1;
}

void App::renderSettingsWindow() {
    auto& settings = settingsWindow_;
    const HWND window = settings.window;
    if (!settings.open || settings.painting || !window || !IsWindowVisible(window) || IsIconic(window)) return;
    ID3D11Device* device = renderer_.device();
    if (!device || renderer_.deviceLost()) return;
    RECT client{};
    GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    settings.painting = true;
    {
        const std::lock_guard<std::recursive_mutex> lock(renderer_.deviceMutex());
        if (settings.overlay.ready() || settings.overlay.initialize(device)) {
            refreshSettingsWindowGeometry(client);
            settingsWindowHover();
            buildSettingsWindowScene(client);
            if (const auto surface = settings.surface.backBuffer(device, window, static_cast<UINT>(client.right),
                                                                 static_cast<UINT>(client.bottom))) {
                settings.overlay.drawSheetWindow(surface.Get(), settings.scene);
                settings.surface.present();
            }
            renderSettingsList(device);
        }
    }
    settings.painting = false;
}

void App::renderSettingsList(ID3D11Device* device) {
    auto& settings = settingsWindow_;
    const HWND list = settings.listWindow;
    if (!list || !settings.dropDown) return;
    buildSettingsListScene();
    const OverlayRect& frame = settings.dropDown->frame;
    const auto width = static_cast<UINT>(std::max(0L, std::lround(frame.width)));
    const auto height = static_cast<UINT>(std::max(0L, std::lround(frame.height)));
    if (width == 0 || height == 0) return;
    if (const auto surface = settings.listSurface.backBuffer(device, list, width, height)) {
        settings.overlay.drawMenuWindow(surface.Get(), settings.listScene, !settings.listFramed);
        settings.listSurface.present();
        // Shown once it has lines to show, over the settings window and
        // leaving it the keys.
        if (!IsWindowVisible(list)) ShowWindow(list, SW_SHOWNOACTIVATE);
    }
}

// ---------------------------------------------------------------------------
// The pointer

PanelHit App::settingsWindowHitAt(POINT local) const {
    if (!settingsWindow_.open) return {};
    return settingsPanelHitTest(static_cast<float>(local.x), static_cast<float>(local.y), settingsWindow_.layout,
                                settingsWindow_.rows, kPanelRailInset * settingsWindow_.scale);
}

void App::settingsWindowHover() {
    auto& settings = settingsWindow_;
    if (settings.dropDown) {
        // An open list lights its own lines, and nothing beneath it.
        settings.hotKind = PanelHitKind::None;
        settings.hotRow = settings.hotPart = -1;
        settings.tooltip.clear();
        settings.tooltipAnchor = {};
        return;
    }
    PanelHit hit = settings.pointerInside ? settingsWindowHitAt(settings.pointer) : PanelHit{};
    // A greyed row is the sheet's, lit by nothing, but its hint is shown.
    if (hit.kind == PanelHitKind::Sheet && hit.row < 0) {
        hit.row = panelGreyedRowAt(static_cast<float>(settings.pointer.x), static_cast<float>(settings.pointer.y),
                                   settings.layout, settings.rows);
    }
    const bool over = hit.kind != PanelHitKind::None && hit.kind != PanelHitKind::Outside;
    // A drag keeps its own control lit wherever the pointer goes.
    PanelHitKind kind = over ? hit.kind : PanelHitKind::None;
    int row = over ? hit.row : -1;
    int part = over ? hit.part : -1;
    if (settings.drag == SettingsWindow::Drag::Slider) {
        kind = PanelHitKind::Slider;
        row = settings.dragRow;
        part = settings.dragPart;
    } else if (settings.drag == SettingsWindow::Drag::Scrollbar) {
        kind = PanelHitKind::Scrollbar;
        row = part = -1;
    }
    const ULONGLONG now = GetTickCount64();
    if (kind != settings.hotKind || row != settings.hotRow || part != settings.hotPart) settings.hotSince = now;
    settings.hotKind = kind;
    settings.hotRow = row;
    settings.hotPart = part;
    // A row's hint once the pointer has rested on it, as the player's
    // controls name themselves.
    settings.tooltip.clear();
    settings.tooltipAnchor = {};
    if (over && settings.drag == SettingsWindow::Drag::None && now - settings.hotSince >= kTooltipDelayMs &&
        row >= 0 && row < static_cast<int>(settings.rows.size()) &&
        row < static_cast<int>(settings.layout.rows.size())) {
        settings.tooltip = settings.rows[static_cast<std::size_t>(row)].hint;
        settings.tooltipAnchor = settings.layout.rows[static_cast<std::size_t>(row)].row;
    }
}

// As a press on the sheet (handleVideoMessage, WM_LBUTTONDOWN): a slider
// and the scrollbar drag at once, a press anywhere else that moves on
// becomes a drag of the content, and one that does not is a click when it
// lets go where it began.
void App::settingsWindowPress(POINT local) {
    auto& settings = settingsWindow_;
    settingsWindowCancelPress();
    settings.released.reset();
    settings.pointer = local;
    settings.pointerInside = true;
    const auto capture = [&] {
        if (settings.window) SetCapture(settings.window);
    };
    if (settings.dropDown) {
        // The list is a window of its own: a press here only closes it, as
        // one off a list box's does -- on its own box too, which would
        // otherwise open it again.
        closeSettingsDropDown();
        settingsWindowHover();
        return;
    }
    const PanelHit hit = settingsWindowHitAt(local);
    // A press anywhere but on a spin box's number sets what was typed into
    // one, as a box losing the keyboard does, and then does its own.
    if (hit.kind != PanelHitKind::Field) endSpinEdit(true);
    switch (hit.kind) {
    case PanelHitKind::Field:
        // A spin box's number: typed into from here.
        beginSpinEdit(hit.row, static_cast<float>(local.x));
        return;
    case PanelHitKind::Slider:
        settings.drag = SettingsWindow::Drag::Slider;
        settings.dragRow = hit.row;
        settings.dragPart = hit.part;
        capture();
        settingsWindowMove(local, true);
        return;
    case PanelHitKind::Scrollbar:
        // On the thumb it is grabbed where it is; on the track the thumb
        // jumps under the pointer.
        settings.thumbGrab = panelThumbGrab(settings.layout, settings.scale, static_cast<float>(local.y), {});
        settings.drag = SettingsWindow::Drag::Scrollbar;
        capture();
        settingsWindowMove(local, true);
        return;
    case PanelHitKind::Sheet:
        settings.pressArmed = true;
        break;
    case PanelHitKind::Close:
    case PanelHitKind::Tab:
    case PanelHitKind::Toggle:
    case PanelHitKind::Segment:
    case PanelHitKind::Button:
    case PanelHitKind::Trailing:
    case PanelHitKind::Item:
    case PanelHitKind::Tile:
    case PanelHitKind::Add:
    case PanelHitKind::DropDown:
        settings.pressedKind = hit.kind;
        settings.pressedRow = hit.row;
        settings.pressedPart = hit.part;
        if (hit.row >= 0 && hit.row < static_cast<int>(settings.rows.size())) {
            const auto& row = settings.rows[static_cast<std::size_t>(hit.row)];
            settings.pressedId = row.id;
            settings.pressedParam = panelRowParam(row, hit.part);
        }
        settings.pressArmed = true;
        break;
    default:
        return;
    }
    settings.pressPoint = local;
    settings.pressScroll = settingsTabScroll_[static_cast<std::size_t>(settingsTab_)];
    capture();
    // A spin box's arrow steps at the press, as Windows' own do, and again
    // while it is held there; it is never a drag of the content.
    if (hit.kind == PanelHitKind::Button && hit.row >= 0 && hit.row < static_cast<int>(settings.rows.size()) &&
        settings.rows[static_cast<std::size_t>(hit.row)].spinBox) {
        settings.pressArmed = false;
        settings.spinning = true;
        if (settings.window) SetTimer(settings.window, kSpinTimer, kSpinRepeatDelayMs, nullptr);
        activateSettingsHit(hit);
    }
}

void App::settingsSpinStep() {
    auto& settings = settingsWindow_;
    if (!settings.spinning) {
        stopSettingsSpin();
        return;
    }
    if (settings.window) SetTimer(settings.window, kSpinTimer, kSpinRepeatMs, nullptr);
    // Only while the pointer is on the arrow pressed, as on a scrollbar's:
    // moved off, it waits; back on, it steps again.
    const PanelHit hit = settingsWindowHitAt(settings.pointer);
    if (hit.kind != settings.pressedKind || hit.row != settings.pressedRow || hit.part != settings.pressedPart ||
        hit.row < 0 || hit.row >= static_cast<int>(settings.rows.size())) {
        return;
    }
    const auto& row = settings.rows[static_cast<std::size_t>(hit.row)];
    if (row.id != settings.pressedId || panelRowParam(row, hit.part) != settings.pressedParam) return;
    activateSettingsHit(hit);
}

void App::stopSettingsSpin() {
    auto& settings = settingsWindow_;
    if (settings.spinning && settings.window) KillTimer(settings.window, kSpinTimer);
    settings.spinning = false;
}

// ---------------------------------------------------------------------------
// A spin box's number typed

void App::beginSpinEdit(int row, float x) {
    auto& settings = settingsWindow_;
    if (row < 0 || row >= static_cast<int>(settings.rows.size())) return;
    const auto& source = settings.rows[static_cast<std::size_t>(row)];
    const auto value = spinBoxValue(source.id);
    if (!source.spinBox || !source.enabled || !value) return;
    if (!settings.spinEdit || settings.spinEdit->id != source.id || settings.spinEdit->param != source.param) {
        endSpinEdit(true);
        SettingsWindow::SpinEdit edit;
        edit.id = source.id;
        edit.param = source.param;
        // All of it chosen at first, so what is typed takes its place.
        edit.field.text = std::to_wstring(*value);
        textFieldSelectAll(edit.field);
        settings.spinEdit = std::move(edit);
    } else if (row < static_cast<int>(settings.layout.rows.size())) {
        // Typed into already: the caret where it was pressed, measured as the
        // painter draws the digits (estimated where no font is ready).
        TextFieldState& field = settings.spinEdit->field;
        const float left = settings.layout.rows[static_cast<std::size_t>(row)].value.x +
                           PanelMetrics{}.dropDownPad * settings.scale;
        const auto widthOf = [&](std::size_t count) {
            const std::wstring digits = field.text.substr(0, std::min(count, field.text.size()));
            const float measured = settings.overlay.measureText(digits, OverlayTextStyle::Body, settings.scale);
            return measured > 0.0F || digits.empty() ? measured : panelTextWidthEstimate(digits) * settings.scale;
        };
        textFieldMoveTo(field, textFieldIndexAt(field.text, x - left, widthOf), false);
    }
    settings.spinEdit->caretTick = GetTickCount64();
}

void App::spinEditKey(WPARAM key, bool ctrl, bool shift) {
    auto& settings = settingsWindow_;
    if (!settings.spinEdit) return;
    TextFieldState& field = settings.spinEdit->field;
    textFieldClamp(field);
    switch (key) {
    // The digits are one word: Ctrl takes the arrows and the erasers to an end.
    case VK_LEFT: textFieldStep(field, -1, ctrl, shift); break;
    case VK_RIGHT: textFieldStep(field, 1, ctrl, shift); break;
    case VK_HOME: textFieldMoveTo(field, 0, shift); break;
    case VK_END: textFieldMoveTo(field, field.text.size(), shift); break;
    case VK_BACK: textFieldErase(field, -1, ctrl); break;
    case VK_DELETE:
        // Shift+Delete cuts, as in Windows' edit box.
        if (shift && !ctrl && textFieldHasSelection(field)) {
            spinEditApply(TextFieldEdit::Cut);
            break;
        }
        textFieldErase(field, 1, ctrl);
        break;
    case VK_INSERT:
        if (ctrl && !shift) spinEditApply(TextFieldEdit::Copy);
        else if (shift && !ctrl) spinEditApply(TextFieldEdit::Paste);
        break;
    case 'A':
        if (ctrl) spinEditApply(TextFieldEdit::SelectAll);
        break;
    case 'C':
        if (ctrl) spinEditApply(TextFieldEdit::Copy);
        break;
    case 'X':
        if (ctrl) spinEditApply(TextFieldEdit::Cut);
        break;
    case 'V':
        if (ctrl) spinEditApply(TextFieldEdit::Paste);
        break;
    case VK_UP:
    case VK_DOWN: {
        // As its arrows, from what was typed; the number stays typed, all of
        // it chosen.
        const SettingId id = settings.spinEdit->id;
        const auto value = spinBoxValue(id);
        if (!value) break;
        setSpinBoxValue(id, (field.text.empty() ? *value : spinBoxNumber(field.text)) + (key == VK_UP ? 1 : -1));
        field.text = std::to_wstring(spinBoxValue(id).value_or(*value));
        textFieldSelectAll(field);
        break;
    }
    case VK_RETURN:
    case VK_TAB:
        endSpinEdit(true);
        return;
    case VK_ESCAPE:
        // Only the typing: the window stays.
        endSpinEdit(false);
        return;
    default:
        break;
    }
    settings.spinEdit->caretTick = GetTickCount64();
}

void App::spinEditCharacter(wchar_t character) {
    auto& settings = settingsWindow_;
    if (!settings.spinEdit) return;
    // Digits alone. Backspace, Enter, Esc and Ctrl's letters come as
    // characters too, after the keys that acted on them; they are not digits.
    const std::wstring digit = spinBoxDigits(std::wstring_view(&character, 1));
    if (digit.empty()) return;
    textFieldInsert(settings.spinEdit->field, digit, kSpinBoxDigits);
    settings.spinEdit->caretTick = GetTickCount64();
}

bool App::spinEditApply(TextFieldEdit edit) {
    auto& settings = settingsWindow_;
    if (!settings.spinEdit) return false;
    // A paste takes the clipboard's digits alone, as typing does; with
    // none it does nothing. The clipboard is read for a paste only.
    const std::wstring pasted =
        edit == TextFieldEdit::Paste ? spinBoxDigits(fieldClipboard(settings.window)) : std::wstring();
    std::wstring copied;
    const bool edited = textFieldApplyEdit(settings.spinEdit->field, edit, false, pasted, copied, kSpinBoxDigits);
    fieldSetClipboard(copied, settings.window);
    settings.spinEdit->caretTick = GetTickCount64();
    return edited;
}

void App::settingsWindowRightClick(POINT local) {
    auto& settings = settingsWindow_;
    // An open list or menu only closes, as under a left press.
    if (settings.dropDown) {
        closeSettingsDropDown();
        settingsWindowHover();
        return;
    }
    // Only a spin box's number has a menu; elsewhere a right-click is nothing.
    const PanelHit hit = settingsWindowHitAt(local);
    if (hit.kind != PanelHitKind::Field || hit.row < 0 || hit.row >= static_cast<int>(settings.rows.size())) return;
    const PanelRow& row = settings.rows[static_cast<std::size_t>(hit.row)];
    // Not typed into yet, it takes the typing as under a left press, all of
    // it chosen; typed into, its caret and selection stay, for the menu.
    if (!settings.spinEdit || settings.spinEdit->id != row.id || settings.spinEdit->param != row.param) {
        beginSpinEdit(hit.row, static_cast<float>(local.x));
    }
    if (!settings.spinEdit) return;
    SettingsWindow::DropDown open;
    open.editMenu = true;
    open.at = local;
    open.id = row.id;
    open.param = row.param;
    open.items = fieldEditMenu(settings.spinEdit->field, false, spinBoxDigits(fieldClipboard(settings.window)),
                               kSpinBoxDigits).items;
    if (settings.window && renderer_.device()) createSettingsListWindow();
    settings.dropDown = std::move(open);
    layoutSettingsDropDown();
    settingsWindowHover();
}

void App::endSpinEdit(bool apply) {
    auto& settings = settingsWindow_;
    if (!settings.spinEdit) return;
    const SettingsWindow::SpinEdit edit = std::move(*settings.spinEdit);
    settings.spinEdit.reset();
    // Nothing typed leaves the number as it was; one past its range is held
    // to it by what it sets, which says so.
    if (!apply || edit.field.text.empty()) return;
    const int typed = spinBoxNumber(edit.field.text);
    if (spinBoxValue(edit.id) != typed) setSpinBoxValue(edit.id, typed);
}

void App::settingsWindowMove(POINT local, bool leftDown) {
    auto& settings = settingsWindow_;
    settings.pointer = local;
    settings.pointerInside = true;
    if (settings.dropDown) {
        // The list's lines are lit from its own window.
        settingsWindowHover();
        return;
    }
    float& scroll = settingsTabScroll_[static_cast<std::size_t>(settingsTab_)];
    switch (settings.drag) {
    case SettingsWindow::Drag::Slider:
        if (settings.dragRow >= 0 && settings.dragRow < static_cast<int>(settings.layout.rows.size()) &&
            settings.dragRow < static_cast<int>(settings.rows.size())) {
            const auto index = static_cast<std::size_t>(settings.dragRow);
            const auto& geometry = settings.layout.rows[index];
            if (settings.rows[index].kind == PanelRowKind::Equalizer) {
                // A band of the equalizer, up and down its own band.
                if (settings.dragPart >= 0 && settings.dragPart < static_cast<int>(geometry.parts.size())) {
                    settings.dragFraction = verticalRailFraction(
                        static_cast<float>(local.y), geometry.parts[static_cast<std::size_t>(settings.dragPart)],
                        kPanelRailInset * settings.scale);
                    applyPanelSlider(settings.rows[index], settings.dragFraction, settings.dragPart);
                }
            } else {
                settings.dragFraction = railFraction(static_cast<float>(local.x), geometry.control,
                                                     kPanelRailInset * settings.scale);
                applyPanelSlider(settings.rows[index], settings.dragFraction);
            }
        }
        break;
    case SettingsWindow::Drag::Scroll:
        // The content follows the pointer, as a page dragged by hand.
        scroll = std::clamp(settings.pressScroll - static_cast<float>(local.y - settings.pressPoint.y), 0.0F,
                            settings.layout.maxScroll);
        break;
    case SettingsWindow::Drag::Scrollbar:
        scroll = panelScrollForThumbTop(settings.layout, settings.scale,
                                        static_cast<float>(local.y) - settings.thumbGrab);
        break;
    case SettingsWindow::Drag::None:
        if (leftDown && settings.pressArmed &&
            std::abs(local.y - settings.pressPoint.y) >= GetSystemMetrics(SM_CYDRAG) &&
            settings.layout.maxScroll > 0.0F) {
            // A press that moves on is a scroll, not a click.
            settings.pressArmed = false;
            settings.pressedKind = PanelHitKind::None;
            settings.pressedRow = settings.pressedPart = -1;
            settings.pressedId = SettingId::None;
            settings.pressedParam = -1;
            settings.drag = SettingsWindow::Drag::Scroll;
            settingsWindowMove(local, leftDown);
            return;
        }
        break;
    }
    settingsWindowHover();
}

void App::settingsWindowRelease(POINT local) {
    auto& settings = settingsWindow_;
    // Read before the click acts: what it opens may stand here next.
    settings.released = panelRepeatControl(settingsWindowHitAt(local), settings.rows);
    // A spin box's arrow has stepped already, at its press and while held.
    const bool spun = settings.spinning;
    stopSettingsSpin();
    if (settings.drag != SettingsWindow::Drag::None) {
        settingsWindowMove(local, true);
        const bool slider = settings.drag == SettingsWindow::Drag::Slider;
        settings.drag = SettingsWindow::Drag::None;
        settings.dragRow = settings.dragPart = -1;
        if (settings.window && GetCapture() == settings.window) ReleaseCapture();
        if (slider) {
            scheduleAppSettingsSave();
            updateTitle();
        }
        settingsWindowHover();
        return;
    }
    const PanelHitKind kind = settings.pressedKind;
    const int row = settings.pressedRow;
    const int part = settings.pressedPart;
    const SettingId id = settings.pressedId;
    const int param = settings.pressedParam;
    settings.pressedKind = PanelHitKind::None;
    settings.pressedRow = settings.pressedPart = -1;
    settings.pressedId = SettingId::None;
    settings.pressedParam = -1;
    settings.pressArmed = false;
    if (settings.window && GetCapture() == settings.window) ReleaseCapture();
    if (kind == PanelHitKind::None || spun) return;
    const PanelHit hit = settingsWindowHitAt(local);
    if (hit.kind != kind || hit.row != row || hit.part != part) return;
    // The rows are built again every frame; the same number must still be
    // the same row.
    if (row >= 0 && row < static_cast<int>(settings.rows.size())) {
        const auto& current = settings.rows[static_cast<std::size_t>(row)];
        if (current.id != id || panelRowParam(current, part) != param) return;
    }
    activateSettingsHit(hit);
}

void App::settingsWindowCancelPress() {
    auto& settings = settingsWindow_;
    // A slider let go of elsewhere keeps where it was dragged to, a spin
    // box where it was stepped to.
    if (settings.drag == SettingsWindow::Drag::Slider) scheduleAppSettingsSave();
    stopSettingsSpin();
    settings.drag = SettingsWindow::Drag::None;
    settings.dragRow = settings.dragPart = -1;
    settings.pressedKind = PanelHitKind::None;
    settings.pressedRow = settings.pressedPart = -1;
    settings.pressedId = SettingId::None;
    settings.pressedParam = -1;
    settings.pressArmed = false;
    if (settings.window && GetCapture() == settings.window) ReleaseCapture();
}

void App::scrollSettingsWindow(int wheelDelta) {
    auto& settings = settingsWindow_;
    if (settings.dropDown) {
        // An open list holds the rows still, and scrolls itself where the
        // screen cut it short: two lines a notch, the line then under the
        // pointer lit.
        settings.dropDown->layout.scroll -=
            static_cast<float>(wheelDelta) / WHEEL_DELTA * 2.0F * PanelMenuMetrics{}.itemHeight * settings.scale;
        layoutSettingsDropDown();
        if (settings.dropDown && settings.listWindow) {
            POINT pointer{};
            GetCursorPos(&pointer);
            ScreenToClient(settings.listWindow, &pointer);
            auto& open = *settings.dropDown;
            const int line = panelMenuItemAt(open.layout, open.items, static_cast<float>(pointer.x),
                                             static_cast<float>(pointer.y));
            if (line >= 0) open.highlighted = line;
        }
        return;
    }
    float& scroll = settingsTabScroll_[static_cast<std::size_t>(settingsTab_)];
    scroll = std::clamp(scroll - static_cast<float>(wheelDelta) / WHEEL_DELTA * 56.0F * settingsWindow_.scale, 0.0F,
                        settingsWindow_.layout.maxScroll);
}

void App::activateSettingsHit(const PanelHit& hit) {
    // Any click ends a key box's wait; the buttons over the Keys tab's list
    // and its key boxes act on the wait it ended.
    const std::optional<KeyCapture> ended = capturingKey() ? keyCapture_ : std::nullopt;
    keyCapture_.reset();
    switch (hit.kind) {
    case PanelHitKind::Close:
        closeSettingsWindow();
        return;
    case PanelHitKind::Tab:
        if (hit.part >= 0 && hit.part < kSettingsTabCount) selectSettingsTab(static_cast<SettingsTab>(hit.part));
        return;
    case PanelHitKind::DropDown:
        openSettingsDropDown(hit.row);
        return;

    case PanelHitKind::Toggle:
    case PanelHitKind::Segment:
    case PanelHitKind::Button:
    case PanelHitKind::Trailing:
    case PanelHitKind::Item:
    case PanelHitKind::Tile:
    case PanelHitKind::Add:
        break;
    default:
        return;
    }
    if (hit.row < 0 || hit.row >= static_cast<int>(settingsWindow_.rows.size())) return;
    // A copy: what the row does can build the rows again.
    const PanelRow row = settingsWindow_.rows[static_cast<std::size_t>(hit.row)];
    activatePanelRow(row, hit, ended);
}

// ---------------------------------------------------------------------------
// A drop-down's list

void App::openSettingsDropDown(int row) {
    auto& settings = settingsWindow_;
    closeSettingsDropDown();
    if (row < 0 || row >= static_cast<int>(settings.rows.size())) return;
    const auto& source = settings.rows[static_cast<std::size_t>(row)];
    if (source.kind != PanelRowKind::Choice || !source.dropDown || !source.enabled || source.options.empty()) return;
    SettingsWindow::DropDown open;
    open.row = row;
    open.id = source.id;
    open.param = source.param;
    open.highlighted = source.selected;
    // Its window, the first time; with none -- no renderer -- the list is
    // state alone.
    if (settings.window && renderer_.device()) createSettingsListWindow();
    settings.dropDown = std::move(open);
    layoutSettingsDropDown();
    // Opened on the chosen line, as a list box is.
    if (settings.dropDown) {
        settings.dropDown->layout.scroll = panelMenuScrollToShow(settings.dropDown->layout, settings.dropDown->highlighted);
        layoutSettingsDropDown();
    }
    settingsWindowHover();
}

void App::closeSettingsDropDown() {
    auto& settings = settingsWindow_;
    if (!settings.dropDown) return;
    settings.dropDown.reset();
    // Hidden, kept for the next list; it never had the keys to hand back.
    if (const HWND list = settings.listWindow) {
        if (GetCapture() == list) ReleaseCapture();
        ShowWindow(list, SW_HIDE);
    }
}

void App::layoutSettingsDropDown() {
    auto& settings = settingsWindow_;
    if (!settings.dropDown) return;
    auto& open = *settings.dropDown;
    const auto& layout = settings.layout;
    // Its row: a drop-down Choice for a list, the spin box being typed for
    // its menu, which goes with the typing.
    open.row = -1;
    for (std::size_t i = 0; i < settings.rows.size() && i < layout.rows.size(); ++i) {
        const auto& row = settings.rows[i];
        if (row.id != open.id || row.param != open.param) continue;
        if (open.editMenu ? row.spinBox : row.kind == PanelRowKind::Choice && row.dropDown) {
            open.row = static_cast<int>(i);
            break;
        }
    }
    const bool typed = settings.spinEdit && settings.spinEdit->id == open.id && settings.spinEdit->param == open.param;
    if (open.row < 0 || !settings.rows[static_cast<std::size_t>(open.row)].enabled || (open.editMenu && !typed)) {
        closeSettingsDropDown();
        return;
    }
    const auto index = static_cast<std::size_t>(open.row);
    const OverlayRect& band = index < layout.pinnedRows ? layout.pinned : layout.content;
    const OverlayRect& shown = open.editMenu ? layout.rows[index].value : layout.rows[index].control;
    if (!shown.visible() || shown.bottom() <= band.y || shown.y >= band.bottom()) {
        closeSettingsDropDown();
        return;
    }
    // A list hangs from its box; a menu from the pointer that opened it.
    const OverlayRect anchor = open.editMenu
        ? OverlayRect{static_cast<float>(open.at.x), static_cast<float>(open.at.y), 0.0F, 0.0F}
        : layout.rows[index].control;
    if (!open.editMenu) {
        // Its lines from the row as it stands, the chosen one ticked: an
        // output plugged in while it is open joins it. A menu keeps the
        // lines it opened with: the number cannot change under it.
        const auto& row = settings.rows[index];
        PanelMenu menu;
        for (std::size_t i = 0; i < row.options.size(); ++i) {
            menu.option(static_cast<unsigned>(i + 1), row.options[i], row.selected == static_cast<int>(i));
        }
        open.items = std::move(menu.items);
    }
    if (open.highlighted >= static_cast<int>(open.items.size())) open.highlighted = -1;
    if (open.pressed >= static_cast<int>(open.items.size())) open.pressed = -1;
    std::vector<float> labelWidths;
    std::vector<float> shortcutWidths;
    settings.overlay.measurePanelMenu(open.items, settings.scale, labelWidths, shortcutWidths);
    // A list as wide as its box, or its widest line where that is wider --
    // with no least width of a menu's, so not a little wider than a narrow
    // box -- a menu as wide as menus are; as tall as its lines; then hung
    // from the box's bottom-left on the screen, within the work area of the
    // box's monitor. With no window the client stands for the screen.
    PanelMenuMetrics fitted;
    if (!open.editMenu) fitted.minWidth = 0.0F;
    const PanelMenuLayout natural = panelMenuLayout(open.items, labelWidths, shortcutWidths,
                                                    {0.0F, 0.0F, anchor.width, anchor.height}, {}, settings.scale,
                                                    0.0F, PanelMenuPlacement::Below, fitted);
    OverlayRect box = anchor;
    OverlayRect work{0.0F, 0.0F, layout.sheet.right(), layout.sheet.bottom()};
    if (settings.window) {
        POINT origin{};
        ClientToScreen(settings.window, &origin);
        box.x += static_cast<float>(origin.x);
        box.y += static_cast<float>(origin.y);
        const RECT boxRect{std::lround(box.x), std::lround(box.y), std::lround(box.right()), std::lround(box.bottom())};
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromRect(&boxRect, MONITOR_DEFAULTTONEAREST), &monitor)) {
            work = overlayRect(monitor.rcWork);
        }
    }
    open.frame = panelMenuWindowFrame(box, work, natural.card.width, natural.card.height, settings.scale);
    open.layout = panelMenuLayout(open.items, labelWidths, shortcutWidths, {},
                                  {0.0F, 0.0F, open.frame.width, open.frame.height}, settings.scale, open.layout.scroll,
                                  PanelMenuPlacement::Fill);
    // Its window follows: the settings window moved, or the list grew.
    if (const HWND list = settings.listWindow) {
        const RECT wanted{std::lround(open.frame.x), std::lround(open.frame.y), std::lround(open.frame.right()),
                          std::lround(open.frame.bottom())};
        RECT now{};
        GetWindowRect(list, &now);
        if (!EqualRect(&now, &wanted)) {
            SetWindowPos(list, nullptr, wanted.left, wanted.top, wanted.right - wanted.left, wanted.bottom - wanted.top,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
    }
}

bool App::createSettingsListWindow() {
    auto& settings = settingsWindow_;
    if (settings.listWindow) return true;
    if (!settings.window) return false;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    // The shadow under a Windows menu.
    windowClass.style = CS_DROPSHADOW;
    windowClass.lpfnWndProc = &App::settingsListWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kSettingsListClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        appendDiagnostic("Settings list window class could not be registered");
        return false;
    }
    // A popup owned by the settings window, so it stays over it, that never
    // takes the keys and has no taskbar button.
    const HWND list = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kSettingsListClass, L"", WS_POPUP, 0, 0, 1,
                                      1, settings.window, nullptr, instance_, this);
    if (!list) {
        appendDiagnostic("Settings list window could not be created");
        return false;
    }
    // Windows 11's rounded corners, its frame in the card's hairline colour.
    const DWORD round = 2;
    const DWORD cornerAttribute = 33;
    const COLORREF frame = RGB(60, 64, 71);
    const DWORD frameAttribute = 34;
    settings.listFramed = SUCCEEDED(DwmSetWindowAttribute(list, cornerAttribute, &round, sizeof(round))) &&
                          SUCCEEDED(DwmSetWindowAttribute(list, frameAttribute, &frame, sizeof(frame)));
    return true;
}

LRESULT CALLBACK App::settingsListWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->settingsWindow_.listWindow = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleSettingsListMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleSettingsListMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    auto& settings = settingsWindow_;
    const HWND list = settings.listWindow;
    const auto lineAt = [&] {
        const auto& open = *settings.dropDown;
        return panelMenuItemAt(open.layout, open.items, static_cast<float>(GET_X_LPARAM(lParam)),
                               static_cast<float>(GET_Y_LPARAM(lParam)));
    };
    switch (message) {
    case WM_MOUSEACTIVATE:
        // A click on it leaves the keys, and the list, with the settings
        // window.
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        // The line under the pointer is lit; off the lines the last one
        // stays lit, for Enter.
        if (settings.dropDown) {
            const int line = lineAt();
            if (line >= 0) settings.dropDown->highlighted = line;
        }
        return 0;
    case WM_LBUTTONDOWN:
        // A press marks the line its release chooses.
        if (settings.dropDown) {
            auto& open = *settings.dropDown;
            open.pressed = lineAt();
            if (open.pressed >= 0) open.highlighted = open.pressed;
            if (list) SetCapture(list);
        }
        return 0;
    case WM_LBUTTONUP: {
        // Let go on the line it was pressed on, it chooses that line.
        if (!settings.dropDown) return 0;
        auto& open = *settings.dropDown;
        const int pressed = open.pressed;
        open.pressed = -1;
        if (list && GetCapture() == list) ReleaseCapture();
        if (pressed >= 0 && pressed == lineAt()) chooseSettingsDropDown(pressed);
        return 0;
    }
    case WM_RBUTTONUP:
        // On a spin box's menu the right button chooses as the left does, as
        // on the main window's menus (panelMenuRightClick); until 2026-11-07
        // it did nothing there. A list's lines take the left button alone.
        if (settings.dropDown && settings.dropDown->editMenu) {
            const int line = lineAt();
            if (line >= 0) chooseSettingsDropDown(line);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (settings.dropDown && reinterpret_cast<HWND>(lParam) != list) settings.dropDown->pressed = -1;
        return 0;
    case WM_MOUSEWHEEL:
        // The main loop scrolls it wherever the wheel is sent; this is for
        // one that reaches the window another way.
        scrollSettingsWindow(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_PAINT: {
        // Painted with the settings window, once a frame.
        PAINTSTRUCT paint{};
        BeginPaint(list, &paint);
        EndPaint(list, &paint);
        renderSettingsWindow();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        // With the settings window, which owns it.
        SetWindowLongPtrW(list, GWLP_USERDATA, 0);
        settings.listWindow = nullptr;
        settings.listSurface.release();
        return DefWindowProcW(list, message, wParam, lParam);
    default:
        return list ? DefWindowProcW(list, message, wParam, lParam) : 0;
    }
}

void App::chooseSettingsDropDown(int index) {
    auto& settings = settingsWindow_;
    if (!settings.dropDown) return;
    const int row = settings.dropDown->row;
    const bool editMenu = settings.dropDown->editMenu;
    const bool chosen = index >= 0 && index < static_cast<int>(settings.dropDown->items.size()) &&
                        panelMenuSelectable(settings.dropDown->items[static_cast<std::size_t>(index)]);
    const unsigned command = chosen ? settings.dropDown->items[static_cast<std::size_t>(index)].command : 0;
    // Closed first: what the choice does builds the rows again.
    closeSettingsDropDown();
    if (!chosen) return;
    if (editMenu) {
        if (const auto edit = fieldMenuEdit(command)) spinEditApply(*edit);
        return;
    }
    activateSettingsHit({PanelHitKind::Segment, row, index});
}

void App::settingsDropDownKey(WPARAM key) {
    auto& settings = settingsWindow_;
    if (!settings.dropDown) return;
    auto& open = *settings.dropDown;
    const PanelMenuMetrics metrics{};
    // A page is as many lines as the list shows.
    const int page = std::max(1, static_cast<int>(open.layout.view.height / (metrics.itemHeight * settings.scale)));
    int line = open.highlighted;
    switch (key) {
    case VK_ESCAPE:
        closeSettingsDropDown();
        return;
    case VK_RETURN:
    case VK_SPACE:
        if (line >= 0) chooseSettingsDropDown(line);
        else closeSettingsDropDown();
        return;
    case VK_UP:
    case VK_LEFT:
        line = panelMenuStep(open.items, line, -1);
        break;
    case VK_DOWN:
    case VK_RIGHT:
        line = panelMenuStep(open.items, line, 1);
        break;
    case VK_HOME:
        line = panelMenuStep(open.items, -1, 1);
        break;
    case VK_END:
        line = panelMenuStep(open.items, -1, -1);
        break;
    case VK_PRIOR:
    case VK_NEXT: {
        // A spin box's menu has no pages, as the main window's menus have
        // none (panelMenuKey): a page's jump landed on its greyed lines and
        // separator, and Enter there only closed it.
        if (open.editMenu) return;
        // A page's worth of lines, stopping at the first or the last.
        const int count = static_cast<int>(open.items.size());
        line = std::clamp((line < 0 ? 0 : line) + (key == VK_PRIOR ? -page : page), 0, std::max(0, count - 1));
        break;
    }
    default:
        return;
    }
    open.highlighted = line;
    open.layout.scroll = panelMenuScrollToShow(open.layout, line);
    layoutSettingsDropDown();
}

bool App::settingsWindowShortcut(WPARAM key, LPARAM lParam, bool alt) {
    if (keyActionFor(keyBindings_, keyChordOf(key, alt)) == KeyAction::Back) {
        if (!(lParam & (1LL << 30))) closeSettingsWindow();
        return true;
    }
    return runShortcut(key, lParam, alt);
}

bool App::pointerOverSettingsWindow(POINT screen) const {
    const HWND window = settingsWindow_.window;
    if (!window || !settingsWindow_.open || !IsWindowVisible(window)) return false;
    const HWND hit = WindowFromPoint(screen);
    return hit && GetAncestor(hit, GA_ROOT) == window;
}

LRESULT App::settingsWindowFrameHit(POINT screen) const {
    const HWND window = settingsWindow_.window;
    if (!window) return HTCLIENT;
    POINT local = screen;
    ScreenToClient(window, &local);
    RECT client{};
    GetClientRect(window, &client);
    const float band = static_cast<float>(resizeFrameThickness(windowDpi(window)));
    switch (settingsWindowFrameHitTest(static_cast<float>(local.x), static_cast<float>(local.y),
                                       static_cast<float>(client.right), band, 16.0F * settingsWindow_.scale,
                                       settingsWindow_.layout)) {
    case WindowFrameHit::Caption: return HTCAPTION;
    case WindowFrameHit::Top: return HTTOP;
    case WindowFrameHit::TopLeft: return HTTOPLEFT;
    case WindowFrameHit::TopRight: return HTTOPRIGHT;
    default: return HTCLIENT;
    }
}

}  // namespace quaddeck
