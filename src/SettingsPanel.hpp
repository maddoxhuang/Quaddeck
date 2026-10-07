#pragma once

// The settings sheet: a column of rows that slides in over the right edge
// of the video. This header is the row model, the geometry and the hit test,
// all pure, so the core tests can exhaust the layout and the panel can be
// driven without a window. App fills the rows from its state each frame
// (AppPanel.cpp) and Overlay paints them. The normal settings are split
// over tabs (SettingsTab): a strip under the header that stays put while
// the rows of the chosen tab scroll beneath it. The header carries a
// switch between the settings and the Emby browser, which shares the sheet.

#include "OverlayLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace quaddeck {

// Item is a list entry: the Emby browser's libraries, folders, episodes.
// Tiles is one line of the browser's grid: pictures with titles under them.
enum class PanelRowKind { Header, Note, Toggle, Choice, Slider, Buttons, Item, Tiles, MediaDetail };

// The pages of the normal settings sheet, in the order of its tab strip.
// Emby is not one: the browser is the other face of the sheet, reached by
// the header's switch, and its account lives there.
enum class SettingsTab { Playback, Audio, Subtitles, Picture, General };
inline constexpr int kSettingsTabCount = 5;

inline const wchar_t* settingsTabName(SettingsTab tab) {
    switch (tab) {
    case SettingsTab::Playback: return L"Playback";
    case SettingsTab::Audio: return L"Audio";
    case SettingsTab::Subtitles: return L"Subtitles";
    case SettingsTab::Picture: return L"Picture";
    case SettingsTab::General: return L"General";
    }
    return L"";
}

enum class SettingId {
    None,
    RepeatAll,
    SeekMode,
    Decoder,
    MasterVolume,
    Mute,
    PaneAudio,
    PaneVolume,
    PaneOffset,
    ResetOffsets,
    PaneRepeat,
    Layout,
    AllView,
    ZoomAll,
    Shader,
    ShaderFile,
    Vibrance,
    PinBar,
    NasCache,
    SuperResolution,
    RtxHdr,
    EmbyAccount,
    EmbyNav,
    EmbyItem,
    EmbyMore,
    EmbyView,
    EmbySort,
    EmbyUnplayed,
    EmbyFlat,
    PlayOrder,
    KeyframeSeek,
    LocalNav,
    LocalSort,
    LocalItem,
    LocalReplaceChoice,
    LocalReplaceCancel,
    FileTypes,
    Style,
    Session,
    SubtitleShow,
    SubtitleLanguage,
    SubtitleSize,
    SubtitlePosition,
    SubtitleBackground,
    SubtitlePane,
    EmbyDetailAction,
    EmbyDetailSeason,
    EmbyDetailEpisode,
    EmbyLibraryDetails,
    EmbyReplaceChoice,
    EmbyReplaceCancel,
};

// One movie/series information surface. Text stays separate from cached
// image identities; missing artwork never prevents navigation or playback.
struct PanelMediaDetail {
    std::wstring title, metadata, mediaInfo, overview, progressText;
    std::string posterKey, backdropKey, logoKey;
    bool expanded{};
    float progress{};
    // DirectWrite measurements at the current text-column width/scale.
    // The pure layout also works without a renderer, using conservative
    // text heights. App measures after building rows, before hit geometry.
    std::array<float, 4> measuredTextHeights{}; // title, metadata, mediaInfo, overview
    float measuredTextWidth{};
    float measuredScale{};
};

