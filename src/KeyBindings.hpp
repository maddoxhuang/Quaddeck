#pragma once

// The keyboard's shortcuts: what the player does from a key, the keys that
// do each by default, and the table of keys the viewer has chosen on the
// sheet's Keys tab. Pure: a key is a Windows virtual-key code, written out
// here rather than taken from <windows.h>.
// App makes a chord of each key press and looks it up here (AppKeys.cpp);
// settings.qconfig keeps the actions whose keys differ from these defaults
// (Session.hpp), so a later build's new defaults reach everything else.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quaddeck {

// The virtual-key codes the defaults and the names need.
inline constexpr unsigned kVkBack = 0x08;
inline constexpr unsigned kVkTab = 0x09;
inline constexpr unsigned kVkReturn = 0x0D;
inline constexpr unsigned kVkEscape = 0x1B;
inline constexpr unsigned kVkSpace = 0x20;
inline constexpr unsigned kVkPrior = 0x21;
inline constexpr unsigned kVkNext = 0x22;
inline constexpr unsigned kVkEnd = 0x23;
inline constexpr unsigned kVkHome = 0x24;
inline constexpr unsigned kVkLeft = 0x25;
inline constexpr unsigned kVkUp = 0x26;
inline constexpr unsigned kVkRight = 0x27;
inline constexpr unsigned kVkDown = 0x28;
inline constexpr unsigned kVkF1 = 0x70;
inline constexpr unsigned kVkF4 = 0x73;
inline constexpr unsigned kVkF5 = 0x74;
inline constexpr unsigned kVkF6 = 0x75;
inline constexpr unsigned kVkF24 = 0x87;
inline constexpr unsigned kVkOemComma = 0xBC;
inline constexpr unsigned kVkOemPeriod = 0xBE;
inline constexpr unsigned kVkOemSlash = 0xBF;
inline constexpr unsigned kVkOemOpenBracket = 0xDB;
inline constexpr unsigned kVkOemBackslash = 0xDC;
inline constexpr unsigned kVkOemCloseBracket = 0xDD;

// A chord's modifiers.
inline constexpr unsigned kKeyCtrl = 1U;
inline constexpr unsigned kKeyShift = 2U;
inline constexpr unsigned kKeyAlt = 4U;
inline constexpr unsigned kKeyModifiers = kKeyCtrl | kKeyShift | kKeyAlt;

// One key with the modifiers held for it; a zero key is no key.
struct KeyChord {
    unsigned key{};
    unsigned modifiers{};
    bool empty() const { return key == 0; }
    friend bool operator==(const KeyChord&, const KeyChord&) = default;
};

// In the order the Keys tab lists them, group by group. The order is the
// table's index and nothing else: the settings file names an action by its
// id (keyActionInfo), so actions can be added anywhere.
enum class KeyAction : std::uint8_t {
    PlayPause, Restart, SeekBack, SeekForward, SeekBackLong, SeekForwardLong, ResetOffsets,
    LoopA, LoopB, LoopToggle,
    VolumeUp, VolumeDown, Mute, AudioV1, AudioV2, AudioV3, AudioV4, AudioV5,
    AudioTrackNext, AudioEarlier, AudioLater, AudioDelayReset, Equalizer, Normalizer, NightMode,
    SwapChannels, VoiceRemoval, VoiceEmphasis, AudioOutputNext,
    OpenFiles, PreviousFile, NextFile, List, Emby, EmbySearch,
    SortName, SortSize, SortDate, SortLength, SortRandom, SortReleased, SortPlayed, SortUpdated,
    OpenSession, SaveSession,
    SubtitlesToggle, SubtitleNext, SubtitleLoad, SubtitleEarlier, SubtitleLater, SubtitleTimingReset,
    SubtitleBigger, SubtitleSmaller, SubtitleRaise, SubtitleLower, SubtitlePlaceReset,
    // In LayoutMode's order, as the Arrangement menu lists them.
    ArrangeAutomatic, ArrangeOneRow, ArrangeTwoRows, ArrangeOneColumn, ArrangePortrait, ArrangeLandscape,
    // One video alone (Solo), by its place on the screen; every video again.
    SoloV1, SoloV2, SoloV3, SoloV4, SoloV5, AllVideos,
    BrightnessDown, BrightnessUp, ContrastDown, ContrastUp, SaturationDown, SaturationUp, HueDown, HueUp,
    ColorReset,
    Fullscreen, Back, Settings, PinBar,
};
inline constexpr std::size_t kKeyActionCount = static_cast<std::size_t>(KeyAction::PinBar) + 1;
inline constexpr std::size_t kArrangementKeyCount = 6;
inline constexpr std::size_t kSoloKeyCount = 5;
// Each action takes up to two keys, as PotPlayer's and most players' do.
inline constexpr std::size_t kKeySlots = 2;

