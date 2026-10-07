// App: the drawn interface. Builds the OverlayScene each frame from player
// state, hit-tests the pointer against OverlayLayout geometry, owns hover and
// drag state, the transport bar's reveal/auto-hide and notices.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace quaddeck {

using namespace app_internal;

namespace {

// How long a pane's chrome and the bar take to fade.
constexpr float kChromeFadeMs = 160.0F;
// Notices fade over their last stretch rather than vanishing.
constexpr ULONGLONG kNoticeFadeMs = 300;
// A hot control shows its name after resting on it this long.
constexpr ULONGLONG kTooltipDelayMs = 600;
constexpr float kRailInset = 12.0F;
constexpr float kBarSeekInset = 8.0F;
constexpr float kVolumeInset = 10.0F;
constexpr float kPanelSlideMs = 200.0F;
constexpr float kPanelRailInset = 8.0F;

const wchar_t* barItemName(BarItem item) {
    switch (item) {
    case BarItem::Previous: return L"Previous video  (Page Up)";
    case BarItem::Next: return L"Next video  (Page Down)";
    case BarItem::Play: return L"Play / Pause  (Space)";
    case BarItem::Stop: return L"Stop";
    case BarItem::Audio: return L"Mute  (M)";
    case BarItem::Volume: return L"Master volume  (mouse wheel)";
    case BarItem::Subtitles: return L"Subtitles  (Alt+L next  ·  Alt+H hide)";
    case BarItem::Layout: return L"Arrangement of the videos";
    case BarItem::Menu: return L"Menu  (right-click)";
    case BarItem::Fullscreen: return L"Fullscreen  (Alt+Enter)";
    case BarItem::Settings: return L"Settings  (F5)  ·  U pins the bar";
    default: return L"";
    }
}

const wchar_t* captionItemName(CaptionItem item, bool maximized) {
    switch (item) {
    case CaptionItem::Minimize: return L"Minimize";
    case CaptionItem::Maximize: return maximized ? L"Restore" : L"Maximize";
    case CaptionItem::Close: return L"Close  (Alt+F4)";
    default: return L"";
    }
}

const wchar_t* chipName(PaneChip chip) {
    switch (chip) {
    case PaneChip::Audio: return L"Output this video's audio  (1-5)";
    case PaneChip::Pause: return L"Pause / resume this video";
    case PaneChip::Solo: return L"Solo this video / back to grid";
    case PaneChip::Repeat: return L"Repeat this video  (Independent mode)";
    case PaneChip::Close: return L"Close this video";
    default: return L"";
    }
}

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
    if (controlsVisible_ == visible) return;
    controlsVisible_ = visible;
    dockAnimationTick_ = GetTickCount64();
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
    }
}

void App::updateAutoHideControls() {
    if (!window_) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    POINT local = cursor;
    ScreenToClient(window_, &local);
    RECT client{};
    GetClientRect(window_, &client);
    const bool inside = local.x >= 0 && local.y >= 0 && local.x < client.right && local.y < client.bottom;
    // The bar wakes when a moving pointer reaches the bottom strip, and while
    // it is up its whole band keeps it up.
    const int bandHeight = static_cast<int>(std::lround(BarMetrics{}.bandHeight * uiScale_));
    const bool inInteractiveBand = inside && pointerInDockInteractiveBand(
        local.y, client.bottom, dockProgress_, bandHeight, dp(kControlRevealZone));
    // The caption wakes the same way from the top edge, and while it is up
    // its strip keeps it up.
    const float captionZone = !captionShown() ? 0.0F
        : dockProgress_ > 0.05F ? captionInset() : static_cast<float>(dp(kControlRevealZone));
    const bool inCaptionBand = inside && static_cast<float>(local.y) < captionZone;
    const ULONGLONG now = GetTickCount64();
    if (contextMenuOpen_) {
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
    } else if ((!inside || GetForegroundWindow() != window_) && cursorHidden_) {
        ShowCursor(TRUE);
        cursorHidden_ = false;
    }
    const bool editing = controlDrag_ != ControlDrag::None || draggingPane_ ||
                         pressedBarItem_ >= 0 || GetCapture() != nullptr;
    if (controlDrag_ != ControlDrag::None || pressedBarItem_ >= 0 || hotBarItem_ >= 0 ||
        pressedCaptionItem_ >= 0 || hotCaptionItem_ >= 0) {
        controlsLastInteraction_ = now;
    }
    if (moved && !editing && (inInteractiveBand || inCaptionBand)) {
        controlsLastInteraction_ = now;
        setControlsVisible(true);
    }
    if (!controlsPinned_ && !editing && controlsVisible_ &&
        now - controlsLastInteraction_ >= kControlHideDelayMs) {
        setControlsVisible(false);
    }
    // PotPlayer hides its skin only while there is video to look at: an
    // empty window keeps its title and its controls.
    if (controlsPinned_ || !anyPaneLoaded()) setControlsVisible(true);
    if (inside && GetForegroundWindow() == window_ && !cursorHidden_ && !editing &&
        now - pointerLastMoved_ >= kPointerHideDelayMs) {
        ShowCursor(FALSE);
        cursorHidden_ = true;
    }
}

