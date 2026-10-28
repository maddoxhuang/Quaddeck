// App: the keyboard. Turns a key press into a chord, looks it up in the
// viewer's keys (KeyBindings.hpp) and does the action; fills the sheet's
// Keys tab, where a key box waits for the next key press to become its key.

#include "App.hpp"
#include "AppInternal.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace quaddeck {

using namespace app_internal;

namespace {

// The list orders on keys, both ways round: a key does the order, and the
// sort drop-down names the key beside the order.
constexpr std::array<std::pair<KeyAction, emby::SortKey>, emby::kSortKeyCount> kSortKeys{{
    {KeyAction::SortName, emby::SortKey::Name},
    {KeyAction::SortSize, emby::SortKey::Size},
    {KeyAction::SortDate, emby::SortKey::DateAdded},
    {KeyAction::SortLength, emby::SortKey::Runtime},
    {KeyAction::SortRandom, emby::SortKey::Random},
    {KeyAction::SortReleased, emby::SortKey::Release},
    {KeyAction::SortPlayed, emby::SortKey::LastPlayed},
    {KeyAction::SortUpdated, emby::SortKey::Updated},
}};

}  // namespace

KeyChord App::keyChordOf(WPARAM key, bool alt) const {
    unsigned modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= kKeyCtrl;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= kKeyShift;
    if (alt) modifiers |= kKeyAlt;
    return {static_cast<unsigned>(key), modifiers};
}

bool App::runShortcut(WPARAM key, LPARAM lParam, bool alt) {
    const auto action = keyActionFor(keyBindings_, keyChordOf(key, alt));
    if (!action) return false;
    // A held key repeats, and only the steps -- seeking, the volume, the
    // subtitles' timing, size and place -- are meant to: a held toggle would
    // flip back and forth, a held Page Down open one file after another.
    // Bit 30 says the key was already down.
    if ((lParam & (1LL << 30)) && !keyActionInfo(*action).repeats) return true;
    runKeyAction(*action);
    return true;
}

