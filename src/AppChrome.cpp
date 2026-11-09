// App: the drawn interface. Builds the OverlayScene each frame from player
// state, hit-tests the pointer against OverlayLayout geometry, owns hover and
// drag state, the transport bar's reveal/auto-hide and notices.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <iterator>

namespace quaddeck {

using namespace app_internal;

namespace {

// How long a pane's chrome and the bar take to fade.
constexpr float kChromeFadeMs = 160.0F;
// Notices fade over their last stretch rather than vanishing.
constexpr ULONGLONG kNoticeFadeMs = 300;
constexpr float kRailInset = 12.0F;
constexpr float kBarSeekInset = 8.0F;
constexpr float kVolumeInset = 10.0F;
constexpr float kPanelSlideMs = 200.0F;
// A sheet's drop-down fades in and slides this far from its button.
constexpr float kPanelMenuFadeMs = 110.0F;
constexpr float kPanelMenuSlide = 6.0F;

// The pointer is on the top row of its monitor, as far up as a pointer
// pushed out of the way goes.
bool pointerAtScreenTop(POINT cursor) {
    MONITORINFO monitor{sizeof(monitor)};
    return GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor) &&
           cursor.y <= monitor.rcMonitor.top;
}

// A control's tooltip: its name, then the first key of the action it does
// as the viewer has the keys (App::withShortcut), then any further words.
const wchar_t* barItemName(BarItem item) {
    switch (item) {
    case BarItem::Previous: return L"Previous video";
    case BarItem::Next: return L"Next video";
    case BarItem::Play: return L"Play / Pause";
    case BarItem::Stop: return L"Stop";
    case BarItem::Audio: return L"Mute";
    case BarItem::Volume: return L"Master volume  (mouse wheel)";
    case BarItem::Subtitles: return L"Subtitles on / off";
    case BarItem::AllVideos: return L"All videos";
    case BarItem::SoloV1: return L"Solo V1 / back to grid";
    case BarItem::SoloV2: return L"Solo V2 / back to grid";
    case BarItem::SoloV3: return L"Solo V3 / back to grid";
    case BarItem::SoloV4: return L"Solo V4 / back to grid";
    case BarItem::SoloV5: return L"Solo V5 / back to grid";
    case BarItem::Sync: return L"Seek bars, offsets and repeat";
    case BarItem::Layout: return L"Arrangement of the videos";
    case BarItem::Sets: return L"Video sets";
    case BarItem::Fullscreen: return L"Fullscreen";
    default: return L"";
    }
}

std::optional<KeyAction> barItemKey(BarItem item) {
    switch (item) {
    case BarItem::Previous: return KeyAction::PreviousFile;
    case BarItem::Next: return KeyAction::NextFile;
    case BarItem::Play: return KeyAction::PlayPause;
    case BarItem::Audio: return KeyAction::Mute;
    case BarItem::Subtitles: return KeyAction::SubtitlesToggle;
    case BarItem::AllVideos: return KeyAction::AllVideos;
    case BarItem::Sets: return KeyAction::VideoSets;
    case BarItem::Fullscreen: return KeyAction::Fullscreen;
    default:
        if (const int position = barItemSoloPosition(item); position >= 0) {
            return soloKeyAction(static_cast<std::size_t>(position));
        }
        return std::nullopt;
    }
}

const wchar_t* captionItemName(CaptionItem item, bool maximized, bool fullscreen) {
    switch (item) {
    case CaptionItem::Minimize: return L"Minimize";
    case CaptionItem::Maximize:
        return fullscreen ? L"Exit fullscreen" : maximized ? L"Restore" : L"Maximize";
    case CaptionItem::Close: return L"Close  (Alt+F4)";
    default: return L"";
    }
}

const wchar_t* chipName(PaneChip chip) {
    switch (chip) {
    case PaneChip::Audio: return L"Output this video's audio";
    case PaneChip::Pause: return L"Pause / resume this video";
    case PaneChip::Solo: return L"Solo this video / back to grid";
    case PaneChip::Repeat: return L"Repeat this video  (Independent mode)";
    case PaneChip::Close: return L"Close this video";
    case PaneChip::Subtitles: return L"This video's subtitles on / off";
    default: return L"";
    }
}

constexpr const wchar_t* kRightClickToChoose = L"  \x00B7  right-click to choose";

// An offset as the sync panel reads it: signed, to the hundredth, its minus
// the steps' own; zero bare.
std::wstring syncOffsetText(double seconds) {
    if (std::abs(seconds) < 0.005) return L"0.00 s";
    wchar_t text[32]{};
    std::swprintf(text, std::size(text), L"%+.2f s", seconds);
    std::wstring result = text;
    if (!result.empty() && result.front() == L'-') result.front() = L'\x2212';
    return result;
}

// The sync panel's steps of an offset, and the wheel's a notch.
constexpr std::array<double, kSyncSteps> kSyncOffsetSteps{-1.0, -0.1, 0.1, 1.0};
constexpr double kSyncWheelStep = 0.1;

}  // namespace

// ---------------------------------------------------------------------------
// Setup

void App::createControls() {
    if (!overlay_.initialize(renderer_.device())) {
        appendDiagnostic("Direct2D overlay unavailable; the interface will not be drawn");
    }
    renderer_.setOverlayPainter([this](IDXGISurface* surface, bool transparentLayer) {
        overlay_.draw(surface, scene_, transparentLayer);
    });
    RECT client{};
    GetClientRect(window_, &client);
    dockAnimationTick_ = GetTickCount64();
    layoutControls(static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom));
    updateHoverControls();
}

void App::layoutControls(unsigned width, unsigned height) {
    contentHeight_ = height;
    if (videoWindow_) {
        SetWindowPos(videoWindow_, HWND_BOTTOM, 0, 0, static_cast<int>(width),
                     static_cast<int>(height), SWP_NOACTIVATE);
    }
}

void App::applyDpi(unsigned dpi) {
    uiScale_ = dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;
    if (!window_) return;
    // The drawn interface rescales itself from uiScale_ on the next frame;
    // only the measured pill widths are cached.
    pillWidths_.fill(-1.0F);
    if (IsIconic(window_)) return;  // WM_SIZE lays it out on restore.
    RECT client{};
    GetClientRect(window_, &client);
    layoutControls(static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom));
}

// Kept for their call sites: the drawn interface reads player state when the
// scene is built, so there is nothing to push into a control any more.
void App::updateControls() {}
void App::layoutHoverControls() {}

// ---------------------------------------------------------------------------
// Bar reveal and fading

void App::toggleControls() {
    controlsPinned_ = !controlsPinned_;
    controlsLastInteraction_ = GetTickCount64();
    setControlsVisible(true);
    showNotice(controlsPinned_ ? L"Control bar pinned" : L"Control bar auto-hides");
    updateTitle();
    scheduleAppSettingsSave();
}

void App::setControlsVisible(bool visible) {
    // The fades' clock is updateDockAnimation's alone: restarted here, a
    // change made every frame left them no time to move.
    controlsVisible_ = visible;
}

void App::updateDockAnimation() {
    const ULONGLONG now = GetTickCount64();
    if (dockAnimationTick_ == 0) dockAnimationTick_ = now;
    const float elapsed = static_cast<float>(std::min<ULONGLONG>(now - dockAnimationTick_, 50));
    dockAnimationTick_ = now;
    dockProgress_ = advanceOverlayAnimation(dockProgress_, controlsVisible_, elapsed, kDockAnimationMs);
    settingsAlpha_ = advanceOverlayAnimation(settingsAlpha_, settingsOpen_, elapsed, kPanelSlideMs);
    for (std::size_t pane = 0; pane < paneChromeAlpha_.size(); ++pane) {
        paneChromeAlpha_[pane] = advanceOverlayAnimation(
            paneChromeAlpha_[pane], paneChromeWanted_[pane], elapsed, kChromeFadeMs);
        paneTimelineAlpha_[pane] = advanceOverlayAnimation(
            paneTimelineAlpha_[pane], paneTimelineWanted_[pane], elapsed, kChromeFadeMs);
    }
}

void App::updateAutoHideControls() {
    if (!window_) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    updateAutoHideControls(cursor);
}

void App::updateAutoHideControls(POINT cursor) {
    if (!window_) return;
    POINT local = cursor;
    ScreenToClient(window_, &local);
    RECT client{};
    GetClientRect(window_, &client);
    // Over the settings window the pointer is not on the picture beneath.
    const bool inside = local.x >= 0 && local.y >= 0 && local.x < client.right && local.y < client.bottom &&
                        !pointerOverSettingsWindow(cursor);
    // The bar wakes when a moving pointer reaches the bottom strip, and while
    // it is up its whole band keeps it up.
    const int bandHeight = static_cast<int>(std::lround(BarMetrics{}.bandHeight * uiScale_));
    const bool inInteractiveBand = inside && pointerInDockInteractiveBand(
        local.y, client.bottom, dockProgress_, bandHeight, dp(kControlRevealZone));
    // The caption wakes the same way from the top edge, and while it is up
    // its strip keeps it up.
    const float captionZone = dockProgress_ > 0.05F ? captionInset() : static_cast<float>(dp(kControlRevealZone));
    const bool inCaptionBand = inside && static_cast<float>(local.y) < captionZone;
    // The "All" capsule wakes where it stands too, not only from the edge
    // below the strips.
    const bool onCapsuleSpot = inside && capsuleRevealWanted(barLayout_, static_cast<float>(local.x),
                                                             static_cast<float>(local.y), 8.0F * uiScale_);
    const ULONGLONG now = GetTickCount64();
    // An open menu or sync panel holds the bar and the pointer.
    if (panelMenu_ || syncPanel_) {
        pointerLastMoved_ = controlsLastInteraction_ = now;
        if (cursorHidden_) { ShowCursor(TRUE); cursorHidden_ = false; }
        return;
    }
    const bool moved = cursor.x != lastPointerScreen_.x || cursor.y != lastPointerScreen_.y;
    if (moved) {
        lastPointerScreen_ = cursor;
        pointerLastMoved_ = now;
        if (cursorHidden_) {
            ShowCursor(TRUE);
            cursorHidden_ = false;
        }
    } else if ((!inside || !playerForeground()) && cursorHidden_) {
        ShowCursor(TRUE);
        cursorHidden_ = false;
    }
    const bool editing = controlDrag_ != ControlDrag::None || draggingPane_ ||
                         pressedBarItem_ >= 0 || GetCapture() != nullptr;
    // A pointer resting on the caption's strip keeps it up in a window,
    // beside its buttons as on them: the strip is what moves the window, the
    // picture never does (paneDragHasTarget). Until 2026-11-07 it hid under a
    // still pointer, and a press where it had been was the picture's. Beside
    // the buttons only QuadDeck's own strip under the pointer holds it, not
    // another window over it -- one QuadDeck owns neither, as a dialog or a
    // settings list -- and not a pointer pushed against the top of the
    // screen out of the way, as over a maximized window or one snapped to
    // the top: that one, and fullscreen, where nothing moves the window,
    // still let the strip and the bar go. A button lit under the pointer --
    // lit only with QuadDeck's own window there (updateHoverControls) --
    // holds them anywhere, the screen's top and fullscreen too, as before
    // (hotCaptionItem_): Close in a maximized window's corner is where a
    // pointer is flung to close it.
    const bool onShownCaption = inside && !fullscreen_ && onCaptionStrip(local) && !pointerAtScreenTop(cursor) &&
                                pointerOnPlayer(cursor, false);
    if (controlDrag_ != ControlDrag::None || pressedBarItem_ >= 0 || hotBarItem_ >= 0 ||
        pressedCaptionItem_ >= 0 || hotCaptionItem_ >= 0 || onShownCaption) {
        controlsLastInteraction_ = now;
    }
    if (moved && !editing && (inInteractiveBand || inCaptionBand || onCapsuleSpot)) {
        controlsLastInteraction_ = now;
        setControlsVisible(true);
    }
    // PotPlayer hides its skin only while there is video to look at: an
    // empty window keeps its title and its controls, never hidden first
    // and shown again.
    const bool keepControls = controlsPinned_ || !anyPaneLoaded();
    if (!keepControls && !editing && controlsVisible_ &&
        now - controlsLastInteraction_ >= kControlHideDelayMs) {
        setControlsVisible(false);
    }
    if (keepControls) setControlsVisible(true);
    // The pointer holding the caption up in a window, below the screen's
    // top, is shown with it, so what holds the controls is seen -- hidden
    // already too, as when the strip comes up under a still pointer; until
    // 2026-11-07 it went after its delay, nothing lit. One that holds them
    // only by a lit button, at the screen's top or in fullscreen, still
    // hides after its delay, the button staying lit.
    if (onShownCaption && cursorHidden_) {
        ShowCursor(TRUE);
        cursorHidden_ = false;
    } else if (inside && playerForeground() && !cursorHidden_ && !editing && !onShownCaption &&
               now - pointerLastMoved_ >= kPointerHideDelayMs) {
        ShowCursor(FALSE);
        cursorHidden_ = true;
    }
}