// ---------------------------------------------------------------------------
// Geometry and hover

std::wstring App::paneLabel(std::size_t pane) const {
    std::wostringstream label;
    label << L"V" << (positionForPane(pane) + 1) << L"  ·  "
          << std::setprecision(3) << playbackRates_[pane] << L"×  ·  "
          << viewModeName(paneViews_[pane].mode);
    return label.str();
}

float App::paneCoveredBottom(const RectF& cell, bool settled) const {
    if ((!settled && dockProgress_ <= 0.001F) || !barLayout_.row.visible()) return 0.0F;
    const float clearance = 8.0F * uiScale_;
    const auto active = activePanes();
    const bool multiple = std::count(active.begin(), active.end(), true) > 1;
    const float rowTop = (multiple && barLayout_.scopeLabel.visible()
        ? std::min(barLayout_.row.y, barLayout_.scopeLabel.y) : barLayout_.row.y) - clearance;
    const float cellBottom = cell.y + cell.height;
    if (cellBottom <= rowTop) return 0.0F;
    return (cellBottom - rowTop) * (settled ? 1.0F : dockProgress_);
}

float App::captionAlpha() const {
    return captionShown() ? std::max(dockProgress_, settingsAlpha_) : 0.0F;
}

float App::captionInset() const {
    return captionShown() ? CaptionMetrics{}.rowHeight * uiScale_ : 0.0F;
}

bool App::bottomTimelineVisible() const {
    return !perPaneTimelines() || singleLoadedPane() >= 0 || linkedBrowserSeekBars();
}

bool App::paneDragMovesWindow() const {
    if (!captionShown()) return false;
    const auto active = activePanes();
    int visible = 0;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (active[pane] && chromeCells_[pane].width > 0.0F && chromeCells_[pane].height > 0.0F) ++visible;
    }
    return visible <= 1;
}

void App::refreshChromeGeometry(const RECT& client) {
    const auto aspects = paneAspectRatios();
    chromeCells_ = currentLayoutCells(
        static_cast<float>(client.right), static_cast<float>(client.bottom), aspects);
    captionLayout_ = captionShown()
        ? captionLayout(static_cast<float>(client.right), static_cast<float>(client.bottom), uiScale_)
        : CaptionLayout{};
    // The docked Emby browser lies over the right part of the video; the
    // bar keeps to the uncovered part so its buttons stay reachable.
    barLayout_ = transportBarLayout(videoAreaWidth(static_cast<float>(client.right)),
                                    static_cast<float>(client.bottom), uiScale_, volumeOpen_,
                                    singleLoadedPane() >= 0, BarMetrics{}, bottomTimelineVisible(),
                                    severalPanesLoaded());
    const auto active = activePanes();
    // Count loaded sources before Solo filters the visible cells. A single
    // source uses the master rail; Solo still needs its own local timeline.
    const bool showPaneTimeline = std::count(active.begin(), active.end(), true) > 1;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!active[pane]) {
            paneChrome_[pane] = {};
            continue;
        }
        const std::wstring label = paneLabel(pane);
        if (label != pillLabels_[pane] || pillWidths_[pane] < 0.0F) {
            pillLabels_[pane] = label;
            pillWidths_[pane] = overlay_.ready()
                ? overlay_.measureText(label, OverlayTextStyle::BodyBold, uiScale_)
                : 90.0F * uiScale_;
        }
        const bool expanded = static_cast<int>(pane) == hoverPane_ ||
                              pressedChipPane_ == static_cast<int>(pane) ||
                              (controlDrag_ == ControlDrag::PaneVolume && dragPane_ == static_cast<int>(pane));
        // The pill sits below the caption's strip whether or not it is up,
        // so it never runs from a pointer that wakes the caption.
        paneChrome_[pane] = paneChromeLayout(
            chromeCells_[pane], uiScale_, pillWidths_[pane], expanded,
            paneCoveredBottom(chromeCells_[pane]), audioPaneEnabled(pane), PaneChromeMetrics{},
            std::max(0.0F, captionInset() - chromeCells_[pane].y));
        if (!showPaneTimeline) {
            // Rendering and hit testing share this geometry.
            paneChrome_[pane].timeline = {};
            paneChrome_[pane].timeLabel = {};
        }
    }
    if (settingsOpen_ || settingsAlpha_ > 0.0F) refreshPanelGeometry(client);
}

