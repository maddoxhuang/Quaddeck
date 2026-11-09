#pragma once

// Geometry of the drawn user interface: the transport bar along the bottom
// of the window and the chrome each video pane wears (the strip along its
// bottom: its seek rail, its tag and its buttons). Pure functions on floats
// keep the layout independent of a window, a device or a font. Everything is
// in pixels; every 96-DPI metric is multiplied by `scale` here, once.
//
// Overlay.cpp draws these rectangles and App hit-tests them. Neither has
// any other geometry of its own.

#include "Core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>

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

// In the order they sit on the bar. Previous and Next are there with one
// video loaded: they step through its folder or its Emby list, and
// Subtitles is that video's switch; among several each has its own on its
// strip. While a video set plays, Previous and Next are there whatever is
// loaded, and step to the set before or after it. Sync and Layout are
// there only with several: Sync opens the sync panel (the seek bars' mode,
// each video's offset and repeat), Layout the arrangement menu. So are the
// videos' buttons: All videos shows every video, SoloV1 to SoloV5 one
// alone, a button for each video loaded, so one alone gives way to another
// without the grid between. Sets opens the video sets in the sheet. There
// is no menu button: a right-click opens the menu anywhere on the picture.
// Nor is there a settings button: F5 and the menu open the sheet.
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
    AllVideos,
    SoloV1,
    SoloV2,
    SoloV3,
    SoloV4,
    SoloV5,
    Sync,
    Layout,
    Sets,
    Fullscreen,
};
inline constexpr std::size_t kBarItemCount = 19;

// The button that shows the video at a place on the screen alone (0 for
// V1), and back: the place a button shows alone, or -1 for any other.
inline BarItem soloBarItem(std::size_t position) {
    return static_cast<BarItem>(static_cast<std::size_t>(BarItem::SoloV1) + std::min(position, kMaxPanes - 1));
}

inline int barItemSoloPosition(BarItem item) {
    const int index = static_cast<int>(item) - static_cast<int>(BarItem::SoloV1);
    return index >= 0 && index < static_cast<int>(kMaxPanes) ? index : -1;
}

struct TransportBarLayout {
    // The gradient scrim the row sits on. Not a control.
    OverlayRect band;
    // The strip the controls occupy; the pointer inside it keeps the bar open.
    // As a capsule, the capsule itself: there is no band then.
    OverlayRect row;
    // The capsule's "All", a label, not a control. The full bar has none:
    // until 2026-10-21 it had an "All, linked" label over its left end.
    OverlayRect scopeLabel;
    std::array<OverlayRect, kBarItemCount> items{};
    // The bar is the "All" capsule (capsuleBarLayout), and the line in it
    // between the keys of playback and those of the window -- or, with the
    // videos' buttons between them, the lines before and after those.
    bool capsule{};
    OverlayRect divider;
    OverlayRect videosDivider;
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
    // The capsule. Its bottom stands `capsuleLift` above the client's, clear
    // of a pane strip's rail band (PaneChromeMetrics: 8 + 30 + 22) and 6 more.
    float capsuleHeight{40.0F};
    float capsuleButton{32.0F};
    float capsuleGap{2.0F};
    float capsulePadding{6.0F};
    float capsuleLabelWidth{44.0F};
    float capsuleLabelHeight{22.0F};
    float capsuleDivider{13.0F};
    float capsuleLift{66.0F};
};

namespace detail {

// The popup stands on the speaker, reaching down to it so the pointer can
// climb from the button into the rail without a gap.
inline void standVolumeOnSpeaker(TransportBarLayout& result, bool volumeOpen, float s, const BarMetrics& metrics) {
    const auto& audio = result.items[static_cast<std::size_t>(BarItem::Audio)];
    if (!volumeOpen || !audio.visible()) return;
    const float popupWidth = std::min(metrics.volumePopupWidth * s, audio.width);
    const float top = std::max(0.0F, audio.y - metrics.volumePopupHeight * s);
    result.items[static_cast<std::size_t>(BarItem::Volume)] = {
        audio.x + (audio.width - popupWidth) * 0.5F, top, popupWidth, std::max(0.0F, audio.y - top)};
}

}  // namespace detail

// A moving pointer wakes the bar from the bottom edge; the capsule also
// where it stands, `margin` around it included, so it is found where it is
// seen. Hidden or not, its layout is there to be looked up.
inline bool capsuleRevealWanted(const TransportBarLayout& bar, float px, float py, float margin) {
    if (!bar.capsule || !bar.row.visible()) return false;
    margin = std::max(0.0F, margin);
    return px >= bar.row.x - margin && px < bar.row.right() + margin &&
           py >= bar.row.y - margin && py < bar.row.bottom() + margin;
}

