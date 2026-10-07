#pragma once

// Geometry of the drawn user interface: the transport bar along the bottom
// of the window and the chrome each video pane wears (its label pill, action
// chips and timeline). Pure functions on floats, so the core test binary can
// exhaust them without a window, a device or a font. Everything is in
// pixels; every 96-DPI metric is multiplied by `scale` here, once.
//
// Overlay.cpp draws these rectangles and App hit-tests them. Neither has
// any other geometry of its own.

#include "Core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace quaddeck {

struct OverlayRect {
    float x{};
    float y{};
    float width{};
    float height{};
    bool visible() const { return width > 0.0F && height > 0.0F; }
    float right() const { return x + width; }
    float bottom() const { return y + height; }
    bool contains(float px, float py) const {
        return visible() && px >= x && py >= y && px < right() && py < bottom();
    }
};

inline OverlayRect overlayRect(const RectF& rect) {
    return {rect.x, rect.y, rect.width, rect.height};
}

// ---------------------------------------------------------------------------
// Transport bar

// In the order they sit on the bar. Previous and Next are there only with
// one video loaded: they step through its folder or its Emby list. Layout
// is there only with several: it opens the arrangement menu.
enum class BarItem {
    Previous,
    Play,
    Next,
    Stop,
    Time,
    Seek,
    Audio,
    Volume,
    Subtitles,
    Layout,
    Menu,
    Fullscreen,
    Settings,
};
inline constexpr std::size_t kBarItemCount = 13;

struct TransportBarLayout {
    // The gradient scrim the row sits on. Not a control.
    OverlayRect band;
    // The strip the controls occupy; the pointer inside it keeps the bar open.
    OverlayRect row;
    // Non-interactive master-control scope label, drawn for multiple panes.
    OverlayRect scopeLabel;
    std::array<OverlayRect, kBarItemCount> items{};
    const OverlayRect& operator[](BarItem item) const {
        return items[static_cast<std::size_t>(item)];
    }
};

// 96-DPI metrics of the bar.
struct BarMetrics {
    float bandHeight{112.0F};
    float rowHeight{40.0F};
    float button{40.0F};
    float gap{6.0F};
    float side{16.0F};
    float bottomMargin{16.0F};
    float timeWidth{124.0F};
    // The volume is a popup standing on the speaker button, not a row
    // item: the seek rail keeps its length whether it is open or not.
    float volumePopupWidth{28.0F};
    float volumePopupHeight{120.0F};
    float seekPreferred{160.0F};
    float seekMinimumWithPlay{40.0F};
};