struct PanelRow {
    PanelRowKind kind{PanelRowKind::Note};
    SettingId id{SettingId::None};
    // Display position for per-pane rows, option index for others.
    int param{-1};
    std::wstring label;
    // Right-aligned text on a slider or buttons row: "65%", "+0.50 s".
    std::wstring value;
    bool on{};
    bool enabled{true};
    // Segments of a Choice row, or the captions of a Buttons row.
    std::vector<std::wstring> options;
    int selected{-1};
    float slider01{};
    // A slider may carry one small toggle at its right end (per-pane mute).
    bool trailingToggle{};
    bool trailingOn{};
    // Tiles: options are the titles; one entry per tile in each of these.
    std::vector<std::string> tileKeys;      // picture cache key, empty for none
    std::vector<std::wstring> tileBadges;   // "✓", "▶ 40%", "23:40"
    std::vector<int> tileParams;            // the item each tile stands for
    float tileWidth{220.0F};                // 96-DPI width of one tile
    float tileAspect{16.0F / 9.0F};         // picture width over height
    // Choice: segments on one line, 0 for the sheet's default of three.
    int segmentsPerLine{};
    // Item: this row is the item being played. Tiles: `selected` is the
    // tile being played (-1 for none); one entry per tile in these too.
    bool current{};
    std::vector<std::wstring> tileMarks;    // top-left: "▶ 40%", "✓"
    std::vector<float> tileProgress;        // 0..1 along the picture's bottom, 0 for none
    // Item: an accent pill before the value ("Sub"). Tiles: one per tile,
    // at the picture's bottom-left; empty for none.
    std::wstring tag;
    std::vector<std::wstring> tileTags;
    // MediaDetail: 0 primary, 1 from beginning, 2 more/less, 3 add,
    // 4 add from beginning. Empty captions hide slots without renumbering.
    PanelMediaDetail mediaDetail;
    // Item/Tiles: an independent add action; the ordinary entry still
    // opens details or plays. Tile availability follows its item index.
    bool addAvailable{};
    std::vector<bool> tileAddAvailable;
    // Buttons only: opt into wrapping at this minimum width in DIPs.
    // Zero preserves existing fixed one-line rows. Empty slots stay hidden.
    float buttonMinWidth{};
    // Note only: reserve its complete wrapped text height before laying out
    // later rows. Ordinary notes retain the fixed height on existing pages.
    bool wrapNote{};
    float measuredNoteHeight{};
    float measuredNoteWidth{};
    float measuredNoteScale{};
};

inline constexpr float kPanelWrappedNoteLineHeight = 20.0F;
inline constexpr float kPanelWrappedNoteTopPad = 2.0F;
inline constexpr float kPanelWrappedNoteBottomPad = 6.0F;

inline OverlayRect panelWrappedNoteTextBox(const OverlayRect& row, float scale) {
    return {row.x, row.y + kPanelWrappedNoteTopPad * scale, row.width,
        std::max(0.0F, row.height - (kPanelWrappedNoteTopPad + kPanelWrappedNoteBottomPad) * scale)};
}

// What a hit on a row means: a Tiles row has one item per tile.
inline int panelRowParam(const PanelRow& row, int part) {
    if (row.kind != PanelRowKind::Tiles) return row.param;
    if (part < 0 || static_cast<std::size_t>(part) >= row.tileParams.size()) return -1;
    return row.tileParams[static_cast<std::size_t>(part)];
}

struct PanelMediaDetailGeometry {
    OverlayRect poster, title, metadata, mediaInfo, progress, progressText, overview;
    std::array<OverlayRect, 5> actions{};
    float height{};
    bool stacked{};
};

// Font-free fallback for geometry tests and the time before DWrite is ready.
// One em per code unit deliberately reserves more than ordinary Latin text.
inline float panelMediaTextHeight(const std::wstring& value, float width, float fontSize, float lineHeight) {
    if (value.empty() || width <= 0.0F) return 0.0F;
    const float capacity = std::max(1.0F, std::floor(width / std::max(1.0F, fontSize)));
    float lines = 1.0F, used = 0.0F;
    for (const wchar_t c : value) {
        if (c == L'\r') continue;
        if (c == L'\n') { lines += 1.0F; used = 0.0F; continue; }
        if (used >= capacity) { lines += 1.0F; used = 0.0F; }
        used += 1.0F;
    }
    return lines * lineHeight;
}