enum class KeyGroup : std::uint8_t { Playback, Audio, Files, Subtitles, Arrangement, Picture, Window };
inline constexpr std::size_t kKeyGroupCount = 7;

inline const wchar_t* keyGroupName(KeyGroup group) {
    switch (group) {
    case KeyGroup::Playback: return L"Playback";
    case KeyGroup::Audio: return L"Volume and audio";
    case KeyGroup::Files: return L"Files and lists";
    case KeyGroup::Subtitles: return L"Subtitles";
    case KeyGroup::Arrangement: return L"Arrangement";
    case KeyGroup::Picture: return L"Picture colour";
    case KeyGroup::Window: return L"Window";
    }
    return L"";
}

using KeySlots = std::array<KeyChord, kKeySlots>;

// `id` names the action in settings.qconfig and never changes. A step --
// a seek, the volume, the subtitles' timing, size and place -- repeats
// while its key is held; any other action happens once a press, so a held
// toggle does not flip back and forth.
struct KeyActionInfo {
    KeyAction action;
    const char* id;
    const wchar_t* label;
    KeyGroup group;
    bool repeats;
    KeySlots defaults;
};

namespace key_detail {

constexpr KeyChord plain(unsigned key) { return {key, 0}; }
constexpr KeyChord ctrl(unsigned key) { return {key, kKeyCtrl}; }
constexpr KeyChord alt(unsigned key) { return {key, kKeyAlt}; }
constexpr KeyChord shift(unsigned key) { return {key, kKeyShift}; }

inline constexpr std::array<KeyActionInfo, kKeyActionCount> kActions{{
    {KeyAction::PlayPause, "play-pause", L"Play / pause", KeyGroup::Playback, false, {plain(kVkSpace)}},
    {KeyAction::Restart, "restart", L"Back to the start", KeyGroup::Playback, false, {plain(kVkHome), plain('R')}},
    {KeyAction::SeekBack, "seek-back", L"Back 5 seconds", KeyGroup::Playback, true, {plain(kVkLeft)}},
    {KeyAction::SeekForward, "seek-forward", L"Forward 5 seconds", KeyGroup::Playback, true, {plain(kVkRight)}},
    {KeyAction::SeekBackLong, "seek-back-long", L"Back 30 seconds", KeyGroup::Playback, true, {ctrl(kVkLeft)}},
    {KeyAction::SeekForwardLong, "seek-forward-long", L"Forward 30 seconds", KeyGroup::Playback, true,
     {ctrl(kVkRight)}},
    {KeyAction::ResetOffsets, "reset-offsets", L"All offsets to zero", KeyGroup::Playback, false, {plain('0')}},
    {KeyAction::LoopA, "loop-a", L"Set loop A", KeyGroup::Playback, false, {plain(kVkOemOpenBracket)}},
    {KeyAction::LoopB, "loop-b", L"Set loop B", KeyGroup::Playback, false, {plain(kVkOemCloseBracket)}},
    {KeyAction::LoopToggle, "loop", L"A-B loop on / off", KeyGroup::Playback, false,
     {plain(kVkOemBackslash), plain('L')}},
    {KeyAction::VolumeUp, "volume-up", L"Volume up", KeyGroup::Audio, true, {plain(kVkUp)}},
    {KeyAction::VolumeDown, "volume-down", L"Volume down", KeyGroup::Audio, true, {plain(kVkDown)}},
    {KeyAction::Mute, "mute", L"Mute", KeyGroup::Audio, false, {plain('M')}},
    {KeyAction::AudioV1, "audio-1", L"V1's audio on / off", KeyGroup::Audio, false, {plain('1')}},
    {KeyAction::AudioV2, "audio-2", L"V2's audio on / off", KeyGroup::Audio, false, {plain('2')}},
    {KeyAction::AudioV3, "audio-3", L"V3's audio on / off", KeyGroup::Audio, false, {plain('3')}},
    {KeyAction::AudioV4, "audio-4", L"V4's audio on / off", KeyGroup::Audio, false, {plain('4')}},
    {KeyAction::AudioV5, "audio-5", L"V5's audio on / off", KeyGroup::Audio, false, {plain('5')}},
    // PotPlayer's audio keys: Alt+A the next track; Shift with , . / the
    // sound's timing, as the subtitles' are without it; Shift with E, N,
    // T, V and A the equalizer, the normalizer, the swapped channels, the
    // voice taken out and the voice brought forward. Shift+D dims the loud
    // parts at night; Ctrl+Shift+A moves to the next output.
    {KeyAction::AudioTrackNext, "audio-track-next", L"Next audio track", KeyGroup::Audio, false, {alt('A')}},
    {KeyAction::AudioEarlier, "audio-earlier", L"Sound earlier by 50 ms", KeyGroup::Audio, true,
     {shift(kVkOemComma)}},
    {KeyAction::AudioLater, "audio-later", L"Sound later by 50 ms", KeyGroup::Audio, true, {shift(kVkOemPeriod)}},
    {KeyAction::AudioDelayReset, "audio-delay-reset", L"Sound in step with the picture", KeyGroup::Audio, false,
     {shift(kVkOemSlash)}},
    {KeyAction::Equalizer, "equalizer", L"Equalizer on / off", KeyGroup::Audio, false, {shift('E')}},
    {KeyAction::Normalizer, "normalizer", L"Normalize loudness on / off", KeyGroup::Audio, false, {shift('N')}},
    {KeyAction::NightMode, "night-mode", L"Night mode: off, light, strong", KeyGroup::Audio, false,
     {shift('D')}},
    {KeyAction::SwapChannels, "swap-channels", L"Swap left and right", KeyGroup::Audio, false, {shift('T')}},
    {KeyAction::VoiceRemoval, "voice-removal", L"Voice removal on / off", KeyGroup::Audio, false, {shift('V')}},
    {KeyAction::VoiceEmphasis, "voice-emphasis", L"Voice emphasis on / off", KeyGroup::Audio, false,
     {shift('A')}},
    {KeyAction::AudioOutputNext, "audio-output-next", L"Next audio output", KeyGroup::Audio, false,
     {{'A', kKeyCtrl | kKeyShift}}},
    {KeyAction::OpenFiles, "open-files", L"Open files", KeyGroup::Files, false, {plain('O')}},
    {KeyAction::PreviousFile, "previous-file", L"Previous file", KeyGroup::Files, false, {plain(kVkPrior)}},
    {KeyAction::NextFile, "next-file", L"Next file", KeyGroup::Files, false, {plain(kVkNext)}},
    {KeyAction::List, "list", L"Folder list / Emby", KeyGroup::Files, false, {plain(kVkF6)}},
    {KeyAction::Emby, "emby", L"Emby browser", KeyGroup::Files, false, {ctrl('E')}},
    {KeyAction::EmbySearch, "emby-search", L"Search Emby", KeyGroup::Files, false, {ctrl('F')}},
    // PotPlayer's playlist sorts on its keys (Ctrl+5 is "by extension"
    // there, which a library has no use for).
    {KeyAction::SortName, "sort-name", L"Sort by name", KeyGroup::Files, false, {ctrl('4')}},
    {KeyAction::SortSize, "sort-size", L"Sort by size", KeyGroup::Files, false, {ctrl('6')}},
    {KeyAction::SortDate, "sort-date", L"Sort by date", KeyGroup::Files, false, {ctrl('7')}},
    {KeyAction::SortLength, "sort-length", L"Sort by length", KeyGroup::Files, false, {ctrl('8')}},
    {KeyAction::SortRandom, "sort-random", L"Random order", KeyGroup::Files, false, {ctrl('9')}},
    // Emby's other orders, on the Ctrl digits below PotPlayer's, in the
    // sort drop-down's order.
    {KeyAction::SortReleased, "sort-released", L"Sort by release date", KeyGroup::Files, false, {ctrl('1')}},
    {KeyAction::SortPlayed, "sort-played", L"Sort by last played", KeyGroup::Files, false, {ctrl('2')}},
    {KeyAction::SortUpdated, "sort-updated", L"Sort by last updated", KeyGroup::Files, false, {ctrl('3')}},
    {KeyAction::OpenSession, "open-session", L"Open session", KeyGroup::Files, false, {ctrl('O')}},
    {KeyAction::SaveSession, "save-session", L"Save session", KeyGroup::Files, false, {ctrl('S')}},
    // PotPlayer's subtitle keys: Alt with H, L, O, the size and the place;
    // . shows the lines earlier, , later, / as their file says.
    {KeyAction::SubtitlesToggle, "subtitles", L"Subtitles on / off", KeyGroup::Subtitles, false, {alt('H')}},
    {KeyAction::SubtitleNext, "subtitle-next", L"Next subtitle", KeyGroup::Subtitles, false, {alt('L')}},
    {KeyAction::SubtitleLoad, "subtitle-load", L"Load subtitle file", KeyGroup::Subtitles, false, {alt('O')}},
    {KeyAction::SubtitleEarlier, "subtitle-earlier", L"Earlier by 0.5 s", KeyGroup::Subtitles, true,
     {plain(kVkOemPeriod)}},
    {KeyAction::SubtitleLater, "subtitle-later", L"Later by 0.5 s", KeyGroup::Subtitles, true,
     {plain(kVkOemComma)}},
    {KeyAction::SubtitleTimingReset, "subtitle-timing-reset", L"Timing as in the file", KeyGroup::Subtitles, false,
     {plain(kVkOemSlash)}},
    {KeyAction::SubtitleBigger, "subtitle-bigger", L"Larger subtitles", KeyGroup::Subtitles, true, {alt(kVkPrior)}},
    {KeyAction::SubtitleSmaller, "subtitle-smaller", L"Smaller subtitles", KeyGroup::Subtitles, true,
     {alt(kVkNext)}},
    {KeyAction::SubtitleRaise, "subtitle-raise", L"Raise subtitles", KeyGroup::Subtitles, true, {alt(kVkUp)}},
    {KeyAction::SubtitleLower, "subtitle-lower", L"Lower subtitles", KeyGroup::Subtitles, true, {alt(kVkDown)}},
    {KeyAction::SubtitlePlaceReset, "subtitle-place-reset", L"Subtitles in place", KeyGroup::Subtitles, false,
     {alt(kVkHome)}},
    // Named as the Arrangement menu names them; Alt with the menu's place.
    {KeyAction::ArrangeAutomatic, "arrange-automatic", L"Automatic", KeyGroup::Arrangement, false, {alt('1')}},
    {KeyAction::ArrangeOneRow, "arrange-one-row", L"One row", KeyGroup::Arrangement, false, {alt('2')}},
    {KeyAction::ArrangeTwoRows, "arrange-two-rows", L"Two rows", KeyGroup::Arrangement, false, {alt('3')}},
    {KeyAction::ArrangeOneColumn, "arrange-one-column", L"One column", KeyGroup::Arrangement, false, {alt('4')}},
    {KeyAction::ArrangePortrait, "arrange-portrait-focus", L"Portrait focus", KeyGroup::Arrangement, false,
     {alt('5')}},
    {KeyAction::ArrangeLandscape, "arrange-landscape-focus", L"Landscape focus", KeyGroup::Arrangement, false,
     {alt('6')}},
    // Shift with the video's number -- the number alone is its sound --
    // shows it alone, from the grid or from another video alone, and again
    // shows them all, as its Solo chip does; Shift+0 shows them all.
    {KeyAction::SoloV1, "solo-1", L"Solo V1 on / off", KeyGroup::Arrangement, false, {shift('1')}},
    {KeyAction::SoloV2, "solo-2", L"Solo V2 on / off", KeyGroup::Arrangement, false, {shift('2')}},
    {KeyAction::SoloV3, "solo-3", L"Solo V3 on / off", KeyGroup::Arrangement, false, {shift('3')}},
    {KeyAction::SoloV4, "solo-4", L"Solo V4 on / off", KeyGroup::Arrangement, false, {shift('4')}},
    {KeyAction::SoloV5, "solo-5", L"Solo V5 on / off", KeyGroup::Arrangement, false, {shift('5')}},
    {KeyAction::AllVideos, "all-videos", L"All videos", KeyGroup::Arrangement, false, {shift('0')}},
    // PotPlayer's colour keys: Q sets every control back, W and E the
    // brightness, R and T the contrast, Y and U the saturation, I and O the
    // hue. Its R, U and O are Restart's, the bar's and Open's here, so
    // those three take Shift.
    {KeyAction::BrightnessDown, "brightness-down", L"Brightness down", KeyGroup::Picture, true, {plain('W')}},
    {KeyAction::BrightnessUp, "brightness-up", L"Brightness up", KeyGroup::Picture, true, {plain('E')}},
    {KeyAction::ContrastDown, "contrast-down", L"Contrast down", KeyGroup::Picture, true, {shift('R')}},
    {KeyAction::ContrastUp, "contrast-up", L"Contrast up", KeyGroup::Picture, true, {plain('T')}},
    {KeyAction::SaturationDown, "saturation-down", L"Saturation down", KeyGroup::Picture, true,
     {plain('Y')}},
    {KeyAction::SaturationUp, "saturation-up", L"Saturation up", KeyGroup::Picture, true, {shift('U')}},
    {KeyAction::HueDown, "hue-down", L"Hue down", KeyGroup::Picture, true, {plain('I')}},
    {KeyAction::HueUp, "hue-up", L"Hue up", KeyGroup::Picture, true, {shift('O')}},
    {KeyAction::ColorReset, "color-reset", L"Colour controls back to 0", KeyGroup::Picture, false, {plain('Q')}},
    {KeyAction::Fullscreen, "fullscreen", L"Fullscreen", KeyGroup::Window, false, {alt(kVkReturn)}},
    {KeyAction::Back, "back", L"Close / leave Solo", KeyGroup::Window, false, {plain(kVkEscape)}},
    {KeyAction::Settings, "settings", L"Settings", KeyGroup::Window, false, {plain(kVkF5)}},
    {KeyAction::PinBar, "pin-bar", L"Keep the bar visible", KeyGroup::Window, false, {plain('U')}},
}};

constexpr bool tableInOrder() {
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        if (static_cast<std::size_t>(kActions[i].action) != i) return false;
        if (i > 0 && kActions[i].group < kActions[i - 1].group) return false;
    }
    return true;
}
static_assert(tableInOrder(), "The shortcut table is indexed by KeyAction and listed group by group");

}  // namespace key_detail