void App::runKeyAction(KeyAction action) {
    switch (action) {
    case KeyAction::PlayPause: togglePlayback(); break;
    case KeyAction::Restart:
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
    case KeyAction::SeekBack: seekRelative(-5.0); break;
    case KeyAction::SeekForward: seekRelative(5.0); break;
    case KeyAction::SeekBackLong: seekRelative(-30.0); break;
    case KeyAction::SeekForwardLong: seekRelative(30.0); break;
    case KeyAction::ResetOffsets: resetAllOffsets(); break;
    case KeyAction::LoopA: setMasterLoopA(clock_.position()); break;
    case KeyAction::LoopB: setMasterLoopB(clock_.position()); break;
    case KeyAction::LoopToggle: toggleMasterLoop(); break;
    // PotPlayer's volume keys, a wheel notch a press.
    case KeyAction::VolumeUp: stepVolume(1); break;
    case KeyAction::VolumeDown: stepVolume(-1); break;
    case KeyAction::Mute: toggleMute(); break;
    case KeyAction::AudioV1:
    case KeyAction::AudioV2:
    case KeyAction::AudioV3:
    case KeyAction::AudioV4:
    case KeyAction::AudioV5:
        toggleAudioPane(paneForPosition(static_cast<std::size_t>(action) - static_cast<std::size_t>(KeyAction::AudioV1)));
        break;
    case KeyAction::AudioTrackNext:
    case KeyAction::AudioEarlier:
    case KeyAction::AudioLater:
    case KeyAction::AudioDelayReset:
    case KeyAction::Equalizer:
    case KeyAction::Normalizer:
    case KeyAction::NightMode:
    case KeyAction::SwapChannels:
    case KeyAction::VoiceRemoval:
    case KeyAction::VoiceEmphasis:
    case KeyAction::AudioOutputNext:
        handleAudioKey(action);
        break;
    case KeyAction::OpenFiles: openFilesDialog(); break;
    case KeyAction::PreviousFile: openAdjacentFile(-1); break;
    case KeyAction::NextFile: openAdjacentFile(1); break;
    case KeyAction::List: toggleEmbyBrowser(); break;
    case KeyAction::Emby: embyOpenBrowser(); break;
    case KeyAction::EmbySearch:
        // The search box, from any page of the Emby browser.
        if (settingsOpen_ && embyBrowserOpen_ && browserSource_ == BrowserSource::Emby) embyOpenSearch();
        break;
    case KeyAction::SortName:
    case KeyAction::SortSize:
    case KeyAction::SortDate:
    case KeyAction::SortLength:
    case KeyAction::SortRandom:
    case KeyAction::SortReleased:
    case KeyAction::SortPlayed:
    case KeyAction::SortUpdated:
        for (const auto& [key, sort] : kSortKeys) {
            if (key == action) sortListBy(static_cast<int>(sort));
        }
        break;
    case KeyAction::OpenSession: loadSessionDialog(); break;
    case KeyAction::SaveSession: saveSessionDialog(); break;
    case KeyAction::SubtitlesToggle: toggleSubtitles(subtitlePane()); break;
    case KeyAction::SubtitleNext:
    case KeyAction::SubtitleLoad:
    case KeyAction::SubtitleEarlier:
    case KeyAction::SubtitleLater:
    case KeyAction::SubtitleTimingReset: {
        // The video the pointer was last on, or the only one.
        const int pane = subtitlePane();
        if (pane < 0) break;
        const auto index = static_cast<std::size_t>(pane);
        if (action == KeyAction::SubtitleNext) cycleSubtitle(index);
        else if (action == KeyAction::SubtitleLoad) loadSubtitleDialog(index);
        else if (action == KeyAction::SubtitleEarlier) nudgeSubtitleDelay(index, -0.5);
        else if (action == KeyAction::SubtitleLater) nudgeSubtitleDelay(index, 0.5);
        else resetSubtitleDelay(index);
        break;
    }
    case KeyAction::SubtitleBigger: setSubtitleSize(subtitleSettings_.size + 0.1F); break;
    case KeyAction::SubtitleSmaller: setSubtitleSize(subtitleSettings_.size - 0.1F); break;
    case KeyAction::SubtitleRaise: setSubtitlePosition(subtitleSettings_.position + 0.02F); break;
    case KeyAction::SubtitleLower: setSubtitlePosition(subtitleSettings_.position - 0.02F); break;
    case KeyAction::SubtitlePlaceReset: setSubtitlePosition(SubtitleSettings{}.position); break;
    // As the Arrangement menu and the Picture tab choose it, with one video
    // too: the choice is kept for the next set.
    case KeyAction::ArrangeAutomatic:
    case KeyAction::ArrangeOneRow:
    case KeyAction::ArrangeTwoRows:
    case KeyAction::ArrangeOneColumn:
    case KeyAction::ArrangePortrait:
    case KeyAction::ArrangeLandscape:
        chooseLayout(static_cast<LayoutMode>(*keyActionArrangement(action)));
        break;
    // As the bar's buttons for the videos choose it.
    case KeyAction::SoloV1:
    case KeyAction::SoloV2:
    case KeyAction::SoloV3:
    case KeyAction::SoloV4:
    case KeyAction::SoloV5:
        soloVideoAt(*keyActionSoloPosition(action));
        break;
    case KeyAction::AllVideos: showAllVideos(); break;
    // The colour controls, as the Video tab sets them (AppVideo.cpp).
    case KeyAction::BrightnessDown:
    case KeyAction::BrightnessUp:
    case KeyAction::ContrastDown:
    case KeyAction::ContrastUp:
    case KeyAction::SaturationDown:
    case KeyAction::SaturationUp:
    case KeyAction::HueDown:
    case KeyAction::HueUp:
    case KeyAction::ColorReset:
        handleVideoKey(action);
        break;
    case KeyAction::Fullscreen: toggleFullscreen(); break;
    case KeyAction::Back:
        if (settingsOpen_) closeSettingsPanel();
        else if (soloPane_ >= 0) { soloPane_ = -1; layoutHoverControls(); }
        else if (fullscreen_) toggleFullscreen();
        break;
    case KeyAction::Settings: toggleSettingsPanel(); break;
    case KeyAction::PinBar: toggleControls(); break;
    }
}

// ---------------------------------------------------------------------------
// The Keys tab

bool App::keysTabShown() const {
    return settingsWindow_.open && settingsTab_ == SettingsTab::Keys;
}

bool App::capturingKey() const { return keyCapture_ && keysTabShown() && !panelMenu_; }

bool App::captureKey(WPARAM key, LPARAM lParam, bool alt) {
    // Held: down since before the box waited, or still down from the press
    // it took.
    if (lParam & (1LL << 30)) return true;
    // A modifier on its own: the key it goes with is still to come.
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN ||
        (key >= VK_LSHIFT && key <= VK_RMENU)) {
        return true;
    }
    const KeyChord chord = keyChordOf(key, alt);
    if (chord == KeyChord{kVkEscape, 0}) {
        keyCapture_.reset();
        return true;
    }
    const std::wstring name = keyChordName(chord);
    if (!keyChordUsable(chord)) {
        // Windows' own chords and keys a shortcut cannot be made of; the
        // box goes on waiting.
        showNotice(name + L" cannot be a shortcut");
        return false;
    }
    const KeyCapture capture = *keyCapture_;
    keyCapture_.reset();
    const auto taken = bindKey(keyBindings_, capture.action, capture.slot, chord);
    std::wstring notice = name + L":  " + keyActionInfo(capture.action).label;
    if (taken) notice += std::wstring(L"  \x00B7  no longer ") + keyActionInfo(*taken).label;
    showNotice(notice);
    scheduleAppSettingsSave();
    return true;
}

