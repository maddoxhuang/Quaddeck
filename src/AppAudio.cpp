// App: the sound's own settings. Which output it plays on, following
// Windows' default or one the user chose, and what happens when outputs come
// and go; the speakers it is mixed for; how loud the master volume may go;
// its timing against the picture, for every video and for one; the speed
// that keeps its pitch; the language a video starts in; and the effects the
// mix goes through -- the equalizer and its presets, the normalizer, night
// mode, the channels, the voice, the crossfeed and the widening. Menus,
// keys and the Audio tab all act through here.
//
// The output is changed by taking every voice down and bringing new ones up
// (AudioOutput::reopen): each pane's sound is then sought again where its
// picture is, as an audio-track change is.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "TextEncoding.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace quaddeck {

using namespace app_internal;

namespace {

// Outputs come and go in bursts -- a headset adds and removes several
// endpoints at once -- so the list is read once they have settled.
constexpr auto kAudioDeviceSettle = std::chrono::milliseconds(250);
// An engine that stopped on an error is brought back after this, and tried
// again as long as it will not open while outputs keep changing.
constexpr auto kAudioReopenDelay = std::chrono::milliseconds(300);
constexpr double kPaneAudioDelayLimit = kAudioDelayLimitMs / 1000.0;

// A band's gain over it on the graphic equalizer, in dB: "+2.5", "鈭?.2", "0".
std::wstring gainText(float db) {
    if (std::abs(db) < 0.05F) return L"0";
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << (db > 0.0F ? L"+" : L"\x2212") << std::abs(db);
    return text.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// The output

void App::startAudioOutput() {
    audioStarted_ = true;
    if (!audioDevices_.start()) appendDiagnostic("Audio outputs cannot be listed; Windows' default is used");
    refreshAudioOutputs();
    if (!audio_.initialize(wantedAudioOutput(), audioSettings_.deviceChannels)) {
        appendDiagnostic("Audio output unavailable: " + audio_.error());
    } else {
        logAudioOutput("Audio output");
    }
    applyAudioEffects();
}

void App::refreshAudioOutputs() {
    audioOutputs_ = audioDevices_.outputs();
    defaultAudioOutput_ = audioDevices_.defaultOutput();
}

bool App::audioOutputListed(const std::wstring& id) const {
    return std::any_of(audioOutputs_.begin(), audioOutputs_.end(),
                       [&](const AudioDeviceInfo& output) { return output.id == id; });
}

std::wstring App::audioOutputName(const std::wstring& id) const {
    for (const auto& output : audioOutputs_) {
        if (output.id == id) return output.name;
    }
    return id == audioSettings_.deviceId && !audioSettings_.deviceName.empty() ? audioSettings_.deviceName
                                                                                : std::wstring(L"Audio output");
}

// The chosen output while it is connected, otherwise Windows' default.
std::wstring App::wantedAudioOutput() const {
    if (!audioSettings_.deviceId.empty() && audioOutputListed(audioSettings_.deviceId)) return audioSettings_.deviceId;
    return defaultAudioOutput_;
}

void App::logAudioOutput(const char* what) {
    std::ostringstream message;
    message << what << ": " << wideToUtf8Text(audioOutputName(audio_.requestedDevice()))
            << (audio_.openedRequestedDevice() ? "" : " (did not open; Windows' default instead)")
            << ", " << audio_.deviceChannelCount() << " channels, voices in " << audio_.sourceChannels();
    appendDiagnostic(message.str());
}

// Every pane's sound stops, the output is taken down and brought up on the
// output wanted, and each pane is sought again in the new layout.
bool App::reopenAudioOutput(const char* reason) {
    for (const auto& source : sources_) {
        if (source) source->setAudioEnabled(false);
    }
    const bool opened = audio_.reopen(wantedAudioOutput(), audioSettings_.deviceChannels);
    if (opened) {
        audioReopenFailed_ = false;
        logAudioOutput(reason);
    } else if (!audioReopenFailed_) {
        audioReopenFailed_ = true;
        appendDiagnostic(std::string(reason) + ": no audio output opened (" + audio_.error() + ")");
    }
    for (const auto& source : sources_) {
        if (source) source->setAudioOutputLayout(audio_.sourceChannels(), audio_.sourceChannelMask());
    }
    reaimAllAudio();
    refreshAudioControls();
    return opened;
}

void App::reaimPaneAudio(std::size_t pane) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    const double position = clock_.position();
    sources_[pane]->setAudioEnabled(false);
    const auto generation = sources_[pane]->requestAudioSeek(currentSourceTime(pane));
    audio_.flush(pane, generation);
    const bool useAudio = shouldOutputAudio(pane, position, clock_.isPlaying());
    sources_[pane]->setAudioEnabled(useAudio);
    audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
    if (useAudio) audio_.play(pane);
}

void App::reaimAllAudio() {
    // The barrier owns every generation while it waits; rebuilding it at the
    // same target carries the change through, as an audio-track change does.
    if (seekBarrier_.active()) {
        seekAbsolute(clock_.position());
        return;
    }
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) reaimPaneAudio(pane);
}