// All rectangles are in pixels and share the same source for paint and hit
// testing. rowBox.height is ignored; the content determines the row's height.
inline PanelMediaDetailGeometry panelMediaDetailLayout(const PanelRow& row, const OverlayRect& rowBox,
                                                       float scale) {
    PanelMediaDetailGeometry result;
    if (!std::isfinite(scale) || !std::isfinite(rowBox.width) || scale <= 0.0F || rowBox.width <= 0.0F) return result;
    const auto& detail = row.mediaDetail;
    const float s = scale, width = rowBox.width;
    result.stacked = width < 680.0F * s;
    const float top = rowBox.y + 20.0F * s;
    const float posterWidth = result.stacked ? std::min(160.0F * s, width * 0.56F)
                                             : std::clamp(width * 0.24F, 160.0F * s, 420.0F * s);
    result.poster = {rowBox.x + (result.stacked ? (width - posterWidth) * 0.5F : 0.0F),
                     top, posterWidth, posterWidth * 1.5F};
    const float textX = result.stacked ? rowBox.x : result.poster.right() + 32.0F * s;
    const float textWidth = std::max(0.0F, rowBox.right() - textX);
    float y = result.stacked ? result.poster.bottom() + 24.0F * s : top;
    const bool measured = std::abs(detail.measuredTextWidth - textWidth) < 0.5F &&
                          std::abs(detail.measuredScale - s) < 0.001F;
    const auto textHeight = [&](std::size_t index, const std::wstring& value, float size, float line) {
        if (value.empty()) return 0.0F;
        const float known = detail.measuredTextHeights[index];
        if (measured && std::isfinite(known) && known > 0.0F) return known;
        return panelMediaTextHeight(value, textWidth, size * s, line * s);
    };
    const float titleHeight = textHeight(0, detail.title, result.stacked ? 26.0F : 32.0F,
                                         result.stacked ? 34.0F : 42.0F);
    result.title = {textX, y, textWidth, detail.logoKey.empty() ? titleHeight : std::max(84.0F * s, titleHeight)};
    if (result.title.visible()) y = result.title.bottom() + 16.0F * s;
    const auto block = [&](OverlayRect& box, float height, float gap) {
        if (height <= 0.0F) return;
        box = {textX, y, textWidth, height};
        y += height + gap * s;
    };
    block(result.metadata, textHeight(1, detail.metadata, 14.0F, 22.0F), 10.0F);
    block(result.mediaInfo, textHeight(2, detail.mediaInfo, 12.0F, 20.0F), 18.0F);
    const auto hasAction = [&](std::size_t index) {
        return index < row.options.size() && !row.options[index].empty();
    };
    constexpr std::array<std::size_t, 4> playbackActions{0, 1, 3, 4};
    int buttons = 0;
    for (const auto index : playbackActions) if (hasAction(index)) ++buttons;
    if (buttons > 0) {
        const float gap = 12.0F * s;
        const int perLine = std::clamp(static_cast<int>((textWidth + gap) / (184.0F * s + gap)), 1, buttons);
        const float buttonWidth = perLine == 1 ? std::min(textWidth, 280.0F * s)
            : std::min(232.0F * s, (textWidth - (perLine - 1) * gap) / perLine);
        int placed = 0;
        for (const auto index : playbackActions) {
            if (!hasAction(index)) continue;
            result.actions[index] = {textX + (placed % perLine) * (buttonWidth + gap),
                y + (placed / perLine) * 56.0F * s, std::max(0.0F, buttonWidth), 44.0F * s};
            ++placed;
        }
        y += ((buttons + perLine - 1) / perLine) * 56.0F * s + 6.0F * s;
    }
    if (!detail.progressText.empty() || (std::isfinite(detail.progress) && detail.progress > 0.0F)) {
        block(result.progress, 10.0F * s, 8.0F);
        block(result.progressText, panelMediaTextHeight(detail.progressText, textWidth, 12.0F * s, 20.0F * s), 18.0F);
    }
    const float overviewHeight = textHeight(3, detail.overview, 14.0F, 22.0F);
    block(result.overview, detail.expanded ? overviewHeight : std::min(66.0F * s, overviewHeight), 8.0F);
    if (!detail.overview.empty() && hasAction(2)) {
        result.actions[2] = {textX, y, std::min(112.0F * s, textWidth), 34.0F * s};
        y += 42.0F * s;
    }
    result.height = std::max(y, result.poster.bottom()) - rowBox.y + 24.0F * s;
    return result;
}

struct PanelRowGeometry {
    OverlayRect row;
    // The switch of a Toggle row, the rail band of a Slider row.
    OverlayRect control;
    // Segments or buttons.
    std::vector<OverlayRect> parts;
    // Item: slot zero. Tiles: slots match parts/tileParams, including
    // empty rectangles for entries without an Add action.
    std::vector<OverlayRect> addParts;
    OverlayRect trailing;
    PanelMediaDetailGeometry mediaDetail;
};