inline const KeyActionInfo& keyActionInfo(KeyAction action) {
    return key_detail::kActions[std::min(static_cast<std::size_t>(action), kKeyActionCount - 1)];
}

// The audio switch of the video at a place on the screen (0 for V1).
inline KeyAction audioKeyAction(std::size_t position) {
    return static_cast<KeyAction>(static_cast<std::size_t>(KeyAction::AudioV1) + std::min<std::size_t>(position, 4));
}

// The key of an arrangement, by LayoutMode's value, and back: the
// arrangement an action chooses, or none for any other action.
inline KeyAction arrangementKeyAction(std::size_t mode) {
    return static_cast<KeyAction>(static_cast<std::size_t>(KeyAction::ArrangeAutomatic) +
                                  std::min(mode, kArrangementKeyCount - 1));
}

inline std::optional<std::size_t> keyActionArrangement(KeyAction action) {
    const auto index = static_cast<std::size_t>(action);
    const auto first = static_cast<std::size_t>(KeyAction::ArrangeAutomatic);
    if (index < first || index - first >= kArrangementKeyCount) return std::nullopt;
    return index - first;
}

// The key that shows the video at a place on the screen alone (0 for V1),
// and back: the place an action shows alone, or none for any other action.
inline KeyAction soloKeyAction(std::size_t position) {
    return static_cast<KeyAction>(static_cast<std::size_t>(KeyAction::SoloV1) +
                                  std::min(position, kSoloKeyCount - 1));
}