// Each tick: outputs that came, went or became the default, and an engine
// that stopped.
void App::serviceAudioOutput() {
    const auto now = std::chrono::steady_clock::now();
    if (audioDevices_.takeChanged()) {
        audioDevicesChanged_ = true;
        audioDevicesChangedAt_ = now;
    }
    if (const long failure = audio_.takeCriticalError()) {
        std::ostringstream message;
        message << "Audio engine stopped: 0x" << std::hex << static_cast<unsigned long>(failure)
                << "; opening the output again";
        appendDiagnostic(message.str());
        audioReopenPending_ = true;
        audioReopenAt_ = now + kAudioReopenDelay;
    }
    if (audioDevicesChanged_ && now - audioDevicesChangedAt_ >= kAudioDeviceSettle) {
        audioDevicesChanged_ = false;
        const bool chosenWasThere = audioSettings_.deviceId.empty() || audioOutputListed(audioSettings_.deviceId);
        refreshAudioOutputs();
        const bool chosenIsThere = audioSettings_.deviceId.empty() || audioOutputListed(audioSettings_.deviceId);
        const std::wstring wanted = wantedAudioOutput();
        if (wanted != audio_.requestedDevice() || !audio_.isOpen()) {
            audioReopenPending_ = true;
            audioReopenAt_ = now;
            if (chosenWasThere && !chosenIsThere) {
                showNotice(audioSettings_.deviceName + L" disconnected; playing on " + audioOutputName(wanted));
            } else if (!wanted.empty()) {
                showNotice(L"Playing on " + audioOutputName(wanted));
            }
        }
    }
    if (audioReopenPending_ && now >= audioReopenAt_) {
        audioReopenPending_ = false;
        reopenAudioOutput("Audio output reopened");
    }
}

void App::chooseAudioOutput(const std::wstring& id) {
    if (id == audioSettings_.deviceId) return;
    audioSettings_.deviceId = id;
    audioSettings_.deviceName = id.empty() ? std::wstring{} : audioOutputName(id);
    if (wantedAudioOutput() != audio_.requestedDevice() || !audio_.isOpen()) reopenAudioOutput("Audio output chosen");
    showNotice(id.empty() ? L"Audio output: Windows default (" + audioOutputName(defaultAudioOutput_) + L")"
                          : L"Audio output: " + audioSettings_.deviceName);
    scheduleAppSettingsSave();
}

void App::cycleAudioOutput() {
    // Windows' default, then each output in the list's order.
    std::vector<std::wstring> ids{std::wstring{}};
    for (const auto& output : audioOutputs_) ids.push_back(output.id);
    const auto current = std::find(ids.begin(), ids.end(), audioSettings_.deviceId);
    const std::size_t next = current == ids.end() ? 0 : (static_cast<std::size_t>(current - ids.begin()) + 1) % ids.size();
    chooseAudioOutput(ids[next]);
}

void App::setAudioDeviceChannels(bool deviceChannels) {
    if (audioSettings_.deviceChannels == deviceChannels) return;
    audioSettings_.deviceChannels = deviceChannels;
    reopenAudioOutput("Audio speakers changed");
    showNotice(deviceChannels ? L"Speakers: as the output has them (" +
                                    std::to_wstring(audio_.sourceChannels()) + L" channels)"
                              : std::wstring(L"Speakers: stereo"));
    scheduleAppSettingsSave();
}

// ---------------------------------------------------------------------------
// Volume

float App::maxMasterVolume() const { return static_cast<float>(audioSettings_.maxVolume) / 100.0F; }

void App::setMasterVolume(float volume) {
    const float level = std::clamp(volume, 0.0F, maxMasterVolume());
    audio_.setVolume(level);
    if (level > 0.0F && audio_.muted()) audio_.setMuted(false);
}

void App::setMaxMasterVolume(int percent) {
    audioSettings_.maxVolume = clampAudioMaxVolume(percent);
    if (audio_.volume() > maxMasterVolume()) audio_.setVolume(maxMasterVolume());
    showNotice(L"Volume can reach " + std::to_wstring(audioSettings_.maxVolume) + L"%");
    scheduleAppSettingsSave();
}

// ---------------------------------------------------------------------------
// Timing and speed

double App::paneAudioDelay(std::size_t pane) const {
    if (pane >= audioDelays_.size()) return 0.0;
    return std::clamp(audioSettings_.delayMs / 1000.0 + audioDelays_[pane], -10.0, 10.0);
}

void App::prepareSourceAudio(VideoSource& source, std::size_t pane) const {
    source.setAudioOutputLayout(audio_.sourceChannels(), audio_.sourceChannelMask());
    source.setAudioDelay(paneAudioDelay(pane));
    source.setAudioTempo(audioSettings_.pitchCorrection ? playbackRates_[pane] : 1.0);
}