// Among several videos with no shared timeline the bar would be a row of
// keys on either side of nothing, and each pane's strip would have to stand
// on it. Instead the keys that act on every video -- play, stop, sound, the
// sync panel, the arrangement and fullscreen -- are one capsule labelled
// "All", centred on the bottom above the strips, which keep to their own
// bottom edges. A line parts playback's keys from the rest. With `videos`
// of two or more, the videos' buttons stand between two lines after
// playback's: All videos, then one for each of V1 up to that many.
// `setSteps` puts Previous and Next on either side of play, while a video
// set plays; `setsButton` the button of the video sets before fullscreen.
// Too narrow, the capsule lets go of its label, the sets' button, the
// arrangement, sync, the videos' buttons (all of them at once), Previous
// and Next (together), stop, sound and fullscreen in that order; play
// stays. It is centred on `centreX` (the middle of `width` when
// negative): the window's middle, so a browser docked over the right of
// the picture does not move it. Only where the browser would cover it is it
// pushed left, just clear of it.
inline TransportBarLayout capsuleBarLayout(float width, float height, float scale, bool volumeOpen,
                                           const BarMetrics& metrics = {}, float centreX = -1.0F,
                                           std::size_t videos = 0, bool setSteps = false,
                                           bool setsButton = false) {
    TransportBarLayout result;
    result.capsule = true;
    const float s = std::max(0.0F, scale);
    width = std::max(0.0F, width);
    height = std::max(0.0F, height);
    if (width <= 0.0F || height <= 0.0F || s <= 0.0F) return result;
    const float rowHeight = metrics.capsuleHeight * s;
    if (height < metrics.capsuleLift * s + rowHeight) return result;
    const float top = height - metrics.capsuleLift * s - rowHeight;
    const float button = metrics.capsuleButton * s;
    const float gap = metrics.capsuleGap * s;
    const float padding = metrics.capsulePadding * s;
    const float labelWidth = metrics.capsuleLabelWidth * s;
    const float labelHeight = metrics.capsuleLabelHeight * s;
    const float divider = metrics.capsuleDivider * s;
    const float side = std::min(metrics.side * s, width / 8.0F);
    const auto index = [](BarItem item) { return static_cast<std::size_t>(item); };
    std::array<bool, kBarItemCount> present{};
    for (const auto item : {BarItem::Play, BarItem::Stop, BarItem::Audio, BarItem::Sync, BarItem::Layout,
                            BarItem::Fullscreen}) {
        present[index(item)] = true;
    }
    present[index(BarItem::Previous)] = present[index(BarItem::Next)] = setSteps;
    present[index(BarItem::Sets)] = setsButton;
    const std::size_t soloButtons = videos >= 2 ? std::min(videos, kMaxPanes) : 0;
    const auto showVideos = [&](bool shown) {
        present[index(BarItem::AllVideos)] = shown && soloButtons > 0;
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            present[index(soloBarItem(position))] = shown && position < soloButtons;
        }
    };
    showVideos(true);
    bool withLabel = true;
    // Lays the capsule out from `left`, or only measures it, and returns its width.
    const auto place = [&](float left, bool commit) {
        float x = left + padding;
        if (withLabel) {
            if (commit) result.scopeLabel = {x, top + (rowHeight - labelHeight) * 0.5F, labelWidth, labelHeight};
            x += labelWidth + padding;
        }
        bool afterButton = false;
        const auto put = [&](BarItem item) {
            if (!present[index(item)]) return;
            if (afterButton) x += gap;
            if (commit) result.items[index(item)] = {x, top + (rowHeight - button) * 0.5F, button, button};
            x += button;
            afterButton = true;
        };
        // A line after the buttons so far, where buttons follow it.
        const auto line = [&](OverlayRect& target, std::initializer_list<BarItem> following) {
            bool any = false;
            for (const auto item : following) any = any || present[index(item)];
            if (!afterButton || !any) return;
            const float thickness = std::max(1.0F, s);
            if (commit) {
                target = {x + (divider - thickness) * 0.5F, top + (rowHeight - 14.0F * s) * 0.5F, thickness,
                          14.0F * s};
            }
            x += divider;
            afterButton = false;
        };
        put(BarItem::Previous);
        put(BarItem::Play);
        put(BarItem::Next);
        put(BarItem::Stop);
        put(BarItem::Audio);
        line(result.divider,
             {BarItem::AllVideos, BarItem::Sync, BarItem::Layout, BarItem::Sets, BarItem::Fullscreen});
        put(BarItem::AllVideos);
        for (std::size_t position = 0; position < kMaxPanes; ++position) put(soloBarItem(position));
        line(result.videosDivider, {BarItem::Sync, BarItem::Layout, BarItem::Sets, BarItem::Fullscreen});
        put(BarItem::Sync);
        put(BarItem::Layout);
        put(BarItem::Sets);
        put(BarItem::Fullscreen);
        return x + padding - left;
    };
    const float room = width - side * 2.0F;
    if (place(0.0F, false) > room) withLabel = false;
    // Next stands for the pair of steps, which leave together.
    for (const auto item : {BarItem::Sets, BarItem::Layout, BarItem::Sync, BarItem::AllVideos, BarItem::Next,
                            BarItem::Stop, BarItem::Audio, BarItem::Fullscreen}) {
        if (place(0.0F, false) <= room) break;
        if (item == BarItem::AllVideos) showVideos(false);
        else if (item == BarItem::Next) present[index(BarItem::Previous)] = present[index(BarItem::Next)] = false;
        else present[index(item)] = false;
    }
    const float capsuleWidth = place(0.0F, false);
    if (capsuleWidth > width) return result;
    const float edge = std::min(side, (width - capsuleWidth) * 0.5F);
    const float centre = centreX >= 0.0F ? centreX : width * 0.5F;
    const float left = std::clamp(centre - capsuleWidth * 0.5F, edge, width - capsuleWidth - edge);
    result.row = {left, top, capsuleWidth, rowHeight};
    place(left, true);
    detail::standVolumeOnSpeaker(result, volumeOpen, s, metrics);
    return result;
}