// ---------------------------------------------------------------------------
// Geometry and hover

std::wstring App::paneLabel(std::size_t pane) const {
    return L"V" + std::to_wstring(positionForPane(pane) + 1);
}

float App::paneCoveredBottom(const RectF& cell, bool settled) const {
    if ((!settled && dockProgress_ <= 0.001F) || !barLayout_.row.visible()) return 0.0F;
    // The capsule is over the panes it is beside only.
    if (barLayout_.capsule &&
        (cell.x >= barLayout_.row.right() || cell.x + cell.width <= barLayout_.row.x)) return 0.0F;
    const float clearance = 8.0F * uiScale_;
    const float rowTop = barLayout_.row.y - clearance;
    const float cellBottom = cell.y + cell.height;
    if (cellBottom <= rowTop) return 0.0F;
    return (cellBottom - rowTop) * (settled ? 1.0F : dockProgress_);
}

float App::paneSubtitleClearance(std::size_t pane, bool settled) const {
    if (pane >= kMaxPanes) return 0.0F;
    const RectF& cell = chromeCells_[pane];
    const float bar = settled ? (dockProgress_ > 0.5F ? paneCoveredBottom(cell, true) : 0.0F)
                              : paneCoveredBottom(cell);
    // The strip stands on what the bar covers; the capsule stands over it.
    const OverlayRect& strip = paneChrome_[pane].strip;
    if (!strip.visible()) return bar;
    const float alpha = std::clamp(paneTimelineAlpha_[pane], 0.0F, 1.0F);
    const float lifted = strip.height * (settled ? (alpha > 0.5F ? 1.0F : 0.0F) : alpha);
    return barLayout_.capsule ? std::max(bar, lifted) : bar + lifted;
}

float App::captionAlpha() const {
    return std::max(dockProgress_, settingsAlpha_);
}

float App::captionInset() const {
    return CaptionMetrics{}.rowHeight * uiScale_;
}

bool App::onCaptionStrip(POINT local) const {
    return captionAlpha() > 0.05F &&
           captionLayout_.row.contains(static_cast<float>(local.x), static_cast<float>(local.y));
}

bool App::emptyHintShown() const {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (paneLogicallyLoaded(pane)) return false;
    }
    return true;
}

bool App::emptyOpenButtonActive() const {
    return emptyHintShown() && !sheetTakesPointer() && emptyHint_.open.visible();
}

bool App::emptyEmbyButtonActive() const {
    return emptyHintShown() && !sheetTakesPointer() && emptyHint_.emby.visible();
}

bool App::bottomTimelineVisible() const {
    return !severalPanesLoaded() || (perPaneTimelines() ? linkedBrowserSeekBars()
                                                       : activeSeekMode() == SeekMode::Linked);
}

bool App::paneDragHasTarget() const {
    const auto active = activePanes();
    int visible = 0;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (active[pane] && chromeCells_[pane].width > 0.0F && chromeCells_[pane].height > 0.0F) ++visible;
    }
    return visible > 1;
}

// The widths of a pane's longest time, whole and short, at the scale drawn
// now; the overlay keeps a string's width while it is asked for every
// frame. Until the overlay can measure they are 0, and the layout gives the
// time its full room.
void App::measurePaneTime(std::size_t pane, const std::wstring& whole, const std::wstring& shortened) {
    if (pane >= kMaxPanes) return;
    const bool ready = overlay_.ready();
    timeTextWidths_[pane] = ready ? overlay_.measureText(whole, OverlayTextStyle::Small, uiScale_) : 0.0F;
    timeShortTextWidths_[pane] = ready ? overlay_.measureText(shortened, OverlayTextStyle::Small, uiScale_) : 0.0F;
}

void App::refreshChromeGeometry(const RECT& client) {
    const auto aspects = paneAspectRatios();
    chromeCells_ = currentLayoutCells(
        static_cast<float>(client.right), static_cast<float>(client.bottom), aspects);
    captionLayout_ = captionLayout(static_cast<float>(client.right), static_cast<float>(client.bottom), uiScale_);
    // The docked Emby browser lies over the right part of the video; the
    // bar keeps to the uncovered part so its buttons stay reachable. The
    // capsule stays in the window's middle, moving left only to keep clear.
    barLayout_ = transportBarLayout(videoAreaWidth(static_cast<float>(client.right)),
                                    static_cast<float>(client.bottom), uiScale_, volumeOpen_,
                                    singleLoadedPane() >= 0, BarMetrics{}, bottomTimelineVisible(),
                                    severalPanesLoaded(), static_cast<float>(client.right) * 0.5F,
                                    soloButtonCount(), videoSetPlaying() >= 0, true);
    // Open file is always there; signed out there is no Open Emby beside
    // it: the hint's F6 and the menu still reach the sign-in page.
    emptyHint_ = emptyHintLayout(static_cast<float>(client.right), static_cast<float>(client.bottom), uiScale_,
                                 barLayout_.row.visible() ? barLayout_.row.y : static_cast<float>(client.bottom),
                                 emby_.session().signedIn());
    const auto active = activePanes();
    // Count loaded sources before Solo filters the visible cells. A single
    // source uses the master rail; Solo still needs its own strip.
    const bool onlyVideo = std::count(active.begin(), active.end(), true) <= 1;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!active[pane]) {
            paneChrome_[pane] = {};
            continue;
        }
        if (!onlyVideo) {
            const std::wstring label = paneLabel(pane);
            if (label != pillLabels_[pane] || pillWidths_[pane] < 0.0F) {
                pillLabels_[pane] = label;
                // Before the overlay can measure, about what "V1" measures.
                pillWidths_[pane] = overlay_.ready()
                    ? overlay_.measureText(label, OverlayTextStyle::BodyBold, uiScale_)
                    : 20.0F * uiScale_;
            }
        }
        PaneChromeOptions options;
        options.pillTextWidth = pillWidths_[pane];
        options.timeTextWidth = timeTextWidths_[pane];
        options.timeShortTextWidth = timeShortTextWidths_[pane];
        // The only video wears no strip: its tag and its repeat, close and
        // Solo are the right-click menu's, its rail, time, volume, sound,
        // subtitles and pause the bar's. Sound and pause come back while
        // they are off, as a pane silenced or paused beside others is left,
        // so it can be set back here: the bar's play does not resume it.
        // Videos that share one timeline seek on the bar's rail, which
        // marks where each ends; their strips are the row alone.
        options.withPill = options.withTime = !onlyVideo;
        options.withTimeline = !onlyVideo && !bottomTimelineVisible();
        options.withVolume = audioPaneEnabled(pane) && !onlyVideo;
        options.volumeOpen = paneVolumeOpen_ == static_cast<int>(pane);
        // The strips keep to their bottom edges under the capsule.
        options.coveredBottom = barLayout_.capsule ? 0.0F : paneCoveredBottom(chromeCells_[pane]);
        // Below the caption's strip whether or not it is up.
        options.coveredTop = std::max(0.0F, captionInset() - chromeCells_[pane].y);
        if (onlyVideo) {
            options.chipsShown.fill(false);
            options.chipsShown[static_cast<std::size_t>(PaneChip::Audio)] = !audioPaneEnabled(pane);
            options.chipsShown[static_cast<std::size_t>(PaneChip::Pause)] = sourcePaused_[pane];
        }
        // Rendering and hit testing share this geometry.
        paneChrome_[pane] = paneChromeLayout(chromeCells_[pane], uiScale_, options);
    }
    if (settingsOpen_ || settingsAlpha_ > 0.0F) refreshPanelGeometry(client);
    // Which lays an open menu out; one over the picture follows its button
    // without the sheet too.
    else layoutPanelMenu();
    layoutSyncPanel();
}

OverlayHit App::hitTestAt(POINT local) const {
    if (const int item = captionItemAt(static_cast<float>(local.x), static_cast<float>(local.y),
                                       captionLayout_, captionAlpha() > 0.05F);
        item >= 0) {
        return {OverlayHitKind::CaptionItem, -1, item};
    }
    if (emptyOpenButtonActive() &&
        emptyHint_.open.contains(static_cast<float>(local.x), static_cast<float>(local.y))) {
        return {OverlayHitKind::EmptyOpen, -1, -1};
    }
    if (emptyEmbyButtonActive() &&
        emptyHint_.emby.contains(static_cast<float>(local.x), static_cast<float>(local.y))) {
        return {OverlayHitKind::EmptyEmby, -1, -1};
    }
    PaneArray<bool> chromeActive{};
    PaneArray<bool> timelineActive{};
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        chromeActive[pane] = paneChromeAlpha_[pane] > 0.05F;
        timelineActive[pane] = paneTimelineWanted_[pane] && paneTimelineAlpha_[pane] > 0.05F;
    }
    return overlayHitTest(static_cast<float>(local.x), static_cast<float>(local.y),
                          barLayout_, dockProgress_ > 0.05F, paneChrome_, chromeActive,
                          chromeCells_, activePanes(), &timelineActive);
}

void App::updateHoverControls() {
    POINT cursor{};
    GetCursorPos(&cursor);
    updateHoverControls(cursor);
}

// The window under the pointer is QuadDeck's -- its own or a child of it --
// or, `owned`, one it owns too, such as a dialog it opened or a list the
// settings window drops down.
bool App::pointerOnPlayer(POINT cursor, bool owned) const {
    const HWND hit = WindowFromPoint(cursor);
    return hit && window_ && GetAncestor(hit, owned ? GA_ROOTOWNER : GA_ROOT) == window_;
}

// QuadDeck's window is the one in front, with the keys.
bool App::playerForeground() const {
    return window_ && GetForegroundWindow() == window_;
}