void App::syncPaneAudioTempo(std::size_t pane) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    sources_[pane]->setAudioTempo(audioSettings_.pitchCorrection ? playbackRates_[pane] : 1.0);
}

void App::applyPaneAudioDelay(std::size_t pane) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    sources_[pane]->setAudioDelay(paneAudioDelay(pane));
    if (seekBarrier_.active()) seekAbsolute(clock_.position());
    else reaimPaneAudio(pane);
}

void App::nudgePaneAudioDelay(std::size_t pane, double seconds) {
    if (pane >= sources_.size() || !paneLogicallyLoaded(pane)) return;
    double delay = std::clamp(audioDelays_[pane] + seconds, -kPaneAudioDelayLimit, kPaneAudioDelayLimit);
    delay = std::round(delay * 1000.0) / 1000.0;
    if (delay == audioDelays_[pane]) return;
    audioDelays_[pane] = delay;
    applyPaneAudioDelay(pane);
    const std::wstring name = severalPanesLoaded() ? paneLabel(pane) + L" sound " : std::wstring(L"Sound ");
    showNotice(name + (seconds < 0.0 ? L"earlier" : L"later") + L"  (" + audioDelayText(delay) + L")");
}

void App::resetPaneAudioDelay(std::size_t pane) {
    if (pane >= sources_.size() || !paneLogicallyLoaded(pane)) return;
    const bool changed = audioDelays_[pane] != 0.0;
    audioDelays_[pane] = 0.0;
    if (changed) applyPaneAudioDelay(pane);
    showNotice(severalPanesLoaded() ? paneLabel(pane) + L" sound in step with its picture"
                                    : std::wstring(L"Sound in step with the picture"));
}

void App::nudgeAllAudioDelay(int milliseconds) {
    const int delay = milliseconds == 0
        ? 0 : std::clamp(audioSettings_.delayMs + milliseconds, -kAudioDelayLimitMs, kAudioDelayLimitMs);
    if (delay == audioSettings_.delayMs) return;
    audioSettings_.delayMs = delay;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (sources_[pane]) sources_[pane]->setAudioDelay(paneAudioDelay(pane));
    }
    reaimAllAudio();
    showNotice(L"Every video's sound " + audioDelayText(delay / 1000.0));
    scheduleAppSettingsSave();
}

void App::setPitchCorrection(bool on) {
    if (audioSettings_.pitchCorrection == on) return;
    audioSettings_.pitchCorrection = on;
    // Only a pane at another speed sounds any different.
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane] || playbackRates_[pane] == 1.0) continue;
        syncPaneAudioTempo(pane);
        if (!seekBarrier_.active()) reaimPaneAudio(pane);
    }
    if (seekBarrier_.active()) seekAbsolute(clock_.position());
    showNotice(on ? L"Pitch kept at other speeds" : L"Pitch follows the speed");
    scheduleAppSettingsSave();
}

// ---------------------------------------------------------------------------
// Tracks and language

// The pane the audio keys act on: the one the pointer is on or the only
// one, else the first one heard, else the first one there.
int App::audioKeyPane() const {
    if (const int pane = commandPane(); pane >= 0) return pane;
    if (const int single = singleLoadedPane(); single >= 0) return single;
    int first = -1;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!paneLogicallyLoaded(pane)) continue;
        if (audioPaneEnabled(pane)) return static_cast<int>(pane);
        if (first < 0) first = static_cast<int>(pane);
    }
    return first;
}

// The track a pane plays, or the one it was asked for: a change waits for
// the audio worker's next seek, which a pane whose sound is off does not
// make, so the menu's list, the Audio tab's drop-down and the next-track
// key go from the one asked for.
int App::paneAudioTrack(std::size_t pane) const {
    if (pane >= sources_.size() || !sources_[pane]) return -1;
    const int requested = sources_[pane]->requestedAudioTrack();
    for (const auto& track : sources_[pane]->audioTracks()) {
        if (track.streamIndex == requested) return requested;
    }
    return sources_[pane]->activeAudioTrack();
}

void App::cycleAudioTrack(std::size_t pane) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    const auto tracks = sources_[pane]->audioTracks();
    if (tracks.size() < 2) {
        showNotice(tracks.empty() ? L"No audio track" : L"Only one audio track");
        return;
    }
    const int active = paneAudioTrack(pane);
    std::size_t next = 0;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (tracks[index].streamIndex == active) next = (index + 1) % tracks.size();
    }
    selectAudioTrack(pane, tracks[next].streamIndex);
}