// Lay the bar out for a client of `width` x `height`. The seek rail takes
// whatever is left; when even the preferred rail does not fit, controls are
// removed as complete units in a fixed order, because each remains reachable
// from the keyboard, the menu or the settings window. Play and the rail are
// the final pair; below their minimum the rail alone spans the row. A
// control that is out never comes back at a narrower width. Without a shared
// timeline, the middle stays blank and the controls on either side retain
// their positions and hit targets -- among several videos the bar is then
// the "All" capsule instead (capsuleBarLayout), centred on `capsuleCentreX`.
// `singleVideo` puts the step buttons and the subtitle switch on the bar,
// `severalVideos` the sync and arrangement buttons, and with them `videos`
// of two or more the videos' buttons, All videos and one for each of V1 up
// to that many, between the sound and sync; they leave together, after sync.
// `setSteps` puts the step buttons on the bar however many videos there
// are, while a video set plays; `setsButton` the video sets' button before
// fullscreen, which leaves first.
inline TransportBarLayout transportBarLayout(
    float width, float height, float scale, bool volumeOpen, bool singleVideo = false,
    const BarMetrics& metrics = {}, bool showTimeline = true, bool severalVideos = false,
    float capsuleCentreX = -1.0F, std::size_t videos = 0, bool setSteps = false, bool setsButton = false) {
    if (!severalVideos) videos = 0;
    if (severalVideos && !showTimeline) {
        return capsuleBarLayout(width, height, scale, volumeOpen, metrics, capsuleCentreX, videos, setSteps,
                                setsButton);
    }
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

    std::array<float, kBarItemCount> widths{};
    widths.fill(metrics.button);
    const auto index = [](BarItem item) { return static_cast<std::size_t>(item); };
    widths[index(BarItem::Time)] = metrics.timeWidth;
    widths[index(BarItem::Seek)] = 0.0F;
    widths[index(BarItem::Volume)] = 0.0F;
    for (auto& value : widths) value *= s;
    std::array<bool, kBarItemCount> present{};
    present.fill(true);
    present[index(BarItem::Volume)] = false;
    present[index(BarItem::Previous)] = singleVideo || setSteps;
    present[index(BarItem::Next)] = singleVideo || setSteps;
    present[index(BarItem::Subtitles)] = singleVideo;
    present[index(BarItem::Sets)] = setsButton;
    present[index(BarItem::Sync)] = severalVideos;
    present[index(BarItem::Layout)] = severalVideos;
    present[index(BarItem::Time)] = showTimeline;
    present[index(BarItem::Seek)] = showTimeline;
    const std::size_t soloButtons = videos >= 2 ? std::min(videos, kMaxPanes) : 0;
    const auto showVideos = [&](bool shown) {
        present[index(BarItem::AllVideos)] = shown && soloButtons > 0;
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            present[index(soloBarItem(position))] = shown && position < soloButtons;
        }
    };
    showVideos(true);
    const float side = std::min(metrics.side * s, width / 8.0F);
    const float gap = metrics.gap * s;

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
    // All videos stands for all the videos' buttons, which leave together.
    constexpr std::array<BarItem, 11> hideOrder{
        BarItem::Sets, BarItem::Layout, BarItem::Sync, BarItem::AllVideos, BarItem::Subtitles, BarItem::Stop,
        BarItem::Time, BarItem::Audio, BarItem::Fullscreen, BarItem::Previous, BarItem::Next};
    for (const auto item : hideOrder) {
        if (required(showTimeline ? metrics.seekPreferred * s : 0.0F) <= width) break;
        if (item == BarItem::AllVideos) showVideos(false);
        else present[index(item)] = false;
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
    detail::standVolumeOnSpeaker(result, volumeOpen, s, metrics);
    return result;
}

// ---------------------------------------------------------------------------
// Window caption

// The window has no Windows title bar. As in PotPlayer with "auto hide main
// skin while video screen is active", a strip drawn over the top of the
// picture carries the title and the window buttons, and comes and goes with
// the transport bar -- in fullscreen too, where nothing on it moves the window.
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

// ---------------------------------------------------------------------------
// The empty window

// With nothing loaded the picture is a drop hint: a title, the keys under
// it, and a row of buttons: Open file, the O key's dialog, and beside it --
// `withEmby`, while signed in to Emby -- one that opens the browser, so a
// video or the library is a click away before anything plays. They stand
// as one group about the middle, the row centred under the keys; it keeps
// above `barTop`, the bar's row, and is left out where it would not fit.
// A window too narrow for both keeps Open file alone.
struct EmptyHintLayout {
    OverlayRect title;
    OverlayRect keys;
    OverlayRect open;
    OverlayRect emby;
};

// 96-DPI metrics of the hint.
struct EmptyHintMetrics {
    float titleHeight{36.0F};
    float keysGap{6.0F};
    float keysHeight{24.0F};
    float buttonGap{18.0F};
    float buttonWidth{148.0F};
    float buttonHeight{36.0F};
    float buttonSpacing{12.0F};
};