struct PanelLayout {
    OverlayRect sheet;
    OverlayRect header;
    OverlayRect close;
    // The header's switch between the settings and the browser, left of
    // the close button; empty when the sheet was laid out without one.
    OverlayRect sheetSwitch;
    // The tab strip under the header, when the sheet has tabs: the strip
    // and one rectangle per tab. The content starts below it and scrolls
    // beneath it; both are empty for a sheet without tabs.
    OverlayRect tabStrip;
    std::vector<OverlayRect> tabs;
    OverlayRect content;
    // The strip at the content's right edge that takes the scrollbar; empty
    // when everything fits.
    OverlayRect scrollbar;
    std::vector<PanelRowGeometry> rows;
    float contentHeight{};
    float maxScroll{};
    // The scroll the rows were laid out with, clamped.
    float scroll{};
};

// The scrollbar's thumb: as tall as the visible share of the content, at
// least 24 px, placed by the scroll. The whole strip's width, for hitting.
inline OverlayRect panelScrollThumb(const PanelLayout& layout, float scale) {
    if (!layout.scrollbar.visible() || layout.maxScroll <= 0.0F || layout.contentHeight <= 0.0F) return {};
    const float track = layout.scrollbar.height;
    const float height = std::min(track, std::max(24.0F * std::max(0.0F, scale),
                                                  track * layout.content.height / layout.contentHeight));
    const float y = layout.scrollbar.y + (track - height) * std::clamp(layout.scroll / layout.maxScroll, 0.0F, 1.0F);
    return {layout.scrollbar.x, y, layout.scrollbar.width, height};
}

// The scroll that puts the thumb's top at `thumbTop`: dragging the thumb.
inline float panelScrollForThumbTop(const PanelLayout& layout, float scale, float thumbTop) {
    const OverlayRect thumb = panelScrollThumb(layout, scale);
    if (!thumb.visible()) return 0.0F;
    const float travel = layout.scrollbar.height - thumb.height;
    if (travel <= 0.0F) return 0.0F;
    return std::clamp((thumbTop - layout.scrollbar.y) / travel, 0.0F, 1.0F) * layout.maxScroll;
}

// 96-DPI metrics.
struct PanelMetrics {
    float width{420.0F};
    float sideGutter{16.0F};
    float pad{20.0F};
    float headerHeight{56.0F};
    float closeSize{32.0F};
    float headerRow{44.0F};
    float noteRow{30.0F};
    float toggleRow{44.0F};
    float itemRow{40.0F};
    float sliderRow{62.0F};
    float sliderBand{24.0F};
    float buttonsRow{46.0F};
    float labelLine{24.0F};
    float segment{34.0F};
    float segmentGap{6.0F};
    float switchWidth{44.0F};
    float switchHeight{24.0F};
    float trailingSize{32.0F};
    float bottomPad{24.0F};
    int segmentsPerLine{3};
    float tileGap{12.0F};
    float tileLabel{40.0F};   // two lines of small text under a picture
    float tabHeight{30.0F};
    float tabGap{4.0F};
    float tabMinWidth{56.0F}; // narrower than this and the tabs take another line
    float tabStripPad{8.0F};
    float sheetSwitchWidth{88.0F};
};

// How many tiles of about `tileWidth` go on one line of a sheet
// `innerWidth` wide: the count that fits, one more when more than half of
// another would, so the line is then filled by stretching or shrinking
// the tiles (panelTileWidthFor) instead of leaving a gap at the right.
inline int panelTilesPerLine(float innerWidth, float tileWidth, float scale, const PanelMetrics& m = {}) {
    const float tile = (tileWidth + m.tileGap) * scale;
    if (tile <= 0.0F) return 1;
    const float ratio = (innerWidth + m.tileGap * scale) / tile;
    const int whole = static_cast<int>(ratio);
    return std::max(1, ratio - static_cast<float>(whole) >= 0.5F ? whole + 1 : whole);
}

// The 96-DPI width that makes `perLine` tiles fill `innerWidth` exactly.
inline float panelTileWidthFor(float innerWidth, int perLine, float scale, const PanelMetrics& m = {}) {
    if (perLine <= 0 || scale <= 0.0F) return 0.0F;
    const float gaps = m.tileGap * scale * static_cast<float>(perLine - 1);
    return std::max(1.0F, (innerWidth - gaps) / static_cast<float>(perLine) / scale);
}