inline std::optional<std::size_t> keyActionSoloPosition(KeyAction action) {
    const auto index = static_cast<std::size_t>(action);
    const auto first = static_cast<std::size_t>(KeyAction::SoloV1);
    if (index < first || index - first >= kSoloKeyCount) return std::nullopt;
    return index - first;
}

inline std::optional<KeyAction> keyActionNamed(std::string_view id) {
    for (const auto& info : key_detail::kActions) {
        if (id == info.id) return info.action;
    }
    return std::nullopt;
}

// Every action's keys, by KeyAction; within an action the bound keys come
// first (compactKeySlots), so a lone key is always the first.
using KeyBindings = std::array<KeySlots, kKeyActionCount>;

inline KeyBindings defaultKeyBindings() {
    KeyBindings bindings{};
    for (const auto& info : key_detail::kActions) bindings[static_cast<std::size_t>(info.action)] = info.defaults;
    return bindings;
}

inline void compactKeySlots(KeySlots& slots) {
    std::stable_partition(slots.begin(), slots.end(), [](const KeyChord& chord) { return !chord.empty(); });
}

inline bool keyFunctionKey(unsigned key) { return key >= kVkF1 && key <= kVkF24; }

// A key a shortcut can be made of. Not a modifier on its own, a lock, a
// mouse button, an input method's key, Print Screen, Sleep, nor one of the
// keyboard's media, volume and browser keys: Windows makes application
// commands of those, and the player answers the commands themselves. Nor a
// chord Windows keeps for itself: Alt+F4, Alt+Tab, Alt+Esc, Alt+Space,
// Ctrl+Esc and Ctrl+Shift+Esc.
inline bool keyChordUsable(KeyChord chord) {
    const unsigned key = chord.key;
    if (key == 0 || key > 0xFE || (chord.modifiers & ~kKeyModifiers) != 0) return false;
    if (key <= 0x06) return false;                                         // mouse buttons, Ctrl+Break
    if (key >= 0x10 && key <= 0x12) return false;                          // Shift, Ctrl, Alt
    if (key == 0x14 || key == 0x90) return false;                          // Caps Lock, Num Lock
    if ((key >= 0x15 && key <= 0x1A) || (key >= 0x1C && key <= 0x1F)) return false;  // input methods
    if (key == 0x2C || key == 0x5B || key == 0x5C || key == 0x5F) return false;      // Print Screen, Win, Sleep
    if (key >= 0xA0 && key <= 0xB7) return false;  // left/right modifiers; browser, volume, media, launch
    if (key == 0xE5 || key == 0xE7) return false;  // an input method's key, a typed character
    const unsigned mods = chord.modifiers;
    if (mods == kKeyAlt && (key == kVkF4 || key == kVkTab || key == kVkEscape || key == kVkSpace)) return false;
    if (mods == (kKeyAlt | kKeyShift) && key == kVkTab) return false;
    if ((mods == kKeyCtrl || mods == (kKeyCtrl | kKeyShift)) && key == kVkEscape) return false;
    return true;
}