void App::setAudioLanguage(AudioLanguage language) {
    audioSettings_.language = language;
    // The videos open now take the language too, where they have it.
    const std::string_view wanted = audioLanguageTag(language);
    for (std::size_t pane = 0; pane < sources_.size() && !wanted.empty(); ++pane) {
        if (!sources_[pane]) continue;
        const auto tracks = sources_[pane]->audioTracks();
        std::vector<AudioTrackCandidate> candidates;
        for (const auto& track : tracks) {
            candidates.push_back({track.language, track.title, track.isDefault, track.commentary});
        }
        const int chosen = chooseAudioTrack(candidates, wanted);
        if (chosen >= 0 && tracks[static_cast<std::size_t>(chosen)].streamIndex != paneAudioTrack(pane)) {
            selectAudioTrack(pane, tracks[static_cast<std::size_t>(chosen)].streamIndex);
        }
    }
    showNotice(std::wstring(L"Audio language: ") + audioLanguageLabel(language));
    scheduleAppSettingsSave();
}

// ---------------------------------------------------------------------------
// Effects

void App::applyAudioEffects() { audio_.setEffects(audioSettings_.effects); }

void App::changeAudioEffects(const AudioEffectSettings& effects, const std::wstring& notice) {
    audioSettings_.effects = clampAudioEffects(effects);
    applyAudioEffects();
    if (!notice.empty()) showNotice(notice);
    scheduleAppSettingsSave();
}

void App::toggleEqualizer() {
    auto effects = audioSettings_.effects;
    effects.equalizer = !effects.equalizer;
    std::wstring notice = effects.equalizer ? L"Equalizer on" : L"Equalizer off";
    if (effects.equalizer && audioSettings_.equalizerPreset >= 0) {
        notice += std::wstring(L"  (") + kEqualizerPresets[static_cast<std::size_t>(audioSettings_.equalizerPreset)].name + L")";
    }
    changeAudioEffects(effects, notice);
}

void App::chooseEqualizerPreset(std::size_t preset) {
    if (preset >= kEqualizerPresets.size()) return;
    applyEqualizerPreset(audioSettings_, preset);
    auto effects = audioSettings_.effects;
    effects.equalizer = true;
    changeAudioEffects(effects, std::wstring(L"Equalizer: ") + kEqualizerPresets[preset].name);
}

// A band or the preamp moved by hand: the equalizer is on, its bands are
// the user's own.
void App::setEqualizerGain(int band, float db) {
    auto effects = audioSettings_.effects;
    const float value = std::round(std::clamp(db, -kEqualizerRange, kEqualizerRange) * 2.0F) / 2.0F;
    if (band < 0) {
        effects.preamp = value;
    } else if (static_cast<std::size_t>(band) < kEqualizerBands) {
        effects.bands[static_cast<std::size_t>(band)] = value;
        audioSettings_.equalizerPreset = -1;
    }
    effects.equalizer = true;
    changeAudioEffects(effects, {});
}

void App::toggleNormalizer() {
    auto effects = audioSettings_.effects;
    effects.normalize = !effects.normalize;
    changeAudioEffects(effects, effects.normalize ? L"Normalize loudness on" : L"Normalize loudness off");
}

void App::setNightMode(int strength) {
    auto effects = audioSettings_.effects;
    effects.compression = std::clamp(strength, 0, kAudioStrengthCount - 1);
    changeAudioEffects(effects, std::wstring(L"Night mode: ") + audioStrengthLabel(effects.compression));
}

void App::setStereoMode(AudioStereoMode mode) {
    auto effects = audioSettings_.effects;
    effects.stereo = mode;
    changeAudioEffects(effects, std::wstring(L"Channels: ") + audioStereoModeLabel(mode));
}

// The key's mode, or back to stereo from it.
void App::toggleStereoMode(AudioStereoMode mode) {
    setStereoMode(audioSettings_.effects.stereo == mode ? AudioStereoMode::Stereo : mode);
}

void App::toggleVoiceEmphasis() {
    auto effects = audioSettings_.effects;
    effects.voiceEmphasis = !effects.voiceEmphasis;
    changeAudioEffects(effects, effects.voiceEmphasis ? L"Voice emphasis on" : L"Voice emphasis off");
}

void App::toggleCrossfeed() {
    auto effects = audioSettings_.effects;
    effects.crossfeed = !effects.crossfeed;
    changeAudioEffects(effects, effects.crossfeed ? L"Headphone crossfeed on" : L"Headphone crossfeed off");
}

void App::setStereoWidening(int strength) {
    auto effects = audioSettings_.effects;
    effects.widening = std::clamp(strength, 0, kAudioStrengthCount - 1);
    changeAudioEffects(effects, std::wstring(L"Stereo widening: ") + audioStrengthLabel(effects.widening));
}

// ---------------------------------------------------------------------------
// Menus and keys