OverlayHit App::hitTestAt(POINT local) const {
    if (const int item = captionItemAt(static_cast<float>(local.x), static_cast<float>(local.y),
                                       captionLayout_, captionAlpha() > 0.05F);
        item >= 0) {
        return {OverlayHitKind::CaptionItem, -1, item};
    }
    PaneArray<bool> chromeActive{};
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        chromeActive[pane] = paneChromeAlpha_[pane] > 0.05F;
    }
    return overlayHitTest(static_cast<float>(local.x), static_cast<float>(local.y),
                          barLayout_, dockProgress_ > 0.05F, paneChrome_, chromeActive,
                          chromeCells_, activePanes());
}

void App::updateHoverControls() {
    POINT cursor{};
    GetCursorPos(&cursor);
    updateHoverControls(cursor);
}

void App::updateHoverControls(POINT cursor) {
    if (!videoWindow_) return;
    POINT local = cursor;
    ScreenToClient(videoWindow_, &local);
    RECT client{};
    GetClientRect(videoWindow_, &client);
    const auto active = activePanes();
    const bool pointerFresh = GetTickCount64() - pointerLastMoved_ < kPointerHideDelayMs;
    const bool pointerInsidePlayer = local.x >= 0 && local.y >= 0 &&
                                     local.x < client.right && local.y < client.bottom;
    const bool playerForeground = GetForegroundWindow() == window_;
    refreshChromeGeometry(client);

    const bool barUp = dockProgress_ > 0.05F;
    // The bar's footprint is its row and the margin beneath it; the scrim
    // above the row is still the video.
    const bool overBar = barUp && barLayout_.row.visible() && pointerInsidePlayer &&
                         static_cast<float>(local.y) >= barLayout_.row.y;
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
    hoverPane_ = pointerFresh && pointerInsidePlayer ? geometricPane : -1;

    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        const bool eligible = active[pane] && chromeCells_[pane].width > 0.0F &&
                              chromeCells_[pane].height > 0.0F;
        const bool dragging = controlDrag_ == ControlDrag::PaneSeek &&
                              dragPane_ == static_cast<int>(pane);
        paneChromeWanted_[pane] = shouldShowPaneTimeline(
            eligible, pointerFresh, pointerInsidePlayer, playerForeground, dragging);
    }

    // The settings sheet, when up, takes the pointer before anything under
    // it; only the caption's strip stands above its column.
    const PanelHit panelHit = pointerInsidePlayer && !overCaption ? panelHitAt(local) : PanelHit{};
    const bool overSheet = panelHit.kind != PanelHitKind::None && panelHit.kind != PanelHitKind::Outside;
    hotPanelKind_ = overSheet ? panelHit.kind : PanelHitKind::None;
    hotPanelRow_ = overSheet ? panelHit.row : -1;
    hotPanelPart_ = overSheet ? panelHit.part : -1;
    if (controlDrag_ == ControlDrag::PanelSlider) {
        hotPanelKind_ = PanelHitKind::Slider;
        hotPanelRow_ = panelDragRow_;
    }
    if (controlDrag_ == ControlDrag::PanelScrollbar) hotPanelKind_ = PanelHitKind::Scrollbar;

    // Hot states. A drag keeps its own target hot regardless of the pointer.
    const OverlayHit hit = pointerInsidePlayer && !overSheet ? hitTestAt(local) : OverlayHit{};
    int hotBar = -1;
    int hotCaption = -1;
    int hotChipPane = -1;
    int hotChip = -1;
    int hotPill = -1;
    int hotRail = -1;
    int hotVolume = -1;
    switch (hit.kind) {
    case OverlayHitKind::BarItem: hotBar = hit.item; break;
    case OverlayHitKind::CaptionItem: hotCaption = hit.item; break;
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
    const bool hotChanged = hotBar != hotBarItem_ || hotCaption != hotCaptionItem_ || hotChipPane != hotChipPane_ ||
                            hotChip != hotChip_ || hotPill != hotPillPane_ || hotRail != hotRailPane_ ||
                            hotVolume != hotVolumePane_;
    hotBarItem_ = hotBar;
    hotCaptionItem_ = hotCaption;
    hotChipPane_ = hotChipPane;
    hotChip_ = hotChip;
    hotPillPane_ = hotPill;
    hotRailPane_ = hotRail;
    hotVolumePane_ = hotVolume;
    seekHot_ = hotBar == static_cast<int>(BarItem::Seek);
    lastLocalPointer_ = local;
    pointerInsidePlayer_ = pointerInsidePlayer;
    embyEdgeHover(local, client, pointerInsidePlayer);

    // The volume rail unfolds while the pointer is on the speaker or the rail
    // itself, and stays out for a drag.
    const bool wantVolume = hotBar == static_cast<int>(BarItem::Audio) ||
                            hotBar == static_cast<int>(BarItem::Volume) ||
                            controlDrag_ == ControlDrag::Volume;
    if (wantVolume != volumeOpen_) {
        volumeOpen_ = wantVolume;
        refreshChromeGeometry(client);
    }

    const ULONGLONG now = GetTickCount64();
    if (hotChanged) hotSince_ = now;
    tooltipText_.clear();
    tooltipAnchor_ = {};
    if (controlDrag_ == ControlDrag::None && pressedBarItem_ < 0 && pressedCaptionItem_ < 0 &&
        now - hotSince_ >= kTooltipDelayMs) {
        if (hotCaption >= 0) {
            tooltipText_ = captionItemName(static_cast<CaptionItem>(hotCaption), window_ && IsZoomed(window_));
            tooltipAnchor_ = captionLayout_.items[static_cast<std::size_t>(hotCaption)];
        } else if (hotBar >= 0 && hotBar != static_cast<int>(BarItem::Seek) &&
            hotBar != static_cast<int>(BarItem::Time)) {
            tooltipText_ = barItemName(static_cast<BarItem>(hotBar));
            if (singleLoadedPane() < 0 && anyPaneLoaded() &&
                (hotBar == static_cast<int>(BarItem::Play) || hotBar == static_cast<int>(BarItem::Stop) ||
                 hotBar == static_cast<int>(BarItem::Audio) || hotBar == static_cast<int>(BarItem::Volume))) tooltipText_ = L"All panes: " + tooltipText_;
            tooltipAnchor_ = barLayout_.items[static_cast<std::size_t>(hotBar)];
        } else if (hotChip >= 0 && hotChipPane >= 0) {
            tooltipText_ = static_cast<PaneChip>(hotChip) == PaneChip::Repeat && singleLoadedPane() >= 0
                ? L"Repeat this video when it ends"
                : chipName(static_cast<PaneChip>(hotChip));
            tooltipAnchor_ = paneChrome_[static_cast<std::size_t>(hotChipPane)]
                .chips[static_cast<std::size_t>(hotChip)];
        } else if (hotPill >= 0) {
            tooltipText_ = L"Video menu  ·  drag the picture to swap panes";
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
    bool anyLoaded = false;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        anyLoaded = anyLoaded || paneLogicallyLoaded(pane);
    }
    scene.anyLoaded = anyLoaded;
    scene.showEmptyHint = !anyLoaded;

    scene.bar = barLayout_;
    scene.barAlpha = dockProgress_;
    scene.playing = playbackIntended();
    scene.muted = audio_.muted();
    scene.fullscreen = fullscreen_;
    scene.pinned = controlsPinned_;
    scene.seekable = duration_ > 0.0;
    scene.volume01 = std::clamp(audio_.volume(), 0.0F, 1.0F);
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
    const int subtitleTarget = subtitlePane();
    scene.subtitlesOn = subtitleSettings_.show && subtitleTarget >= 0 &&
        subtitleSelection_[static_cast<std::size_t>(subtitleTarget)].kind != SubtitleKind::None;
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
        out.hovered = static_cast<int>(pane) == hoverPane_;
        out.targeted = pane == embyPlaybackTarget();
        out.label = pillLabels_[pane];
        out.audioOn = audioPaneEnabled(pane);
        out.paused = sourcePaused_[pane];
        out.solo = soloPane_ == static_cast<int>(pane);
        // One video's repeat is what happens when it ends; among several
        // it is the video's own flag.
        out.repeat = singleLoadedPane() == static_cast<int>(pane) ? playOrder_ == PlayOrder::RepeatOne
                                                                   : sourceAutoRepeat_[pane];
        const bool usable = sources_[pane] && sources_[pane]->ready() && sources_[pane]->duration() > 0.0;
        out.seekable = usable;
        const std::wstring position = L"V" + std::to_wstring(positionForPane(pane) + 1) + L"  ";
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
            if (subtitleSettings_.show && isActive) {
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
                    out.subtitleLift = paneCoveredBottom(chromeCells_[pane]);
                }
            }
            const auto cache = sources_[pane]->cacheStats();
            if (cache.enabled && cache.aheadSeconds > 0.0) {
                out.buffered01 = static_cast<float>(
                    std::min(sourceDuration, shown + cache.aheadSeconds) / sourceDuration);
            }
            std::wstring state;
            if (!isActive) state = L"wait  ";
            else if (sourcePaused_[pane]) state = L"paused  ";
            out.timeText = position + state + formatTime(shown) + L" / " + formatTime(sourceDuration);
        } else {
            out.timeText = position + L"—";
        }
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
    panel.title = !embyBrowserOpen_ ? L"Settings"
                  : browserSource_ == BrowserSource::Local ? L"Folder" : L"Emby";
    panel.thumbnails = &thumbnails_;
    if (settingsAlpha_ > 0.0F) {
        panel.layout = panelLayout_;
        panel.rows = panelRows_;
    } else {
        panel.layout = {};
        panel.rows.clear();
    }
    panel.tabs.clear();
    panel.tab = -1;
    if (!embyBrowserOpen_) {
        for (int i = 0; i < kSettingsTabCount; ++i) panel.tabs.emplace_back(settingsTabName(static_cast<SettingsTab>(i)));
        panel.tab = static_cast<int>(settingsTab_);
    }
    panel.switchCaption = embyBrowserOpen_ ? L"\x2039 Settings" : L"Emby \x203A";
    panel.hotKind = hotPanelKind_;
    panel.hotRow = hotPanelRow_;
    panel.hotPart = hotPanelPart_;
    panel.pressedKind = pressedPanelKind_;
    panel.pressedRow = pressedPanelRow_;
    panel.pressedPart = pressedPanelPart_;
    panel.dragRow = controlDrag_ == ControlDrag::PanelSlider ? panelDragRow_ : -1;
    panel.scrollbarHot = hotPanelKind_ == PanelHitKind::Scrollbar;
    panel.scrollbarDragging = controlDrag_ == ControlDrag::PanelScrollbar;
}