// The action holding exactly this chord, and in which of its slots.
inline std::optional<std::pair<KeyAction, std::size_t>> keyChordOwner(const KeyBindings& bindings, KeyChord chord) {
    if (chord.empty()) return std::nullopt;
    for (std::size_t action = 0; action < bindings.size(); ++action) {
        for (std::size_t slot = 0; slot < kKeySlots; ++slot) {
            if (bindings[action][slot] == chord) return std::pair{static_cast<KeyAction>(action), slot};
        }
    }
    return std::nullopt;
}

// What a key press does: the action holding its chord or, with Shift held
// and that chord unbound, the action of the same chord without Shift -- as
// a shortcut always did before keys could be chosen, when Shift was never
// looked at. Ctrl and Alt are part of the chord.
inline std::optional<KeyAction> keyActionFor(const KeyBindings& bindings, KeyChord chord) {
    if (const auto owner = keyChordOwner(bindings, chord)) return owner->first;
    if ((chord.modifiers & kKeyShift) == 0) return std::nullopt;
    chord.modifiers &= ~kKeyShift;
    if (const auto owner = keyChordOwner(bindings, chord)) return owner->first;
    return std::nullopt;
}

// A text box, while it is typed into, has every key before the shortcuts:
// a letter, a space or an arrow is text there. Only a function key that
// opens or closes the settings or the list goes past it.
inline bool keyChordLeavesTextBox(const KeyBindings& bindings, KeyChord chord) {
    if (!keyFunctionKey(chord.key)) return false;
    const auto action = keyActionFor(bindings, chord);
    return action == KeyAction::Settings || action == KeyAction::List;
}