inline EmptyHintLayout emptyHintLayout(float width, float height, float scale, float barTop, bool withEmby,
                                       const EmptyHintMetrics& metrics = {}) {
    EmptyHintLayout result;
    const float s = std::max(0.0F, scale);
    width = std::max(0.0F, width);
    height = std::max(0.0F, height);
    if (width <= 0.0F || height <= 0.0F || s <= 0.0F) return result;
    const float titleHeight = metrics.titleHeight * s;
    const float keysTop = titleHeight + metrics.keysGap * s;
    const float keysBottom = keysTop + metrics.keysHeight * s;
    const float buttonTop = keysBottom + metrics.buttonGap * s;
    const float buttonHeight = metrics.buttonHeight * s;
    const float top = height * 0.5F - (buttonTop + buttonHeight) * 0.5F;
    result.title = {0.0F, top, width, titleHeight};
    result.keys = {0.0F, top + keysTop, width, metrics.keysHeight * s};
    const float buttonWidth = metrics.buttonWidth * s;
    const float y = top + buttonTop;
    if (y + buttonHeight > std::min(height, barTop)) return result;
    const float pairWidth = buttonWidth * 2.0F + metrics.buttonSpacing * s;
    if (withEmby && pairWidth <= width) {
        const float x = (width - pairWidth) * 0.5F;
        result.open = {x, y, buttonWidth, buttonHeight};
        result.emby = {x + buttonWidth + metrics.buttonSpacing * s, y, buttonWidth, buttonHeight};
    } else if (buttonWidth <= width) {
        result.open = {(width - buttonWidth) * 0.5F, y, buttonWidth, buttonHeight};
    }
    return result;
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

// A pane's buttons, in the order they are kept in, not the order they stand
// in on its strip. Subtitles is the video's own subtitle switch.
enum class PaneChip { Audio, Pause, Solo, Repeat, Close, Subtitles };
inline constexpr std::size_t kPaneChipCount = 6;
inline constexpr std::array<bool, kPaneChipCount> kEveryPaneChip{true, true, true, true, true, true};

// Each video among several wears a strip along its bottom edge: its seek
// rail, and under it one row -- on the left its "V2" tag, which opens its
// menu, its pause and its time; on the right its volume, sound, subtitles,
// repeat, Solo and close. The tag shows on every pane while the pointer
// moves, the rest of the strip on the pane under the pointer. While the
// videos share one timeline the bar's rail is theirs: the strip is the row
// alone, and the bar marks where each video ends. A row with no rail over
// it -- that one, a pane too short for its rail, the only video's -- has no
// scrim: it is one capsule from the left of the row, the tag, pause and
// time, then sound, subtitles, repeat, Solo and close, the volume standing
// up over the speaker as the bar's does.
struct PaneChromeLayout {
    // The "V2" tag at the left end of the row. The only video has none.
    OverlayRect pill;
    // The buttons. One left out, or dropped because the pane is too narrow
    // for it, takes no room.
    std::array<OverlayRect, kPaneChipCount> chips{};
    // The video's volume rail, unfolded to the left of its speaker while
    // asked for, only for a video being heard. `volumeUpright` on a row
    // with no scrim: a popup standing on the speaker, reaching down to it.
    OverlayRect volume;
    bool volumeUpright{};
    // Where its time reads; `timeShort` when only the current time fits.
    OverlayRect time;
    bool timeShort{};
    // The seek rail's hit band, above the row.
    OverlayRect timeline;
    // The rail and the row: a press there on none of the controls is the
    // strip's, not the picture's. Only with a rail, or, for a tagged video
    // whose rail is the bar's, the row as far as its capsule reaches.
    OverlayRect strip;
    // The scrim the strip is drawn on. Not a control.
    OverlayRect band;
    // A row with no scrim: the capsule it stands on, round all it holds.
    // None round the tag alone, nor under a scrim.
    OverlayRect capsule;
    const OverlayRect& operator[](PaneChip chip) const {
        return chips[static_cast<std::size_t>(chip)];
    }
};

struct PaneChromeMetrics {
    float margin{12.0F};
    float bottomPadding{8.0F};
    float pillHeight{24.0F};
    float pillPadding{10.0F};
    float chip{30.0F};
    float chipGap{6.0F};
    float pillChipGap{8.0F};
    // Between the left of the row and its right.
    float groupGap{12.0F};
    float volumeWidth{96.0F};
    float timeWidth{120.0F};
    float timeShortWidth{60.0F};
    float railBand{22.0F};
    // How far the scrim reaches above the rail.
    float bandRise{28.0F};
    // The room between the caption's strip and the bottom the bar leaves
    // that the row needs, and the row and the rail with some picture.
    float minimumHeightForRow{40.0F};
    float minimumHeightForRail{96.0F};
    // A row with no scrim: how far its capsule reaches beyond what it
    // holds, the room after the time's measured text, and the volume
    // standing over the speaker, as the bar's -- folded where the pane
    // leaves it less than `volumePopupMinimum` above the speaker.
    float capsulePaddingX{5.0F};
    float capsulePaddingY{4.0F};
    float timeTextTrail{2.0F};
    float volumePopupWidth{28.0F};
    float volumePopupHeight{120.0F};
    float volumePopupMinimum{48.0F};
};

struct PaneChromeOptions {
    // The measured width of the tag's text.
    float pillTextWidth{};
    // The measured widths of the time's longest text, with and without its
    // duration. On a row with no scrim the time takes only that, so the
    // buttons after it stand beside it; 0, not yet measured, its full room.
    float timeTextWidth{};
    float timeShortTextWidth{};
    bool withPill{true};
    bool withTime{true};
    bool withTimeline{true};
    // The video is being heard, so its volume may unfold: while `volumeOpen`.
    bool withVolume{};
    bool volumeOpen{};
    // How many pixels of the pane's bottom the transport bar covers now; the
    // strip stands above them.
    float coveredBottom{};
    // How far down from the pane's top the window caption reaches; the
    // strip stays below it.
    float coveredTop{};
    std::array<bool, kPaneChipCount> chipsShown = kEveryPaneChip;
};

// Lay a pane's strip out. Where the row is too narrow for everything,
// things leave it in a fixed order -- Solo, repeat, the duration, the time,
// close, subtitles, sound, pause -- each still in the pane's menu; the tag
// stays. Under a scrim the volume, unfolded, takes the time's room before
// it stays folded; on a row with no scrim it takes none, standing over the
// speaker.
inline PaneChromeLayout paneChromeLayout(const RectF& cell, float scale, const PaneChromeOptions& options = {},
                                         const PaneChromeMetrics& metrics = {}) {
    PaneChromeLayout result;
    const float s = std::max(0.0F, scale);
    if (cell.width <= 0.0F || cell.height <= 0.0F || s <= 0.0F) return result;
    const float top = cell.y + std::clamp(options.coveredTop, 0.0F, cell.height);
    const float bottom = cell.y + cell.height - std::clamp(options.coveredBottom, 0.0F, cell.height);
    const float room = bottom - top;
    if (room < metrics.minimumHeightForRow * s) return result;
    const float rowHeight = metrics.chip * s;
    const float rowTop = bottom - metrics.bottomPadding * s - rowHeight;
    // A rail of its own puts the row under a scrim, with its two ends apart;
    // without one the row is one capsule from the left.
    const bool scrim = options.withTimeline && room >= metrics.minimumHeightForRail * s;
    if (scrim) {
        result.timeline = {cell.x, rowTop - metrics.railBand * s, cell.width, metrics.railBand * s};
        result.strip = {cell.x, result.timeline.y, cell.width, bottom - result.timeline.y};
        const float bandTop = std::max(top, result.timeline.y - metrics.bandRise * s);
        result.band = {cell.x, bandTop, cell.width, bottom - bandTop};
    }

    // What the row would hold, left then right, and what each is worth.
    enum Slot { Pill, Pause, Time, Volume, Audio, Subtitles, Repeat, Solo, Close, SlotCount };
    const auto shown = [&](PaneChip chip) { return options.chipsShown[static_cast<std::size_t>(chip)]; };
    const float margin = std::min(metrics.margin * s, cell.width / 4.0F);
    const float chip = metrics.chip * s;
    const float pillHeight = metrics.pillHeight * s;
    std::array<bool, SlotCount> present{};
    std::array<float, SlotCount> widths{};
    present[Pill] = options.withPill;
    present[Pause] = shown(PaneChip::Pause);
    present[Time] = options.withTime;
    present[Audio] = shown(PaneChip::Audio);
    present[Volume] = present[Audio] && options.withVolume && options.volumeOpen;
    present[Subtitles] = shown(PaneChip::Subtitles);
    present[Repeat] = shown(PaneChip::Repeat);
    present[Solo] = shown(PaneChip::Solo);
    present[Close] = shown(PaneChip::Close);
    widths.fill(chip);
    // The tag is never cut below a circle, unless the pane is narrower.
    widths[Pill] = std::min(std::max(std::max(0.0F, options.pillTextWidth) + metrics.pillPadding * s * 2.0F,
                                     pillHeight),
                            cell.width - margin * 2.0F);
    // Under a scrim the time has its room; on a capsule, once measured, the
    // room of its longest text, so the buttons after it do not move as it
    // counts.
    const auto timeWidth = [&](bool shortened) {
        const float measured = shortened ? options.timeShortTextWidth : options.timeTextWidth;
        if (!scrim && measured > 0.0F) return measured + metrics.timeTextTrail * s;
        return (shortened ? metrics.timeShortWidth : metrics.timeWidth) * s;
    };
    widths[Time] = timeWidth(false);
    widths[Volume] = metrics.volumeWidth * s;
    constexpr std::array<Slot, 3> left{Pill, Pause, Time};
    constexpr std::array<Slot, 6> right{Volume, Audio, Subtitles, Repeat, Solo, Close};
    // The capsule's order: the left end's, then the right end's buttons.
    constexpr std::array<Slot, 8> capsuleOrder{Pill, Pause, Time, Audio, Subtitles, Repeat, Solo, Close};
    // The tag stands a little further from what follows it than buttons
    // from each other.
    const auto gapAfter = [&](int previous) {
        return (previous == Pill ? metrics.pillChipGap : metrics.chipGap) * s;
    };
    const auto required = [&] {
        if (!scrim) {
            float width = 0.0F;
            int previous = -1;
            for (const Slot slot : capsuleOrder) {
                if (!present[slot]) continue;
                if (previous >= 0) width += gapAfter(previous);
                width += widths[slot];
                previous = slot;
            }
            return margin * 2.0F + width;
        }
        float leftWidth = 0.0F;
        int previous = -1;
        for (const Slot slot : left) {
            if (!present[slot]) continue;
            if (previous >= 0) leftWidth += gapAfter(previous);
            leftWidth += widths[slot];
            previous = slot;
        }
        float rightWidth = 0.0F;
        for (const Slot slot : right) {
            if (!present[slot]) continue;
            if (rightWidth > 0.0F) rightWidth += metrics.chipGap * s;
            rightWidth += widths[slot];
        }
        const float between = leftWidth > 0.0F && rightWidth > 0.0F ? metrics.groupGap * s : 0.0F;
        return margin * 2.0F + leftWidth + between + rightWidth;
    };
    // Time is there twice: first the duration goes, then the time. The tag
    // stays, cut to the pane.
    const bool volumeAsked = present[Volume];
    present[Volume] = false;
    const auto shortenTime = [&] {
        result.timeShort = true;
        widths[Time] = timeWidth(true);
    };
    constexpr std::array<Slot, 8> leaveOrder{Solo, Repeat, Time, Time, Close, Subtitles, Audio, Pause};
    for (const Slot slot : leaveOrder) {
        if (required() <= cell.width) break;
        if (slot == Time && present[Time] && !result.timeShort) {
            shortenTime();
            continue;
        }
        present[slot] = false;
    }
    // Under a scrim the volume is fitted to what is left, so that unfolding
    // it moves no button the pointer may be on: the time, at the other end
    // of the row, makes room for it, shortened and then gone; where even
    // that is not enough it stays folded.
    if (scrim && volumeAsked && present[Audio]) {
        const bool timeWas = present[Time];
        const bool shortWas = result.timeShort;
        const float widthWas = widths[Time];
        present[Volume] = true;
        if (required() > cell.width && present[Time] && !result.timeShort) shortenTime();
        if (required() > cell.width) present[Time] = false;
        if (required() > cell.width) {
            present[Volume] = false;
            present[Time] = timeWas;
            result.timeShort = shortWas;
            widths[Time] = widthWas;
        }
    }
    if (!present[Time]) result.timeShort = false;

    const auto chipBox = [&](PaneChip chip) -> OverlayRect& { return result.chips[static_cast<std::size_t>(chip)]; };
    const auto place = [&](Slot slot, const OverlayRect& box) {
        switch (slot) {
        case Pill: result.pill = {box.x, rowTop + (rowHeight - pillHeight) * 0.5F, box.width, pillHeight}; break;
        case Pause: chipBox(PaneChip::Pause) = box; break;
        case Time: result.time = box; break;
        // It reaches the speaker, so the pointer on its way from one to the
        // other never falls through a gap and folds it away.
        case Volume: result.volume = {box.x, box.y, box.width + metrics.chipGap * s, box.height}; break;
        case Audio: chipBox(PaneChip::Audio) = box; break;
        case Subtitles: chipBox(PaneChip::Subtitles) = box; break;
        case Repeat: chipBox(PaneChip::Repeat) = box; break;
        case Solo: chipBox(PaneChip::Solo) = box; break;
        default: chipBox(PaneChip::Close) = box; break;
        }
    };

    const float start = cell.x + margin;
    float x = start;
    int previous = -1;
    if (!scrim) {
        // One capsule from the left: everything in a line, the buttons
        // after the time.
        bool beyondTag = false;
        for (const Slot slot : capsuleOrder) {
            if (!present[slot]) continue;
            if (previous >= 0) x += gapAfter(previous);
            place(slot, {x, rowTop, widths[slot], rowHeight});
            x += widths[slot];
            previous = slot;
            beyondTag = beyondTag || slot != Pill;
        }
        if (beyondTag) {
            const float padX = metrics.capsulePaddingX * s;
            const float padY = metrics.capsulePaddingY * s;
            result.capsule = {start - padX, rowTop - padY, x - start + padX * 2.0F, rowHeight + padY * 2.0F};
        }
        // One of several on the bar's rail: its row is its strip as far as
        // the capsule reaches. Beyond it is the picture.
        if (!options.withTimeline && options.withPill && previous >= 0) {
            const float from = beyondTag ? result.capsule.x : start;
            const float to = beyondTag ? result.capsule.right() : x;
            result.strip = {from, rowTop, to - from, bottom - rowTop};
        }
        // The volume stands on the speaker, as the bar's does, taking no
        // room from the row; inside the pane, or not at all.
        if (volumeAsked && present[Audio]) {
            const OverlayRect& speaker = chipBox(PaneChip::Audio);
            const float width = std::min(metrics.volumePopupWidth * s, speaker.width);
            const float popupTop = std::max(top, speaker.y - metrics.volumePopupHeight * s);
            if (speaker.y - popupTop >= metrics.volumePopupMinimum * s) {
                result.volume = {speaker.x + (speaker.width - width) * 0.5F, popupTop, width, speaker.y - popupTop};
                result.volumeUpright = true;
            }
        }
        return result;
    }
    for (const Slot slot : left) {
        if (!present[slot]) continue;
        if (previous >= 0) x += gapAfter(previous);
        place(slot, {x, rowTop, widths[slot], rowHeight});
        x += widths[slot];
        previous = slot;
    }
    float rightEdge = cell.x + cell.width - margin;
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
        const Slot slot = *it;
        if (!present[slot]) continue;
        rightEdge -= widths[slot];
        place(slot, {rightEdge, rowTop, widths[slot], rowHeight});
        rightEdge -= metrics.chipGap * s;
    }
    return result;
}

