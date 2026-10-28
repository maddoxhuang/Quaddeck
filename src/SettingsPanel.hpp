#pragma once

// The sheet: a column of rows that slides in over the right edge of the
// video, and the settings window, which is the same column in a window of
// its own. This header is the row model, the geometry and the hit test,
// all independent of the native window. App fills the rows from its state each frame
// (AppPanel.cpp, AppSettingsWindow.cpp) and Overlay paints them. The
// settings are split over tabs (SettingsTab): a column down the window's
// left, one tab a line under its title, beside the chosen tab's rows. A
// page's leading rows can stay put while the rest scroll (PanelRow::pinned):
// the browsers' title, way around and view. The sheet itself has one face
// (SheetFace), the Emby browser.

#include "OverlayLayout.hpp"
#include "TextField.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace quaddeck {

// Item is a list entry: the Emby browser's libraries, folders, episodes.
// Tiles is one line of the browser's grid: pictures with titles under them.
// TextField is a one-line box typed into where it is drawn: the browser's
// search, the Emby sign-in's server, user and password.
// Shortcut is one action of the Keys tab: its name, and at the right a box
// for each of its keys (`options`, empty for a free one); `selected` is the
// box waiting for a key press, and `on` says its keys are not the defaults.
// Equalizer is a graphic equalizer: an upright slider per band side by
// side, each with its value over it and its caption under it.
enum class PanelRowKind { Header, Note, Toggle, Choice, Slider, Buttons, Item, Tiles, MediaDetail, TextField, CompactItem,
                          Shortcut, Equalizer };

// The pages of the settings window, in the order of its tabs. Emby
// is not one: the browser is a face of the sheet, reached by the header's
// switch, and its account lives there.
enum class SettingsTab { Playback, Audio, Subtitles, Picture, Video, General, Keys };
inline constexpr int kSettingsTabCount = 7;

inline const wchar_t* settingsTabName(SettingsTab tab) {
    switch (tab) {
    case SettingsTab::Playback: return L"Playback";
    case SettingsTab::Audio: return L"Audio";
    case SettingsTab::Subtitles: return L"Subtitles";
    case SettingsTab::Picture: return L"Picture";
    case SettingsTab::Video: return L"Video";
    case SettingsTab::General: return L"General";
    case SettingsTab::Keys: return L"Keys";
    }
    return L"";
}

// The icon beside a tab's name: a Segoe Fluent Icons / MDL2 Assets code
// point, the same in both faces.
inline wchar_t settingsTabGlyph(SettingsTab tab) {
    switch (tab) {
    case SettingsTab::Playback: return L'\xE768';  // Play
    case SettingsTab::Audio: return L'\xE767';     // Volume
    case SettingsTab::Subtitles: return L'\xED1E'; // Subtitles
    case SettingsTab::Picture: return L'\xE8B9';   // Picture
    case SettingsTab::Video: return L'\xE7F4';     // TVMonitor
    case SettingsTab::General: return L'\xE713';   // Setting
    case SettingsTab::Keys: return L'\xE765';      // KeyboardClassic
    }
    return L'\0';
}

// The sheet's face: the Emby browser, or the F6 folder list in its place.
// The settings are not one: they were the sheet's other face before 1.4.0,
// and now they are a window of their own, which F5 and the menu bring up.
// A header switch would be captioned with the face it leads to and stand
// beside the close glyph.
enum class SheetFace { Emby };