// Puts `chord` in `slot` of `action`, in place of the key there, taking it
// from wherever it was -- one chord does one thing -- and answers the
// action it was taken from when that was another one. The action's keys
// are compacted afterwards. A chord that cannot be a shortcut changes
// nothing.
inline std::optional<KeyAction> bindKey(KeyBindings& bindings, KeyAction action, std::size_t slot, KeyChord chord) {
    const auto index = static_cast<std::size_t>(action);
    if (index >= bindings.size() || slot >= kKeySlots || !keyChordUsable(chord)) return std::nullopt;
    std::optional<KeyAction> taken;
    if (const auto owner = keyChordOwner(bindings, chord)) {
        auto& slots = bindings[static_cast<std::size_t>(owner->first)];
        slots[owner->second] = {};
        if (owner->first != action) {
            compactKeySlots(slots);
            taken = owner->first;
        }
    }
    bindings[index][slot] = chord;
    compactKeySlots(bindings[index]);
    return taken;
}

// Gives an action its default keys back, taking each from the action that
// has it now, and answers those actions. The way back to a key the waiting
// box cannot take, such as Esc.
inline std::vector<KeyAction> restoreDefaultKeys(KeyBindings& bindings, KeyAction action) {
    const auto index = static_cast<std::size_t>(action);
    std::vector<KeyAction> taken;
    if (index >= bindings.size()) return taken;
    bindings[index] = {};
    for (const KeyChord& chord : keyActionInfo(action).defaults) {
        const auto owner = keyChordOwner(bindings, chord);
        if (!owner) continue;
        auto& slots = bindings[static_cast<std::size_t>(owner->first)];
        slots[owner->second] = {};
        compactKeySlots(slots);
        if (std::find(taken.begin(), taken.end(), owner->first) == taken.end()) taken.push_back(owner->first);
    }
    bindings[index] = keyActionInfo(action).defaults;
    return taken;
}