// The player's sound, one line of the picture menu: where it goes, then
// what is done to it on the way.
void App::appendAudioMenu(PanelMenu& menu) const {
    PanelMenu outputs;
    outputs.option(CmdAudioOutputFirst, L"Windows default  (" + audioOutputName(defaultAudioOutput_) + L")",
                   audioSettings_.deviceId.empty());
    for (std::size_t index = 0; index < audioOutputs_.size() && index + 1 < kAudioOutputMenuLimit; ++index) {
        outputs.option(CmdAudioOutputFirst + static_cast<unsigned>(index + 1), audioOutputs_[index].name,
                       audioOutputs_[index].id == audioSettings_.deviceId);
    }
    if (!audioSettings_.deviceId.empty() && !audioOutputListed(audioSettings_.deviceId)) {
        outputs.option(CmdAudioOutputFirst + static_cast<unsigned>(kAudioOutputMenuLimit - 1),
                       audioSettings_.deviceName + L"  (not connected)", true, {}, false);
    }
    menu.submenu(L"Output", std::move(outputs));
    PanelMenu speakers;
    speakers.option(CmdAudioSpeakersStereo, L"Stereo", !audioSettings_.deviceChannels);
    speakers.option(CmdAudioSpeakersDevice,
                    L"Every speaker of the output  (" + std::to_wstring(audio_.deviceChannelCount()) + L")",
                    audioSettings_.deviceChannels);
    menu.submenu(L"Speakers", std::move(speakers));
    menu.separator();

    const auto& effects = audioSettings_.effects;
    menu.check(CmdAudioEqualizer, L"Equalizer", effects.equalizer, shortcut(KeyAction::Equalizer));
    PanelMenu presets;
    for (std::size_t preset = 0; preset < kEqualizerPresets.size(); ++preset) {
        presets.option(CmdEqualizerPresetFirst + static_cast<unsigned>(preset), kEqualizerPresets[preset].name,
                       audioSettings_.equalizerPreset == static_cast<int>(preset));
    }
    menu.submenu(L"Equalizer preset", std::move(presets));
    menu.check(CmdAudioNormalize, L"Normalize loudness", effects.normalize, shortcut(KeyAction::Normalizer));
    PanelMenu night;
    for (int strength = 0; strength < kAudioStrengthCount; ++strength) {
        night.option(CmdAudioNightFirst + static_cast<unsigned>(strength), audioStrengthLabel(strength),
                     effects.compression == strength);
    }
    menu.submenu(L"Night mode", std::move(night));
    PanelMenu channels;
    for (int mode = 0; mode < kAudioStereoModeCount; ++mode) {
        const auto stereo = static_cast<AudioStereoMode>(mode);
        const std::wstring key = stereo == AudioStereoMode::Swapped ? shortcut(KeyAction::SwapChannels)
                                 : stereo == AudioStereoMode::VoiceRemoval ? shortcut(KeyAction::VoiceRemoval)
                                                                             : std::wstring{};
        channels.option(CmdAudioStereoFirst + static_cast<unsigned>(mode), audioStereoModeLabel(stereo),
                        effects.stereo == stereo, key);
    }
    menu.submenu(L"Channels", std::move(channels));
    menu.check(CmdAudioVoiceEmphasis, L"Voice emphasis", effects.voiceEmphasis, shortcut(KeyAction::VoiceEmphasis));
    menu.check(CmdAudioCrossfeed, L"Headphone crossfeed", effects.crossfeed);
    PanelMenu widening;
    for (int strength = 0; strength < kAudioStrengthCount; ++strength) {
        widening.option(CmdAudioWideningFirst + static_cast<unsigned>(strength), audioStrengthLabel(strength),
                        effects.widening == strength);
    }
    menu.submenu(L"Stereo widening", std::move(widening));
    menu.check(CmdAudioPitch, L"Keep pitch at other speeds", audioSettings_.pitchCorrection);
    menu.separator();
    menu.option(CmdAudioSettings, L"Audio settings...");
}

void App::appendPaneAudioSyncMenu(PanelMenu& menu, std::size_t pane) const {
    const bool loaded = paneLogicallyLoaded(pane);
    menu.option(CmdAudioEarlier, L"Sound earlier by 50 ms", false, shortcut(KeyAction::AudioEarlier), loaded);
    menu.option(CmdAudioLater, L"Sound later by 50 ms", false, shortcut(KeyAction::AudioLater), loaded);
    menu.option(CmdAudioDelayReset, L"In step with the picture  (" + audioDelayText(audioDelays_[pane]) + L")",
                false, shortcut(KeyAction::AudioDelayReset), loaded && audioDelays_[pane] != 0.0);
}