// The upright volume as drawn and dragged: its popup down to the capsule's
// top edge. The rest, the capsule's edge over the speaker, is there only so
// the pointer climbing from the speaker crosses no gap.
inline OverlayRect uprightVolumePill(const PaneChromeLayout& chrome) {
    if (!chrome.volumeUpright || !chrome.volume.visible()) return {};
    const float bottom = chrome.capsule.visible() ? std::min(chrome.volume.bottom(), chrome.capsule.y)
                                                  : chrome.volume.bottom();
    return {chrome.volume.x, chrome.volume.y, chrome.volume.width, std::max(0.0F, bottom - chrome.volume.y)};
}

// Position along a rail, 0..1, for a pointer x inside the rail's hit band.
// `inset` is the horizontal padding between the band and the drawn line.
inline float railFraction(float px, const OverlayRect& rail, float inset) {
    const float left = rail.x + std::max(0.0F, inset);
    const float right = rail.right() - std::max(0.0F, inset);
    if (right <= left) return 0.0F;
    return std::clamp((px - left) / (right - left), 0.0F, 1.0F);
}

// How strongly a pane's marks are drawn -- the blue edge of the pane being
// heard and the selected pane's inner outline: only among several panes,
// and they fade with that pane's chrome, so a still pointer leaves the
// picture bare.
inline float paneMarkAlpha(bool marked, int activePaneCount, float chromeAlpha) {
    if (!marked || activePaneCount <= 1) return 0.0F;
    return std::clamp(chromeAlpha, 0.0F, 1.0F);
}