inline const wchar_t* sheetFaceCaption(SheetFace face) {
    switch (face) {
    case SheetFace::Emby: return L"Emby \x203A";
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
    LocalView,
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
    // The browser's search box; its clear button is the row's Trailing hit.
    EmbySearch,
    // Under it, on a search opened in a folder: part 0 searches that
    // folder, part 1 every library.
    EmbySearchScope,
    // The sign-in page's boxes: `param` 0 the server, 1 the user, 2 the
    // password. Its Sign in button is an EmbyAccount row.
    EmbySignInField,
    // An information page's cast and crew: a line of pictures (a tile's
    // param is the person's index), and the button that shows all of them.
    EmbyDetailPerson,
    EmbyDetailCast,
    // A person's page: the information row at its top (More / Less).
    EmbyPersonAction,
    EmbyToolbar, EmbyRetry, LocalToolbar, LocalRetry,
    // The Keys tab: an action's key boxes (`param` is the KeyAction, a hit's
    // part the box), and the buttons over the list -- Reset all keys, or,
    // while a box waits for a key, Clear this key, Default keys and Cancel.
    ShortcutKey,
    ShortcutCommand,
    // The Audio tab's own rows (AppAudio.cpp): the output and its speakers,
    // how loud it may go, every video's delay; the effects; the equalizer,
    // whose bands are EqualizerBand rows (`param` the band, -1 the preamp);
    // and a video's track and timing (`param` its place on the screen).
    AudioDevice,
    AudioSpeakers,
    AudioMaxVolume,
    AudioDelayAll,
    AudioNormalize,
    AudioNightMode,
    AudioChannels,
    AudioVoice,
    AudioCrossfeed,
    AudioWidening,
    AudioPitch,
    AudioLanguage,
    Equalizer,
    EqualizerPreset,
    EqualizerBand,
    PaneAudioTrack,
    PaneAudioDelay,
    // The Video tab's rows (AppVideo.cpp): the renderer, the graphics card,
    // the display fullscreen goes to, the output's depth, deinterlacing, the
    // scaler, the levels, and the colour controls (`param` the control) with
    // the button that sets them back.
    VideoRenderer,
    VideoAdapter,
    VideoDisplay,
    VideoDepth,
    VideoDeinterlace,
    VideoScaling,
    VideoLevels,
    VideoColor,
    VideoColorReset,
};

// One movie/series information surface. Text stays separate from cached
// image identities; missing artwork never prevents navigation or playback.
struct PanelMediaDetail {
    std::wstring title, metadata, mediaInfo, overview, progressText;
    std::string posterKey, backdropKey, logoKey;
    bool expanded{};
    bool compactActions{};
    float progress{};
    // Places on the screen (0 for V1), -1 for none: the video playing this
    // item, whose colour its progress is in, and the one its Play button
    // names, whose colour the button is.
    int playingPosition{-1};
    int targetPosition{-1};
    // DirectWrite measurements at the current text-column width/scale.
    // The pure layout also works without a renderer, using conservative
    // text heights. App measures after building rows, before hit geometry.
    std::array<float, 4> measuredTextHeights{}; // title, metadata, mediaInfo, overview
    float measuredTextWidth{};
    float measuredScale{};
};

// A TextField row: what has been typed with its caret and selection, the
// grey words shown while it is empty, and what an input method is still
// composing at the caret -- drawn there, underlined, with its own caret.
// The row's label, when it has one, stands above the box.
struct PanelTextField {
    TextFieldState edit;
    std::wstring placeholder;
    std::wstring composition;
    std::size_t compositionCaret{};
    // Typed into now; the caret shows only then, and only in the blink's
    // visible half (App keeps the time).
    bool focused{};
    bool caretShown{};
    // A magnifier at the box's left (a search); a clear button at its right
    // while it holds anything; every character shown as a dot (a password).
    bool magnifier{};
    bool clearable{};
    bool masked{};
};

struct PanelRow {
    PanelRowKind kind{PanelRowKind::Note};
    SettingId id{SettingId::None};
    // Display position for per-pane rows, option index for others.
    int param{-1};
    // Item and Tiles of a browser list: built bare -- what lays the row out
    // and names its items, none of its text, marks or pictures -- for App
    // to fill only while it is on screen or near it, so a long list costs a
    // frame what the screen shows rather than its length. Beside `kind`, as
    // the layout reads both for every row.
    bool contentPending{};
    std::wstring label;
    std::wstring hint;
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
    std::vector<int> tileParams;            // the item each tile (or band) stands for
    float tileWidth{220.0F};                // 96-DPI width of one tile
    float tileAspect{16.0F / 9.0F};         // picture width over height
    // Choice: segments on one line, 0 for the sheet's default of three.
    int segmentsPerLine{};
    // Choice: a drop-down rather than segments, for a long list or long
    // captions -- the chosen option in a box at the row's right, the list
    // in a card that opens under it. `value` is what the box says while no
    // option is chosen.
    bool dropDown{};
    // Buttons: a spin box rather than buttons -- `value` in a box at the
    // row's right, the arrow up over the arrow down at its right end (parts
    // 0 and 1, named by `options`). `slider01` is where the value stands
    // between its least and its most: an arrow is dimmed at its end. Its
    // number is a `Field` hit; while it is typed, `field` holds the digits
    // and is `focused`, and the box shows them in place of `value`.
    bool spinBox{};
    // Equalizer: one band per caption in `options`, `levels` where each
    // stands from 0 at the bottom to 1 at the top, `levelTexts` its value
    // over it and `tileParams` what each stands for; the first `apartBands`
    // stand apart from the rest (a preamp). `on` lights the levels in the
    // accent, as an equalizer switched on.
    std::vector<float> levels;
    std::vector<std::wstring> levelTexts;
    int apartBands{};
    // A Choice or Buttons row laid out compact (PanelMetrics::compactControls):
    // the width of each caption in `options`, of the label and of the value,
    // in 96-DPI units, as the painter measures them (Overlay::measureCaptions).
    // A width missing or negative is estimated from its text.
    std::vector<float> captionWidths;
    float labelWidth{-1.0F};
    float valueWidth{-1.0F};
    // Item: this row is the item being played. Tiles: `selected` is the
    // first tile being played (-1 for none); one entry per tile in these too.
    bool current{};
    // The video playing it, by its place on the screen (0 for V1), whose
    // colour the mark is drawn in: -1 for none. Tiles: one per tile.
    int playingPosition{-1};
    std::vector<int> tilePlaying;
    // Toggle, Slider, Buttons and CompactItem rows about one video: its
    // place on the screen (0 for V1), -1 for none. Its name is in a pale
    // shade of that video's colour, a switch, slider or picture frame of it
    // in the colour.
    int video{-1};
    std::vector<std::wstring> tileMarks;    // top-left: "▶ 40%", "✓"
    std::vector<float> tileProgress;        // 0..1 along the picture's bottom, 0 for none
    // Item: an accent pill before the value ("Sub"). Tiles: one per tile,
    // at the picture's bottom-left; empty for none.
    std::wstring tag;
    std::vector<std::wstring> tileTags;
    // Item and Buttons: the account's Emby favorite, a heart before the tag
    // or at the end of the label line. Tiles: one per tile, a heart under
    // the top-right badge. Painted only; it takes no click of its own.
    bool favorite{};
    std::vector<bool> tileFavorites;
    // MediaDetail: 0 primary, 1 from beginning, 2 more/less, 3 add,
    // 4 add from beginning, 5 favorite (`on` while it is one). Empty
    // captions hide slots without renumbering.
    PanelMediaDetail mediaDetail;
    PanelTextField field;
    // Item/Tiles: an independent add action; the ordinary entry still
    // opens details or plays. Tile availability follows its item index.
    bool addAvailable{};
    std::vector<bool> tileAddAvailable;
    // Buttons only: opt into wrapping at this minimum width in DIPs.
    // Zero preserves existing fixed one-line rows. Empty slots stay hidden.
    float buttonMinWidth{};
    // Buttons: drawn in the accent, the action a form's Enter takes.
    bool primary{};
    // Buttons, the sheet's: one per caption, a button that switches
    // something on and off -- a browser toolbar's filter -- lit as a chosen
    // segment while it is on. The settings window's compact buttons do not
    // light.
    std::vector<bool> buttonsOn;
    // Note: in the warning colour -- something went wrong.
    bool alert{};
    // Note only: reserve its complete wrapped text height before laying out
    // later rows. Ordinary notes retain the fixed height on existing pages.
    bool wrapNote{};
    float measuredNoteHeight{};
    float measuredNoteWidth{};
    float measuredNoteScale{};
    // The leading rows marked so stay under the header, below any tab
    // strip, while the rest scroll beneath them. Only a leading run counts:
    // a pinned row after one that scrolls scrolls too.
    bool pinned{};
};

// A browser list among the sheet's rows: one Item row per entry, or one
// Tiles row per line of `perLine` entries, the entries named by params
// counting from `firstParam` in steps of `paramStep`. App keeps a list's
// rows from frame to frame while the run keeps its shape, and fills only
// those near the screen.
struct PanelListRun {
    std::size_t at{};  // the index of its first row among the sheet's rows
    std::size_t entries{};
    PanelRowKind kind{PanelRowKind::Item};
    SettingId id{SettingId::None};
    int firstParam{};
    int paramStep{1};
    int perLine{1};
    float tileWidth{220.0F};
    float tileAspect{16.0F / 9.0F};

    std::size_t rowCount() const {
        if (kind != PanelRowKind::Tiles) return entries;
        const auto line = static_cast<std::size_t>(std::max(1, perLine));
        return (entries + line - 1) / line;
    }
    bool contains(std::size_t row) const { return row >= at && row - at < rowCount(); }
    // Everything but where it starts.
    bool sameShape(const PanelListRun& other) const {
        return entries == other.entries && kind == other.kind && id == other.id && firstParam == other.firstParam &&
               paramStep == other.paramStep && (kind != PanelRowKind::Tiles ||
               (perLine == other.perLine && tileWidth == other.tileWidth && tileAspect == other.tileAspect));
    }
    // Its `index`-th row, bare: kind, id and the params of its entries; a
    // Tiles row also its tiles' size and one empty title per tile.
    PanelRow bareRow(std::size_t index) const {
        PanelRow row;
        row.kind = kind;
        row.id = id;
        row.contentPending = true;
        if (kind != PanelRowKind::Tiles) {
            row.param = firstParam + static_cast<int>(index) * paramStep;
            return row;
        }
        const auto line = static_cast<std::size_t>(std::max(1, perLine));
        const std::size_t start = index * line;
        const std::size_t end = std::min(entries, start + line);
        row.tileWidth = tileWidth;
        row.tileAspect = tileAspect;
        row.options.resize(end > start ? end - start : 0);
        row.tileParams.reserve(row.options.size());
        for (std::size_t entry = start; entry < end; ++entry) {
            row.tileParams.push_back(firstParam + static_cast<int>(entry) * paramStep);
        }
        return row;
    }
};

// `rows` with each run's rows put in, bare, where the run says.
inline std::vector<PanelRow> panelRowsWithRuns(std::vector<PanelRow> rows, const std::vector<PanelListRun>& runs) {
    if (runs.empty()) return rows;
    std::size_t total = rows.size();
    for (const auto& run : runs) total += run.rowCount();
    std::vector<PanelRow> result;
    result.reserve(total);
    std::size_t next = 0;
    for (const auto& run : runs) {
        while (result.size() < run.at && next < rows.size()) result.push_back(std::move(rows[next++]));
        for (std::size_t index = 0; index < run.rowCount(); ++index) result.push_back(run.bareRow(index));
    }
    while (next < rows.size()) result.push_back(std::move(rows[next++]));
    return result;
}

// The row that shows the entry `param` names in the list `id`; none when
// no run of it lists the entry.
inline std::optional<std::size_t> panelListRowOf(const std::vector<PanelListRun>& runs, SettingId id, int param) {
    for (const auto& run : runs) {
        if (run.id != id || run.paramStep == 0) continue;
        const long long offset = static_cast<long long>(param) - run.firstParam;
        if (offset % run.paramStep != 0) continue;
        const long long entry = offset / run.paramStep;
        if (entry < 0 || static_cast<unsigned long long>(entry) >= run.entries) continue;
        const auto index = static_cast<std::size_t>(entry);
        if (run.kind != PanelRowKind::Tiles) return run.at + index;
        return run.at + index / static_cast<std::size_t>(std::max(1, run.perLine));
    }
    return std::nullopt;
}

// Calls `visit` with the index of every row that belongs to no run.
template <typename Visit>
void forEachRowOutsideRuns(std::size_t rowCount, const std::vector<PanelListRun>& runs, Visit visit) {
    std::size_t index = 0;
    for (const auto& run : runs) {
        for (; index < run.at && index < rowCount; ++index) visit(index);
        index = std::max(index, run.at + run.rowCount());
    }
    for (; index < rowCount; ++index) visit(index);
}

inline constexpr float kPanelWrappedNoteLineHeight = 20.0F;
inline constexpr float kPanelWrappedNoteTopPad = 2.0F;
inline constexpr float kPanelWrappedNoteBottomPad = 6.0F;

inline OverlayRect panelWrappedNoteTextBox(const OverlayRect& row, float scale) {
    return {row.x, row.y + kPanelWrappedNoteTopPad * scale, row.width,
        std::max(0.0F, row.height - (kPanelWrappedNoteTopPad + kPanelWrappedNoteBottomPad) * scale)};
}

// What a hit on a row means: a Tiles row has one item per tile, an
// Equalizer row one per band.
inline int panelRowParam(const PanelRow& row, int part) {
    if (row.kind != PanelRowKind::Tiles && row.kind != PanelRowKind::Equalizer) return row.param;
    if (part < 0 || static_cast<std::size_t>(part) >= row.tileParams.size()) return -1;
    return row.tileParams[static_cast<std::size_t>(part)];
}

struct PanelMediaDetailGeometry {
    OverlayRect poster, title, metadata, mediaInfo, progress, progressText, overview;
    std::array<OverlayRect, 6> actions{};
    float height{};
    bool stacked{};
};

// Font-free fallback for the time before DWrite is ready.
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
    if (!detail.compactActions) {
        block(result.metadata, textHeight(1, detail.metadata, 14.0F, 22.0F), 10.0F);
        block(result.mediaInfo, textHeight(2, detail.mediaInfo, 12.0F, 20.0F), 18.0F);
    }
    const auto hasAction = [&](std::size_t index) {
        return index < row.options.size() && !row.options[index].empty();
    };
    // The favorite goes last among the buttons, after what plays.
    constexpr std::array<std::size_t, 5> buttonActions{0, 1, 3, 4, 5};
    int buttons = 0;
    for (const auto index : buttonActions) if (hasAction(index)) ++buttons;
    if (buttons > 0 && detail.compactActions) {
        const float gap = 8.0F * s, height = 40.0F * s;
        float x = textX;
        for (const std::size_t index : {0U, 3U, 5U}) {
            if (!hasAction(index)) continue;
            const bool dropdown = index != 5 && hasAction(index + 1);
            const float groupWidth = std::min(textWidth, (index == 0 ? 180.0F : index == 3 ? 140.0F : 40.0F) * s);
            if (x > textX && x + groupWidth > textX + textWidth + 0.1F) { x = textX; y += height + gap; }
            const float arrow = dropdown ? std::min(32.0F * s, groupWidth * 0.3F) : 0.0F;
            result.actions[index] = {x, y, std::max(0.0F, groupWidth - arrow), height};
            if (dropdown) result.actions[index + 1] = {x + groupWidth - arrow, y, arrow, height};
            x += groupWidth + gap;
        }
        y += height + 16.0F * s;
        block(result.metadata, textHeight(1, detail.metadata, 14.0F, 22.0F), 10.0F);
        block(result.mediaInfo, textHeight(2, detail.mediaInfo, 12.0F, 20.0F), 18.0F);
    } else if (buttons > 0) {
        const float gap = 12.0F * s;
        const int perLine = std::clamp(static_cast<int>((textWidth + gap) / (184.0F * s + gap)), 1, buttons);
        const float buttonWidth = perLine == 1 ? std::min(textWidth, 280.0F * s)
            : std::min(232.0F * s, (textWidth - (perLine - 1) * gap) / perLine);
        int placed = 0;
        for (const auto index : buttonActions) {
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
    if (hasAction(2)) {
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
    // A Choice or Buttons row laid out compact: its label's box and a
    // Buttons row's value's, beside the controls or on the line above them.
    bool compact{};
    OverlayRect label;
    OverlayRect value;
};

// A card the settings window's rows stand on (PanelMetrics::cards): its box
// and the rows on it, `first` up to `end`.
struct PanelCard {
    OverlayRect box;
    std::size_t first{};
    std::size_t end{};
};

struct PanelLayout {
    OverlayRect sheet;
    OverlayRect header;
    OverlayRect close;
    // The header's switches to the sheet's other faces, left of the close
    // button in their order, the last beside it; empty when the sheet was
    // laid out without any.
    std::vector<OverlayRect> sheetSwitches;
    // The column of tabs down the sheet's left, top to bottom, when it has
    // tabs: the column, the sheet's title at its top (beside the header,
    // which with the content and rows takes the rest of the width) and one
    // rectangle per tab below. All empty for a sheet without tabs.
    OverlayRect tabColumn;
    OverlayRect tabTitle;
    std::vector<OverlayRect> tabs;
    // A window's own caption strip across its top (PanelMetrics::
    // windowCaption), as the player's: the title in `tabTitle` at its left
    // and `close` square at its right end. Empty for a sheet over the
    // picture, whose close is in its header.
    OverlayRect caption;
    // The band of pinned rows under the header, and how
    // many of the leading rows lie in it; empty and zero when none do, or
    // when they would take more than half of the room below the header.
    OverlayRect pinned;
    std::size_t pinnedRows{};
    OverlayRect content;
    // The strip at the content's right edge that takes the scrollbar; empty
    // when everything fits.
    OverlayRect scrollbar;
    std::vector<PanelRowGeometry> rows;
    // The cards down the content, scrolled with their rows; empty for a
    // sheet laid out without them.
    std::vector<PanelCard> cards;
    float contentHeight{};
    float maxScroll{};
    // The scroll the rows were laid out with, clamped.
    float scroll{};
    // The first MediaDetail row, whose backdrop lies behind the whole
    // sheet, or -1: the painter need not look for it down a long list.
    int mediaDetailRow{-1};
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

// A video being played, marked on the scrollbar where the list shows it:
// `y` is the mark's middle, `position` the video's place on the screen
// (0 for V1), whose colour it is drawn in, and `scroll` the scroll that
// shows its row in the middle of the list.
struct PanelScrollMark {
    float y{};
    int position{};
    float scroll{};
};

// The scroll that brings the row `row`'s middle to the content's, as far as
// the list's ends let it.
inline float panelScrollCentring(const PanelLayout& layout, std::size_t row) {
    const OverlayRect& box = layout.rows[row].row;
    const float middle = box.y - layout.content.y + layout.scroll + box.height * 0.5F;
    return std::clamp(middle - layout.content.height * 0.5F, 0.0F, layout.maxScroll);
}

// Where the scrollbar marks the row `row`: the thumb's middle once that row
// is scrolled to the middle of the content, so a press on the mark, which
// brings the thumb's middle under the pointer, shows the row. Negative for
// a pinned row, one not laid out, or a sheet with nothing to scroll.
inline float panelScrollMarkY(const PanelLayout& layout, std::size_t row, float scale) {
    const OverlayRect thumb = panelScrollThumb(layout, scale);
    if (!thumb.visible() || row < layout.pinnedRows || row >= layout.rows.size()) return -1.0F;
    const float travel = std::max(0.0F, layout.scrollbar.height - thumb.height);
    return layout.scrollbar.y + travel * (panelScrollCentring(layout, row) / layout.maxScroll) + thumb.height * 0.5F;
}

// A press at `y` on the scrollbar: how far below the thumb's top the
// pointer holds it while it is dragged. On a mark, or within a few pixels
// of one, the thumb goes where that mark's row is in the middle exactly --
// a pixel of a long list's track is many rows. Else on the thumb it is
// held where it was pressed, and on the track by its middle.
inline float panelThumbGrab(const PanelLayout& layout, float scale, float y,
                            const std::vector<PanelScrollMark>& marks) {
    const OverlayRect thumb = panelScrollThumb(layout, scale);
    const PanelScrollMark* nearest = nullptr;
    for (const auto& mark : marks) {
        const float distance = std::abs(mark.y - y);
        if (distance <= 5.0F * scale && (!nearest || distance < std::abs(nearest->y - y))) nearest = &mark;
    }
    if (nearest && thumb.visible() && layout.maxScroll > 0.0F) {
        const float travel = std::max(0.0F, layout.scrollbar.height - thumb.height);
        return y - (layout.scrollbar.y + travel * (nearest->scroll / layout.maxScroll));
    }
    return y >= thumb.y && y <= thumb.bottom() ? y - thumb.y : thumb.height * 0.5F;
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
    // A switch as wide and tall as a menu's (PanelMenuMetrics).
    float switchWidth{40.0F};
    float switchHeight{20.0F};
    float trailingSize{32.0F};
    float bottomPad{24.0F};
    int segmentsPerLine{3};
    float tileGap{12.0F};
    float tileLabel{40.0F};   // two lines of small text under a picture
    float tabColumnWidth{200.0F};
    float tabInset{10.0F};    // between the tabs and the column's sides
    float tabHeight{40.0F};
    float tabGap{4.0F};
    // The settings window: a caption strip of its own across its top, as
    // tall as the player's with the same square close at its right end
    // (CaptionMetrics), its title small at its left; under it the header,
    // shorter, its first tab level with it.
    bool windowCaption{};
    float windowHeaderHeight{48.0F};
    // The settings window's rows on cards, as Windows' own settings: the
    // rows between two headers on one card, inset from its sides, and each
    // header a title over its card. The pinned rows stand on none.
    bool cards{};
    float cardPad{16.0F};        // between a card's sides and its rows
    float cardEdge{4.0F};        // above its first row and under its last
    float cardGap{12.0F};        // under a card, and above one the content starts with
    float cardHeaderRow{36.0F};  // a header over a card
    float cardRadius{8.0F};
    float sheetSwitchWidth{88.0F};
    float textFieldRow{60.0F};
    float textFieldHeight{40.0F};
    float shortcutRow{40.0F};
    float shortcutKeyWidth{104.0F};
    float shortcutKeyHeight{28.0F};
    // The settings window's Choice and Buttons rows: each segment or button
    // as wide as its caption, not a share of the row. They stand at the
    // row's right with the label at its left, as a toggle's switch does,
    // when all of them fit there; else on lines of their own under the
    // label, from the left. A Choice row's segments are joined in a track.
    bool compactControls{};
    float compactRow{44.0F};
    float compactTrack{32.0F};
    float compactInset{3.0F};
    float compactSegmentGap{2.0F};
    // As tall as the track, a drop-down's box and a spin box beside them.
    float compactButton{32.0F};
    float compactCaptionPad{14.0F};
    float compactSegmentMinimum{56.0F};
    float compactButtonMinimum{64.0F};
    float compactLabelGap{24.0F};
    float compactValueGap{12.0F};
    float compactLineGap{6.0F};
    // A drop-down's box at the row's right: as wide as its widest option
    // with the padding and the chevron, but no narrower than the minimum
    // and no wider than its share of the row. The two pads and the chevron
    // are as wide as a list's tick column, inset and card padding
    // (PanelMenuMetrics), so the list hung from the box is the box's width.
    float dropDownHeight{32.0F};
    float dropDownMinimum{150.0F};
    float dropDownShare{0.55F};
    float dropDownPad{12.0F};
    float dropDownChevron{34.0F};
    // A spin box's at the row's right, as high as a drop-down's box, its
    // arrows in a column at its right end.
    float spinBoxWidth{96.0F};
    float spinBoxArrows{26.0F};
    // A graphic equalizer: the line of values over its sliders, the line of
    // captions under them, and the gap that sets the leading bands apart.
    float equalizerRow{236.0F};
    float equalizerValueLine{26.0F};
    float equalizerCaptionLine{30.0F};
    float equalizerApartGap{24.0F};
};

// The width of the column of tabs down a sheet `sheetWidth` wide: what the
// metrics ask, but never more than half of the sheet.
inline float panelTabColumnWidth(float sheetWidth, float scale, const PanelMetrics& m = {}) {
    return std::clamp(m.tabColumnWidth * std::max(0.0F, scale), 0.0F, std::max(0.0F, sheetWidth) * 0.5F);
}

// How far a row on a card stands in from either side of the content: the
// card's padding, nothing for a sheet without cards.
inline float panelCardInset(float scale, const PanelMetrics& m = {}) {
    return m.cards ? m.cardPad * std::max(0.0F, scale) : 0.0F;
}

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

// A line of text's width before DirectWrite has measured it, in 96-DPI
// units at `em`: a little over half of it for a Latin letter, the whole of
// it for a CJK character, so the estimate errs wide.
inline float panelTextWidthEstimate(const std::wstring& text, float em = 14.0F) {
    float width = 0.0F;
    for (const wchar_t c : text) width += (c >= 0x2E80 ? 1.0F : 0.56F) * em;
    return width;
}

// A compact Choice or Buttons row (PanelMetrics::compactControls) in a row
// `innerWidth` wide at `x`, `y`: its height and, given a geometry, its parts,
// label and value. Every part is its caption's width and the padding either
// side, no less than the minimum; a Buttons row's value stands just left of
// its buttons. Beside the label they end at the row's right, one line at the
// row's own height. Under it, with the value at the label line's right end,
// they run from its left on one line when they fit there; else they are a
// grid of cells all as wide as the widest, as many columns as fit and then
// as few as that many lines allow, so every line but the last is full. A
// Choice row's segments, on one line or in a grid, share one track.
inline float panelCompactRow(const PanelRow& row, float x, float y, float innerWidth, float s,
                             const PanelMetrics& m, PanelRowGeometry* geometry = nullptr) {
    const bool choice = row.kind == PanelRowKind::Choice;
    const std::size_t count = row.options.size();
    // As the sheet's Buttons rows, an empty caption hides its slot only
    // where the row asks to wrap.
    const auto shown = [&](std::size_t i) { return choice || row.buttonMinWidth <= 0.0F || !row.options[i].empty(); };
    const float minimum = (choice ? m.compactSegmentMinimum : m.compactButtonMinimum) * s;
    const auto partWidth = [&](std::size_t i) {
        const float caption = i < row.captionWidths.size() && row.captionWidths[i] >= 0.0F
            ? row.captionWidths[i] : panelTextWidthEstimate(row.options[i]);
        return std::max(minimum, (caption + 2.0F * m.compactCaptionPad) * s);
    };
    const auto textWidth = [](float measured, const std::wstring& text, float em) {
        if (text.empty()) return 0.0F;
        return measured >= 0.0F ? measured : panelTextWidthEstimate(text, em);
    };
    const float gap = (choice ? m.compactSegmentGap : m.segmentGap) * s;
    const float inset = choice ? m.compactInset * s : 0.0F;
    const float partHeight = (choice ? m.compactTrack - 2.0F * m.compactInset : m.compactButton) * s;
    // Between a grid's lines: a Choice's segments inside their track, a
    // Buttons row's buttons as far apart as lines of them always were.
    const float lineGap = choice ? gap : m.compactLineGap * s;
    int shownCount = 0;
    float parts = 0.0F;
    float widest = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
        if (!shown(i)) continue;
        const float width = partWidth(i);
        parts += (shownCount > 0 ? gap : 0.0F) + width;
        widest = std::max(widest, width);
        ++shownCount;
    }
    const float controls = shownCount > 0 ? parts + 2.0F * inset : 0.0F;
    const float label = textWidth(row.labelWidth, row.label, 14.0F) * s;
    const float value = choice ? 0.0F : textWidth(row.valueWidth, row.value, 12.0F) * s;
    const float labelSpace = label > 0.0F ? label + m.compactLabelGap * s : 0.0F;
    const float valueSpace = value > 0.0F ? value + m.compactValueGap * s : 0.0F;
    const bool beside = labelSpace + valueSpace + controls <= innerWidth;
    const float room = std::max(0.0F, innerWidth - 2.0F * inset);
    const bool oneLine = beside || parts <= room;
    int columns = shownCount;
    int lines = shownCount > 0 ? 1 : 0;
    if (!oneLine) {
        const int fit = std::max(1, static_cast<int>((room + gap) / (widest + gap)));
        lines = (shownCount + fit - 1) / fit;
        columns = (shownCount + lines - 1) / lines;
    }
    const float block = lines > 0
        ? 2.0F * inset + static_cast<float>(lines) * partHeight + static_cast<float>(lines - 1) * lineGap : 0.0F;

    float height = 0.0F;
    float top = 0.0F;
    float left = x;
    if (beside) {
        height = m.compactRow * s;
        top = (height - block) * 0.5F;
        left = x + innerWidth - controls;
    } else {
        top = label > 0.0F || value > 0.0F ? m.labelLine * s : 6.0F * s;
        height = top + block + 8.0F * s;
    }
    if (!geometry) return height;

    geometry->compact = true;
    if (beside) {
        const float valueX = left - valueSpace;
        geometry->label = {x, y, std::max(0.0F, valueX - (label > 0.0F ? m.compactLabelGap * s : 0.0F) - x), height};
        if (value > 0.0F) geometry->value = {valueX, y, value, height};
    } else {
        geometry->label = {x, y, innerWidth, m.labelLine * s};
        if (value > 0.0F) geometry->value = geometry->label;
    }
    geometry->parts.assign(count, OverlayRect{});
    int placed = 0;
    float cursor = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
        if (!shown(i)) continue;
        if (oneLine) {
            const float width = partWidth(i);
            geometry->parts[i] = {left + inset + cursor, y + top + inset, width, partHeight};
            cursor += width + gap;
        } else {
            const auto column = static_cast<float>(placed % columns);
            const auto line = static_cast<float>(placed / columns);
            geometry->parts[i] = {left + inset + column * (widest + gap), y + top + inset + line * (partHeight + lineGap),
                                  widest, partHeight};
        }
        ++placed;
    }
    return height;
}

// A drop-down Choice row in its box: the label at the left, as a toggle's,
// and the box at the right, upright in the middle of the row.
inline void panelDropDownRow(const PanelRow& row, PanelRowGeometry& geometry, float s, const PanelMetrics& m) {
    float widest = 0.0F;
    for (std::size_t i = 0; i < row.options.size(); ++i) {
        widest = std::max(widest, i < row.captionWidths.size() && row.captionWidths[i] >= 0.0F
            ? row.captionWidths[i] : panelTextWidthEstimate(row.options[i]));
    }
    if (row.selected < 0 && !row.value.empty()) {
        widest = std::max(widest, row.valueWidth >= 0.0F ? row.valueWidth : panelTextWidthEstimate(row.value));
    }
    const OverlayRect& box = geometry.row;
    const float minimum = std::min(box.width, m.dropDownMinimum * s);
    const float width = std::clamp((widest + 2.0F * m.dropDownPad + m.dropDownChevron) * s, minimum,
                                   std::max(minimum, box.width * m.dropDownShare));
    const float height = std::min(m.dropDownHeight * s, box.height);
    geometry.compact = true;
    geometry.control = {box.right() - width, box.y + (box.height - height) * 0.5F, width, height};
    geometry.label = {box.x, box.y, std::max(0.0F, geometry.control.x - m.compactLabelGap * s - box.x), box.height};
}

// A spin box: the label at the left, as a toggle's, and the box at the
// right, upright in the middle of the row; its arrows split the column at
// its right end, the one up over the one down.
inline void panelSpinBoxRow(PanelRowGeometry& geometry, float s, const PanelMetrics& m) {
    const OverlayRect& box = geometry.row;
    const float width = std::min(m.spinBoxWidth * s, box.width);
    const float height = std::min(m.dropDownHeight * s, box.height);
    const float arrows = std::min(m.spinBoxArrows * s, width);
    geometry.compact = true;
    geometry.control = {box.right() - width, box.y + (box.height - height) * 0.5F, width, height};
    const float half = std::round(height * 0.5F);
    geometry.parts = {{geometry.control.right() - arrows, geometry.control.y, arrows, half},
                      {geometry.control.right() - arrows, geometry.control.y + half, arrows, height - half}};
    geometry.value = {geometry.control.x, geometry.control.y, width - arrows, height};
    geometry.label = {box.x, box.y, std::max(0.0F, geometry.control.x - m.compactLabelGap * s - box.x), box.height};
}

// A graphic equalizer's bands: as many columns as captions across the row,
// those set apart a gap from the rest, each column's band of travel between
// the line of values and the line of captions. `control` is that band
// across the row.
inline void panelEqualizerRow(const PanelRow& row, PanelRowGeometry& geometry, float s, const PanelMetrics& m) {
    const OverlayRect& box = geometry.row;
    const std::size_t count = row.options.size();
    if (count == 0) return;
    const bool apart = row.apartBands > 0 && static_cast<std::size_t>(row.apartBands) < count;
    const float gap = apart ? m.equalizerApartGap * s : 0.0F;
    const float column = std::max(0.0F, (box.width - gap) / static_cast<float>(count));
    const float top = box.y + m.equalizerValueLine * s;
    const float bottom = std::max(top, box.bottom() - m.equalizerCaptionLine * s);
    geometry.control = {box.x, top, box.width, bottom - top};
    geometry.parts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float x = box.x + static_cast<float>(i) * column +
                        (apart && i >= static_cast<std::size_t>(row.apartBands) ? gap : 0.0F);
        geometry.parts.push_back({x, top, column, bottom - top});
    }
}

inline float panelRowHeight(const PanelRow& row, float scale, const PanelMetrics& m = {}, float rowWidth = -1.0F) {
    switch (row.kind) {
    case PanelRowKind::Header: return (m.cards ? m.cardHeaderRow : m.headerRow) * scale;
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
    case PanelRowKind::CompactItem: return 84.0F * scale;
    case PanelRowKind::Tiles:
        return (row.tileWidth / std::max(0.1F, row.tileAspect) + m.tileLabel + m.tileGap) * scale;
    case PanelRowKind::Slider: return m.sliderRow * scale;
    case PanelRowKind::Equalizer: return m.equalizerRow * scale;
    case PanelRowKind::Choice: {
        if (row.dropDown) return m.compactRow * scale;
        if (m.compactControls) {
            return panelCompactRow(row, 0.0F, 0.0F,
                                   rowWidth >= 0.0F ? rowWidth : std::max(0.0F, m.width - 2.0F * m.pad) * scale, scale, m);
        }
        const int lines = panelChoiceLines(static_cast<int>(row.options.size()),
                                           row.segmentsPerLine > 0 ? row.segmentsPerLine : m.segmentsPerLine);
        return (m.labelLine + lines * m.segment + std::max(0, lines - 1) * m.segmentGap + 8.0F) * scale;
    }
    case PanelRowKind::Buttons: {
        if (row.spinBox) return m.compactRow * scale;
        if (m.compactControls) {
            return panelCompactRow(row, 0.0F, 0.0F,
                                   rowWidth >= 0.0F ? rowWidth : std::max(0.0F, m.width - 2.0F * m.pad) * scale, scale, m);
        }
        const int lines = panelChoiceLines(panelButtonCount(row), panelButtonsPerLine(row, rowWidth, scale, m));
        return ((row.label.empty() ? m.buttonsRow : m.buttonsRow + m.labelLine) +
            std::max(0, lines - 1) * (m.segment + m.segmentGap)) * scale;
    }
    case PanelRowKind::MediaDetail:
        return panelMediaDetailLayout(row, {0.0F, 0.0F, rowWidth >= 0.0F ? rowWidth :
            std::max(0.0F, m.width - 2.0F * m.pad) * scale, 0.0F}, scale).height;
    case PanelRowKind::TextField:
        // A label stands above a sign-in box; the search box has none.
        return (row.label.empty() ? m.textFieldRow : m.labelLine + m.textFieldHeight + 12.0F) * scale;
    case PanelRowKind::Shortcut: return m.shortcutRow * scale;
    }
    return 0.0F;
}

// A row's parts, placed from its box (`geometry.row`): a toggle's switch, a
// slider's band and trailing toggle, segments, buttons, tiles and their Add
// buttons, an information page's blocks. Whatever the geometry held goes.
inline void panelRowParts(PanelRowGeometry& geometry, const PanelRow& row, float scale, const PanelMetrics& m = {}) {
    const float s = std::max(0.0F, scale);
    geometry.control = {};
    geometry.trailing = {};
    geometry.parts.clear();
    geometry.addParts.clear();
    geometry.mediaDetail = {};
    geometry.compact = false;
    geometry.label = {};
    geometry.value = {};
    const float innerX = geometry.row.x;
    const float innerWidth = geometry.row.width;
    const float y = geometry.row.y;
    const float rowHeight = geometry.row.height;
    switch (row.kind) {
    case PanelRowKind::CompactItem:
        geometry.control = {innerX + 8.0F * s, y + 14.0F * s,
            std::min(100.0F * s, std::max(0.0F, innerWidth * 0.3F)), 56.0F * s};
        break;
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
    case PanelRowKind::TextField: {
        // The box across the row, under its label if it has one; its clear
        // button at the right end only while there is something to clear.
        const float h = std::min(m.textFieldHeight * s, rowHeight);
        const float top = row.label.empty() ? y + (rowHeight - h) * 0.5F : y + m.labelLine * s;
        geometry.control = {innerX, top, innerWidth, h};
        if (row.field.clearable && (!row.field.edit.text.empty() || !row.field.composition.empty())) {
            const float size = std::min(m.trailingSize * s, h);
            geometry.trailing = {geometry.control.right() - 4.0F * s - size,
                                 geometry.control.y + (h - size) * 0.5F, size, size};
        }
        break;
    }
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
    case PanelRowKind::Equalizer:
        panelEqualizerRow(row, geometry, s, m);
        break;
    case PanelRowKind::Choice: {
        if (row.dropDown) {
            panelDropDownRow(row, geometry, s, m);
            break;
        }
        if (m.compactControls) {
            panelCompactRow(row, innerX, y, innerWidth, s, m, &geometry);
            break;
        }
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
        if (row.spinBox) {
            panelSpinBoxRow(geometry, s, m);
            break;
        }
        if (m.compactControls) {
            panelCompactRow(row, innerX, y, innerWidth, s, m, &geometry);
            break;
        }
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
    case PanelRowKind::Shortcut: {
        // The key boxes side by side at the right end, together no more
        // than three fifths of the row; the action's name has the rest.
        const int count = static_cast<int>(row.options.size());
        if (count <= 0) break;
        const float gap = m.segmentGap * s;
        const float keyWidth = std::min(m.shortcutKeyWidth * s,
            std::max(0.0F, (innerWidth * 0.6F - gap * static_cast<float>(count - 1)) / static_cast<float>(count)));
        const float keyHeight = std::min(m.shortcutKeyHeight * s, rowHeight);
        const float top = y + (rowHeight - keyHeight) * 0.5F;
        for (int i = 0; i < count; ++i) {
            geometry.parts.push_back({innerX + innerWidth - static_cast<float>(count - i) * keyWidth -
                                          static_cast<float>(count - 1 - i) * gap,
                                      top, keyWidth, keyHeight});
        }
        break;
    }
    default:
        break;
    }
}

// Where a TextField's text runs: after the magnifier, if any, at the box's
// left, before the clear button or the box's rounded end. The painter draws
// the text here and App places the caret of a press and the input method's
// windows by it, so both measure from the same edge.
inline OverlayRect panelTextFieldTextBox(const PanelRow& row, const PanelRowGeometry& geometry, float scale) {
    const float s = std::max(0.0F, scale);
    const OverlayRect& box = geometry.control;
    const float left = box.x + (row.field.magnifier ? 42.0F : 14.0F) * s;
    const float right = geometry.trailing.visible() ? geometry.trailing.x - 4.0F * s : box.right() - 16.0F * s;
    return {left, box.y, std::max(0.0F, right - left), box.height};
}

// A password's dot: the black circle Windows' own password box shows; a
// bullet is too small to count at the box's size.
inline constexpr wchar_t kPanelPasswordDot = L'\x25CF';

// The text a TextField shows: what was typed with an input method's
// composition at the caret, or, for a password, a dot for each code unit,
// so the caret's index into either counts the same.
inline std::wstring panelTextFieldShown(const PanelTextField& field) {
    std::wstring shown = textFieldShown(field.edit, field.composition);
    if (field.masked) shown.assign(shown.size(), kPanelPasswordDot);
    return shown;
}

// The drawn caret's width, which a long line's scroll leaves room for.
inline float panelTextFieldCaretWidth(float scale) { return std::max(1.0F, std::round(scale)); }

// `slide` is 0 while the sheet is off screen and 1 when fully in; the sheet
// travels a short distance rather than its whole width so the fade carries
// the motion. `scroll` is clamped to what the content allows.
// `sheetWidth` overrides the sheet's width when positive: the Emby browser
// takes the whole client with nothing playing and the right part of it --
// docked, as PotPlayer's playlist -- beside a video.
// `captionTop` is the window caption's strip: the sheet's column still reaches the
// top edge, under the window buttons, but its header and rows start below.
// `tabCount` puts that many tabs in a column down the sheet's left, under
// its title, and the header and rows beside it; their captions are App's
// (settingsTabName) and the rows are the chosen tab's.
// `sheetSwitches` puts that many switches to the sheet's other faces in the
// header, the last beside the close glyph, each as wide as one switch has
// always been while the room beside the title allows, else sharing it.
// Laid out into `layout`, whose rows' storage is used again: App lays the
// sheet out every frame, a long list's rows with it. A bare row
// (contentPending) gets its box alone; App places the parts of those it
// fills (panelRowParts).
inline void settingsPanelLayoutInto(
    PanelLayout& layout, float width, float height, float scale, const std::vector<PanelRow>& rows,
    float scroll, float slide = 1.0F, const PanelMetrics& m = {}, float sheetWidthOverride = 0.0F,
    float captionTop = 0.0F, int tabCount = 0, int sheetSwitches = 0) {
    {
        std::vector<PanelRowGeometry> kept = std::move(layout.rows);
        layout = PanelLayout{};
        layout.rows = std::move(kept);
    }
    const float s = std::max(0.0F, scale);
    if (width <= 0.0F || height <= 0.0F || s <= 0.0F) {
        layout.rows.clear();
        return;
    }
    // A window's own caption strip, the player's height, over everything.
    const CaptionMetrics caption{};
    const float captionHeight = m.windowCaption ? std::min(caption.rowHeight * s, height) : 0.0F;
    const float headerTop = std::clamp(std::max(captionTop, captionHeight), 0.0F, height);
    const float sheetWidth = sheetWidthOverride > 0.0F
        ? std::min(width, sheetWidthOverride)
        : std::min(m.width * s, std::max(0.0F, width - m.sideGutter * s));
    const float travel = 48.0F * s * (1.0F - std::clamp(slide, 0.0F, 1.0F));
    layout.sheet = {width - sheetWidth + travel, 0.0F, sheetWidth, height};
    // With tabs, their column takes the sheet's left and the header, the
    // content and the rows the rest.
    const float tabColumnWidth = tabCount > 0 ? panelTabColumnWidth(sheetWidth, s, m) : 0.0F;
    const float headerHeight =
        std::min((m.windowCaption ? m.windowHeaderHeight : m.headerHeight) * s, height - headerTop);
    layout.header = {layout.sheet.x + tabColumnWidth, headerTop, sheetWidth - tabColumnWidth, headerHeight};
    const float closeSize = std::min(m.closeSize * s, headerHeight);
    if (m.windowCaption) {
        // Close square at the strip's right end and the title small at its
        // left, inset as the player's (captionLayout).
        layout.caption = {layout.sheet.x, 0.0F, sheetWidth, captionHeight};
        const float button = std::min(caption.button * s, sheetWidth);
        layout.close = {layout.sheet.right() - button, 0.0F, button, captionHeight};
        const float inset = caption.titleInset * s;
        layout.tabTitle = {layout.sheet.x + inset, 0.0F,
                           std::max(0.0F, layout.close.x - layout.sheet.x - 2.0F * inset), captionHeight};
    } else {
        layout.close = {layout.sheet.right() - m.pad * s - closeSize,
                        headerTop + (headerHeight - closeSize) * 0.5F, closeSize, closeSize};
    }
    if (sheetSwitches > 0) {
        const float gap = 8.0F * s;
        // Beside the close glyph, or at the header's end where the close is
        // in a window's caption.
        const float end = m.windowCaption ? layout.header.right() - m.pad * s + gap : layout.close.x;
        const float room = std::max(0.0F, end - gap - layout.header.x - m.pad * s);
        const float count = static_cast<float>(sheetSwitches);
        const float switchWidth = std::min(m.sheetSwitchWidth * s,
                                           std::max(0.0F, (room - gap * (count - 1.0F)) / count));
        layout.sheetSwitches.reserve(static_cast<std::size_t>(sheetSwitches));
        for (int i = 0; i < sheetSwitches; ++i) {
            const float right = end - gap - static_cast<float>(sheetSwitches - 1 - i) * (switchWidth + gap);
            layout.sheetSwitches.push_back(
                {right - switchWidth, headerTop + (headerHeight - closeSize) * 0.5F, switchWidth, closeSize});
        }
    }
    const float pad = m.pad * s;
    const float innerX = layout.header.x + pad;
    const float innerWidth = std::max(0.0F, layout.header.width - pad * 2.0F);
    float contentTop = headerTop + headerHeight;
    if (tabCount > 0) {
        // The column reaches the sheet's top and bottom; the sheet's title
        // stands at its top level with the header, and the tabs follow
        // one a line, as a window's navigation. Under a window's caption,
        // which has the title, the first tab stands level with the header.
        layout.tabColumn = {layout.sheet.x, layout.sheet.y, tabColumnWidth, height};
        if (!m.windowCaption) layout.tabTitle = {layout.sheet.x, headerTop, tabColumnWidth, headerHeight};
        const float inset = m.tabInset * s;
        const float tabHeight = m.tabHeight * s;
        const float gap = m.tabGap * s;
        const float firstTab = m.windowCaption ? headerTop + (headerHeight - tabHeight) * 0.5F : contentTop + gap;
        layout.tabs.reserve(static_cast<std::size_t>(tabCount));
        for (int i = 0; i < tabCount; ++i) {
            layout.tabs.push_back({layout.sheet.x + inset, firstTab + static_cast<float>(i) * (tabHeight + gap),
                                   std::max(0.0F, tabColumnWidth - 2.0F * inset), tabHeight});
        }
    }
    // The leading pinned rows, unless they would leave the rest less than
    // half of the room: a short window scrolls everything as before.
    float pinnedHeight = 0.0F;
    while (layout.pinnedRows < rows.size() && rows[layout.pinnedRows].pinned) {
        pinnedHeight += panelRowHeight(rows[layout.pinnedRows], s, m, innerWidth);
        ++layout.pinnedRows;
    }
    if (pinnedHeight > std::max(0.0F, height - contentTop) * 0.5F) {
        layout.pinnedRows = 0;
    } else if (layout.pinnedRows > 0) {
        layout.pinned = {layout.header.x, contentTop, layout.header.width, pinnedHeight};
        contentTop += pinnedHeight;
    }
    layout.content = {layout.header.x, contentTop, layout.header.width, std::max(0.0F, height - contentTop)};

    // With cards, a row of the content but a header stands on one, inset
    // from its sides; a card opens at the first such row after a header,
    // or at the content's top, and closes at the next header or the end.
    const float cardInset = panelCardInset(s, m);
    const float cardEdge = m.cardEdge * s;
    const float cardGap = m.cardGap * s;
    const auto onCard = [&](std::size_t index) {
        return m.cards && index >= layout.pinnedRows && rows[index].kind != PanelRowKind::Header;
    };

    // Each row's box but its place first, the content's height with them.
    layout.rows.resize(rows.size());
    float total = 0.0F;
    bool cardOpen = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const bool card = onCard(index);
        const float rowWidth = card ? std::max(0.0F, innerWidth - 2.0F * cardInset) : innerWidth;
        const float rowHeight = panelRowHeight(rows[index], s, m, rowWidth);
        layout.rows[index].row = {innerX + (card ? cardInset : 0.0F), 0.0F, rowWidth, rowHeight};
        if (index < layout.pinnedRows) continue;
        if (card && !cardOpen) total += (index == layout.pinnedRows ? cardGap : 0.0F) + cardEdge;
        if (!card && cardOpen) total += cardEdge + cardGap;
        cardOpen = card;
        total += rowHeight;
    }
    if (cardOpen) total += cardEdge;
    layout.contentHeight = total + m.bottomPad * s;
    layout.maxScroll = std::max(0.0F, layout.contentHeight - layout.content.height);
    const float clampedScroll = std::clamp(scroll, 0.0F, layout.maxScroll);
    layout.scroll = clampedScroll;
    if (layout.maxScroll > 0.0F) {
        const float strip = std::min(m.pad * s, 14.0F * s);
        layout.scrollbar = {layout.sheet.right() - strip, layout.content.y, strip, layout.content.height};
    }

    // Pinned rows go down their band unscrolled, the others down the content
    // and their cards with them.
    float pinnedY = layout.pinned.y;
    float scrolledY = layout.content.y - clampedScroll;
    cardOpen = false;
    const auto closeCard = [&](std::size_t end) {
        PanelCard& open = layout.cards.back();
        open.end = end;
        open.box.height = scrolledY + cardEdge - open.box.y;
    };
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        auto& geometry = layout.rows[index];
        float& cursor = index < layout.pinnedRows ? pinnedY : scrolledY;
        if (index >= layout.pinnedRows) {
            const bool card = onCard(index);
            if (card && !cardOpen) {
                if (index == layout.pinnedRows) cursor += cardGap;
                layout.cards.push_back({{innerX, cursor, innerWidth, 0.0F}, index, index});
                cursor += cardEdge;
            }
            if (!card && cardOpen) {
                closeCard(index);
                cursor += cardEdge + cardGap;
            }
            cardOpen = card;
        }
        geometry.row.y = cursor;
        cursor += geometry.row.height;
        if (row.kind == PanelRowKind::MediaDetail && layout.mediaDetailRow < 0) {
            layout.mediaDetailRow = static_cast<int>(index);
        }
        if (!row.contentPending) {
            panelRowParts(geometry, row, s, m);
            continue;
        }
        // A bare row: its box alone, whatever the slot held before.
        geometry.control = {};
        geometry.trailing = {};
        geometry.compact = false;
        geometry.label = {};
        geometry.value = {};
        if (!geometry.parts.empty()) geometry.parts.clear();
        if (!geometry.addParts.empty()) geometry.addParts.clear();
        // Only a laid-out information page has a height (or NaN).
        if (!(geometry.mediaDetail.height == 0.0F)) geometry.mediaDetail = {};
    }
    if (cardOpen) closeCard(rows.size());
}