// Lay the bar out for a client of `width` x `height`. The seek rail takes
// whatever is left; when even the preferred rail does not fit, controls are
// removed as complete units in a fixed order, because each remains reachable
// from the keyboard, the menu or the settings sheet. Play and the rail are
// the final pair; below their minimum the rail alone spans the row. A
// control that is out never comes back at a narrower width. Without a shared
// timeline, the middle stays blank and the controls on either side retain
// their positions and hit targets. `singleVideo` puts the step buttons on
// the bar, `severalVideos` the arrangement button.
inline TransportBarLayout transportBarLayout(
    float width, float height, float scale, bool volumeOpen, bool singleVideo = false,
    const BarMetrics& metrics = {}, bool showTimeline = true, bool severalVideos = false) {
    TransportBarLayout result;
    const float s = std::max(0.0F, scale);
    width = std::max(0.0F, width);
    height = std::max(0.0F, height);
    if (width <= 0.0F || height <= 0.0F || s <= 0.0F) return result;

    const float rowHeight = std::min(metrics.rowHeight * s, height);
    const float bottomMargin = std::min(metrics.bottomMargin * s,
                                        std::max(0.0F, height - rowHeight));
    result.band = {0.0F, std::max(0.0F, height - metrics.bandHeight * s),
                   width, std::min(metrics.bandHeight * s, height)};
    result.row = {0.0F, height - bottomMargin - rowHeight, width, rowHeight};

    std::array<float, kBarItemCount> widths{
        metrics.button, metrics.button, metrics.button, metrics.button, metrics.timeWidth, 0.0F,
        metrics.button, 0.0F, metrics.button, metrics.button,
        metrics.button, metrics.button, metrics.button};
    for (auto& value : widths) value *= s;
    std::array<bool, kBarItemCount> present{};
    present.fill(true);
    const auto index = [](BarItem item) { return static_cast<std::size_t>(item); };
    present[index(BarItem::Volume)] = false;
    present[index(BarItem::Previous)] = singleVideo;
    present[index(BarItem::Next)] = singleVideo;
    present[index(BarItem::Layout)] = severalVideos;
    present[index(BarItem::Time)] = showTimeline;
    present[index(BarItem::Seek)] = showTimeline;
    const float side = std::min(metrics.side * s, width / 8.0F);
    const float gap = metrics.gap * s;
    const float scopeHeight = 18.0F * s;
    const float scopeTop = result.row.y - 24.0F * s;
    if (scopeTop >= 0.0F && width - 2.0F * side >= 40.0F * s) {
        result.scopeLabel = {side, scopeTop, 40.0F * s, scopeHeight};
    }

    const auto required = [&](float seekWidth) {
        float total = showTimeline ? seekWidth : 0.0F;
        int count = showTimeline ? 1 : 0;
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            if (i == index(BarItem::Seek) || !present[i]) continue;
            total += widths[i];
            ++count;
        }
        return side * 2.0F + total + gap * static_cast<float>(std::max(0, count - 1));
    };
    constexpr std::array<BarItem, 10> hideOrder{
        BarItem::Settings, BarItem::Layout, BarItem::Menu, BarItem::Subtitles, BarItem::Stop,
        BarItem::Time, BarItem::Audio, BarItem::Fullscreen, BarItem::Previous, BarItem::Next};
    for (const auto item : hideOrder) {
        if (required(showTimeline ? metrics.seekPreferred * s : 0.0F) <= width) break;
        present[index(item)] = false;
    }
    if (showTimeline && present[index(BarItem::Play)] &&
        required(metrics.seekMinimumWithPlay * s) > width) {
        present[index(BarItem::Play)] = false;
    }

    float fixed = 0.0F;
    int visibleCount = showTimeline ? 1 : 0;
    for (std::size_t i = 0; i < kBarItemCount; ++i) {
        if (i == index(BarItem::Seek) || !present[i]) continue;
        fixed += widths[i];
        ++visibleCount;
    }
    const float freeWidth = std::max(
        0.0F, width - side * 2.0F - fixed - gap * static_cast<float>(std::max(0, visibleCount - 1)));
    widths[index(BarItem::Seek)] = showTimeline ? freeWidth : 0.0F;
    present[index(BarItem::Seek)] = showTimeline && freeWidth > 0.0F;

    float x = side;
    bool placed = false;
    for (std::size_t i = 0; i < kBarItemCount; ++i) {
        if (i == index(BarItem::Seek) && !showTimeline && placed) {
            // Reserve the flexible middle without creating a visible rail or
            // an invisible seek hit target. Keep the right-hand controls put.
            x += freeWidth;
        }
        if (!present[i] || widths[i] <= 0.0F) continue;
        if (placed) x += gap;
        const float clipped = std::min(widths[i], std::max(0.0F, width - x));
        if (clipped <= 0.0F) continue;
        result.items[i] = {x, result.row.y, clipped, rowHeight};
        x += clipped;
        placed = true;
    }
    // The volume popup stands on the speaker, reaching down to it so the
    // pointer can climb from the button into the rail without a gap.
    const auto& audio = result.items[index(BarItem::Audio)];
    if (volumeOpen && audio.visible()) {
        const float popupWidth = std::min(metrics.volumePopupWidth * s, audio.width);
        const float top = std::max(0.0F, audio.y - metrics.volumePopupHeight * s);
        result.items[index(BarItem::Volume)] = {
            audio.x + (audio.width - popupWidth) * 0.5F, top, popupWidth, std::max(0.0F, audio.y - top)};
    }
    return result;
}