// ---------------------------------------------------------------------------
// Hit testing

enum class OverlayHitKind {
    None, Video, BarItem, PanePill, PaneChip, PaneVolume, PaneTimeline, CaptionItem, EmptyEmby, EmptyOpen
};

struct OverlayHit {
    OverlayHitKind kind{OverlayHitKind::None};
    int pane{-1};
    int item{-1};
};

// The bar wins over the panes beneath it. Within a pane the strip's buttons
// and volume win over its tag, and the tag over the rail; the rest of the
// strip is its own, not the picture's. `barActive` is false while the bar is
// hidden, so its invisible row does not swallow clicks; `chromeActive` is the
// same for each pane's tag and `stripActive` for the rest of its strip.
//
// A pane's strip is up while the pointer is over the pane, moving, in the
// foreground player, and not over the bar, the caption or the sheet -- or
// while it is `held`: a drag or a press on it, or a menu opened from it.
inline bool paneTimelineRevealWanted(bool eligible, bool fresh, bool inside, bool foreground,
                                     bool held, bool overPane, bool occluded) {
    return eligible && (held || (fresh && inside && foreground && overPane && !occluded));
}

inline OverlayHit overlayHitTest(
    float px, float py,
    const TransportBarLayout& bar, bool barActive,
    const PaneArray<PaneChromeLayout>& chrome, const PaneArray<bool>& chromeActive,
    const PaneArray<RectF>& cells, const PaneArray<bool>& active,
    const PaneArray<bool>* stripActive = nullptr) {
    if (barActive) {
        for (std::size_t i = 0; i < kBarItemCount; ++i) {
            if (bar.items[i].contains(px, py)) {
                return {OverlayHitKind::BarItem, -1, static_cast<int>(i)};
            }
        }
        // Empty space in the transport row and its bottom margin belongs to
        // the bar too. Otherwise a drag on a hidden rail swaps the pane below.
        // The capsule has no band: it is its own footprint.
        if (px >= bar.row.x && px < bar.row.right() &&
            py >= bar.row.y && py < std::max(bar.row.bottom(), bar.band.bottom())) return {};
    }
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!active[pane] || !overlayRect(cells[pane]).contains(px, py)) continue;
        const int index = static_cast<int>(pane);
        const bool strip = stripActive ? (*stripActive)[pane] : chromeActive[pane];
        if (strip) {
            for (std::size_t chip = 0; chip < kPaneChipCount; ++chip) {
                if (chrome[pane].chips[chip].contains(px, py)) {
                    return {OverlayHitKind::PaneChip, index, static_cast<int>(chip)};
                }
            }
            if (chrome[pane].volume.contains(px, py)) return {OverlayHitKind::PaneVolume, index, -1};
        }
        if ((chromeActive[pane] || strip) && chrome[pane].pill.contains(px, py)) {
            return {OverlayHitKind::PanePill, index, -1};
        }
        if (strip) {
            if (chrome[pane].timeline.contains(px, py)) return {OverlayHitKind::PaneTimeline, index, -1};
            // Between its controls: neither a click on the picture nor the
            // start of a drag to swap.
            if (chrome[pane].strip.contains(px, py)) return {};
        }
        return {OverlayHitKind::Video, index, -1};
    }
    return {};
}