inline PanelLayout settingsPanelLayout(
    float width, float height, float scale, const std::vector<PanelRow>& rows,
    float scroll, float slide = 1.0F, const PanelMetrics& m = {}, float sheetWidthOverride = 0.0F,
    float captionTop = 0.0F, int tabCount = 0, int sheetSwitches = 0) {
    PanelLayout layout;
    settingsPanelLayoutInto(layout, width, height, scale, rows, scroll, slide, m, sheetWidthOverride, captionTop,
                            tabCount, sheetSwitches);
    return layout;
}

// The first of the rows laid down the content -- those after the pinned
// ones, which follow each other in order -- whose bottom is at `y` or below:
// where a scan of what shows from `y` down starts.
inline std::size_t panelFirstRowReaching(const PanelLayout& layout, float y) {
    const auto begin = layout.rows.begin() + static_cast<std::ptrdiff_t>(std::min(layout.pinnedRows, layout.rows.size()));
    return static_cast<std::size_t>(std::partition_point(begin, layout.rows.end(), [y](const PanelRowGeometry& geometry) {
        return geometry.row.bottom() < y;
    }) - layout.rows.begin());
}

// Field is a TextField's box, which takes the caret on the press itself;
// a search box's clear button is Trailing, a click like any other.
// DropDown is a drop-down Choice's box, which opens its list. A band of an
// Equalizer row is a Slider with the band in `part` and its level, from the
// bottom up, in `fraction`.
enum class PanelHitKind { None, Outside, Sheet, Close, Toggle, Segment, Button, Slider, Trailing, Item, Tile, Scrollbar, Add, Tab, Switch, Field,
                          DropDown };

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
    // A switch is hit by its index in `part`, as a tab is.
    for (std::size_t i = 0; i < layout.sheetSwitches.size(); ++i) {
        if (layout.sheetSwitches[i].contains(px, py)) return {PanelHitKind::Switch, -1, static_cast<int>(i)};
    }
    // A tab is hit by its index in `part`; the column around the tabs is
    // the sheet.
    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        if (layout.tabs[i].contains(px, py)) return {PanelHitKind::Tab, -1, static_cast<int>(i)};
    }
    if (layout.tabColumn.contains(px, py)) return {PanelHitKind::Sheet};
    if (layout.scrollbar.visible() && layout.scrollbar.contains(px, py)) return {PanelHitKind::Scrollbar};
    // The pinned band holds the leading rows, the content the others.
    const bool pinned = layout.pinned.contains(px, py);
    if (!pinned && !layout.content.contains(px, py)) return {PanelHitKind::Sheet};
    // The content's rows follow each other down, so the search starts at the
    // first one reaching the point and stops at the first one below it.
    const std::size_t first = pinned ? 0 : panelFirstRowReaching(layout, py);
    const std::size_t last = pinned ? std::min(layout.pinnedRows, layout.rows.size()) : layout.rows.size();
    for (std::size_t i = first; i < last && i < rows.size(); ++i) {
        const auto& geometry = layout.rows[i];
        if (!pinned && geometry.row.y > py) break;
        if (!geometry.row.contains(px, py)) continue;
        const int index = static_cast<int>(i);
        if (!rows[i].enabled) return {PanelHitKind::Sheet};
        switch (rows[i].kind) {
        case PanelRowKind::Toggle:
            // The whole row flips a toggle, not just the switch.
            return {PanelHitKind::Toggle, index};
        case PanelRowKind::CompactItem:
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
        case PanelRowKind::Equalizer:
            for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
                if (geometry.parts[part].contains(px, py)) {
                    return {PanelHitKind::Slider, index, static_cast<int>(part),
                            verticalRailFraction(py, geometry.parts[part], railInset)};
                }
            }
            return {PanelHitKind::Sheet};
        case PanelRowKind::Choice:
            if (rows[i].dropDown) {
                return geometry.control.contains(px, py) ? PanelHit{PanelHitKind::DropDown, index}
                                                         : PanelHit{PanelHitKind::Sheet};
            }
            for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
                if (geometry.parts[part].contains(px, py)) {
                    return {PanelHitKind::Segment, index, static_cast<int>(part)};
                }
            }
            return {PanelHitKind::Sheet};
        case PanelRowKind::MediaDetail:
        case PanelRowKind::Buttons:
        case PanelRowKind::Shortcut:
            // A spin box's number is typed into, as a text box is.
            if (rows[i].spinBox && geometry.value.contains(px, py)) return {PanelHitKind::Field, index};
            for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
                if (geometry.parts[part].contains(px, py)) {
                    return {PanelHitKind::Button, index, static_cast<int>(part)};
                }
            }
            return {PanelHitKind::Sheet};
        case PanelRowKind::TextField:
            if (geometry.trailing.visible() && geometry.trailing.contains(px, py)) return {PanelHitKind::Trailing, index};
            if (geometry.control.contains(px, py)) return {PanelHitKind::Field, index};
            return {PanelHitKind::Sheet};
        default:
            return {PanelHitKind::Sheet};
        }
    }
    return {PanelHitKind::Sheet};
}