// ---------------------------------------------------------------------------
// Notices

void App::showNotice(const std::wstring& text) {
    noticeText_ = text;
    noticeUntil_ = GetTickCount64() + 1500;
}

void App::expireNotice(ULONGLONG now) {
    if (!noticeUntil_ || now < noticeUntil_) return;
    noticeUntil_ = 0;
    noticeText_.clear();
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

// ---------------------------------------------------------------------------
// Seeking from the drawn rails

void App::seekFromSourceBar(std::size_t pane, double target) {
    if (pane >= sources_.size() || !sources_[pane] || sources_[pane]->duration() <= 0.0) return;
    const double sourceDuration = sources_[pane]->duration();
    target = std::clamp(target, 0.0, sourceDuration);

    if (linkedBrowserSeekBars()) {
        alignBrowserPanesToTime(target);
        return;
    }

    const double timelinePosition = clock_.position();
    const bool activeBeforeDrag = sourceIsActive(
        timelinePosition, startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]);
    if (activeSeekMode() == SeekMode::Linked && activeBeforeDrag &&
        !sourcePaused_[pane] &&
        linkedTimelineCanReachSourceTime(target, syncAdjustments_[pane])) {
        // Dragging a rail is coarse positioning, and holding every pane until
        // the slowest one has decoded its exact frame is what made this
        // unusable with four 4K HEVC sources.
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
            shown.position = browserSeekAligned_ ? clock_.position()
                                                 : currentSourceTime(static_cast<std::size_t>(reference));
            shown.duration = 0.0;
            const auto origins = paneOrigins();
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
                if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto ||
                    !sources_[pane] || !sources_[pane]->ready()) continue;
                shown.duration = std::max(shown.duration, sources_[pane]->duration());
            }
        }
        if (deviceRecoveryPending_) {
            shown.position = browserSeekAligned_ ? clock_.position() : deviceRecoveryBrowserPosition_;
            double longest = 0.0;
            const auto origins = paneOrigins();
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
                if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
                longest = std::max(longest, sources_[pane] ? sources_[pane]->duration()
                                                          : deviceRecoverySourceDurations_[pane]);
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
        if (dragPane_ >= 0) {
            dragFraction_ = railFraction(
                x, paneChrome_[static_cast<std::size_t>(dragPane_)].timeline, kRailInset * uiScale_);
        }
        break;
    case ControlDrag::Volume: {
        dragFraction_ = verticalRailFraction(static_cast<float>(local.y), barLayout_[BarItem::Volume],
                                             kVolumeInset * uiScale_);
        audio_.setVolume(dragFraction_);
        if (dragFraction_ > 0.0F && audio_.muted()) audio_.setMuted(false);
        break;
    }
    case ControlDrag::PaneVolume:
        if (dragPane_ >= 0) {
            dragFraction_ = railFraction(
                x, paneChrome_[static_cast<std::size_t>(dragPane_)].volume, kRailInset * uiScale_);
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
        showNotice(L"Volume " + std::to_wstring(static_cast<int>(std::lround(dragFraction_ * 100.0F))) + L"%");
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
    case BarItem::Subtitles: {
        const auto& box = barLayout_[BarItem::Subtitles];
        POINT point{static_cast<LONG>(box.x), static_cast<LONG>(box.y)};
        ClientToScreen(videoWindow_, &point);
        showSubtitleMenu(subtitlePane(), point);
        break;
    }
    case BarItem::Layout: {
        const auto& box = barLayout_[BarItem::Layout];
        POINT point{static_cast<LONG>(box.x), static_cast<LONG>(box.y)};
        ClientToScreen(videoWindow_, &point);
        showLayoutMenu(point);
        break;
    }
    case BarItem::Menu: {
        const auto& box = barLayout_[BarItem::Menu];
        POINT point{static_cast<LONG>(box.x), static_cast<LONG>(box.y)};
        ClientToScreen(videoWindow_, &point);
        showContextMenu(-1, point);
        break;
    }
    case BarItem::Fullscreen: toggleFullscreen(); break;
    case BarItem::Settings: toggleSettingsPanel(); break;
    default: break;
    }
}