inline void clearKey(KeyBindings& bindings, KeyAction action, std::size_t slot) {
    const auto index = static_cast<std::size_t>(action);
    if (index >= bindings.size() || slot >= kKeySlots) return;
    bindings[index][slot] = {};
    compactKeySlots(bindings[index]);
}

inline bool keyActionCustomized(const KeyBindings& bindings, KeyAction action) {
    const auto index = static_cast<std::size_t>(action);
    return index < bindings.size() && bindings[index] != keyActionInfo(action).defaults;
}

inline bool keyBindingsCustomized(const KeyBindings& bindings) {
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (keyActionCustomized(bindings, static_cast<KeyAction>(index))) return true;
    }
    return false;
}

// The defaults with the viewer's own keys for the actions in `chosen`; an
// action left out keeps its defaults, but not a default key that one of the
// chosen actions has: the viewer's choice wins over a newer build's default.
// Answers nothing for keys that are not a table: an unusable chord, or one
// chord in two places.
inline std::optional<KeyBindings> keyBindingsFrom(
    const std::array<std::optional<KeySlots>, kKeyActionCount>& chosen) {
    KeyBindings bindings = defaultKeyBindings();
    std::array<KeyChord, kKeyActionCount * kKeySlots> seen{};
    std::size_t seenCount = 0;
    for (std::size_t action = 0; action < chosen.size(); ++action) {
        if (!chosen[action]) continue;
        for (const KeyChord& chord : *chosen[action]) {
            if (chord.empty()) {
                if (chord.modifiers != 0) return std::nullopt;
                continue;
            }
            if (!keyChordUsable(chord) ||
                std::find(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(seenCount), chord) !=
                    seen.begin() + static_cast<std::ptrdiff_t>(seenCount)) {
                return std::nullopt;
            }
            seen[seenCount++] = chord;
        }
        bindings[action] = *chosen[action];
        compactKeySlots(bindings[action]);
    }
    for (std::size_t action = 0; action < chosen.size(); ++action) {
        if (chosen[action]) continue;
        for (KeyChord& chord : bindings[action]) {
            if (std::find(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(seenCount), chord) !=
                seen.begin() + static_cast<std::ptrdiff_t>(seenCount)) {
                chord = {};
            }
        }
        compactKeySlots(bindings[action]);
    }
    return bindings;
}