// ---------------------------------------------------------------------------
// The settings window

// What the window manager is told a point of the settings window is. Its
// sheet fills the window and has no Windows title bar over it: the sides
// and the bottom keep Windows' own resize frame, a band of `resizeBand`
// along the top edge sizes it (`corner` how far along that edge a corner
// reaches), and its caption strip and the header outside its close
// button move it, as a caption would.
inline WindowFrameHit settingsWindowFrameHitTest(float px, float py, float width, float resizeBand, float corner,
                                                 const PanelLayout& layout) {
    if (px < 0.0F || py < 0.0F || px >= width) return WindowFrameHit::Client;
    if (py < resizeBand) {
        if (px < corner) return WindowFrameHit::TopLeft;
        if (px >= width - corner) return WindowFrameHit::TopRight;
        return WindowFrameHit::Top;
    }
    if (layout.close.contains(px, py)) return WindowFrameHit::Client;
    if (!layout.caption.contains(px, py) && !layout.header.contains(px, py) && !layout.tabTitle.contains(px, py)) {
        return WindowFrameHit::Client;
    }
    return WindowFrameHit::Caption;
}

// Where the settings window first stands, in screen pixels: `width` by
// `height`, centred over the player's window (`player`), as a dialog of
// its owner's, but not above the bottom of `insideTop` (the player's
// caption strip, whose buttons it leaves uncovered where the player is tall
// enough). Kept to the work area, and no larger than it. Later it opens
// where it was left.
inline OverlayRect settingsWindowPlacement(const OverlayRect& player, const OverlayRect& workArea, float width,
                                           float height, float insideTop) {
    const float w = std::max(0.0F, std::min(width, workArea.width));
    const float h = std::max(0.0F, std::min(height, workArea.height));
    float x = player.x + (player.width - w) * 0.5F;
    float y = std::max(player.y + (player.height - h) * 0.5F, player.y + insideTop);
    x = std::clamp(x, workArea.x, std::max(workArea.x, workArea.right() - w));
    y = std::clamp(y, workArea.y, std::max(workArea.y, workArea.bottom() - h));
    return {x, y, w, h};
}

}  // namespace quaddeck