// ---------------------------------------------------------------------------
// The sync panel

// What the Sync key opens, standing over it: the seek bars' mode as two
// segments, Linked and Independent, with a line saying what that means;
// a row for each video -- its tag, its offset, steps of a second and of a
// tenth either way, and its repeat; and at its foot a button that sets
// every offset back to zero, beside a line of help. A card of its own, not
// a menu: a press on it acts and leaves it open, so a video can be nudged
// a few times running.
inline constexpr std::size_t kSyncSteps = 4;

struct SyncPanelRowLayout {
    // The whole row: the wheel anywhere on it steps the offset.
    OverlayRect row;
    OverlayRect tag;
    OverlayRect value;
    // -1 s, -0.1 s, +0.1 s, +1 s.
    std::array<OverlayRect, kSyncSteps> steps{};
    OverlayRect repeat;
};

struct SyncPanelLayout {
    OverlayRect card;
    OverlayRect title;
    // The segments' track, and in it Linked and Independent, by SeekMode.
    OverlayRect track;
    std::array<OverlayRect, 2> modes{};
    OverlayRect hint;
    OverlayRect rule;
    std::array<SyncPanelRowLayout, kMaxPanes> rows{};
    std::size_t rowCount{};
    OverlayRect footRule;
    OverlayRect reset;
    OverlayRect footHint;
};

// 96-DPI metrics of the panel.
struct SyncPanelMetrics {
    float width{414.0F};
    float padding{12.0F};
    float headerHeight{36.0F};
    float trackWidth{222.0F};
    float trackHeight{32.0F};
    float segmentInset{3.0F};
    float segmentGap{2.0F};
    float hintGap{2.0F};
    float hintHeight{18.0F};
    float ruleGap{12.0F};
    float rowsGap{6.0F};
    float rowHeight{36.0F};
    float tagWidth{40.0F};
    float tagHeight{24.0F};
    float valueWidth{76.0F};
    float stepWidth{52.0F};
    float stepHeight{28.0F};
    float stepGap{4.0F};
    float repeat{30.0F};
    float footHeight{30.0F};
    float resetWidth{168.0F};
    // Between the card and its key, and the window's edges.
    float keyGap{6.0F};
    float edge{8.0F};
};

