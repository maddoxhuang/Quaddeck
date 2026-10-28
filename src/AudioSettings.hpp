#pragma once

// The audio choices: the device the sound goes to, how loud it may get, how
// late it plays against the picture, the language a video starts in, and
// the effects the mix goes through. Pure: the presets, the clamps and the
// choice of track need no device.

#include "Subtitles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace quaddeck {

// VLC's ten bands, an octave apart.
inline constexpr std::size_t kEqualizerBands = 10;
inline constexpr std::array<double, kEqualizerBands> kEqualizerFrequencies{
    31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
// How far a band or the preamp may go either way, in dB.
inline constexpr float kEqualizerRange = 20.0F;

inline const wchar_t* equalizerBandLabel(std::size_t band) {
    static constexpr const wchar_t* labels[kEqualizerBands] = {
        L"31 Hz", L"62 Hz", L"125 Hz", L"250 Hz", L"500 Hz", L"1 kHz", L"2 kHz", L"4 kHz", L"8 kHz", L"16 kHz"};
    return band < kEqualizerBands ? labels[band] : L"";
}

// The same under a band of the graphic equalizer, where a column is narrow.
inline const wchar_t* equalizerBandCaption(std::size_t band) {
    static constexpr const wchar_t* captions[kEqualizerBands] = {
        L"31", L"62", L"125", L"250", L"500", L"1K", L"2K", L"4K", L"8K", L"16K"};
    return band < kEqualizerBands ? captions[band] : L"";
}

struct EqualizerPreset {
    const wchar_t* name;
    std::array<float, kEqualizerBands> bands;
};

// VLC's presets, band for band.
inline constexpr std::array<EqualizerPreset, 18> kEqualizerPresets{{
    {L"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {L"Classical", {0, 0, 0, 0, 0, 0, -7.2F, -7.2F, -7.2F, -9.6F}},
    {L"Club", {0, 0, 8, 5.6F, 5.6F, 5.6F, 3.2F, 0, 0, 0}},
    {L"Dance", {9.6F, 7.2F, 2.4F, 0, 0, -5.6F, -7.2F, -7.2F, 0, 0}},
    {L"Full bass", {-8, 9.6F, 9.6F, 5.6F, 1.6F, -4, -8, -10.4F, -11.2F, -11.2F}},
    {L"Full bass and treble", {7.2F, 5.6F, 0, -7.2F, -4.8F, 1.6F, 8, 11.2F, 12, 12}},
    {L"Full treble", {-9.6F, -9.6F, -9.6F, -4, 2.4F, 11.2F, 16, 16, 16, 16.8F}},
    {L"Headphones", {4.8F, 11.2F, 5.6F, -3.2F, -2.4F, 1.6F, 4.8F, 9.6F, 12.8F, 14.4F}},
    {L"Large hall", {10.4F, 10.4F, 5.6F, 5.6F, 0, -4.8F, -4.8F, -4.8F, 0, 0}},
    {L"Live", {-4.8F, 0, 4, 5.6F, 5.6F, 5.6F, 4, 2.4F, 2.4F, 2.4F}},
    {L"Party", {7.2F, 7.2F, 0, 0, 0, 0, 0, 0, 7.2F, 7.2F}},
    {L"Pop", {-1.6F, 4.8F, 7.2F, 8, 5.6F, 0, -2.4F, -2.4F, -1.6F, -1.6F}},
    {L"Reggae", {0, 0, 0, -5.6F, 0, 6.4F, 6.4F, 0, 0, 0}},
    {L"Rock", {8, 4.8F, -5.6F, -8, -3.2F, 4, 8.8F, 11.2F, 11.2F, 11.2F}},
    {L"Ska", {-2.4F, -4.8F, -4, 0, 4, 5.6F, 8.8F, 9.6F, 11.2F, 9.6F}},
    {L"Soft", {4.8F, 1.6F, 0, -2.4F, 0, 4, 8, 9.6F, 11.2F, 12}},
    {L"Soft rock", {4, 4, 2.4F, 0, -4, -5.6F, -3.2F, 0, 2.4F, 8.8F}},
    {L"Techno", {8, 5.6F, 0, -5.6F, -4.8F, 0, 8, 9.6F, 9.6F, 8.8F}},
}};

// The preamp a preset comes with: half its largest boost taken off ahead of
// the bands, so a loud preset does not lean on the limiter all the time.
inline float equalizerPresetPreamp(const EqualizerPreset& preset) {
    const float boost = *std::max_element(preset.bands.begin(), preset.bands.end());
    return boost > 0.0F ? -boost * 0.5F : 0.0F;
}

// What happens to the left and right channels before anything else.
enum class AudioStereoMode : int {
    Stereo,
    Swapped,
    LeftOnly,   // the left channel on both sides
    RightOnly,
    Mono,
    // The difference of the two sides: what is panned to the middle, a
    // singer's or a speaker's voice above all, cancels out.
    VoiceRemoval,
};
inline constexpr int kAudioStereoModeCount = 6;

inline const wchar_t* audioStereoModeLabel(AudioStereoMode mode) {
    switch (mode) {
    case AudioStereoMode::Stereo: return L"Stereo";
    case AudioStereoMode::Swapped: return L"Swapped";
    case AudioStereoMode::LeftOnly: return L"Left only";
    case AudioStereoMode::RightOnly: return L"Right only";
    case AudioStereoMode::Mono: return L"Mono";
    case AudioStereoMode::VoiceRemoval: return L"Voice removal";
    }
    return L"";
}

// The language a video's sound starts in, when its file has several.
enum class AudioLanguage : int { Automatic, Chinese, English, Japanese, Korean };
inline constexpr int kAudioLanguageCount = 5;

inline std::string_view audioLanguageTag(AudioLanguage language) {
    switch (language) {
    case AudioLanguage::Chinese: return "zh";
    case AudioLanguage::English: return "en";
    case AudioLanguage::Japanese: return "ja";
    case AudioLanguage::Korean: return "ko";
    case AudioLanguage::Automatic: break;
    }
    return {};
}

inline const wchar_t* audioLanguageLabel(AudioLanguage language) {
    switch (language) {
    case AudioLanguage::Automatic: return L"Automatic";
    case AudioLanguage::Chinese: return L"\x4E2D\x6587";
    case AudioLanguage::English: return L"English";
    case AudioLanguage::Japanese: return L"\x65E5\x672C\x8A9E";
    case AudioLanguage::Korean: return L"\xD55C\xAD6D\xC5B4";
    }
    return L"";
}

// Night mode's compressor and the stereo widening come in two strengths.
inline constexpr int kAudioStrengthCount = 3;
inline const wchar_t* audioStrengthLabel(int strength) {
    return strength <= 0 ? L"Off" : strength == 1 ? L"Light" : L"Strong";
}

// How loud the master volume may go, a percent at a time: past 100% the mix
// is amplified, and a limiter keeps the peaks from clipping, up to twice the
// sound's own level (AudioOutput::setVolume).
inline constexpr int kAudioMaxVolumeLeast = 100;
inline constexpr int kAudioMaxVolumeMost = 200;
inline constexpr int kAudioDelayStepMs = 50;
// Either way, for the setting that applies to every video and for one
// pane's own; together they never exceed the ten seconds of silence the
// decoder puts ahead of a late start.
inline constexpr int kAudioDelayLimitMs = 5000;

struct AudioEffectSettings {
    bool equalizer{};
    float preamp{};
    std::array<float, kEqualizerBands> bands{};
    bool normalize{};
    int compression{};  // night mode: 0 off, 1 light, 2 strong
    AudioStereoMode stereo{AudioStereoMode::Stereo};
    bool crossfeed{};
    int widening{};  // 0 off, 1 light, 2 strong
    bool voiceEmphasis{};
    // The master volume above 100%, as a gain; not saved, it follows the
    // volume.
    float boost{1.0F};

    bool operator==(const AudioEffectSettings&) const = default;
};

struct AudioSettings {
    // The endpoint chosen, as Windows names it; empty follows Windows'
    // default output. Its name is kept for when it is not connected.
    std::wstring deviceId;
    std::wstring deviceName;
    // The decoder mixes to stereo, or to every speaker the device has.
    bool deviceChannels{};
    int maxVolume{100};
    // Every video's sound against its picture, later when positive.
    int delayMs{};
    // At another speed the sound is stretched rather than sped up, so it
    // keeps its pitch.
    bool pitchCorrection{true};
    AudioLanguage language{AudioLanguage::Automatic};
    // The preset the bands came from, -1 once they were moved by hand.
    int equalizerPreset{};
    AudioEffectSettings effects;

    bool operator==(const AudioSettings&) const = default;
};

inline int clampAudioMaxVolume(int percent) {
    return std::clamp(percent, kAudioMaxVolumeLeast, kAudioMaxVolumeMost);
}

inline AudioEffectSettings clampAudioEffects(AudioEffectSettings effects) {
    const auto db = [](float value) {
        return std::isfinite(value) ? std::clamp(value, -kEqualizerRange, kEqualizerRange) : 0.0F;
    };
    effects.preamp = db(effects.preamp);
    for (float& band : effects.bands) band = db(band);
    effects.compression = std::clamp(effects.compression, 0, kAudioStrengthCount - 1);
    effects.widening = std::clamp(effects.widening, 0, kAudioStrengthCount - 1);
    const int stereo = static_cast<int>(effects.stereo);
    if (stereo < 0 || stereo >= kAudioStereoModeCount) effects.stereo = AudioStereoMode::Stereo;
    effects.boost = std::isfinite(effects.boost) ? std::clamp(effects.boost, 1.0F, 2.0F) : 1.0F;
    return effects;
}

inline AudioSettings clampAudioSettings(AudioSettings settings) {
    settings.maxVolume = clampAudioMaxVolume(settings.maxVolume);
    settings.delayMs = std::clamp(settings.delayMs, -kAudioDelayLimitMs, kAudioDelayLimitMs);
    const int language = static_cast<int>(settings.language);
    if (language < 0 || language >= kAudioLanguageCount) settings.language = AudioLanguage::Automatic;
    if (settings.equalizerPreset < -1 ||
        settings.equalizerPreset >= static_cast<int>(kEqualizerPresets.size())) {
        settings.equalizerPreset = -1;
    }
    if (settings.deviceId.empty()) settings.deviceName.clear();
    settings.effects = clampAudioEffects(settings.effects);
    return settings;
}

// The bands and preamp of a preset, and its index remembered.
inline void applyEqualizerPreset(AudioSettings& settings, std::size_t preset) {
    if (preset >= kEqualizerPresets.size()) return;
    settings.equalizerPreset = static_cast<int>(preset);
    settings.effects.bands = kEqualizerPresets[preset].bands;
    settings.effects.preamp = equalizerPresetPreamp(kEqualizerPresets[preset]);
}

// One audio stream of a container, as the choice of language sees it.
struct AudioTrackCandidate {
    std::string language;
    std::string title;
    bool isDefault{};
    bool commentary{};
};

// The track in the wanted language, -1 when no track speaks it or nothing is
// wanted. Of several, the one that is not a commentary, then the one marked
// default, then the first. Chinese in either script counts as Chinese.
inline int chooseAudioTrack(const std::vector<AudioTrackCandidate>& tracks, std::string_view wanted) {
    if (wanted.empty()) return -1;
    int best = -1;
    int bestRank = 0;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        const std::string tag = subtitleLanguageTag(track.language, utf8ToWideText(track.title));
        if (tag.substr(0, tag.find('-')) != wanted) continue;
        std::string title = track.title;
        for (char& c : title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool commentary = track.commentary || title.find("comment") != std::string::npos;
        const int rank = (commentary ? 0 : 2) + (track.isDefault ? 1 : 0);
        if (best < 0 || rank > bestRank) {
            best = static_cast<int>(index);
            bestRank = rank;
        }
    }
    return best;
}

}  // namespace quaddeck