void App::updateHoverControls(POINT cursor) {
    if (!videoWindow_) return;
    POINT local = cursor;
    ScreenToClient(videoWindow_, &local);
    RECT client{};
    GetClientRect(videoWindow_, &client);
    const auto active = activePanes();
    const bool pointerFresh = GetTickCount64() - pointerLastMoved_ < kPointerHideDelayMs;
    // The settings window over the picture is not the picture: nothing
    // beneath it lights, and the right edge under it opens no browser.
    const bool pointerInsidePlayer = local.x >= 0 && local.y >= 0 &&
                                     local.x < client.right && local.y < client.bottom &&
                                     !pointerOverSettingsWindow(cursor);
    const bool playerForeground = GetForegroundWindow() == window_;
    refreshChromeGeometry(client);

    const bool barUp = dockProgress_ > 0.05F;
    // The bar's footprint is its row and the margin beneath it; the scrim
    // above the row is still the video. The capsule's is the capsule and its
    // volume: beside it, beneath it, the strips are the panes'.
    bool onCapsule = barLayout_.row.contains(static_cast<float>(local.x), static_cast<float>(local.y));
    for (const auto& item : barLayout_.items) {
        onCapsule = onCapsule || item.contains(static_cast<float>(local.x), static_cast<float>(local.y));
    }
    const bool overBar = barUp && barLayout_.row.visible() && pointerInsidePlayer &&
                         (barLayout_.capsule ? onCapsule : static_cast<float>(local.y) >= barLayout_.row.y);
    const bool overCaption = captionAlpha() > 0.05F && pointerInsidePlayer &&
                             captionLayout_.row.contains(static_cast<float>(local.x), static_cast<float>(local.y));
    // Keyboard targeting follows the pointer even while the bar hides the
    // chrome. Only the bar's and the caption's own footprints exclude a pane.
    const int geometricPane = overBar || overCaption ? -1 : activePaneAt(
        static_cast<float>(local.x), static_cast<float>(local.y),
        static_cast<float>(client.right), static_cast<float>(client.bottom), active,
        layoutMode_, expandedPane_, soloPane_, paneAspectRatios(),
        autoLayoutFocus_, autoLayoutFocusPane_);
    // The chrome fades with the pointer, but which pane was last pointed at
    // has to outlive that: a keyboard command aimed at "the pane I am looking
    // at" would otherwise stop working after the pointer sat still.
    if (geometricPane >= 0) lastPointerPane_ = geometricPane;
    // Under an open menu the pane it was opened over keeps its chrome.
    if (!panelMenu_) hoverPane_ = pointerFresh && pointerInsidePlayer ? geometricPane : -1;

    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        const bool eligible = active[pane] && chromeCells_[pane].width > 0.0F &&
                              chromeCells_[pane].height > 0.0F;
        const bool dragging = controlDrag_ == ControlDrag::PaneSeek &&
                              dragPane_ == static_cast<int>(pane);
        paneChromeWanted_[pane] = shouldShowPaneTimeline(
            eligible, pointerFresh, pointerInsidePlayer, playerForeground, dragging);
    }

    // The sheet, when up, takes the pointer before anything under
    // it; only the caption's strip stands above its column.
    PanelHit panelHit = pointerInsidePlayer && !overCaption ? panelHitAt(local) : PanelHit{};
    // A greyed row is the sheet's, lit by nothing, but its hint is shown.
    if (panelHit.kind == PanelHitKind::Sheet && panelHit.row < 0) {
        panelHit.row = panelGreyedRowAt(static_cast<float>(local.x), static_cast<float>(local.y), panelLayout_,
                                        panelRows_);
    }
    // Behind another window -- another program's, a dialog QuadDeck opened,
    // a settings list -- no row lights or shows its hint, as no bar button
    // does; until 2026-11-07 one did. The sheet still takes the pointer
    // there, so nothing beneath it lights either.
    if (panelHit.kind != PanelHitKind::None && panelHit.kind != PanelHitKind::Outside &&
        !pointerOnPlayer(cursor, false)) {
        panelHit = PanelHit{PanelHitKind::Sheet, -1, -1};
    }
    // An open menu holds the pointer: nothing under it lights, and the
    // button it hangs from stays lit until it closes.
    if (panelMenu_) {
        panelMenuHover(local);
        panelMenuTick();
    }
    // So does the sync panel: only its own controls light.
    if (syncPanel_) {
        syncPanel_->hot = pointerInsidePlayer ? syncPanelHitTest(syncPanel_->layout, static_cast<float>(local.x),
                                                                 static_cast<float>(local.y))
                                              : SyncPanelHit{};
    }
    if (panelMenu_ && panelMenu_->inSheet) {
        panelHit = panelMenu_->anchorRow >= 0
            ? PanelHit{PanelHitKind::Button, panelMenu_->anchorRow, panelMenu_->part}
            : PanelHit{PanelHitKind::Sheet, -1, -1};
    } else if (panelMenu_ || syncPanel_) {
        panelHit = sheetTakesPointer() ? PanelHit{PanelHitKind::Sheet, -1, -1} : PanelHit{};
    }
    const bool overSheet = panelHit.kind != PanelHitKind::None && panelHit.kind != PanelHitKind::Outside;
    const bool panelChanged = hotPanelRow_ != (overSheet ? panelHit.row : -1) ||
        hotPanelKind_ != (overSheet ? panelHit.kind : PanelHitKind::None) ||
        hotPanelPart_ != (overSheet ? panelHit.part : -1);
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        const int index = static_cast<int>(pane);
        const PaneChromeLayout& chrome = paneChrome_[pane];
        bool controls = chrome.timeline.visible() || chrome.volume.visible();
        for (const auto& chip : chrome.chips) controls = controls || chip.visible();
        // The pane's strip is up under the pointer, and held up by a drag or
        // a press on it, or a menu opened from it or over its pane.
        const bool held = ((controlDrag_ == ControlDrag::PaneSeek || controlDrag_ == ControlDrag::PaneVolume) &&
                           dragPane_ == index) ||
                          pressedChipPane_ == index || pressedPillPane_ == index ||
                          (panelMenu_ && !panelMenu_->inSheet && panelMenu_->pane == index);
        paneTimelineWanted_[pane] = paneTimelineRevealWanted(
            active[pane] && controls, pointerFresh, pointerInsidePlayer, playerForeground, held,
            geometricPane == index,
            overSheet || overBar || overCaption || panelMenu_.has_value() || syncPanel_.has_value());
    }
    hotPanelKind_ = overSheet ? panelHit.kind : PanelHitKind::None;
    hotPanelRow_ = overSheet ? panelHit.row : -1;
    hotPanelPart_ = overSheet ? panelHit.part : -1;
    if (controlDrag_ == ControlDrag::PanelSlider) {
        hotPanelKind_ = PanelHitKind::Slider;
        hotPanelRow_ = panelDragRow_;
    }
    if (controlDrag_ == ControlDrag::PanelScrollbar) hotPanelKind_ = PanelHitKind::Scrollbar;

    // Hot states. A drag keeps its own target hot regardless of the pointer.
    const OverlayHit hit = panelMenu_ ? panelMenuAnchorHit()
                         : syncPanel_ ? OverlayHit{OverlayHitKind::BarItem, -1, static_cast<int>(BarItem::Sync)}
                         : pointerInsidePlayer && !overSheet ? hitTestAt(local) : OverlayHit{};
    int hotBar = -1;
    int hotCaption = -1;
    int hotChipPane = -1;
    int hotChip = -1;
    int hotPill = -1;
    int hotRail = -1;
    int hotVolume = -1;
    // A bar or caption button lights from the pointer only with QuadDeck's
    // own window under it: one behind another window -- another program's,
    // a dialog QuadDeck opened, a settings list -- was lit there until
    // 2026-11-07, and, lit, held the strip and the bar up for good. The
    // button an open menu or the sync panel hangs from stays lit as before.
    // The empty window's two buttons light the same way, which until then
    // lit and named themselves behind another window.
    const auto lightsFromPointer = [&] { return panelMenu_ || syncPanel_ || pointerOnPlayer(cursor, false); };
    const bool hotEmptyOpen = hit.kind == OverlayHitKind::EmptyOpen && lightsFromPointer();
    const bool hotEmptyEmby = hit.kind == OverlayHitKind::EmptyEmby && lightsFromPointer();
    switch (hit.kind) {
    case OverlayHitKind::BarItem:
        if (lightsFromPointer()) hotBar = hit.item;
        break;
    case OverlayHitKind::CaptionItem:
        if (lightsFromPointer()) hotCaption = hit.item;
        break;
    case OverlayHitKind::PaneChip: hotChipPane = hit.pane; hotChip = hit.item; break;
    case OverlayHitKind::PanePill: hotPill = hit.pane; break;
    case OverlayHitKind::PaneVolume: hotVolume = hit.pane; break;
    case OverlayHitKind::PaneTimeline: hotRail = hit.pane; break;
    default: break;
    }
    if (controlDrag_ == ControlDrag::MasterSeek) hotBar = static_cast<int>(BarItem::Seek);
    if (controlDrag_ == ControlDrag::Volume) hotBar = static_cast<int>(BarItem::Volume);
    if (controlDrag_ == ControlDrag::PaneSeek) hotRail = dragPane_;
    if (controlDrag_ == ControlDrag::PaneVolume) hotVolume = dragPane_;
    const bool hotChanged = panelChanged || hotBar != hotBarItem_ || hotCaption != hotCaptionItem_ || hotChipPane != hotChipPane_ ||
                            hotChip != hotChip_ || hotPill != hotPillPane_ || hotRail != hotRailPane_ ||
                            hotVolume != hotVolumePane_ || hotEmptyOpen != hotEmptyOpen_ ||
                            hotEmptyEmby != hotEmptyEmby_;
    hotBarItem_ = hotBar;
    hotCaptionItem_ = hotCaption;
    hotEmptyOpen_ = hotEmptyOpen;
    hotEmptyEmby_ = hotEmptyEmby;
    hotChipPane_ = hotChipPane;
    hotChip_ = hotChip;
    hotPillPane_ = hotPill;
    hotRailPane_ = hotRail;
    hotVolumePane_ = hotVolume;
    seekHot_ = hotBar == static_cast<int>(BarItem::Seek);
    lastLocalPointer_ = local;
    pointerInsidePlayer_ = pointerInsidePlayer;
    // Only QuadDeck under the pointer, not its rectangle: another window over
    // the right edge neither opens the browser behind it nor holds one open.
    embyEdgeHover(local, client, pointerInsidePlayer && pointerOnPlayer(cursor));

    // The volume rail unfolds while the pointer is on the speaker or the rail
    // itself, and stays out for a drag.
    const bool wantVolume = hotBar == static_cast<int>(BarItem::Audio) ||
                            hotBar == static_cast<int>(BarItem::Volume) ||
                            controlDrag_ == ControlDrag::Volume;
    // A heard video's volume unfolds beside its speaker the same way, or on
    // a capsule stands on it. Its rail reaches the speaker, so the pointer
    // crosses no gap between them.
    int wantPaneVolume = controlDrag_ == ControlDrag::PaneVolume ? dragPane_ : hotVolume;
    if (wantPaneVolume < 0 && hotChipPane >= 0 && hotChip == static_cast<int>(PaneChip::Audio) &&
        audioPaneEnabled(static_cast<std::size_t>(hotChipPane))) {
        wantPaneVolume = hotChipPane;
    }
    if (wantVolume != volumeOpen_ || wantPaneVolume != paneVolumeOpen_) {
        volumeOpen_ = wantVolume;
        paneVolumeOpen_ = wantPaneVolume;
        refreshChromeGeometry(client);
    }

    const ULONGLONG now = GetTickCount64();
    if (hotChanged) hotSince_ = now;
    tooltipText_.clear();
    tooltipAnchor_ = {};
    if (controlDrag_ == ControlDrag::None && pressedBarItem_ < 0 && pressedCaptionItem_ < 0 &&
        !pressedEmptyOpen_ && !pressedEmptyEmby_ && !panelMenu_ && !syncPanel_ && now - hotSince_ >= kTooltipDelayMs) {
        if (overSheet) {
            const int row = panelHit.row;
            if (row >= 0 && row < static_cast<int>(panelRows_.size()) &&
                row < static_cast<int>(panelLayout_.rows.size())) {
                tooltipText_ = panelRows_[row].hint;
                tooltipAnchor_ = panelLayout_.rows[row].row;
            }
        } else if (hotCaption >= 0) {
            tooltipText_ = captionItemName(static_cast<CaptionItem>(hotCaption), window_ && IsZoomed(window_),
                                           fullscreen_);
            if (fullscreen_ && hotCaption == static_cast<int>(CaptionItem::Maximize)) {
                tooltipText_ = withShortcut(tooltipText_, KeyAction::Fullscreen);
            }
            tooltipAnchor_ = captionLayout_.items[static_cast<std::size_t>(hotCaption)];
        } else if (hotEmptyOpen) {
            tooltipText_ = withShortcut(L"Open video files", KeyAction::OpenFiles);
            tooltipAnchor_ = emptyHint_.open;
        } else if (hotEmptyEmby) {
            tooltipText_ = withShortcut(L"Emby browser", KeyAction::Emby);
            tooltipAnchor_ = emptyHint_.emby;
        } else if (hotBar >= 0 && hotBar != static_cast<int>(BarItem::Seek) &&
            hotBar != static_cast<int>(BarItem::Time)) {
            const auto item = static_cast<BarItem>(hotBar);
            tooltipText_ = barItemName(item);
            if ((item == BarItem::Previous || item == BarItem::Next) && videoSetPlaying() >= 0) {
                tooltipText_ = item == BarItem::Previous ? L"Previous video set" : L"Next video set";
            }
            if (const auto key = barItemKey(item)) tooltipText_ = withShortcut(tooltipText_, *key);
            if (item == BarItem::Subtitles) tooltipText_ += kRightClickToChoose;
            if (singleLoadedPane() < 0 && anyPaneLoaded() &&
                (hotBar == static_cast<int>(BarItem::Play) || hotBar == static_cast<int>(BarItem::Stop) ||
                 hotBar == static_cast<int>(BarItem::Audio) || hotBar == static_cast<int>(BarItem::Volume))) tooltipText_ = L"All panes: " + tooltipText_;
            tooltipAnchor_ = barLayout_.items[static_cast<std::size_t>(hotBar)];
        } else if (hotChip >= 0 && hotChipPane >= 0) {
            const bool repeatChip = static_cast<PaneChip>(hotChip) == PaneChip::Repeat;
            tooltipText_ = repeatChip && singleLoadedPane() >= 0
                ? L"Repeat this video when it ends"
                : repeatChip && !paneRepeatFlagInForce(paneOrigins(), static_cast<std::size_t>(hotChipPane), seekMode_)
                ? L"Repeat this video  (not beside F6 or Emby videos)"
                : repeatChip && linkedBrowserSeekBars()
                ? L"Repeat with the others once all have ended  (Linked)"
                : chipName(static_cast<PaneChip>(hotChip));
            // This video's own audio and Solo keys, by its place on the screen.
            if (static_cast<PaneChip>(hotChip) == PaneChip::Audio) {
                const auto position = positionForPane(static_cast<std::size_t>(hotChipPane));
                tooltipText_ = withShortcut(tooltipText_, audioKeyAction(position));
            } else if (static_cast<PaneChip>(hotChip) == PaneChip::Solo) {
                const auto position = positionForPane(static_cast<std::size_t>(hotChipPane));
                tooltipText_ = withShortcut(tooltipText_, soloKeyAction(position));
            } else if (static_cast<PaneChip>(hotChip) == PaneChip::Subtitles) {
                tooltipText_ = withShortcut(tooltipText_, KeyAction::SubtitlesToggle) + kRightClickToChoose;
            }
            tooltipAnchor_ = paneChrome_[static_cast<std::size_t>(hotChipPane)]
                .chips[static_cast<std::size_t>(hotChip)];
        } else if (hotPill >= 0) {
            // Alone (Solo) it has no pane beside it to swap with.
            tooltipText_ = paneDragHasTarget() ? L"Video menu  ·  drag the picture to swap panes" : L"Video menu";
            tooltipAnchor_ = paneChrome_[static_cast<std::size_t>(hotPill)].pill;
        } else if (hotVolume >= 0) {
            tooltipText_ = L"This video's volume";
            tooltipAnchor_ = paneChrome_[static_cast<std::size_t>(hotVolume)].volume;
        }
    }
}