// Segments of a Choice row on one line for a sheet `innerWidth` wide: as
// many as keep each at least `segmentMinimum` wide, three to nine -- a
// playlist's page has its own order beside the browser's eight.
inline int panelSegmentsPerLine(float innerWidth, float scale, int wanted, float segmentMinimum = 64.0F,
                                const PanelMetrics& m = {}) {
    const float segment = (segmentMinimum + m.segmentGap) * scale;
    if (segment <= 0.0F) return std::clamp(wanted, 1, 9);
    const int fit = static_cast<int>((innerWidth + m.segmentGap * scale) / segment);
    return std::clamp(std::min(wanted, fit), std::min(3, std::max(1, wanted)), 9);
}

inline int panelChoiceLines(int options, int perLine) {
    if (options <= 0) return 0;
    return (options + std::max(1, perLine) - 1) / std::max(1, perLine);
}

inline int panelButtonCount(const PanelRow& row) {
    if (row.buttonMinWidth <= 0.0F) return static_cast<int>(row.options.size());
    return static_cast<int>(std::count_if(row.options.begin(), row.options.end(),
        [](const std::wstring& caption) { return !caption.empty(); }));
}

inline int panelButtonsPerLine(const PanelRow& row, float rowWidth, float scale, const PanelMetrics& m = {}) {
    const int count = panelButtonCount(row);
    if (!std::isfinite(row.buttonMinWidth) || row.buttonMinWidth <= 0.0F || scale <= 0.0F) return std::max(1, count);
    const float width = rowWidth >= 0.0F ? rowWidth : (m.width - 2.0F * m.pad) * scale;
    const float unit = (row.buttonMinWidth + m.segmentGap) * scale;
    return std::clamp(static_cast<int>((std::max(0.0F, width) + m.segmentGap * scale) / unit), 1, std::max(1, count));
}

inline float panelRowHeight(const PanelRow& row, float scale, const PanelMetrics& m = {}, float rowWidth = -1.0F) {
    switch (row.kind) {
    case PanelRowKind::Header: return m.headerRow * scale;
    case PanelRowKind::Note: {
        if (!row.wrapNote) return m.noteRow * scale;
        const float width = rowWidth >= 0.0F ? rowWidth : std::max(0.0F, m.width - 2.0F * m.pad) * scale;
        const bool measured = row.measuredNoteWidth == width && row.measuredNoteScale == scale &&
            std::isfinite(row.measuredNoteHeight) && row.measuredNoteHeight > 0.0F;
        // Until DirectWrite is ready, one em per code unit plus another
        // line's share covers the space lost at word-wrap boundaries.
        const float textHeight = measured ? row.measuredNoteHeight :
            2.0F * panelMediaTextHeight(row.label, width, 12.0F * scale, kPanelWrappedNoteLineHeight * scale);
        return std::max(m.noteRow * scale,
            textHeight + (kPanelWrappedNoteTopPad + kPanelWrappedNoteBottomPad) * scale);
    }
    case PanelRowKind::Toggle: return m.toggleRow * scale;
    case PanelRowKind::Item: return m.itemRow * scale;
    case PanelRowKind::Tiles:
        return (row.tileWidth / std::max(0.1F, row.tileAspect) + m.tileLabel + m.tileGap) * scale;
    case PanelRowKind::Slider: return m.sliderRow * scale;
    case PanelRowKind::Choice: {
        const int lines = panelChoiceLines(static_cast<int>(row.options.size()),
                                           row.segmentsPerLine > 0 ? row.segmentsPerLine : m.segmentsPerLine);
        return (m.labelLine + lines * m.segment + std::max(0, lines - 1) * m.segmentGap + 8.0F) * scale;
    }
    case PanelRowKind::Buttons: {
        const int lines = panelChoiceLines(panelButtonCount(row), panelButtonsPerLine(row, rowWidth, scale, m));
        return ((row.label.empty() ? m.buttonsRow : m.buttonsRow + m.labelLine) +
            std::max(0, lines - 1) * (m.segment + m.segmentGap)) * scale;
    }
    case PanelRowKind::MediaDetail:
        return panelMediaDetailLayout(row, {0.0F, 0.0F, rowWidth >= 0.0F ? rowWidth :
            std::max(0.0F, m.width - 2.0F * m.pad) * scale, 0.0F}, scale).height;
    }
    return 0.0F;
}