bool App::handleAudioCommand(unsigned command, int pane) {
    const bool validPane = pane >= 0 && pane < static_cast<int>(kMaxPanes);
    const auto index = validPane ? static_cast<std::size_t>(pane) : std::size_t{0};
    if (command >= CmdAudioOutputFirst && command <= CmdAudioOutputLast) {
        const std::size_t choice = command - CmdAudioOutputFirst;
        if (choice == 0) chooseAudioOutput({});
        else if (choice - 1 < audioOutputs_.size()) chooseAudioOutput(audioOutputs_[choice - 1].id);
    } else if (command == CmdAudioSpeakersStereo || command == CmdAudioSpeakersDevice) {
        setAudioDeviceChannels(command == CmdAudioSpeakersDevice);
    } else if (command == CmdAudioEqualizer) {
        toggleEqualizer();
    } else if (command >= CmdEqualizerPresetFirst && command <= CmdEqualizerPresetLast) {
        chooseEqualizerPreset(command - CmdEqualizerPresetFirst);
    } else if (command == CmdAudioNormalize) {
        toggleNormalizer();
    } else if (command >= CmdAudioNightFirst && command <= CmdAudioNightLast) {
        setNightMode(static_cast<int>(command - CmdAudioNightFirst));
    } else if (command >= CmdAudioStereoFirst && command <= CmdAudioStereoLast) {
        setStereoMode(static_cast<AudioStereoMode>(command - CmdAudioStereoFirst));
    } else if (command == CmdAudioVoiceEmphasis) {
        toggleVoiceEmphasis();
    } else if (command == CmdAudioCrossfeed) {
        toggleCrossfeed();
    } else if (command >= CmdAudioWideningFirst && command <= CmdAudioWideningLast) {
        setStereoWidening(static_cast<int>(command - CmdAudioWideningFirst));
    } else if (command == CmdAudioPitch) {
        setPitchCorrection(!audioSettings_.pitchCorrection);
    } else if (command == CmdAudioSettings) {
        openSettingsWindow();
        selectSettingsTab(SettingsTab::Audio);
    } else if (command == CmdAudioEarlier && validPane) {
        nudgePaneAudioDelay(index, -kAudioDelayStepMs / 1000.0);
    } else if (command == CmdAudioLater && validPane) {
        nudgePaneAudioDelay(index, kAudioDelayStepMs / 1000.0);
    } else if (command == CmdAudioDelayReset && validPane) {
        resetPaneAudioDelay(index);
    } else {
        return false;
    }
    return true;
}