// ---------------------------------------------------------------------------
// The scene

void App::buildOverlayScene() {
    RECT client{};
    GetClientRect(videoWindow_, &client);
    OverlayScene& scene = scene_;
    scene.width = static_cast<float>(client.right);
    scene.height = static_cast<float>(client.bottom);
    scene.scale = uiScale_;
    const auto active = activePanes();
    scene.activePaneCount = static_cast<int>(std::count(active.begin(), active.end(), true));
    const bool anyLoaded = !emptyHintShown();
    scene.anyLoaded = anyLoaded;
    scene.showEmptyHint = !anyLoaded;
    if (!anyLoaded) scene.emptyHintKeys = emptyHintKeys();
    scene.emptyHint = emptyHint_;
    // The sheet lies over the buttons, the browser over the whole window.
    scene.emptyButtonsAlpha = 1.0F - settingsAlpha_;
    scene.emptyOpenHot = hotEmptyOpen_;
    scene.emptyOpenPressed = pressedEmptyOpen_;
    scene.emptyEmbyHot = hotEmptyEmby_;
    scene.emptyEmbyPressed = pressedEmptyEmby_;

    scene.bar = barLayout_;
    scene.barAlpha = dockProgress_;
    scene.scopeText = L"All";
    scene.playing = playbackIntended();
    scene.muted = audio_.muted();
    scene.fullscreen = fullscreen_;
    scene.videoSetPlaying = videoSetPlaying() >= 0;
    scene.seekable = duration_ > 0.0;
    scene.volume01 = std::clamp(audio_.volume() / maxMasterVolume(), 0.0F, 1.0F);
    scene.volumeOpen = volumeOpen_;
    scene.volumeDragging = controlDrag_ == ControlDrag::Volume;
    const double timeline = clock_.position();
    const auto barShown = barTime();
    const double shownTimeline = barShown.position;
    const double shownDuration = barShown.duration;
    scene.seekable = barLayout_[BarItem::Seek].visible() && shownDuration > 0.0;
    const double shownMaster = controlDrag_ == ControlDrag::MasterSeek && shownDuration > 0.0
        ? dragFraction_ * shownDuration : shownTimeline;
    scene.seek01 = shownDuration > 0.0
        ? static_cast<float>(std::clamp(shownMaster / shownDuration, 0.0, 1.0)) : 0.0F;
    // One video's own rail, or the one video's, is in its colour.
    const int seekPane = barShown.pane >= 0 ? barShown.pane : singleLoadedPane();
    scene.seekPosition = seekPane >= 0 ? static_cast<int>(positionForPane(static_cast<std::size_t>(seekPane))) : -1;
    // The rail several videos share marks where each of them ends: on the
    // master timeline, or in source time where the browser's videos align.
    scene.syncLinked = severalPanesLoaded() && bottomTimelineVisible();
    scene.soloPosition = soloPane_ >= 0 && static_cast<std::size_t>(soloPane_) < kMaxPanes &&
                                 active[static_cast<std::size_t>(soloPane_)]
        ? static_cast<int>(positionForPane(static_cast<std::size_t>(soloPane_))) : -1;
    scene.seekEnds.fill(-1.0F);
    if (scene.syncLinked && scene.seekable) {
        const bool sourceTimes = linkedBrowserSeekBars();
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (!sources_[pane] || !sources_[pane]->ready() || sources_[pane]->duration() <= 0.0) continue;
            const double end = sourceTimes ? sources_[pane]->duration() - browserOffsets_[pane]
                : timelineDuration(sources_[pane]->duration(), startDelays_[pane], syncAdjustments_[pane],
                                   playbackRates_[pane]);
            const double fraction = end / shownDuration;
            // The longest ends with the rail itself.
            if (fraction < 0.999) scene.seekEnds[positionForPane(pane)] = static_cast<float>(std::max(0.0, fraction));
        }
    }
    scene.seekHot = seekHot_;
    scene.seekDragging = controlDrag_ == ControlDrag::MasterSeek;
    scene.seekHoverX = -1.0F;
    scene.seekHoverText.clear();
    if ((scene.seekHot || scene.seekDragging) && shownDuration > 0.0 && barLayout_[BarItem::Seek].visible()) {
        const float x = scene.seekDragging
            ? barLayout_[BarItem::Seek].x + kBarSeekInset * uiScale_ +
                  (barLayout_[BarItem::Seek].width - 2.0F * kBarSeekInset * uiScale_) * dragFraction_
            : static_cast<float>(lastLocalPointer_.x);
        const float fraction = scene.seekDragging ? dragFraction_ : railFraction(
            static_cast<float>(lastLocalPointer_.x), barLayout_[BarItem::Seek], kBarSeekInset * uiScale_);
        scene.seekHoverX = x;
        scene.seekHoverText = (singleLoadedPane() < 0 ? L"All: " : L"") + formatTime(fraction * shownDuration);
    }
    scene.timeText = barLayout_[BarItem::Time].visible()
        ? formatTime(shownMaster) + L" / " + formatTime(std::max(0.0, shownDuration))
        : L"";
    scene.hotBarItem = hotBarItem_;
    scene.pressedBarItem = pressedBarItem_;
    scene.caption = captionLayout_;
    scene.captionAlpha = captionAlpha();
    scene.captionTitle = windowTitle_;
    scene.maximized = window_ && IsZoomed(window_);
    scene.hotCaptionItem = hotCaptionItem_;
    scene.pressedCaptionItem = pressedCaptionItem_;
    scene.subtitleSize = subtitleSettings_.size;
    scene.subtitlePosition = subtitleSettings_.position;
    scene.subtitleBackground = subtitleSettings_.background;
    // The bar's switch is the only video's; beside others each has its own.
    scene.subtitlesOn = subtitlesSwitchedOn(singleLoadedPane());
    scene.subtitleLocale = subtitleLocale();

    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        OverlayPaneScene& out = scene.panes[pane];
        out = {};
        out.active = active[pane];
        // A closed pane's libass goes, with the fonts its video brought.
        if (pane >= sources_.size() || !sources_[pane]) assPanes_[pane] = {};
        if (!out.active) continue;
        out.cell = chromeCells_[pane];
        out.chrome = paneChrome_[pane];
        out.chromeAlpha = paneChromeAlpha_[pane];
        out.timelineAlpha = paneTimelineAlpha_[pane];
        out.hovered = static_cast<int>(pane) == hoverPane_;
        out.targeted = pane == embyPlaybackTarget();
        out.position = static_cast<int>(positionForPane(pane));
        out.label = pillLabels_[pane];
        out.audioOn = audioPaneEnabled(pane);
        out.paused = sourcePaused_[pane];
        out.solo = soloPane_ == static_cast<int>(pane);
        // One video's repeat is what happens when it ends; among several
        // it is the video's own flag.
        out.repeat = singleLoadedPane() == static_cast<int>(pane) ? playOrder_ == PlayOrder::RepeatOne
                                                                   : sourceAutoRepeat_[pane];
        out.repeatIdle = paneRepeatSavedButIdle(pane);
        const bool usable = sources_[pane] && sources_[pane]->ready() && sources_[pane]->duration() > 0.0;
        out.seekable = usable;
        if (usable) {
            const double sourceDuration = sources_[pane]->duration();
            const bool isActive = sourceIsActive(timeline, startDelays_[pane],
                                                 syncAdjustments_[pane], playbackRates_[pane]);
            out.waiting = !isActive;
            double shown = 0.0;
            if (controlDrag_ == ControlDrag::PaneSeek && dragPane_ == static_cast<int>(pane)) {
                shown = dragFraction_ * sourceDuration;
            } else if (isActive) {
                shown = std::clamp(mappedSourceTime(pane, timeline), 0.0, sourceDuration);
            }
            out.position01 = static_cast<float>(shown / sourceDuration);
            if (subtitlesShownFor(subtitleSettings_, pane) && isActive) {
                const double at = shown - subtitleDelay_[pane];
                // As their script draws them, where libass can.
                bool drawn = false;
                POINT origin{};
                out.subtitleImage = renderAssSubtitles(pane, at, drawn, origin);
                out.subtitleImageX = origin.x;
                out.subtitleImageY = origin.y;
                if (!drawn) {
                    // Plain lines otherwise. A stream inside the file is
                    // asked of the source, which is still reading it;
                    // anything else was parsed whole.
                    SubtitleLines lines;
                    if (subtitleSelection_[pane].kind == SubtitleKind::Embedded) {
                        lines = sources_[pane]->subtitleLinesAt(at);
                    } else if (subtitles_[pane]) {
                        lines = subtitleLinesAt(*subtitles_[pane], at);
                    }
                    out.subtitle = std::move(lines.bottom);
                    out.subtitleTop = std::move(lines.top);
                    out.subtitleLift = paneSubtitleClearance(pane);
                }
            }
            const auto cache = sources_[pane]->cacheStats();
            if (cache.enabled && cache.aheadSeconds > 0.0) {
                out.buffered01 = static_cast<float>(
                    std::min(sourceDuration, shown + cache.aheadSeconds) / sourceDuration);
            }
            // The row's time, without its duration where the row is narrow.
            const std::wstring waiting = isActive ? L"" : L"wait  ";
            out.timeText = waiting + formatTime(shown) +
                (paneChrome_[pane].timeShort ? L"" : L" / " + formatTime(sourceDuration));
            // Its longest, the duration twice, for the room it takes on a
            // capsule: the time never reads longer, so nothing after it moves.
            const std::wstring whole = formatTime(sourceDuration);
            measurePaneTime(pane, waiting + whole + L" / " + whole, waiting + whole);
            // Over the rail, where the pointer is on it or drags it: the time
            // there, and what a seek there reaches -- this video or all.
            const OverlayRect& rail = paneChrome_[pane].timeline;
            const bool dragging = controlDrag_ == ControlDrag::PaneSeek && dragPane_ == static_cast<int>(pane);
            if ((dragging || hotRailPane_ == static_cast<int>(pane)) && rail.visible()) {
                const float inset = kRailInset * uiScale_;
                const float fraction = dragging
                    ? dragFraction_ : railFraction(static_cast<float>(lastLocalPointer_.x), rail, inset);
                const double target = fraction * sourceDuration;
                out.railX = rail.x + inset + (rail.width - 2.0F * inset) * fraction;
                out.railText = (paneSeekAffectsAll(pane, target) ? std::wstring(L"All linked  ")
                                                                 : paneLabel(pane) + L" only  ") +
                               formatTime(target);
            }
        } else {
            out.timeText = L"\x2014";
            measurePaneTime(pane, out.timeText, out.timeText);
        }
        out.subtitlesOn = subtitlesSwitchedOn(static_cast<int>(pane));
        out.railHot = hotRailPane_ == static_cast<int>(pane);
        out.railDragging = controlDrag_ == ControlDrag::PaneSeek && dragPane_ == static_cast<int>(pane);
        out.hotChip = hotChipPane_ == static_cast<int>(pane) ? hotChip_ : -1;
        out.pressedChip = pressedChipPane_ == static_cast<int>(pane) ? pressedChip_ : -1;
        out.pillHot = hotPillPane_ == static_cast<int>(pane);
        out.pillPressed = pressedPillPane_ == static_cast<int>(pane);
        out.volume01 = controlDrag_ == ControlDrag::PaneVolume && dragPane_ == static_cast<int>(pane)
            ? dragFraction_ : std::clamp(audio_.paneVolume(pane), 0.0F, 1.0F);
        out.volumeHot = hotVolumePane_ == static_cast<int>(pane);
        out.volumeDragging = controlDrag_ == ControlDrag::PaneVolume && dragPane_ == static_cast<int>(pane);
        out.dragSource = draggingPane_ && dragSourcePane_ == static_cast<int>(pane);
        out.dropTarget = draggingPane_ && dropTargetPane_ == static_cast<int>(pane) &&
                         dropTargetPane_ != dragSourcePane_;
    }

    scene.notice = noticeText_;
    scene.noticeAlpha = 0.0F;
    if (noticeUntil_ > 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG remaining = noticeUntil_ > now ? noticeUntil_ - now : 0;
        scene.noticeAlpha = std::min(1.0F, static_cast<float>(remaining) / static_cast<float>(kNoticeFadeMs));
    }
    scene.tooltip = tooltipText_;
    scene.tooltipAnchor = tooltipAnchor_;

    OverlayPanelScene& panel = scene.panel;
    panel.alpha = settingsAlpha_;
    panel.title = embyBrowserOpen_ ? (browserSource_ == BrowserSource::Local ? L"Folder" : L"Emby") : L"Emby";
    panel.thumbnails = &thumbnails_;
    // App's rows and layout, not a copy: a long list would cost a copy of
    // every row each frame.
    const bool shown = settingsAlpha_ > 0.0F;
    panel.sharedLayout = shown ? &panelLayout_ : nullptr;
    panel.sharedRows = shown ? &panelRows_ : nullptr;
    // The tabs are the settings window's; the sheet has none.
    panel.tabs.clear();
    panel.tabGlyphs.clear();
    panel.tab = -1;
    panel.switchCaptions.clear();
    for (const auto face : sheetSwitches()) panel.switchCaptions.emplace_back(sheetFaceCaption(face));
    panel.hotKind = hotPanelKind_;
    panel.hotRow = hotPanelRow_;
    panel.hotPart = hotPanelPart_;
    panel.pressedKind = pressedPanelKind_;
    panel.pressedRow = pressedPanelRow_;
    panel.pressedPart = pressedPanelPart_;
    panel.dragRow = controlDrag_ == ControlDrag::PanelSlider ? panelDragRow_ : -1;
    panel.scrollbarHot = hotPanelKind_ == PanelHitKind::Scrollbar;
    panel.scrollbarDragging = controlDrag_ == ControlDrag::PanelScrollbar;
    if (shown) panel.scrollMarks = panelScrollMarks_;
    else panel.scrollMarks.clear();

    OverlayPanelMenuScene& menu = scene.panelMenu;
    if (panelMenu_ && (shown || !panelMenu_->inSheet)) {
        const auto& open = *panelMenu_;
        const ULONGLONG now = GetTickCount64();
        const auto fadeIn = [&](ULONGLONG since) {
            const float t = std::clamp(static_cast<float>(now - since) / kPanelMenuFadeMs, 0.0F, 1.0F);
            return 1.0F - (1.0F - t) * (1.0F - t);
        };
        const float eased = fadeIn(open.openedAt);
        // The sheet's drop-down fades with the sheet.
        const float alpha = open.inSheet ? settingsAlpha_ : 1.0F;
        menu.items = open.menu.items;
        menu.layout = open.layout;
        menu.anchor = open.anchor;
        menu.bounds = panelMenuBounds();
        menu.placement = open.placement;
        menu.metrics = open.metrics;
        menu.highlighted = open.highlighted;
        menu.pressed = open.pressedDepth == 0 ? open.pressed : -1;
        menu.position = open.pane >= 0 && open.pane < static_cast<int>(kMaxPanes)
            ? static_cast<int>(positionForPane(static_cast<std::size_t>(open.pane))) : -1;
        menu.alpha = eased * alpha;
        // Out of its button: down from one above it, up from one below.
        menu.offset = (1.0F - eased) * kPanelMenuSlide * uiScale_ * (open.layout.above ? 1.0F : -1.0F);
        // Its submenus fade in where they stand.
        menu.submenus.clear();
        for (std::size_t depth = 1; depth <= open.submenus.size(); ++depth) {
            const auto& submenu = open.submenus[depth - 1];
            menu.submenus.push_back({submenu.from, submenu.layout, submenu.highlighted,
                                     open.pressedDepth == depth ? open.pressed : -1,
                                     fadeIn(submenu.openedAt) * alpha});
        }
    } else {
        menu = {};
    }

    OverlaySyncPanelScene& sync = scene.syncPanel;
    sync = {};
    if (syncPanel_) {
        const auto& open = *syncPanel_;
        const float t = std::clamp(static_cast<float>(GetTickCount64() - open.openedAt) / kPanelMenuFadeMs, 0.0F, 1.0F);
        const float eased = 1.0F - (1.0F - t) * (1.0F - t);
        sync.layout = open.layout;
        sync.alpha = eased;
        // It rises out of its key, as a menu above its button comes down.
        sync.offset = (1.0F - eased) * kPanelMenuSlide * uiScale_;
        const SeekMode mode = perPaneTimelines() ? browserSeekMode_ : seekMode_;
        sync.mode = static_cast<int>(mode);
        sync.hint = mode == SeekMode::Independent ? L"Each video seeks on the rail along its own bottom edge."
                  : perPaneTimelines() ? L"The bottom rail sets every video to one time."
                  : L"One rail, at the bottom, moves every video together.";
        const bool independent = activeSeekMode() == SeekMode::Independent;
        for (std::size_t index = 0; index < open.layout.rowCount; ++index) {
            const std::size_t pane = open.panes[index];
            auto& row = sync.rows[index];
            row.position = static_cast<int>(positionForPane(pane));
            row.label = paneLabel(pane);
            const double offset = paneOffsetSeconds(pane);
            row.offsetSet = offset != 0.0;
            row.offset = syncOffsetText(offset);
            row.repeat = sourceAutoRepeat_[pane];
            row.repeatInForce = independent && !paneRepeatSavedButIdle(pane) &&
                                paneRepeatFlagInForce(paneOrigins(), pane, seekMode_);
        }
        sync.resetEnabled = anyPaneOffset();
        sync.resetKey = shortcut(KeyAction::ResetOffsets);
        sync.footHint = independent ? L"Wheel on a row: \x00B1" L"0.1 s" : L"Repeat needs Independent";
        sync.hot = open.hot;
        sync.pressed = open.pressing ? open.pressed : SyncPanelHit{};
    }
}