// `slide` is 0 while the sheet is off screen and 1 when fully in; the sheet
// travels a short distance rather than its whole width so the fade carries
// the motion. `scroll` is clamped to what the content allows.
// `sheetWidth` overrides the sheet's width when positive: the Emby browser
// takes the whole client with nothing playing and the right part of it --
// docked, as PotPlayer's playlist -- beside a video.
// `captionTop` is the window caption's strip: the sheet's column still reaches the
// top edge, under the window buttons, but its header and rows start below.
// `tabCount` puts that many tabs in a strip under the header; their
// captions are App's (settingsTabName) and the rows are the chosen tab's.
// `sheetSwitch` puts the settings/browser switch in the header.
inline PanelLayout settingsPanelLayout(
    float width, float height, float scale, const std::vector<PanelRow>& rows,
    float scroll, float slide = 1.0F, const PanelMetrics& m = {}, float sheetWidthOverride = 0.0F,
    float captionTop = 0.0F, int tabCount = 0, bool sheetSwitch = false) {
    PanelLayout layout;
    const float s = std::max(0.0F, scale);
    if (width <= 0.0F || height <= 0.0F || s <= 0.0F) return layout;
    const float headerTop = std::clamp(captionTop, 0.0F, height);
    const float sheetWidth = sheetWidthOverride > 0.0F
        ? std::min(width, sheetWidthOverride)
        : std::min(m.width * s, std::max(0.0F, width - m.sideGutter * s));
    const float travel = 48.0F * s * (1.0F - std::clamp(slide, 0.0F, 1.0F));
    layout.sheet = {width - sheetWidth + travel, 0.0F, sheetWidth, height};
    const float headerHeight = std::min(m.headerHeight * s, height - headerTop);
    layout.header = {layout.sheet.x, headerTop, sheetWidth, headerHeight};
    const float closeSize = std::min(m.closeSize * s, headerHeight);
    layout.close = {layout.sheet.right() - m.pad * s - closeSize,
                    headerTop + (headerHeight - closeSize) * 0.5F, closeSize, closeSize};
    if (sheetSwitch) {
        const float switchWidth = std::min(m.sheetSwitchWidth * s,
            std::max(0.0F, layout.close.x - 8.0F * s - layout.sheet.x - m.pad * s));
        layout.sheetSwitch = {layout.close.x - 8.0F * s - switchWidth, layout.close.y, switchWidth, closeSize};
    }
    const float pad = m.pad * s;
    const float innerX = layout.sheet.x + pad;
    const float innerWidth = std::max(0.0F, sheetWidth - pad * 2.0F);
    float contentTop = headerTop + headerHeight;
    if (tabCount > 0) {
        // As many tabs on one line as keep each at least tabMinWidth wide,
        // sharing the inner width equally; the rest wrap onto further lines.
        const float gap = m.tabGap * s;
        const int perLine = std::clamp(
            static_cast<int>((innerWidth + gap) / std::max(1.0F, m.tabMinWidth * s + gap)), 1, tabCount);
        const int lines = (tabCount + perLine - 1) / perLine;
        const float tabHeight = m.tabHeight * s;
        const float stripHeight = 2.0F * m.tabStripPad * s + static_cast<float>(lines) * tabHeight +
                                  static_cast<float>(lines - 1) * gap;
        layout.tabStrip = {layout.sheet.x, contentTop, sheetWidth,
                           std::min(stripHeight, std::max(0.0F, height - contentTop))};
        const float tabWidth = std::max(
            0.0F, (innerWidth - gap * static_cast<float>(perLine - 1)) / static_cast<float>(perLine));
        layout.tabs.reserve(static_cast<std::size_t>(tabCount));
        for (int i = 0; i < tabCount; ++i) {
            layout.tabs.push_back({innerX + static_cast<float>(i % perLine) * (tabWidth + gap),
                                   contentTop + m.tabStripPad * s +
                                       static_cast<float>(i / perLine) * (tabHeight + gap),
                                   tabWidth, tabHeight});
        }
        contentTop += layout.tabStrip.height;
    }
    layout.content = {layout.sheet.x, contentTop, sheetWidth, std::max(0.0F, height - contentTop)};

    float total = 0.0F;
    for (const auto& row : rows) total += panelRowHeight(row, s, m, innerWidth);
    layout.contentHeight = total + m.bottomPad * s;
    layout.maxScroll = std::max(0.0F, layout.contentHeight - layout.content.height);
    const float clampedScroll = std::clamp(scroll, 0.0F, layout.maxScroll);
    layout.scroll = clampedScroll;
    if (layout.maxScroll > 0.0F) {
        const float strip = std::min(m.pad * s, 14.0F * s);
        layout.scrollbar = {layout.sheet.right() - strip, layout.content.y, strip, layout.content.height};
    }

    float y = layout.content.y - clampedScroll;
    layout.rows.reserve(rows.size());
    for (const auto& row : rows) {
        PanelRowGeometry geometry;
        const float rowHeight = panelRowHeight(row, s, m, innerWidth);
        geometry.row = {innerX, y, innerWidth, rowHeight};
        switch (row.kind) {
        case PanelRowKind::Item:
            if (row.addAvailable && innerWidth > 16.0F * s) {
                const float buttonWidth = std::min(64.0F * s, innerWidth - 16.0F * s);
                const float buttonHeight = std::min(28.0F * s, rowHeight);
                geometry.addParts.push_back({innerX + innerWidth - buttonWidth - 6.0F * s,
                    y + (rowHeight - buttonHeight) * 0.5F, buttonWidth, buttonHeight});
            }
            break;
        case PanelRowKind::MediaDetail:
            geometry.mediaDetail = panelMediaDetailLayout(row, geometry.row, s);
            geometry.parts.assign(geometry.mediaDetail.actions.begin(), geometry.mediaDetail.actions.end());
            break;
        case PanelRowKind::Toggle: {
            const float w = m.switchWidth * s;
            const float h = m.switchHeight * s;
            geometry.control = {innerX + innerWidth - w, y + (rowHeight - h) * 0.5F, w, h};
            break;
        }
        case PanelRowKind::Slider: {
            const float trailing = row.trailingToggle ? m.trailingSize * s + 8.0F * s : 0.0F;
            const float bandY = y + m.labelLine * s + 6.0F * s;
            geometry.control = {innerX, bandY, std::max(0.0F, innerWidth - trailing), m.sliderBand * s};
            if (row.trailingToggle) {
                const float size = m.trailingSize * s;
                geometry.trailing = {innerX + innerWidth - size,
                                     bandY + (m.sliderBand * s - size) * 0.5F, size, size};
            }
            break;
        }
        case PanelRowKind::Choice: {
            const int count = static_cast<int>(row.options.size());
            const int perLine = std::max(1, row.segmentsPerLine > 0 ? row.segmentsPerLine : m.segmentsPerLine);
            const float gap = m.segmentGap * s;
            const float columnWidth = std::max(
                0.0F, (innerWidth - gap * static_cast<float>(perLine - 1)) / static_cast<float>(perLine));
            for (int i = 0; i < count; ++i) {
                const int line = i / perLine;
                const int column = i % perLine;
                geometry.parts.push_back({
                    innerX + column * (columnWidth + gap),
                    y + m.labelLine * s + line * (m.segment + m.segmentGap) * s,
                    columnWidth, m.segment * s});
            }
            break;
        }
        case PanelRowKind::Tiles: {
            const float tileWidth = row.tileWidth * s;
            const float tileHeight = tileWidth / std::max(0.1F, row.tileAspect) + m.tileLabel * s;
            const float gap = m.tileGap * s;
            for (std::size_t i = 0; i < row.options.size(); ++i) {
                geometry.parts.push_back({innerX + static_cast<float>(i) * (tileWidth + gap), y, tileWidth, tileHeight});
                OverlayRect add;
                if (i < row.tileAddAvailable.size() && row.tileAddAvailable[i] && tileWidth > 12.0F * s) {
                    const auto& tile = geometry.parts.back();
                    const float pictureHeight = tileWidth / std::max(0.1F, row.tileAspect);
                    const float buttonHeight = std::min(28.0F * s, std::max(0.0F, pictureHeight - 12.0F * s));
                    const float buttonWidth = std::min(64.0F * s, tileWidth - 12.0F * s);
                    add = {tile.right() - buttonWidth - 6.0F * s,
                           y + pictureHeight - buttonHeight - 12.0F * s, buttonWidth, buttonHeight};
                }
                geometry.addParts.push_back(add);
            }
            break;
        }
        case PanelRowKind::Buttons: {
            const int count = panelButtonCount(row);
            if (count > 0) {
                const float gap = m.segmentGap * s;
                const int perLine = panelButtonsPerLine(row, innerWidth, s, m);
                const float buttonWidth = std::max(
                    0.0F, (innerWidth - gap * static_cast<float>(perLine - 1)) / static_cast<float>(perLine));
                const float top = y + (row.label.empty() ? 6.0F * s : m.labelLine * s + 4.0F * s);
                geometry.parts.resize(row.options.size());
                int placed = 0;
                for (std::size_t i = 0; i < row.options.size(); ++i) {
                    if (row.buttonMinWidth > 0.0F && row.options[i].empty()) continue;
                    geometry.parts[i] = {innerX + (placed % perLine) * (buttonWidth + gap),
                        top + (placed / perLine) * (m.segment + m.segmentGap) * s, buttonWidth, m.segment * s};
                    ++placed;
                }
            }
            break;
        }
        default:
            break;
        }
        layout.rows.push_back(std::move(geometry));
        y += rowHeight;
    }
    return layout;
}