void App::activateCaptionItem(CaptionItem item) {
    controlsLastInteraction_ = GetTickCount64();
    // Through the system commands, as the title bar's own buttons go, so
    // Windows animates them and the restore size is its own.
    switch (item) {
    case CaptionItem::Minimize: PostMessageW(window_, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case CaptionItem::Maximize:
        PostMessageW(window_, WM_SYSCOMMAND, IsZoomed(window_) ? SC_RESTORE : SC_MAXIMIZE, 0);
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
                   (enabled ? L" repeats (Independent mode)" : L" repeat off"));
        break;
    }
    case PaneChip::Close: closePane(pane); break;
    }
    updateTitle();
    scheduleAppSettingsSave();
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
        updateHoverControls([&] { POINT p = local; ClientToScreen(videoWindow_, &p); return p; }());
        pressedBarItem_ = -1;
        pressedCaptionItem_ = -1;
        pressedChipPane_ = pressedChip_ = -1;
        pressedPillPane_ = -1;
        pressedPanelKind_ = PanelHitKind::None;
        pressedPanelRow_ = pressedPanelPart_ = -1;
        pressedPanelId_ = SettingId::None;
        pressedPanelParam_ = -1;
        // The window buttons stand above the sheet's column as well.
        if (const int item = captionItemAt(static_cast<float>(local.x), static_cast<float>(local.y),
                                           captionLayout_, captionAlpha() > 0.05F);
            item >= 0) {
            pressedCaptionItem_ = item;
            SetCapture(videoWindow_);
            return 0;
        }
        // The video beside a docked browser is still the player: a click
        // there is not a click outside the sheet.
        if (settingsAlpha_ > 0.05F &&
            !(embyBrowserDocked() && panelHitAt(local).kind == PanelHitKind::Outside)) {
            const PanelHit panelHit = panelHitAt(local);
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
                // thumb jumps under the pointer.
                const OverlayRect thumb = panelScrollThumb(panelLayout_, uiScale_);
                const float y = static_cast<float>(local.y);
                panelThumbGrab_ = y >= thumb.y && y <= thumb.bottom() ? y - thumb.y : thumb.height * 0.5F;
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
                if (!draggingPane_ && paneDragMovesWindow()) {
                    // No other pane to drop it on: as in PotPlayer, the
                    // picture drags the window.
                    dragSourcePane_ = -1;
                    if (GetCapture() == videoWindow_) ReleaseCapture();
                    SendMessageW(window_, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
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
                const auto& pill = paneChrome_[static_cast<std::size_t>(pane)].pill;
                POINT point{static_cast<LONG>(pill.x), static_cast<LONG>(pill.bottom() + 4.0F * uiScale_)};
                ClientToScreen(videoWindow_, &point);
                showContextMenu(pane, point);
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
            pressedChipPane_ = pressedChip_ = -1;
            pressedPillPane_ = -1;
            pressedPanelKind_ = PanelHitKind::None;
            pressedPanelRow_ = pressedPanelPart_ = -1;
            panelPressArmed_ = false;
            dragSourcePane_ = -1;
            draggingPane_ = false;
            dropTargetPane_ = -1;
        }
        return 0;
    case WM_RBUTTONUP: {
        SetFocus(window_);
        if (settingsAlpha_ > 0.05F) {
            const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (panelHitAt(local).kind != PanelHitKind::Outside) return 0;
        }
        RECT client{};
        GetClientRect(videoWindow_, &client);
        const int pane = activePaneAt(
            static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)),
            static_cast<float>(client.right), static_cast<float>(client.bottom), activePanes(),
            layoutMode_, expandedPane_, soloPane_, paneAspectRatios(),
            autoLayoutFocus_, autoLayoutFocusPane_);
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(videoWindow_, &screenPoint);
        if (pane >= 0) embySelectTarget(static_cast<std::size_t>(pane));
        showContextMenu(pane, screenPoint);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const POINT local{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (settingsAlpha_ > 0.05F && panelHitAt(local).kind != PanelHitKind::Outside) return 0;
        const OverlayHit hit = hitTestAt(local);
        if (hit.kind == OverlayHitKind::Video && hit.pane >= 0 &&
            sources_[static_cast<std::size_t>(hit.pane)]) {
            toggleSolo(hit.pane);
        }
        return 0;
    }
    case WM_KEYDOWN:
        return SendMessageW(window_, message, wParam, lParam);
    case WM_SYSKEYDOWN:
    case WM_MOUSEWHEEL:
        return SendMessageW(window_, message, wParam, lParam);
    case WM_APPCOMMAND:
        return SendMessageW(window_, message, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(videoWindow_, message, wParam, lParam);
    }
}

}  // namespace quaddeck