// ---------------------------------------------------------------------------
// Notices

void App::showNotice(const std::wstring& text, ULONGLONG milliseconds) {
    noticeText_ = text;
    noticeUntil_ = GetTickCount64() + milliseconds;
}

void App::expireNotice(ULONGLONG now) {
    if (noticeUntil_ && now >= noticeUntil_) {
        noticeUntil_ = 0;
        noticeText_.clear();
    }
    // A build's WARP fallback, once run() has loaded the settings and a
    // session and queued the launch's files, none of them is left to open
    // and no notice is up: after the launch's or the rebuild's own notices,
    // for its whole time.
    if (warpFallbackNoticeDue_ && noticeText_.empty() && externalOpenReady_ &&
        std::none_of(externalOpenBatches_.begin(), externalOpenBatches_.end(),
                     [](const ExternalOpenBatch& batch) { return batch.startup; })) {
        warpFallbackNoticeDue_ = false;
        noticeWarpFallback();
    }
}

void App::toggleMute() {
    audio_.setMuted(!audio_.muted());
    showNotice(audio_.muted() ? L"Muted" : L"Unmuted");
    updateTitle();
    scheduleAppSettingsSave();
}

void App::toggleSolo(int pane) {
    if (pane < 0 || pane >= static_cast<int>(kMaxPanes)) return;
    soloPane_ = soloPane_ == pane ? -1 : pane;
    showNotice(soloPane_ >= 0
        ? L"Solo V" + std::to_wstring(positionForPane(static_cast<std::size_t>(pane)) + 1)
        : L"Back to grid");
}

std::size_t App::soloButtonCount() const {
    const auto active = activePanes();
    const auto count = static_cast<std::size_t>(std::count(active.begin(), active.end(), true));
    return count >= 2 ? count : 0;
}

void App::soloVideoAt(std::size_t position) {
    // The loaded videos have the first places (panesByPosition), numbered
    // as in the grid whatever is alone now.
    if (position >= soloButtonCount()) return;
    const int pane = static_cast<int>(paneForPosition(position));
    if (soloPane_ == pane) {
        showAllVideos();
        return;
    }
    soloPane_ = pane;
    showNotice(L"Solo V" + std::to_wstring(position + 1));
}

void App::showAllVideos() {
    if (soloPane_ < 0) return;
    soloPane_ = -1;
    showNotice(L"Back to grid");
}

// ---------------------------------------------------------------------------
// Seeking from the drawn rails

bool App::paneSeekAffectsAll(std::size_t pane, double target) const {
    if (pane >= sources_.size() || !sources_[pane] || sources_[pane]->duration() <= 0.0) return false;
    if (linkedBrowserSeekBars()) return true;
    return activeSeekMode() == SeekMode::Linked &&
        sourceIsActive(clock_.position(), startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]) &&
        !sourcePaused_[pane] && linkedTimelineCanReachSourceTime(target, syncAdjustments_[pane]);
}

void App::seekFromSourceBar(std::size_t pane, double target) {
    if (pane >= sources_.size() || !sources_[pane] || sources_[pane]->duration() <= 0.0) return;
    const double sourceDuration = sources_[pane]->duration();
    target = std::clamp(target, 0.0, sourceDuration);

    if (linkedBrowserSeekBars()) {
        // This video at that time, the others at it less its offset.
        alignBrowserPanesToTime(target - browserOffsets_[pane]);
        return;
    }

    if (paneSeekAffectsAll(pane, target)) {
        // Dragging a rail is coarse positioning. Avoid waiting for every
        // pane to finish exact decoding before showing the requested position.
        seekAbsolute(timelineForSourceTime(
            target, startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]),
            SeekStyle::Fast);
        return;
    }
    // A waiting or paused source is always shifted directly, even in Linked
    // mode. The same applies when a positive Offset makes the chosen source
    // time earlier than anything the non-negative master clock can represent.
    seekPaneTo(pane, target);
}