enum class PanelHitKind { None, Outside, Sheet, Close, Toggle, Segment, Button, Slider, Trailing, Item, Tile, Scrollbar, Add, Tab, Switch };

struct PanelHit {
    PanelHitKind kind{PanelHitKind::None};
    int row{-1};
    int part{-1};
    float fraction{};
};

// Rows are only live inside the content clip; a disabled row swallows the
// click as "Sheet" so nothing beneath it reacts.
inline PanelHit settingsPanelHitTest(
    float px, float py, const PanelLayout& layout, const std::vector<PanelRow>& rows,
    float railInset = 0.0F) {
    if (!layout.sheet.contains(px, py)) return {PanelHitKind::Outside};
    if (layout.close.contains(px, py)) return {PanelHitKind::Close};
    if (layout.sheetSwitch.contains(px, py)) return {PanelHitKind::Switch};
    // A tab is hit by its index in `part`; the strip around the tabs is
    // the sheet, so a press there can still drag the content.
    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        if (layout.tabs[i].contains(px, py)) return {PanelHitKind::Tab, -1, static_cast<int>(i)};
    }
    if (layout.tabStrip.contains(px, py)) return {PanelHitKind::Sheet};
    if (layout.scrollbar.visible() && layout.scrollbar.contains(px, py)) return {PanelHitKind::Scrollbar};
    if (!layout.content.contains(px, py)) return {PanelHitKind::Sheet};
    for (std::size_t i = 0; i < layout.rows.size() && i < rows.size(); ++i) {
        const auto& geometry = layout.rows[i];
        if (!geometry.row.contains(px, py)) continue;
        const int index = static_cast<int>(i);
        if (!rows[i].enabled) return {PanelHitKind::Sheet};
        switch (rows[i].kind) {
        case PanelRowKind::Toggle:
            // The whole row flips a toggle, not just the switch.
            return {PanelHitKind::Toggle, index};
        case PanelRowKind::Item:
            if (!geometry.addParts.empty() && geometry.addParts[0].contains(px, py)) {
                return {PanelHitKind::Add, index, 0};
            }
            return {PanelHitKind::Item, index};
        case PanelRowKind::Tiles:
            for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
                if (part < geometry.addParts.size() && geometry.addParts[part].contains(px, py)) {
                    return {PanelHitKind::Add, index, static_cast<int>(part)};
                }
                if (geometry.parts[part].contains(px, py)) {
                    return {PanelHitKind::Tile, index, static_cast<int>(part)};
                }
            }
            return {PanelHitKind::Sheet};
        case PanelRowKind::Slider:
            if (geometry.trailing.contains(px, py)) return {PanelHitKind::Trailing, index};
            if (geometry.control.contains(px, py)) {
                return {PanelHitKind::Slider, index, -1, railFraction(px, geometry.control, railInset)};
            }
            return {PanelHitKind::Sheet};
        case PanelRowKind::Choice:
            for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
                if (geometry.parts[part].contains(px, py)) {
                    return {PanelHitKind::Segment, index, static_cast<int>(part)};
                }
            }
            return {PanelHitKind::Sheet};
        case PanelRowKind::MediaDetail:
        case PanelRowKind::Buttons:
            for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
                if (geometry.parts[part].contains(px, py)) {
                    return {PanelHitKind::Button, index, static_cast<int>(part)};
                }
            }
            return {PanelHitKind::Sheet};
        default:
            return {PanelHitKind::Sheet};
        }
    }
    return {PanelHitKind::Sheet};
}

}  // namespace quaddeck