void App::appendKeyRows(std::vector<PanelRow>& rows) const {
    const std::optional<KeyCapture> waiting = capturingKey() ? keyCapture_ : std::nullopt;
    // Over the list, staying put while it scrolls: what to do now, and the
    // buttons that go with it.
    PanelRow note;
    note.kind = PanelRowKind::Note;
    note.wrapNote = true;
    note.pinned = true;
    if (waiting) {
        note.label = std::wstring(L"Press the new key for \x201C") + keyActionInfo(waiting->action).label +
                     L"\x201D, with Ctrl, Alt or Shift if you like. Esc cancels.";
    } else {
        note.label = L"Click a key box, then press the key for it. A key another action had is moved, "
                     L"not shared.";
    }
    rows.push_back(std::move(note));
    PanelRow commands;
    commands.kind = PanelRowKind::Buttons;
    commands.id = SettingId::ShortcutCommand;
    commands.pinned = true;
    if (waiting) {
        commands.options = {L"Clear this key", L"Default keys", L"Cancel"};
    } else {
        commands.options = {L"Reset all keys to the defaults"};
        commands.enabled = keyBindingsCustomized(keyBindings_);
    }
    rows.push_back(std::move(commands));

    std::optional<KeyGroup> group;
    for (std::size_t index = 0; index < kKeyActionCount; ++index) {
        const auto action = static_cast<KeyAction>(index);
        const KeyActionInfo& info = keyActionInfo(action);
        if (group != info.group) {
            group = info.group;
            PanelRow header;
            header.kind = PanelRowKind::Header;
            header.label = keyGroupName(info.group);
            rows.push_back(std::move(header));
        }
        PanelRow row;
        row.kind = PanelRowKind::Shortcut;
        row.id = SettingId::ShortcutKey;
        row.param = static_cast<int>(index);
        row.label = info.label;
        for (const KeyChord& chord : keyBindings_[index]) row.options.push_back(keyChordName(chord));
        row.selected = waiting && waiting->action == action ? static_cast<int>(waiting->slot) : -1;
        row.on = keyActionCustomized(keyBindings_, action);
        std::wstring defaults;
        for (const KeyChord& chord : info.defaults) {
            if (chord.empty()) continue;
            defaults += (defaults.empty() ? L"" : L", ") + keyChordName(chord);
        }
        row.hint = std::wstring(info.label) + L"  \x00B7  default " + (defaults.empty() ? L"none" : defaults);
        if (action == KeyAction::Back) row.hint += L"  \x00B7  closes the sheet, else leaves Solo, else fullscreen; in the settings window, closes it";
        rows.push_back(std::move(row));
    }
}

void App::keyBoxClicked(const PanelRow& row, int part, std::optional<KeyCapture> ended) {
    if (row.param < 0 || static_cast<std::size_t>(row.param) >= kKeyActionCount || part < 0 ||
        static_cast<std::size_t>(part) >= kKeySlots) {
        return;
    }
    const KeyCapture box{static_cast<KeyAction>(row.param), static_cast<std::size_t>(part)};
    // The box that was waiting stops; any other starts.
    if (ended && ended->action == box.action && ended->slot == box.slot) return;
    keyCapture_ = box;
}

void App::keyCommand(int part, std::optional<KeyCapture> ended) {
    if (ended) {
        // Clear this key, or the action's default keys back; Cancel is the
        // end of the wait, which the click already was.
        const std::wstring label = keyActionInfo(ended->action).label;
        if (part == 0) {
            clearKey(keyBindings_, ended->action, ended->slot);
            showNotice(label + L":  key cleared");
        } else if (part == 1) {
            std::wstring notice = label + L":  default keys";
            for (const KeyAction taken : restoreDefaultKeys(keyBindings_, ended->action)) {
                notice += std::wstring(L"  \x00B7  no longer ") + keyActionInfo(taken).label;
            }
            showNotice(notice);
        }
        return;
    }
    if (part == 0 && keyBindingsCustomized(keyBindings_)) {
        keyBindings_ = defaultKeyBindings();
        showNotice(L"Every key is back to its default");
    }
}

// ---------------------------------------------------------------------------
// Keys named in menus, tooltips and the empty window

std::wstring App::shortcut(KeyAction action) const { return keyActionShortcut(keyBindings_, action); }

std::wstring App::withShortcut(std::wstring text, KeyAction action) const {
    const std::wstring key = shortcut(action);
    if (!key.empty()) text += L"  (" + key + L")";
    return text;
}

std::wstring App::sortShortcut(emby::SortKey key) const {
    for (const auto& [action, sort] : kSortKeys) {
        if (sort == key) return shortcut(action);
    }
    return {};
}

std::wstring App::emptyHintKeys() const {
    std::wstring line;
    const auto part = [&](const std::wstring& key, const wchar_t* what) {
        if (key.empty()) return;
        if (!line.empty()) line += L"      ";
        line += key + L"  " + what;
    };
    part(shortcut(KeyAction::OpenFiles), L"open");
    part(L"right-click", L"menu");
    part(shortcut(KeyAction::Settings), L"settings");
    part(shortcut(KeyAction::List), L"Emby");
    return line;
}

}  // namespace quaddeck