App::BarTime App::barTime() const {
    BarTime shown{clock_.position(), duration_, -1};
    if (linkedBrowserSeekBars()) {
        // In browser-linked seek mode the bottom rail offers source seconds,
        // not the master clock that predates separately added videos.
        // Until a video is ready, neither its time nor the aggregate master
        // extent is a usable range for this rail.
        shown.position = 0.0;
        shown.duration = 0.0;
        if (const int reference = linkedBrowserReferencePane(); reference >= 0) {
            // Its own time less its offset is where it stands among the others,
            // counted from before its start while it waits.
            const auto index = static_cast<std::size_t>(reference);
            const double own = std::min(mappedSourceTime(index, clock_.position()), sources_[index]->duration());
            shown.position = browserSeekAligned_ ? clock_.position()
                                                 : std::max(0.0, own - browserOffsets_[index]);
            // The rail runs to where the last of them ends among the others,
            // each counted from its offset, and the set starts over there.
            shown.duration = 0.0;
            const auto origins = paneOrigins();
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
                if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto ||
                    !sources_[pane] || !sources_[pane]->ready()) continue;
                shown.duration = std::max(shown.duration, sources_[pane]->duration() - browserOffsets_[pane]);
            }
        }
        if (deviceRecoveryPending_) {
            shown.position = browserSeekAligned_ ? clock_.position() : deviceRecoveryBrowserPosition_;
            double longest = 0.0;
            const auto origins = paneOrigins();
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
                if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
                longest = std::max(longest, (sources_[pane] ? sources_[pane]->duration()
                                                           : deviceRecoverySourceDurations_[pane]) -
                                                browserOffsets_[pane]);
            }
            shown.duration = longest;
            if (longest <= 0.0) shown.position = 0.0;
        }
        return shown;
    }
    // The only F6 or Emby video starts at its own zero (or its resume point)
    // under a master clock that runs on from whatever the pane played before,
    // so the bar shows where this video is. Until it is ready there is no
    // time of its own to show, and the master clock would be the old one's.
    if (const int own = barTimelinePane(paneOrigins()); own >= 0) {
        const auto index = static_cast<std::size_t>(own);
        shown.pane = own;
        const bool ready = sources_[index] && sources_[index]->ready();
        shown.position = ready ? currentSourceTime(index) : 0.0;
        shown.duration = ready ? sources_[index]->duration() : 0.0;
        return shown;
    }
    // One video is the timeline. Looping between its own A and B it wraps
    // under a clock that runs on, and the bar shows where the video is.
    if (const int single = singleLoadedPane(); single >= 0) {
        const auto index = static_cast<std::size_t>(single);
        // Paused on its own it stays where it was while the clock runs on,
        // so the bar is its own rail and a drag on it moves the held frame.
        if (heldOnlyPane() == single && sources_[index] && sources_[index]->ready()) {
            shown.pane = single;
            shown.position = currentSourceTime(index);
            shown.duration = sources_[index]->duration();
            return shown;
        }
        if (sourceLoopEnabled_[index] && sources_[index] && sources_[index]->ready()) {
            shown.position = timelineForSourceTime(currentSourceTime(index), startDelays_[index],
                                                   syncAdjustments_[index], playbackRates_[index]);
        }
    }
    return shown;
}

// The bar of the only F6 or Emby video is that video's own rail. Nothing else
// is on the deck, so an F6 file still moves with the master clock and lands
// on a keyframe as one video does; an Emby stream, a paused pane and a time
// the clock cannot reach (a point ahead of the pane's offset) move the pane.
void App::seekBarTimelinePane(std::size_t pane, double target) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    if (localPanes_[pane] && !sourcePaused_[pane] &&
        linkedTimelineCanReachSourceTime(target, syncAdjustments_[pane])) {
        seekAbsolute(timelineForSourceTime(
            target, startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]),
            SeekStyle::Fast);
        return;
    }
    seekPaneTo(pane, target);
}

// ---------------------------------------------------------------------------
// Input

void App::beginControlDrag(ControlDrag kind, int pane, POINT local) {
    controlDrag_ = kind;
    dragPane_ = pane;
    SetCapture(videoWindow_);
    updateControlDrag(local);
}

void App::updateControlDrag(POINT local) {
    const float x = static_cast<float>(local.x);
    switch (controlDrag_) {
    case ControlDrag::MasterSeek:
        dragFraction_ = railFraction(x, barLayout_[BarItem::Seek], kBarSeekInset * uiScale_);
        break;
    case ControlDrag::PaneSeek:
        // The pane's own rail goes when the other videos do and this one is
        // left alone; the drag then keeps where it was, and its release
        // seeks there, rather than read the start from an empty rail.
        if (dragPane_ >= 0 && paneChrome_[static_cast<std::size_t>(dragPane_)].timeline.visible()) {
            dragFraction_ = railFraction(
                x, paneChrome_[static_cast<std::size_t>(dragPane_)].timeline, kRailInset * uiScale_);
        }
        break;
    case ControlDrag::Volume: {
        dragFraction_ = verticalRailFraction(static_cast<float>(local.y), barLayout_[BarItem::Volume],
                                             kVolumeInset * uiScale_);
        setMasterVolume(dragFraction_ * maxMasterVolume());
        break;
    }
    case ControlDrag::PaneVolume:
        // Its volume rail goes the same way; the drag keeps its value
        // rather than read zero. On a capsule it stands on the speaker and
        // is dragged up and down, along the rail drawn in its pill.
        if (dragPane_ >= 0 && paneChrome_[static_cast<std::size_t>(dragPane_)].volume.visible()) {
            const PaneChromeLayout& chrome = paneChrome_[static_cast<std::size_t>(dragPane_)];
            dragFraction_ = chrome.volumeUpright
                ? verticalRailFraction(static_cast<float>(local.y), uprightVolumePill(chrome), kRailInset * uiScale_)
                : railFraction(x, chrome.volume, kRailInset * uiScale_);
            audio_.setPaneVolume(static_cast<std::size_t>(dragPane_), dragFraction_);
        }
        break;
    case ControlDrag::PanelSlider:
        if (panelDragRow_ >= 0 && panelDragRow_ < static_cast<int>(panelLayout_.rows.size())) {
            dragFraction_ = railFraction(
                x, panelLayout_.rows[static_cast<std::size_t>(panelDragRow_)].control,
                kPanelRailInset * uiScale_);
            applyPanelSlider(panelDragRow_, dragFraction_);
        }
        break;
    case ControlDrag::PanelScroll:
        // The content follows the pointer, as a page dragged by hand.
        settingsScroll_ = std::clamp(
            panelPressScroll_ - static_cast<float>(local.y - panelPressPoint_.y), 0.0F, panelLayout_.maxScroll);
        break;
    case ControlDrag::PanelScrollbar:
        settingsScroll_ = panelScrollForThumbTop(panelLayout_, uiScale_,
                                                 static_cast<float>(local.y) - panelThumbGrab_);
        break;
    default:
        break;
    }
    controlsLastInteraction_ = GetTickCount64();
}

void App::endControlDrag(bool commit) {
    const ControlDrag kind = controlDrag_;
    const int pane = dragPane_;
    controlDrag_ = ControlDrag::None;
    dragPane_ = -1;
    if (GetCapture() == videoWindow_) ReleaseCapture();
    if (!commit) return;
    switch (kind) {
    case ControlDrag::MasterSeek:
        if (!bottomTimelineVisible()) break;
        if (const auto shown = barTime(); linkedBrowserSeekBars() && shown.duration > 0.0) {
            alignBrowserPanesToTime(dragFraction_ * shown.duration);
            break;
        } else if (shown.pane >= 0) {
            if (shown.duration > 0.0) {
                seekBarTimelinePane(static_cast<std::size_t>(shown.pane), dragFraction_ * shown.duration);
            }
            break;
        }
        if (duration_ > 0.0) {
            // Coarse positioning, so the same fast style the pane rails use.
            seekAbsolute(dragFraction_ * duration_, SeekStyle::Fast);
        }
        break;
    case ControlDrag::PaneSeek:
        if (pane >= 0 && sources_[static_cast<std::size_t>(pane)]) {
            const auto index = static_cast<std::size_t>(pane);
            seekFromSourceBar(index, dragFraction_ * sources_[index]->duration());
        }
        break;
    case ControlDrag::Volume:
        showNotice(L"Volume " + std::to_wstring(static_cast<int>(std::lround(audio_.volume() * 100.0F))) + L"%");
        scheduleAppSettingsSave();
        break;
    case ControlDrag::PaneVolume:
        if (pane >= 0) {
            showNotice(L"V" + std::to_wstring(positionForPane(static_cast<std::size_t>(pane)) + 1) +
                       L" volume " + std::to_wstring(static_cast<int>(std::lround(dragFraction_ * 100.0F))) + L"%");
        }
        scheduleAppSettingsSave();
        break;
    case ControlDrag::PanelSlider:
        panelDragRow_ = -1;
        scheduleAppSettingsSave();
        break;
    default:
        break;
    }
    updateTitle();
}