bool App::handleAudioKey(KeyAction action) {
    switch (action) {
    case KeyAction::AudioTrackNext:
    case KeyAction::AudioEarlier:
    case KeyAction::AudioLater:
    case KeyAction::AudioDelayReset: {
        const int pane = audioKeyPane();
        if (pane < 0) return true;
        const auto index = static_cast<std::size_t>(pane);
        if (action == KeyAction::AudioTrackNext) cycleAudioTrack(index);
        else if (action == KeyAction::AudioEarlier) nudgePaneAudioDelay(index, -kAudioDelayStepMs / 1000.0);
        else if (action == KeyAction::AudioLater) nudgePaneAudioDelay(index, kAudioDelayStepMs / 1000.0);
        else resetPaneAudioDelay(index);
        return true;
    }
    case KeyAction::Equalizer: toggleEqualizer(); return true;
    case KeyAction::Normalizer: toggleNormalizer(); return true;
    case KeyAction::NightMode: setNightMode((audioSettings_.effects.compression + 1) % kAudioStrengthCount); return true;
    case KeyAction::SwapChannels: toggleStereoMode(AudioStereoMode::Swapped); return true;
    case KeyAction::VoiceRemoval: toggleStereoMode(AudioStereoMode::VoiceRemoval); return true;
    case KeyAction::VoiceEmphasis: toggleVoiceEmphasis(); return true;
    case KeyAction::AudioOutputNext: cycleAudioOutput(); return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// The Audio tab

void App::appendAudioSettingsRows(std::vector<PanelRow>& rows) const {
    const auto header = [&](const wchar_t* text) {
        PanelRow row;
        row.kind = PanelRowKind::Header;
        row.label = text;
        rows.push_back(std::move(row));
    };
    const auto note = [&](const std::wstring& text) {
        PanelRow row;
        row.kind = PanelRowKind::Note;
        row.wrapNote = true;
        row.label = text;
        rows.push_back(std::move(row));
    };
    const auto toggle = [&](SettingId id, int param, const std::wstring& label, bool on) {
        PanelRow row;
        row.kind = PanelRowKind::Toggle;
        row.id = id;
        row.param = param;
        row.label = label;
        row.on = on;
        rows.push_back(std::move(row));
    };
    // A few short options are segments beside the label; a long list, or
    // long names, a drop-down.
    const auto choice = [&](SettingId id, int param, const std::wstring& label, std::vector<std::wstring> options,
                            int selected, bool dropDown = false) {
        PanelRow row;
        row.kind = PanelRowKind::Choice;
        row.id = id;
        row.param = param;
        row.label = label;
        row.options = std::move(options);
        row.selected = selected;
        row.dropDown = dropDown;
        rows.push_back(std::move(row));
    };
    // A volume, muted from the button at its end.
    const auto volume = [&](SettingId id, int param, const std::wstring& label, float value01,
                            const std::wstring& value, bool muted) {
        PanelRow row;
        row.kind = PanelRowKind::Slider;
        row.id = id;
        row.param = param;
        row.label = label;
        row.slider01 = std::clamp(value01, 0.0F, 1.0F);
        row.value = value;
        row.trailingToggle = true;
        row.trailingOn = muted;
        rows.push_back(std::move(row));
    };
    const auto buttons = [&](SettingId id, int param, const std::wstring& label, std::vector<std::wstring> captions,
                             const std::wstring& value, bool enabled = true) {
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = id;
        row.param = param;
        row.label = label;
        row.options = std::move(captions);
        row.value = value;
        row.enabled = enabled;
        rows.push_back(std::move(row));
    };
    const auto withKey = [&](const std::wstring& label, KeyAction action) {
        const std::wstring key = shortcut(action);
        return key.empty() ? label : label + L"  (" + key + L")";
    };

    // The rail spans what the volume may reach; its value is the level.
    volume(SettingId::MasterVolume, -1, L"Volume", audio_.volume() / maxMasterVolume(), percent(audio_.volume()),
           audio_.muted());
    rows.back().hint = L"The button at the end mutes every video (" + shortcut(KeyAction::Mute) + L").";
    // The limit a percent at a time, in a spin box: an arrow held steps on.
    buttons(SettingId::AudioMaxVolume, -1, L"Volume can reach", {L"Up", L"Down"},
            std::to_wstring(audioSettings_.maxVolume) + L"%");
    rows.back().spinBox = true;
    rows.back().slider01 = static_cast<float>(audioSettings_.maxVolume - kAudioMaxVolumeLeast) /
                           static_cast<float>(kAudioMaxVolumeMost - kAudioMaxVolumeLeast);
    rows.back().hint = L"From 100% to 200%. Past 100% the sound is amplified, and a limiter keeps it from clipping.";

    // Each video's own: whether it is heard, how loud, which of its tracks
    // and how late against its picture, named in its colour.
    bool videos = false;
    for (std::size_t position = 0; position < kMaxPanes; ++position) {
        const auto pane = paneForPosition(position);
        if (!sources_[pane]) continue;
        if (!videos) header(L"Videos");
        videos = true;
        const int video = static_cast<int>(position);
        const std::wstring name = L"V" + std::to_wstring(position + 1);
        toggle(SettingId::PaneAudio, video, withKey(name + L" sound", static_cast<KeyAction>(
                   static_cast<std::size_t>(KeyAction::AudioV1) + position)), audioPaneEnabled(pane));
        rows.back().video = video;
        rows.back().hint = L"Several videos can be heard at once.";
        volume(SettingId::PaneVolume, video, name + L" volume", audio_.paneVolume(pane), percent(audio_.paneVolume(pane)),
               audio_.paneMuted(pane));
        rows.back().video = video;
        std::vector<std::wstring> tracks;
        int chosen = -1;
        for (const auto& track : sources_[pane]->audioTracks()) {
            if (track.streamIndex == paneAudioTrack(pane)) chosen = static_cast<int>(tracks.size());
            tracks.push_back(audioTrackLabel(track));
        }
        const bool any = !tracks.empty();
        choice(SettingId::PaneAudioTrack, video, name + L" audio track",
               std::move(tracks), chosen, true);
        rows.back().value = L"No audio track";
        rows.back().enabled = any;
        rows.back().video = video;
        buttons(SettingId::PaneAudioDelay, video, name + L" sound timing", {L"Earlier", L"Later", L"In step"},
                audioDelayText(audioDelays_[pane]), paneLogicallyLoaded(pane));
        rows.back().video = video;
        rows.back().hint = L"Its sound against its picture, 50 ms a press.";
    }

    header(L"Output");
    {
        // Windows' default, then the outputs as listed; one chosen that is
        // not connected stays the choice, last.
        std::vector<std::wstring> outputs{L"Windows default"};
        int selected = audioSettings_.deviceId.empty() ? 0 : -1;
        for (std::size_t index = 0; index < audioOutputs_.size(); ++index) {
            outputs.push_back(audioOutputs_[index].name);
            if (audioOutputs_[index].id == audioSettings_.deviceId) selected = static_cast<int>(index + 1);
        }
        if (selected < 0) {
            outputs.push_back(audioSettings_.deviceName + L"  (not connected)");
            selected = static_cast<int>(outputs.size() - 1);
        }
        choice(SettingId::AudioDevice, -1, withKey(L"Output device", KeyAction::AudioOutputNext), std::move(outputs),
               selected, true);
    }
    {
        std::wostringstream playing;
        if (!audio_.isOpen()) {
            playing << L"No audio output is open.";
        } else {
            playing << L"Playing on " << audioOutputName(audio_.requestedDevice()) << L", "
                    << audio_.deviceChannelCount() << L" channels";
            if (!audio_.openedRequestedDevice()) playing << L" (it did not open; Windows' default plays)";
            playing << L".";
        }
        note(playing.str());
    }
    choice(SettingId::AudioSpeakers, -1, L"Speakers",
           {L"Stereo", L"Every speaker  (" + std::to_wstring(audio_.deviceChannelCount()) + L")"},
           audioSettings_.deviceChannels ? 1 : 0);
    rows.back().hint = L"Stereo mixes every track down to two channels; every speaker plays a 5.1 or 7.1 track "
                       L"on the output's own speakers.";
    buttons(SettingId::AudioDelayAll, -1, L"Every video's sound",
            {L"Earlier", L"Later", L"In step"}, audioDelayText(audioSettings_.delayMs / 1000.0));
    rows.back().hint = L"For an output that is late itself, such as Bluetooth headphones: 50 ms a press.";

    header(L"Sound");
    toggle(SettingId::AudioNormalize, -1, withKey(L"Normalize loudness", KeyAction::Normalizer),
           audioSettings_.effects.normalize);
    rows.back().hint = L"Quiet and loud videos come out at much the same level.";
    {
        std::vector<std::wstring> strengths;
        for (int strength = 0; strength < kAudioStrengthCount; ++strength) strengths.emplace_back(audioStrengthLabel(strength));
        choice(SettingId::AudioNightMode, -1, withKey(L"Night mode", KeyAction::NightMode), strengths,
               audioSettings_.effects.compression);
        rows.back().hint = L"Loud scenes are brought down and quiet speech up, for listening at low volume.";
        std::vector<std::wstring> modes;
        for (int mode = 0; mode < kAudioStereoModeCount; ++mode) {
            modes.emplace_back(audioStereoModeLabel(static_cast<AudioStereoMode>(mode)));
        }
        choice(SettingId::AudioChannels, -1, L"Channels", std::move(modes), static_cast<int>(audioSettings_.effects.stereo),
               true);
        rows.back().hint = L"Swapped " + shortcut(KeyAction::SwapChannels) + L", voice removal " +
                           shortcut(KeyAction::VoiceRemoval) + L": what is in the middle cancels out.";
        toggle(SettingId::AudioVoice, -1, withKey(L"Voice emphasis", KeyAction::VoiceEmphasis),
               audioSettings_.effects.voiceEmphasis);
        toggle(SettingId::AudioCrossfeed, -1, L"Headphone crossfeed", audioSettings_.effects.crossfeed);
        rows.back().hint = L"Each ear hears a little of the other side, as from speakers: less tiring on headphones.";
        choice(SettingId::AudioWidening, -1, L"Stereo widening", std::move(strengths), audioSettings_.effects.widening);
    }
    toggle(SettingId::AudioPitch, -1, L"Keep pitch at other speeds", audioSettings_.pitchCorrection);
    {
        std::vector<std::wstring> languages;
        for (int language = 0; language < kAudioLanguageCount; ++language) {
            languages.emplace_back(audioLanguageLabel(static_cast<AudioLanguage>(language)));
        }
        choice(SettingId::AudioLanguage, -1, L"Preferred audio language", std::move(languages),
               static_cast<int>(audioSettings_.language), true);
        rows.back().hint = L"Automatic: the file's default track, or the one Emby names.";
    }

    header(L"Equalizer");
    toggle(SettingId::Equalizer, -1, withKey(L"Use the equalizer", KeyAction::Equalizer), audioSettings_.effects.equalizer);
    {
        std::vector<std::wstring> presets;
        for (const auto& preset : kEqualizerPresets) presets.emplace_back(preset.name);
        choice(SettingId::EqualizerPreset, -1, L"Preset", std::move(presets), audioSettings_.equalizerPreset, true);
        rows.back().value = L"Custom";
    }
    {
        // The preamp set apart, then the ten bands, each from -20 dB at the
        // bottom to +20 dB at the top.
        PanelRow row;
        row.kind = PanelRowKind::Equalizer;
        row.id = SettingId::EqualizerBand;
        row.on = audioSettings_.effects.equalizer;
        row.apartBands = 1;
        row.hint = L"Drag a band up or down, up to 20 dB either way; a double-click sets it back to 0.";
        const auto band = [&](const wchar_t* caption, float db, int param) {
            row.options.emplace_back(caption);
            row.levels.push_back((db + kEqualizerRange) / (2.0F * kEqualizerRange));
            row.levelTexts.push_back(gainText(db));
            row.tileParams.push_back(param);
        };
        band(L"Preamp", audioSettings_.effects.preamp, -1);
        for (std::size_t index = 0; index < kEqualizerBands; ++index) {
            band(equalizerBandCaption(index), audioSettings_.effects.bands[index], static_cast<int>(index));
        }
        rows.push_back(std::move(row));
    }
}

}  // namespace quaddeck