// ---------------------------------------------------------------------------
// Window caption

// The window has no Windows title bar. As in PotPlayer with "auto hide main
// skin while video screen is active", a strip drawn over the top of the
// picture carries the title and the window buttons, and comes and goes with
// the transport bar.
enum class CaptionItem { Minimize, Maximize, Close };
inline constexpr std::size_t kCaptionItemCount = 3;

struct CaptionLayout {
    // The gradient scrim behind the strip. Not a control.
    OverlayRect band;
    // The strip: whatever in it is not a button drags the window.
    OverlayRect row;
    OverlayRect title;
    std::array<OverlayRect, kCaptionItemCount> items{};
    const OverlayRect& operator[](CaptionItem item) const {
        return items[static_cast<std::size_t>(item)];
    }
};

// 96-DPI metrics of the caption, those of Windows 11's own.
struct CaptionMetrics {
    float rowHeight{32.0F};
    float button{46.0F};
    float bandHeight{72.0F};
    float titleInset{14.0F};
};

// The buttons stand at the right, Close outermost; a window too narrow for
// all three keeps Close, then Maximize.
inline CaptionLayout captionLayout(float width, float height, float scale,
                                   const CaptionMetrics& metrics = {}) {
    CaptionLayout result;
    const float s = std::max(0.0F, scale);
    width = std::max(0.0F, width);
    height = std::max(0.0F, height);
    if (width <= 0.0F || height <= 0.0F || s <= 0.0F) return result;
    const float rowHeight = std::min(metrics.rowHeight * s, height);
    result.row = {0.0F, 0.0F, width, rowHeight};
    result.band = {0.0F, 0.0F, width, std::min(metrics.bandHeight * s, height)};
    const float button = metrics.button * s;
    float right = width;
    for (const auto item : {CaptionItem::Close, CaptionItem::Maximize, CaptionItem::Minimize}) {
        if (right < button) break;
        right -= button;
        result.items[static_cast<std::size_t>(item)] = {right, 0.0F, button, rowHeight};
    }
    const float inset = metrics.titleInset * s;
    if (right > inset * 2.0F) result.title = {inset, 0.0F, right - inset * 2.0F, rowHeight};
    return result;
}

// What the window manager is told a point in the client is. The sides and
// the bottom keep Windows' own resize frame, outside the picture; only the
// top, where the title bar was, has its resize band inside it.
// `resizeBand` is 0 for a maximized or fullscreen window, and `corner` is
// how far along the top edge a corner reaches.
enum class WindowFrameHit { Client, Caption, Top, TopLeft, TopRight };

inline WindowFrameHit windowFrameHitTest(
    float px, float py, float width, float resizeBand, float corner,
    const CaptionLayout& caption, bool captionActive) {
    if (px < 0.0F || py < 0.0F || px >= width) return WindowFrameHit::Client;
    if (py < resizeBand) {
        if (px < corner) return WindowFrameHit::TopLeft;
        if (px >= width - corner) return WindowFrameHit::TopRight;
        return WindowFrameHit::Top;
    }
    if (!captionActive || !caption.row.contains(px, py)) return WindowFrameHit::Client;
    for (const auto& item : caption.items) {
        if (item.contains(px, py)) return WindowFrameHit::Client;
    }
    return WindowFrameHit::Caption;
}

// The caption button under a point, -1 for none or while the strip is hidden.
inline int captionItemAt(float px, float py, const CaptionLayout& caption, bool captionActive) {
    if (!captionActive) return -1;
    for (std::size_t i = 0; i < kCaptionItemCount; ++i) {
        if (caption.items[i].contains(px, py)) return static_cast<int>(i);
    }
    return -1;
}