void App::activateBarItem(BarItem item) {
    controlsLastInteraction_ = GetTickCount64();
    switch (item) {
    case BarItem::Previous: openAdjacentFile(-1, singleLoadedPane()); break;
    case BarItem::Play: togglePlayback(); break;
    case BarItem::Next: openAdjacentFile(1, singleLoadedPane()); break;
    case BarItem::Stop: stopPlayback(); break;
    case BarItem::Audio: toggleMute(); break;
    // The only video's switch; its menu is on the right button.
    case BarItem::Subtitles: toggleSubtitles(singleLoadedPane()); break;
    // A video's button shows it alone whatever is shown now, and the grid
    // again once it is, as its key does.
    case BarItem::AllVideos: showAllVideos(); break;
    case BarItem::Sync: toggleSyncPanel(); break;
    case BarItem::Layout: showLayoutMenu(); break;
    case BarItem::Sets: toggleVideoSets(); break;
    case BarItem::Fullscreen: toggleFullscreen(); break;
    default:
        if (const int position = barItemSoloPosition(item); position >= 0) {
            soloVideoAt(static_cast<std::size_t>(position));
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// The sync panel

void App::toggleSyncPanel() {
    if (syncPanel_) {
        closeSyncPanel();
        return;
    }
    closePanelMenu();
    syncPanel_.emplace();
    syncPanel_->openedAt = GetTickCount64();
    layoutSyncPanel();
}

void App::closeSyncPanel() {
    const bool captured = syncPanel_ && syncPanel_->pressing;
    syncPanel_.reset();
    if (captured && videoWindow_ && GetCapture() == videoWindow_) ReleaseCapture();
}

void App::layoutSyncPanel() {
    if (!syncPanel_) return;
    // It goes with its key: one video left, the bar narrowed past it.
    const OverlayRect& key = barLayout_[BarItem::Sync];
    if (!key.visible()) {
        closeSyncPanel();
        return;
    }
    auto& open = *syncPanel_;
    std::size_t rows = 0;
    for (std::size_t position = 0; position < kMaxPanes; ++position) {
        const std::size_t pane = paneForPosition(position);
        if (pane < sources_.size() && sources_[pane]) open.panes[rows++] = pane;
    }
    open.layout = syncPanelLayout(key, windowMenuBounds(), rows, uiScale_);
}

void App::syncPanelPress(POINT local) {
    if (!syncPanel_) return;
    const SyncPanelHit hit = syncPanelHitTest(syncPanel_->layout, static_cast<float>(local.x),
                                              static_cast<float>(local.y));
    // Anywhere else a press only closes it, as one off a menu does: on its
    // key too, which would otherwise open it again.
    if (hit.kind == SyncPanelHitKind::None) {
        closeSyncPanel();
        return;
    }
    controlsLastInteraction_ = GetTickCount64();
    syncPanel_->pressed = hit;
    syncPanel_->pressing = true;
    if (videoWindow_) SetCapture(videoWindow_);
}

void App::syncPanelRelease(POINT local) {
    if (!syncPanel_ || !syncPanel_->pressing) return;
    const SyncPanelHit pressed = syncPanel_->pressed;
    syncPanel_->pressing = false;
    syncPanel_->pressed = {};
    if (videoWindow_ && GetCapture() == videoWindow_) ReleaseCapture();
    // The control it was pressed on, let go on.
    if (!syncPanel_ || pressed.kind == SyncPanelHitKind::Card) return;
    if (syncPanelHitTest(syncPanel_->layout, static_cast<float>(local.x), static_cast<float>(local.y)) == pressed) {
        activateSyncPanel(pressed);
    }
}

bool App::syncPanelWheel(POINT local, int wheelDelta) {
    if (!syncPanel_) return false;
    layoutSyncPanel();
    if (!syncPanel_) return false;
    const SyncPanelHit hit = syncPanelHitTest(syncPanel_->layout, static_cast<float>(local.x),
                                              static_cast<float>(local.y));
    if (hit.kind == SyncPanelHitKind::None) return false;
    if (hit.row < 0 || hit.row >= static_cast<int>(syncPanel_->layout.rowCount) || wheelDelta == 0) return true;
    const std::size_t pane = syncPanel_->panes[static_cast<std::size_t>(hit.row)];
    if (!sources_[pane]) return true;
    controlsLastInteraction_ = GetTickCount64();
    const double notches = static_cast<double>(wheelDelta) / WHEEL_DELTA;
    double offset = paneOffsetSeconds(pane) + notches * kSyncWheelStep;
    // Tenths added up drift off zero; zero is zero.
    if (std::abs(offset) < 0.0005) offset = 0.0;
    setPaneOffset(pane, offset);
    updateTitle();
    scheduleAppSettingsSave();
    return true;
}

void App::activateSyncPanel(const SyncPanelHit& hit) {
    if (!syncPanel_) return;
    controlsLastInteraction_ = GetTickCount64();
    const auto& open = *syncPanel_;
    const bool onRow = hit.row >= 0 && hit.row < static_cast<int>(open.layout.rowCount);
    const std::size_t pane = onRow ? open.panes[static_cast<std::size_t>(hit.row)] : 0;
    switch (hit.kind) {
    case SyncPanelHitKind::Mode:
        if (hit.part == 0 || hit.part == 1) chooseSeekMode(static_cast<SeekMode>(hit.part));
        break;
    case SyncPanelHitKind::Step:
        if (onRow && sources_[pane] && hit.part >= 0 && hit.part < static_cast<int>(kSyncSteps)) {
            double offset = paneOffsetSeconds(pane) + kSyncOffsetSteps[static_cast<std::size_t>(hit.part)];
            if (std::abs(offset) < 0.0005) offset = 0.0;
            setPaneOffset(pane, offset);
        }
        break;
    case SyncPanelHitKind::Repeat:
        // The strip's own repeat, with its notice.
        if (onRow && sources_[pane]) activateChip(pane, PaneChip::Repeat);
        return;
    case SyncPanelHitKind::Reset:
        resetAllOffsets();
        break;
    default:
        return;
    }
    updateTitle();
    scheduleAppSettingsSave();
}

// The bar's subtitle button's right click: the Subtitles menu, over the
// button.
void App::showBarSubtitleMenu() {
    controlsLastInteraction_ = GetTickCount64();
    showSubtitleMenu(singleLoadedPane(), WindowMenuAnchor{{}, static_cast<int>(BarItem::Subtitles), -1});
}

void App::activateCaptionItem(CaptionItem item) {
    controlsLastInteraction_ = GetTickCount64();
    // Through the system commands, as the title bar's own buttons go, so
    // Windows animates them and the restore size is its own.
    switch (item) {
    case CaptionItem::Minimize: PostMessageW(window_, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case CaptionItem::Maximize:
        // In fullscreen it leaves fullscreen, as the bar's button does.
        if (fullscreen_) toggleFullscreen();
        else PostMessageW(window_, WM_SYSCOMMAND, IsZoomed(window_) ? SC_RESTORE : SC_MAXIMIZE, 0);
        break;
    case CaptionItem::Close: PostMessageW(window_, WM_SYSCOMMAND, SC_CLOSE, 0); break;
    }
}

void App::activateChip(std::size_t pane, PaneChip chip) {
    if (pane >= sources_.size()) return;
    switch (chip) {
    case PaneChip::Audio: toggleAudioPane(pane); break;
    case PaneChip::Pause: toggleSourcePause(pane); break;
    case PaneChip::Solo: toggleSolo(static_cast<int>(pane)); break;
    case PaneChip::Repeat: {
        if (singleLoadedPane() == static_cast<int>(pane)) {
            // The one rule for the only video: "When the only video ends".
            setPlayOrder(playOrder_ == PlayOrder::RepeatOne ? PlayOrder::InOrder : PlayOrder::RepeatOne);
            break;
        }
        const bool enabled = !sourceAutoRepeat_[pane];
        setSourceAutoRepeat(pane, enabled);
        showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) +
                   (!enabled ? L" repeat off"
                    : paneRepeatSavedButIdle(pane) ? L" repeat saved; not in force beside F6 or Emby videos"
                    : linkedBrowserSeekBars() ? L" repeats with the others once all have ended"
                    : L" repeats (Independent mode)"));
        break;
    }
    case PaneChip::Close: closePane(pane); break;
    // A switch, as the bar's is for the only video; its menu is on the
    // right button.
    case PaneChip::Subtitles: toggleSubtitles(static_cast<int>(pane)); break;
    }
    updateTitle();
    scheduleAppSettingsSave();
}

std::optional<App::RepeatControl> App::repeatControlAt(POINT local) const {
    const float x = static_cast<float>(local.x);
    const float y = static_cast<float>(local.y);
    if (captionItemAt(x, y, captionLayout_, captionAlpha() > 0.05F) >= 0 || onCaptionStrip(local)) {
        return std::nullopt;
    }
    // The same order as a press: the sheet first, unless the point is on
    // the video beside a docked browser.
    if (sheetTakesPointer()) {
        const PanelHit hit = panelHitAt(local);
        if (!(embyBrowserDocked() && hit.kind == PanelHitKind::Outside)) return panelRepeatControl(hit, panelRows_);
    }
    const OverlayHit hit = hitTestAt(local);
    switch (hit.kind) {
    case OverlayHitKind::BarItem:
        if (hit.item == static_cast<int>(BarItem::Previous) || hit.item == static_cast<int>(BarItem::Next) ||
            hit.item == static_cast<int>(BarItem::Seek) || hit.item == static_cast<int>(BarItem::Volume)) {
            return RepeatControl{2, static_cast<int>(hit.kind), hit.item};
        }
        return std::nullopt;
    case OverlayHitKind::PaneVolume:
    case OverlayHitKind::PaneTimeline:
        return RepeatControl{3, static_cast<int>(hit.kind), hit.pane};
    default:
        return std::nullopt;
    }
}

LRESULT App::handleVideoMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        // The top resize band and the caption's drag area are the main
        // window's to answer for; this child covers its whole client.
        if (frameHitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}) != HTCLIENT) return HTTRANSPARENT;
        return DefWindowProcW(videoWindow_, message, wParam, lParam);
    case WM_SIZE:
        renderer_.resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DROPFILES:
    {
        const HDROP drop = reinterpret_cast<HDROP>(wParam);
        POINT point{};
        int targetPane = -1;
        int pointerPane = -1;
        if (DragQueryPoint(drop, &point)) {
            RECT client{};
            GetClientRect(videoWindow_, &client);
            pointerPane = activePaneAt(static_cast<float>(point.x), static_cast<float>(point.y),
                static_cast<float>(client.right), static_cast<float>(client.bottom), activePanes(),
                layoutMode_, expandedPane_, soloPane_, paneAspectRatios(),
                autoLayoutFocus_, autoLayoutFocusPane_);
            bool hasEmptySlot = false;
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
                hasEmptySlot = hasEmptySlot || !paneLogicallyLoaded(pane);
            }
            targetPane = incomingDropTarget(hasEmptySlot, pointerPane);
        }
        onDrop(drop, targetPane, pointerPane);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        // An open drop-down takes the press: on it an item's, anywhere else
        // only its closing. Its button may have gone since the last frame.
        if (panelMenu_) {
            layoutPanelMenu();
            if (panelMenu_) {
                menuTookPress_ = true;
                panelMenuPress(local);
                return 0;
            }
        }
        // So does the sync panel: on it a control's, elsewhere its closing.
        if (syncPanel_) {
            layoutSyncPanel();
            if (syncPanel_) {
                menuTookPress_ = true;
                syncPanelPress(local);
                return 0;
            }
        }
        menuTookPress_ = false;
        // A press anywhere but on the box typed into or its clear button
        // takes the typing away from it.
        if (typingField_ != TextFieldId::None && fieldAt(panelHitAt(local)) != typingField_) blurField();
        updateHoverControls([&] { POINT p = local; ClientToScreen(videoWindow_, &p); return p; }());
        pressedBarItem_ = -1;
        pressedCaptionItem_ = -1;
        pressedEmptyOpen_ = false;
        pressedEmptyEmby_ = false;
        pressedChipPane_ = pressedChip_ = -1;
        pressedPillPane_ = -1;
        pressedPanelKind_ = PanelHitKind::None;
        pressedPanelRow_ = pressedPanelPart_ = -1;
        pressedPanelId_ = SettingId::None;
        pressedPanelParam_ = -1;
        releasedControl_.reset();
        // The window buttons stand above the sheet's column as well.
        if (const int item = captionItemAt(static_cast<float>(local.x), static_cast<float>(local.y),
                                           captionLayout_, captionAlpha() > 0.05F);
            item >= 0) {
            pressedCaptionItem_ = item;
            SetCapture(videoWindow_);
            return 0;
        }
        // Beside the buttons the strip drags a window, and the main window
        // takes those presses. In fullscreen it hands them here; they are
        // still the strip's, not the sheet's or the picture's beneath it.
        if (onCaptionStrip(local)) return 0;
        // The video beside a docked browser is still the player: a click
        // there is not a click outside the sheet.
        if (sheetTakesPointer() &&
            !(embyBrowserDocked() && panelHitAt(local).kind == PanelHitKind::Outside)) {
            const PanelHit panelHit = panelHitAt(local);
            // The viewer takes the list: a step it was following gives way.
            listFollow_.reset();
            switch (panelHit.kind) {
            case PanelHitKind::Outside:
                closeSettingsPanel();
                return 0;
            case PanelHitKind::Slider:
                panelDragRow_ = panelHit.row;
                beginControlDrag(ControlDrag::PanelSlider, -1, local);
                return 0;
            case PanelHitKind::Scrollbar: {
                // On the thumb it is grabbed where it is; on the track the
                // thumb jumps under the pointer; on a playing video's mark,
                // to that video.
                panelThumbGrab_ = panelThumbGrab(panelLayout_, uiScale_, static_cast<float>(local.y), panelScrollMarks_);
                beginControlDrag(ControlDrag::PanelScrollbar, -1, local);
                return 0;
            }
            case PanelHitKind::Sheet:
                // Empty sheet: only a drag of the content can come of it.
                panelPressArmed_ = true;
                panelPressPoint_ = local;
                panelPressScroll_ = settingsScroll_;
                SetCapture(videoWindow_);
                return 0;
            case PanelHitKind::Field:
                // A text box takes the caret on the press, where it fell.
                focusFieldAt(fieldAt(panelHit), static_cast<float>(local.x));
                return 0;
            case PanelHitKind::Close:
            case PanelHitKind::Switch:
            case PanelHitKind::Tab:
            case PanelHitKind::Toggle:
            case PanelHitKind::Segment:
            case PanelHitKind::Button:
            case PanelHitKind::Trailing:
            case PanelHitKind::Item:
            case PanelHitKind::Tile:
            case PanelHitKind::Add:
                pressedPanelKind_ = panelHit.kind;
                pressedPanelRow_ = panelHit.row;
                pressedPanelPart_ = panelHit.part;
                if (panelHit.row >= 0 && panelHit.row < static_cast<int>(panelRows_.size())) {
                    const auto& row = panelRows_[static_cast<std::size_t>(panelHit.row)];
                    pressedPanelId_ = row.id;
                    pressedPanelParam_ = panelRowParam(row, panelHit.part);
                }
                // A press that moves on becomes a scroll, not a click.
                panelPressArmed_ = true;
                panelPressPoint_ = local;
                panelPressScroll_ = settingsScroll_;
                SetCapture(videoWindow_);
                return 0;
            default:
                return 0;
            }
        }
        const OverlayHit hit = hitTestAt(local);
        if (hit.pane >= 0 && hit.pane < static_cast<int>(kMaxPanes) &&
            hit.kind != OverlayHitKind::BarItem && hit.kind != OverlayHitKind::None) {
            embySelectTarget(static_cast<std::size_t>(hit.pane));
        }
        switch (hit.kind) {
        case OverlayHitKind::BarItem:
            if (hit.item == static_cast<int>(BarItem::Seek)) {
                if (barTime().duration > 0.0) beginControlDrag(ControlDrag::MasterSeek, -1, local);
            } else if (hit.item == static_cast<int>(BarItem::Volume)) {
                beginControlDrag(ControlDrag::Volume, -1, local);
            } else if (hit.item != static_cast<int>(BarItem::Time)) {
                pressedBarItem_ = hit.item;
                SetCapture(videoWindow_);
            }
            controlsLastInteraction_ = GetTickCount64();
            return 0;
        case OverlayHitKind::EmptyOpen:
            pressedEmptyOpen_ = true;
            SetCapture(videoWindow_);
            return 0;
        case OverlayHitKind::EmptyEmby:
            pressedEmptyEmby_ = true;
            SetCapture(videoWindow_);
            return 0;
        case OverlayHitKind::PaneChip:
            pressedChipPane_ = hit.pane;
            pressedChip_ = hit.item;
            SetCapture(videoWindow_);
            return 0;
        case OverlayHitKind::PanePill:
            pressedPillPane_ = hit.pane;
            SetCapture(videoWindow_);
            return 0;
        case OverlayHitKind::PaneVolume:
            beginControlDrag(ControlDrag::PaneVolume, hit.pane, local);
            return 0;
        case OverlayHitKind::PaneTimeline: {
            const auto index = static_cast<std::size_t>(hit.pane);
            if (sources_[index] && sources_[index]->ready() && sources_[index]->duration() > 0.0) {
                beginControlDrag(ControlDrag::PaneSeek, hit.pane, local);
            }
            return 0;
        }
        case OverlayHitKind::Video:
            dragSourcePane_ = hit.pane;
            dragStart_ = local;
            draggingPane_ = false;
            dropTargetPane_ = -1;
            if (dragSourcePane_ >= 0) SetCapture(videoWindow_);
            return 0;
        default:
            return 0;
        }
    }
    case WM_MOUSEMOVE: {
        updateAutoHideControls();
        const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (controlDrag_ != ControlDrag::None) {
            updateControlDrag(local);
            return 0;
        }
        if ((wParam & MK_LBUTTON) && panelPressArmed_) {
            if (std::abs(local.y - panelPressPoint_.y) >= GetSystemMetrics(SM_CYDRAG) &&
                panelLayout_.maxScroll > 0.0F) {
                panelPressArmed_ = false;
                pressedPanelKind_ = PanelHitKind::None;
                pressedPanelRow_ = pressedPanelPart_ = -1;
                pressedPanelId_ = SettingId::None;
                pressedPanelParam_ = -1;
                beginControlDrag(ControlDrag::PanelScroll, -1, local);
            }
            return 0;
        }
        if ((wParam & MK_LBUTTON) && dragSourcePane_ >= 0) {
            const int dx = std::abs(local.x - dragStart_.x);
            const int dy = std::abs(local.y - dragStart_.y);
            if (dx >= GetSystemMetrics(SM_CXDRAG) || dy >= GetSystemMetrics(SM_CYDRAG)) {
                if (!draggingPane_ && !paneDragHasTarget()) {
                    // No other pane to drop it on: the press comes to
                    // nothing. The window moves by its caption strip, not
                    // by the picture (until 2026-11-07 the picture moved
                    // it here, as in PotPlayer).
                    dragSourcePane_ = -1;
                    if (GetCapture() == videoWindow_) ReleaseCapture();
                    return 0;
                }
                draggingPane_ = true;
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            }
            if (draggingPane_) {
                RECT client{};
                GetClientRect(videoWindow_, &client);
                dropTargetPane_ = activePaneAt(
                    static_cast<float>(local.x), static_cast<float>(local.y),
                    static_cast<float>(client.right), static_cast<float>(client.bottom), activePanes(),
                    layoutMode_, expandedPane_, soloPane_, paneAspectRatios(),
                    autoLayoutFocus_, autoLayoutFocusPane_);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (panelMenu_) {
            panelMenuRelease(local);
            return 0;
        }
        if (syncPanel_ && syncPanel_->pressing) {
            syncPanelRelease(local);
            return 0;
        }
        // Read before the click acts: what it opens may stand here next.
        releasedControl_ = repeatControlAt(local);
        const bool sheetPress = panelPressArmed_;
        panelPressArmed_ = false;
        if (controlDrag_ != ControlDrag::None) {
            updateControlDrag(local);
            endControlDrag(true);
            return 0;
        }
        if (pressedCaptionItem_ >= 0) {
            const int item = pressedCaptionItem_;
            pressedCaptionItem_ = -1;
            if (GetCapture() == videoWindow_) ReleaseCapture();
            if (captionItemAt(static_cast<float>(local.x), static_cast<float>(local.y),
                              captionLayout_, captionAlpha() > 0.05F) == item) {
                activateCaptionItem(static_cast<CaptionItem>(item));
            }
            return 0;
        }
        if (pressedPanelKind_ != PanelHitKind::None) {
            releasePanelPress(local);
            return 0;
        }
        if (sheetPress) {
            if (GetCapture() == videoWindow_) ReleaseCapture();
            return 0;
        }
        const OverlayHit hit = hitTestAt(local);
        if (pressedBarItem_ >= 0) {
            const int item = pressedBarItem_;
            pressedBarItem_ = -1;
            if (GetCapture() == videoWindow_) ReleaseCapture();
            if (hit.kind == OverlayHitKind::BarItem && hit.item == item) {
                activateBarItem(static_cast<BarItem>(item));
            }
            return 0;
        }
        if (pressedEmptyOpen_) {
            pressedEmptyOpen_ = false;
            if (GetCapture() == videoWindow_) ReleaseCapture();
            if (hit.kind == OverlayHitKind::EmptyOpen) {
                controlsLastInteraction_ = GetTickCount64();
                openFilesDialog();
            }
            return 0;
        }
        if (pressedEmptyEmby_) {
            pressedEmptyEmby_ = false;
            if (GetCapture() == videoWindow_) ReleaseCapture();
            if (hit.kind == OverlayHitKind::EmptyEmby) {
                controlsLastInteraction_ = GetTickCount64();
                embyOpenBrowser();
            }
            return 0;
        }
        if (pressedChipPane_ >= 0) {
            const int pane = pressedChipPane_;
            const int chip = pressedChip_;
            pressedChipPane_ = pressedChip_ = -1;
            if (GetCapture() == videoWindow_) ReleaseCapture();
            if (hit.kind == OverlayHitKind::PaneChip && hit.pane == pane && hit.item == chip) {
                activateChip(static_cast<std::size_t>(pane), static_cast<PaneChip>(chip));
            }
            return 0;
        }
        if (pressedPillPane_ >= 0) {
            const int pane = pressedPillPane_;
            pressedPillPane_ = -1;
            if (GetCapture() == videoWindow_) ReleaseCapture();
            if (hit.kind == OverlayHitKind::PanePill && hit.pane == pane) {
                showContextMenu(pane, WindowMenuAnchor{{}, -1, pane});
            }
            return 0;
        }
        // The drop is read before the capture goes: ReleaseCapture sends
        // WM_CAPTURECHANGED here first, and that handler cancels the drag,
        // as it must when the pointer is taken by another window mid-drag.
        const int source = draggingPane_ ? dragSourcePane_ : -1;
        dragSourcePane_ = -1;
        draggingPane_ = false;
        dropTargetPane_ = -1;
        if (GetCapture() == videoWindow_) ReleaseCapture();
        if (source >= 0) {
            RECT client{};
            GetClientRect(videoWindow_, &client);
            const int destination = activePaneAt(
                static_cast<float>(local.x), static_cast<float>(local.y),
                static_cast<float>(client.right), static_cast<float>(client.bottom), activePanes(),
                layoutMode_, expandedPane_, soloPane_, paneAspectRatios(),
                autoLayoutFocus_, autoLayoutFocusPane_);
            if (destination >= 0 && destination != source) {
                swapPanes(static_cast<std::size_t>(source), static_cast<std::size_t>(destination));
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != videoWindow_) {
            if (controlDrag_ != ControlDrag::None) endControlDrag(false);
            pressedBarItem_ = -1;
            pressedCaptionItem_ = -1;
            pressedEmptyOpen_ = false;
            pressedEmptyEmby_ = false;
            pressedChipPane_ = pressedChip_ = -1;
            pressedPillPane_ = -1;
            pressedPanelKind_ = PanelHitKind::None;
            pressedPanelRow_ = pressedPanelPart_ = -1;
            panelPressArmed_ = false;
            dragSourcePane_ = -1;
            draggingPane_ = false;
            dropTargetPane_ = -1;
            if (panelMenu_) {
                panelMenu_->pressing = false;
                panelMenu_->pressed = -1;
            }
            if (syncPanel_) {
                syncPanel_->pressing = false;
                syncPanel_->pressed = {};
            }
        }
        return 0;
    case WM_RBUTTONUP: {
        SetFocus(window_);
        const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (panelMenu_) {
            panelMenuRightClick(local);
            return 0;
        }
        // The right button off the sync panel closes it; on it, nothing.
        if (syncPanel_) {
            if (syncPanelHitTest(syncPanel_->layout, static_cast<float>(local.x), static_cast<float>(local.y)).kind ==
                SyncPanelHitKind::None) {
                closeSyncPanel();
            }
            return 0;
        }
        if (sheetTakesPointer()) {
            const PanelHit panelHit = panelHitAt(local);
            if (panelHit.kind != PanelHitKind::Outside) {
                // On a text box: cut, copy, paste and the rest. On an Emby
                // item: mark or unmark it. Anywhere else on the sheet a
                // right-click does nothing.
                const TextFieldId field =
                    panelHit.kind == PanelHitKind::Field ? fieldAt(panelHit) : TextFieldId::None;
                if (field != TextFieldId::None && fieldShown(field)) showFieldMenu(field, local);
                else showEmbyItemMenu(local);
                return 0;
            }
        }
        // A subtitle switch's choices are on its right button: the bar's for
        // the only video, a strip's for its own.
        if (const OverlayHit hit = hitTestAt(local);
            hit.kind == OverlayHitKind::BarItem && hit.item == static_cast<int>(BarItem::Subtitles)) {
            showBarSubtitleMenu();
            return 0;
        } else if (hit.kind == OverlayHitKind::PaneChip && hit.item == static_cast<int>(PaneChip::Subtitles)) {
            embySelectTarget(static_cast<std::size_t>(hit.pane));
            showSubtitleMenu(hit.pane, WindowMenuAnchor{{}, -1, -1, hit.pane, hit.item});
            return 0;
        }
        RECT client{};
        GetClientRect(videoWindow_, &client);
        const int pane = activePaneAt(
            static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)),
            static_cast<float>(client.right), static_cast<float>(client.bottom), activePanes(),
            layoutMode_, expandedPane_, soloPane_, paneAspectRatios(),
            autoLayoutFocus_, autoLayoutFocusPane_);
        if (pane >= 0) embySelectTarget(static_cast<std::size_t>(pane));
        showContextMenu(pane, WindowMenuAnchor{local});
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        // The second of two quick clicks is a press for an open drop-down,
        // and after one the first click closed: a Windows menu took that
        // click in its own window, so it was never half a double-click.
        // The sync panel's steps take quick presses one by one, as presses.
        if (panelMenu_ || syncPanel_ || menuTookPress_) return handleVideoMessage(WM_LBUTTONDOWN, wParam, lParam);
        // The window class asks for double-clicks, so the second of two
        // quick clicks comes here rather than as a press. On the stepper
        // or rail the first one let go on, it is a second click like the
        // first; anywhere else it stays a double-click.
        if (const auto control = repeatControlAt(local); control && control == releasedControl_) {
            return handleVideoMessage(WM_LBUTTONDOWN, wParam, lParam);
        }
        if (onCaptionStrip(local)) return 0;
        if (sheetTakesPointer()) {
            const PanelHit panelHit = panelHitAt(local);
            // Twice on a text box selects what it holds, to type over.
            if (panelHit.kind == PanelHitKind::Field) focusField(fieldAt(panelHit), true);
            if (panelHit.kind != PanelHitKind::Outside) return 0;
        }
        const OverlayHit hit = hitTestAt(local);
        if (hit.kind == OverlayHitKind::Video && hit.pane >= 0 &&
            sources_[static_cast<std::size_t>(hit.pane)]) {
            // The only video has no others to leave: it plays and pauses,
            // as Space does.
            if (singleLoadedPane() == hit.pane) togglePlayback();
            else toggleSolo(hit.pane);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_CHAR:
        return SendMessageW(window_, message, wParam, lParam);
    case WM_SYSKEYDOWN:
    case WM_MOUSEWHEEL:
        return SendMessageW(window_, message, wParam, lParam);
    case WM_SETCURSOR:
        // The text cursor over a text box.
        if (LOWORD(lParam) == HTCLIENT && sheetTakesPointer() && !panelMenu_) {
            POINT pointer{};
            GetCursorPos(&pointer);
            ScreenToClient(videoWindow_, &pointer);
            const PanelHit hit = panelHitAt(pointer);
            if (hit.kind == PanelHitKind::Field && fieldAt(hit) != TextFieldId::None) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
        }
        return DefWindowProcW(videoWindow_, message, wParam, lParam);
    case WM_APPCOMMAND:
        return SendMessageW(window_, message, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(videoWindow_, message, wParam, lParam);
    }
}

}  // namespace quaddeck