// The panel for `rowCount` videos over `key`, kept inside `bounds` -- the
// window under the caption's strip. Where the room above the key is too
// short it reaches down over the key rather than leave the top.
inline SyncPanelLayout syncPanelLayout(const OverlayRect& key, const OverlayRect& bounds, std::size_t rowCount,
                                       float scale, const SyncPanelMetrics& metrics = {}) {
    SyncPanelLayout result;
    const float s = std::max(0.0F, scale);
    if (!key.visible() || !bounds.visible() || s <= 0.0F) return result;
    rowCount = std::min(rowCount, kMaxPanes);
    const float padding = metrics.padding * s;
    const float width = metrics.width * s;
    const float headerBottom = padding + metrics.headerHeight * s;
    const float hintTop = headerBottom + metrics.hintGap * s;
    const float ruleTop = hintTop + metrics.hintHeight * s + metrics.ruleGap * s;
    const float hairline = std::max(1.0F, s);
    const float rowsTop = ruleTop + hairline + metrics.rowsGap * s;
    const float rowsBottom = rowsTop + metrics.rowHeight * s * static_cast<float>(rowCount);
    const float footRuleTop = rowsBottom + metrics.rowsGap * s;
    const float footTop = footRuleTop + hairline + metrics.rowsGap * s;
    const float height = footTop + metrics.footHeight * s + padding;

    const float edge = metrics.edge * s;
    const float lowest = bounds.x + edge;
    const float highest = std::max(lowest, bounds.right() - edge - width);
    const float x = std::clamp(key.x + key.width * 0.5F - width * 0.5F, lowest, highest);
    const float y = std::max(bounds.y, key.y - metrics.keyGap * s - height);
    result.card = {x, y, width, height};

    const float trackWidth = metrics.trackWidth * s;
    result.track = {x + width - padding - trackWidth, y + padding + (metrics.headerHeight - metrics.trackHeight) * s * 0.5F,
                    trackWidth, metrics.trackHeight * s};
    const float inset = metrics.segmentInset * s;
    const float segment = (trackWidth - inset * 2.0F - metrics.segmentGap * s) * 0.5F;
    for (std::size_t mode = 0; mode < result.modes.size(); ++mode) {
        result.modes[mode] = {result.track.x + inset + static_cast<float>(mode) * (segment + metrics.segmentGap * s),
                              result.track.y + inset, segment, result.track.height - inset * 2.0F};
    }
    result.title = {x + padding, y + padding, std::max(0.0F, result.track.x - padding - (x + padding)),
                    metrics.headerHeight * s};
    result.hint = {x + padding, y + hintTop, width - padding * 2.0F, metrics.hintHeight * s};
    // The rules stand a little in from the card's edges.
    const float ruleInset = padding + 6.0F * s;
    result.rule = {x + ruleInset, y + ruleTop, width - ruleInset * 2.0F, hairline};

    result.rowCount = rowCount;
    for (std::size_t index = 0; index < rowCount; ++index) {
        auto& row = result.rows[index];
        const float top = y + rowsTop + metrics.rowHeight * s * static_cast<float>(index);
        const float rowHeight = metrics.rowHeight * s;
        row.row = {x, top, width, rowHeight};
        row.tag = {x + padding, top + (rowHeight - metrics.tagHeight * s) * 0.5F, metrics.tagWidth * s,
                   metrics.tagHeight * s};
        row.value = {row.tag.right() + 10.0F * s, top, metrics.valueWidth * s, rowHeight};
        float stepX = row.value.right() + metrics.stepGap * s;
        for (auto& step : row.steps) {
            step = {stepX, top + (rowHeight - metrics.stepHeight * s) * 0.5F, metrics.stepWidth * s,
                    metrics.stepHeight * s};
            stepX = step.right() + metrics.stepGap * s;
        }
        row.repeat = {x + width - padding - metrics.repeat * s, top + (rowHeight - metrics.repeat * s) * 0.5F,
                      metrics.repeat * s, metrics.repeat * s};
    }
    result.footRule = {x + ruleInset, y + footRuleTop, width - ruleInset * 2.0F, hairline};
    result.reset = {x + padding, y + footTop, metrics.resetWidth * s, metrics.footHeight * s};
    const float hintLeft = result.reset.right() + padding;
    result.footHint = {hintLeft, y + footTop, std::max(0.0F, x + width - padding - hintLeft), metrics.footHeight * s};
    return result;
}

enum class SyncPanelHitKind { None, Card, Mode, Step, Repeat, Reset };

// What a point of the panel is: off it None; a segment (`part` its
// SeekMode); a step (`part` 0..3) or the repeat of row `row`; the reset; or
// the card -- `row` the row it is in, if any, for the wheel.
struct SyncPanelHit {
    SyncPanelHitKind kind{SyncPanelHitKind::None};
    int row{-1};
    int part{-1};
    bool operator==(const SyncPanelHit&) const = default;
};

inline SyncPanelHit syncPanelHitTest(const SyncPanelLayout& layout, float px, float py) {
    if (!layout.card.contains(px, py)) return {};
    for (std::size_t mode = 0; mode < layout.modes.size(); ++mode) {
        if (layout.modes[mode].contains(px, py)) return {SyncPanelHitKind::Mode, -1, static_cast<int>(mode)};
    }
    for (std::size_t index = 0; index < layout.rowCount; ++index) {
        const auto& row = layout.rows[index];
        if (!row.row.contains(px, py)) continue;
        for (std::size_t step = 0; step < kSyncSteps; ++step) {
            if (row.steps[step].contains(px, py)) {
                return {SyncPanelHitKind::Step, static_cast<int>(index), static_cast<int>(step)};
            }
        }
        if (row.repeat.contains(px, py)) return {SyncPanelHitKind::Repeat, static_cast<int>(index), -1};
        return {SyncPanelHitKind::Card, static_cast<int>(index), -1};
    }
    if (layout.reset.contains(px, py)) return {SyncPanelHitKind::Reset, -1, -1};
    return {SyncPanelHitKind::Card, -1, -1};
}

}  // namespace quaddeck