// Position along a vertical rail, 0 at the bottom and 1 at the top.
inline float verticalRailFraction(float py, const OverlayRect& rail, float inset) {
    const float top = rail.y + inset;
    const float bottom = rail.bottom() - inset;
    if (bottom <= top) return 0.0F;
    return std::clamp((bottom - py) / (bottom - top), 0.0F, 1.0F);
}

// ---------------------------------------------------------------------------
// Pane chrome

enum class PaneChip { Audio, Pause, Solo, Repeat, Close };
inline constexpr std::size_t kPaneChipCount = 5;

struct PaneChromeLayout {
    // The "V1 · 1.0× · Fit" label in the pane's top-left corner. Clicking it
    // opens the pane menu.
    OverlayRect pill;
    // Action chips to the right of the pill, shown while the pane is hovered.
    // A chip that would leave the pane is dropped, from the right.
    std::array<OverlayRect, kPaneChipCount> chips{};
    // A volume rail after the chips, only for a pane that is being heard.
    OverlayRect volume;
    // The seek rail's hit band along the bottom edge of the pane.
    OverlayRect timeline;
    // Where the pane's time reads while the rail is hovered or dragged.
    OverlayRect timeLabel;
    const OverlayRect& operator[](PaneChip chip) const {
        return chips[static_cast<std::size_t>(chip)];
    }
};

struct PaneChromeMetrics {
    float margin{12.0F};
    float pillHeight{28.0F};
    float pillPadding{12.0F};
    float chip{30.0F};
    float chipGap{6.0F};
    float pillChipGap{8.0F};
    float volumeWidth{96.0F};
    float railBand{22.0F};
    float railInset{12.0F};
    float timeLabelHeight{20.0F};
    float timeLabelWidth{180.0F};
    // A pane shorter than this shows no rail: there would be no picture left.
    float minimumHeightForRail{72.0F};
    float minimumHeightForPill{40.0F};
};

// `pillTextWidth` is the measured width of the label text in pixels;
// `expanded` adds the chips; `coveredBottom` is how many pixels of the
// pane's bottom edge the transport bar currently occupies, which lifts the
// rail so the two never overlap. `coveredTop` is how far down from the
// pane's top edge the window caption reaches; the pill and its chips sit
// below it, and a pane that cannot hold them there shows neither.
inline PaneChromeLayout paneChromeLayout(
    const RectF& cell, float scale, float pillTextWidth, bool expanded,
    float coveredBottom = 0.0F, bool withVolume = false,
    const PaneChromeMetrics& metrics = {}, float coveredTop = 0.0F) {
    PaneChromeLayout result;
    const float s = std::max(0.0F, scale);
    if (cell.width <= 0.0F || cell.height <= 0.0F || s <= 0.0F) return result;
    const float margin = std::min(metrics.margin * s, cell.width / 4.0F);
    const float innerRight = cell.x + cell.width - margin;
    const float captionTop = std::clamp(coveredTop, 0.0F, cell.height);

    if (cell.height - captionTop >= metrics.minimumHeightForPill * s) {
        const float pillHeight = metrics.pillHeight * s;
        const float pillWidth = std::clamp(
            std::max(0.0F, pillTextWidth) + metrics.pillPadding * s * 2.0F,
            pillHeight, std::max(pillHeight, innerRight - (cell.x + margin)));
        result.pill = {cell.x + margin, cell.y + captionTop + margin, pillWidth, pillHeight};
        if (expanded) {
            const float chip = metrics.chip * s;
            float x = result.pill.right() + metrics.pillChipGap * s;
            const float y = result.pill.y + (pillHeight - chip) * 0.5F;
            for (std::size_t i = 0; i < kPaneChipCount; ++i) {
                if (x + chip > innerRight) break;
                result.chips[i] = {x, y, chip, chip};
                x += chip + metrics.chipGap * s;
            }
            // The volume rail follows the chips and, like them, is dropped
            // rather than squeezed when the pane cannot hold it.
            if (withVolume && result.chips[kPaneChipCount - 1].visible()) {
                const float width = metrics.volumeWidth * s;
                if (x + width <= innerRight) {
                    result.volume = {x, y, width, chip};
                }
            }
        }
    }

    if (cell.height >= metrics.minimumHeightForRail * s) {
        const float band = metrics.railBand * s;
        const float lifted = std::max(0.0F, coveredBottom);
        const float bottom = cell.y + cell.height - lifted;
        const float top = bottom - band;
        // The rail must stay below the pill and inside the pane.
        const float lowest = result.pill.visible() ? result.pill.bottom() + margin : cell.y + captionTop;
        if (top >= lowest) {
            result.timeline = {cell.x, top, cell.width, band};
            const float labelHeight = metrics.timeLabelHeight * s;
            result.timeLabel = {
                cell.x + margin, top - labelHeight,
                std::min(metrics.timeLabelWidth * s, std::max(0.0F, cell.width - margin * 2.0F)),
                labelHeight};
            if (result.timeLabel.y < lowest) result.timeLabel = {};
        }
    }
    return result;
}