// A key's name as a shortcut shows it: the letter or digit, F5, Space,
// Page Up, the punctuation of a US keyboard's key, Num 5 on the keypad.
inline std::wstring keyName(unsigned key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, static_cast<wchar_t>(key));
    if (keyFunctionKey(key)) return L"F" + std::to_wstring(key - kVkF1 + 1);
    if (key >= 0x60 && key <= 0x69) return L"Num " + std::to_wstring(key - 0x60);
    switch (key) {
    case kVkBack: return L"Backspace";
    case kVkTab: return L"Tab";
    case 0x0C: return L"Clear";
    case kVkReturn: return L"Enter";
    case 0x13: return L"Pause";
    case kVkEscape: return L"Esc";
    case kVkSpace: return L"Space";
    case kVkPrior: return L"Page Up";
    case kVkNext: return L"Page Down";
    case kVkEnd: return L"End";
    case kVkHome: return L"Home";
    case kVkLeft: return L"Left";
    case kVkUp: return L"Up";
    case kVkRight: return L"Right";
    case kVkDown: return L"Down";
    case 0x2D: return L"Insert";
    case 0x2E: return L"Delete";
    case 0x2F: return L"Help";
    case 0x5D: return L"Menu";
    case 0x6A: return L"Num *";
    case 0x6B: return L"Num +";
    case 0x6C: return L"Num ,";
    case 0x6D: return L"Num -";
    case 0x6E: return L"Num .";
    case 0x6F: return L"Num /";
    case 0x91: return L"Scroll Lock";
    case 0xBA: return L";";
    case 0xBB: return L"=";
    case kVkOemComma: return L",";
    case 0xBD: return L"-";
    case kVkOemPeriod: return L".";
    case kVkOemSlash: return L"/";
    case 0xC0: return L"`";
    case kVkOemOpenBracket: return L"[";
    case kVkOemBackslash: return L"\\";
    case kVkOemCloseBracket: return L"]";
    case 0xDE: return L"'";
    case 0xE2: return L"102nd key";
    default: return L"Key " + std::to_wstring(key);
    }
}

inline std::wstring keyChordName(KeyChord chord) {
    if (chord.empty()) return {};
    std::wstring name;
    if (chord.modifiers & kKeyCtrl) name += L"Ctrl+";
    if (chord.modifiers & kKeyAlt) name += L"Alt+";
    if (chord.modifiers & kKeyShift) name += L"Shift+";
    return name + keyName(chord.key);
}

// The first key of an action, as a menu line or a tooltip names it; empty
// while the action has none.
inline std::wstring keyActionShortcut(const KeyBindings& bindings, KeyAction action) {
    const auto index = static_cast<std::size_t>(action);
    return index < bindings.size() ? keyChordName(bindings[index][0]) : std::wstring{};
}

}  // namespace quaddeck