// Position along a rail, 0..1, for a pointer x inside the rail's hit band.
// `inset` is the horizontal padding between the band and the drawn line.
inline float railFraction(float px, const OverlayRect& rail, float inset) {
    const float left = rail.x + std::max(0.0F, inset);
    const float right = rail.right() - std::max(0.0F, inset);
    if (right <= left) return 0.0F;
    return std::clamp((px - left) / (right - left), 0.0F, 1.0F);
}

// ---------------------------------------------------------------------------
// Hit testing

enum class OverlayHitKind { None, Video, BarItem, PanePill, PaneChip, PaneVolume, PaneTimeline, CaptionItem };

struct OverlayHit {
    OverlayHitKind kind{OverlayHitKind::None};
    int pane{-1};
    int item{-1};
};

// The bar wins over the panes beneath it, and within a pane the chips win
// over the pill and the pill over the rail. `barActive` is false while the
// bar is hidden, so its invisible row does not swallow clicks; the same for
// `chromeActive` per pane.
inline OverlayHit overlayHitTest(
    float px, float py,
    const TransportBarLayout& bar, bool barActive,
    const PaneArray<PaneChromeLayout>& chrome, const PaneArray<bool>& chromeActive,
    const PaneArray<RectF>& cells, const PaneArray<bool>& active) {
    if (barActive) {
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            if (bar.items[i].contains(px, py)) {
                return {OverlayHitKind::BarItem, -1, static_cast<int>(i)};
            }
        }
        // Empty space in the transport row and its bottom margin belongs to
        // the bar too. Otherwise a drag on a hidden rail swaps the pane below.
        if (px >= bar.row.x && px < bar.row.right() &&
            py >= bar.row.y && py < bar.band.bottom()) return {};
    }
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!active[pane] || !overlayRect(cells[pane]).contains(px, py)) continue;
        const int index = static_cast<int>(pane);
        if (chromeActive[pane]) {
            for (std::size_t chip = 0; chip < kPaneChipCount; ++chip) {
                if (chrome[pane].chips[chip].contains(px, py)) {
                    return {OverlayHitKind::PaneChip, index, static_cast<int>(chip)};
                }
            }
            if (chrome[pane].volume.contains(px, py)) return {OverlayHitKind::PaneVolume, index, -1};
            if (chrome[pane].pill.contains(px, py)) return {OverlayHitKind::PanePill, index, -1};
            if (chrome[pane].timeline.contains(px, py)) {
                return {OverlayHitKind::PaneTimeline, index, -1};
            }
        }
        return {OverlayHitKind::Video, index, -1};
    }
    return {};
}

}  // namespace quaddeck
