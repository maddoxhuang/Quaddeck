#pragma once

#include "FileAssociations.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>

namespace quaddeck {

inline constexpr std::size_t kMaxPanes = 5;
inline constexpr std::size_t kLegacyPaneCount = 4;
inline constexpr unsigned kAllPaneMask = (1U << kMaxPanes) - 1U;

template <typename T>
using PaneArray = std::array<T, kMaxPanes>;

enum class LayoutMode {
    Grid2x2,
    SideBySide,
    Row4,
    Column4,
    PortraitStack,
    LandscapePair
};
// Auto layout normally discovers the one portrait/landscape focus from the
// current media. App can lock that decision after the set has settled so
// replacing a file does not unexpectedly move every visible pane.
enum class AutoLayoutFocus { Dynamic, None, Tall, Wide };
enum class DecodeMode { Automatic, Hardware, Software };
enum class ShaderPreset { Normal, Sharpen, Grayscale, Invert, SmartVibrancePlus };
enum class SeekMode { Linked, Independent };
enum class ViewMode { Fit, Fill, Stretch };

struct SmartVibranceSettings {
    float intensity{1.5F};
    float saturationPivot{0.5F};
    float grayPivot{0.003F};
    float graySharpness{45.0F};
};

inline SmartVibranceSettings clampSmartVibranceSettings(SmartVibranceSettings value) {
    value.intensity = std::clamp(value.intensity, 0.0F, 3.0F);
    value.saturationPivot = std::clamp(value.saturationPivot, 0.2F, 1.0F);
    value.grayPivot = std::clamp(value.grayPivot, 0.0005F, 0.01F);
    value.graySharpness = std::clamp(value.graySharpness, 5.0F, 80.0F);
    return value;
}

inline bool finiteSmartVibranceSettings(const SmartVibranceSettings& value) {
    return std::isfinite(value.intensity) && std::isfinite(value.saturationPivot) &&
           std::isfinite(value.grayPivot) && std::isfinite(value.graySharpness);
}

// The two NVIDIA RTX Video features the driver exposes through private
// D3D11 video-processor stream extensions: Super Resolution upscales the
// decoded surface to the pane's own pixels, RTX Video HDR infers an HDR
// picture from an SDR source. Both are wanted-by-the-user flags; what the
// driver and the display actually allow is a VideoEnhancementStatus.
struct VideoEnhancementSettings {
    bool superResolution{};
    bool rtxHdr{};
    friend bool operator==(const VideoEnhancementSettings& left,
                           const VideoEnhancementSettings& right) {
        return left.superResolution == right.superResolution && left.rtxHdr == right.rtxHdr;
    }
};

struct VideoEnhancementStatus {
    std::wstring adapterName;
    bool nvidiaAdapter{};
    // The driver accepted the extension when it was probed on this device.
    bool superResolutionSupported{};
    bool rtxHdrSupported{};
    // The extension was accepted on the video processor built most recently.
    bool superResolutionApplied{};
    bool rtxHdrApplied{};
    // The display the window is on is in Windows HDR mode.
    bool displayHdr{};
    // The swap chain is presenting 10-bit PQ right now.
    bool hdrOutput{};
    float sdrWhiteNits{200.0F};
};

// How the Emby browser lays a list out and orders it. Device settings,
// saved with QCONFIG, not with a visual preset.
struct EmbyBrowserPrefs {
    int view{2};         // 0 list, 1 posters, 2 thumbnails
    int sort{0};         // emby::SortKey: 0 name, 1 added, 2 released, 3 length, 4 random, 5 played, 6 size, 7 updated
    bool descending{};
    bool unplayed{};
    bool flat{};         // every video under a folder, subfolders flattened
    friend bool operator==(const EmbyBrowserPrefs&, const EmbyBrowserPrefs&) = default;
};

inline EmbyBrowserPrefs clampEmbyBrowserPrefs(EmbyBrowserPrefs value) {
    value.view = std::clamp(value.view, 0, 2);
    value.sort = std::clamp(value.sort, 0, 7);
    return value;
}

// How subtitles are shown and which are chosen unasked. Device settings,
// saved with QCONFIG. The size is a share of what the script asks for (a
// plain subtitle's own size follows the picture's height); the position is
// how far the bottom lines are raised above where their style puts them, in
// shares of the picture's height -- 0 is the script's own place. Lines a
// script places itself stay where it put them. The background is the dark
// box behind plain subtitles; without it they are white letters with a
// black outline. An ASS script draws its own.
struct SubtitleSettings {
    bool show{true};
    int language{};        // SubtitleLanguage: 0 automatic, then the languages it names
    float size{1.0F};
    float position{};
    bool background{};
    friend bool operator==(const SubtitleSettings&, const SubtitleSettings&) = default;
};

inline constexpr float kSubtitleSizeMinimum = 0.5F;
inline constexpr float kSubtitleSizeMaximum = 2.5F;
inline constexpr float kSubtitlePositionMaximum = 0.8F;

inline SubtitleSettings clampSubtitleSettings(SubtitleSettings value) {
    value.language = std::clamp(value.language, 0, 5);
    value.size = std::isfinite(value.size)
        ? std::clamp(value.size, kSubtitleSizeMinimum, kSubtitleSizeMaximum) : 1.0F;
    value.position = std::isfinite(value.position)
        ? std::clamp(value.position, 0.0F, kSubtitlePositionMaximum) : 0.0F;
    return value;
}

// What happens when the only video ends: PotPlayer's playback orders.
// Ctrl+4 / 6 / 7 / 8 / 9 in the window, a choice in the settings sheet and
// a submenu of the popup menu. Several videos keep "Restart all when the
// set finishes".
enum class PlayOrder { PlayOne, RepeatOne, InOrder, RepeatList, Shuffle };
inline constexpr int kPlayOrderCount = 5;

inline PlayOrder clampPlayOrder(int value) {
    return static_cast<PlayOrder>(std::clamp(value, 0, kPlayOrderCount - 1));
}

inline const wchar_t* playOrderName(PlayOrder order) {
    switch (order) {
    case PlayOrder::PlayOne: return L"Play one, then stop";
    case PlayOrder::RepeatOne: return L"Repeat one";
    case PlayOrder::InOrder: return L"Play in order";
    case PlayOrder::RepeatList: return L"Repeat the list";
    default: return L"Shuffle";
    }
}

// The index in a list of `count` to play after `current` ends, or -1 to
// stop: the next one, the next one wrapping round, the same one again, or
// another one at random (`random` decides which, never the same one when
// there is a choice).
inline int playOrderNextIndex(PlayOrder order, int current, int count, unsigned random) {
    if (count <= 0 || current < 0 || current >= count) return -1;
    switch (order) {
    case PlayOrder::PlayOne: return -1;
    case PlayOrder::RepeatOne: return current;
    case PlayOrder::InOrder: return current + 1 < count ? current + 1 : -1;
    case PlayOrder::RepeatList: return (current + 1) % count;
    default: {
        if (count == 1) return current;
        const int pick = static_cast<int>(random % static_cast<unsigned>(count - 1));
        return pick >= current ? pick + 1 : pick;
    }
    }
}

// The one line under the two toggles in the settings sheet. Pure so that
// every branch can be exhausted without a device. A note row is one line
// of about sixty characters at the sheet's width, so one problem is
// reported at a time, the one the user has to fix first.
inline std::wstring videoEnhancementNote(const VideoEnhancementSettings& wanted,
                                         const VideoEnhancementStatus& status) {
    if (!wanted.superResolution && !wanted.rtxHdr) {
        return L"NVIDIA RTX required; eligible SDR includes software decode.";
    }
    if (!status.nvidiaAdapter) {
        return L"Not available: rendering on " +
               (status.adapterName.empty() ? std::wstring(L"a non-NVIDIA GPU") : status.adapterName) + L".";
    }
    if (wanted.superResolution && !status.superResolutionSupported) {
        return L"Driver declined Super Resolution (needs RTX 20+, driver 530+).";
    }
    if (wanted.rtxHdr && !status.rtxHdrSupported) {
        return L"Driver declined RTX Video HDR (needs RTX 20+, driver 551+).";
    }
    if (wanted.rtxHdr && !status.displayHdr) {
        return L"RTX Video HDR needs HDR on for this display in Windows settings.";
    }
    // Super Resolution only runs while a video is shown larger than its own
    // size; at or below it the driver's network would cost a frame for nothing.
    const bool superResolutionIdle = wanted.superResolution && !status.superResolutionApplied;
    if (wanted.superResolution && wanted.rtxHdr) {
        if (!status.hdrOutput) return L"Super Resolution active; RTX Video HDR is switching the output.";
        return superResolutionIdle ? L"PQ output ready; RTX HDR requested. Super Resolution idle."
                                   : L"PQ output ready; RTX HDR + Super Resolution requested.";
    }
    if (wanted.superResolution) {
        return superResolutionIdle ? L"Super Resolution idle: no video shown larger than its size."
                                   : L"Super Resolution requested for eligible video.";
    }
    return status.hdrOutput ? L"PQ output ready; RTX HDR requested for eligible SDR."
                            : L"RTX Video HDR is switching the output to 10-bit PQ.";
}

struct PaneView {
    ViewMode mode{ViewMode::Fit};
    float zoom{1.0F};
};

inline int dockInteractiveHeight(
    float progress, int dockHeight, int revealZone) {
    const float normalized = std::isfinite(progress)
        ? std::clamp(progress, 0.0F, 1.0F) : 0.0F;
    const int visible = static_cast<int>(
        std::lround(static_cast<float>(std::max(0, dockHeight)) * normalized));
    return std::max(std::max(0, revealZone), visible);
}

inline bool pointerInDockInteractiveBand(
    int pointerY, int clientHeight, float progress,
    int dockHeight, int revealZone) {
    if (clientHeight <= 0 || pointerY < 0 || pointerY >= clientHeight) return false;
    return pointerY >= clientHeight -
        dockInteractiveHeight(progress, dockHeight, revealZone);
}

inline float advanceOverlayAnimation(
    float current, bool visible, float elapsedMs, float durationMs = 180.0F) {
    current = std::clamp(current, 0.0F, 1.0F);
    const float target = visible ? 1.0F : 0.0F;
    const float step = std::max(0.0F, elapsedMs) / std::max(1.0F, durationMs);
    return target > current ? std::min(target, current + step)
                            : std::max(target, current - step);
}

// Drag-and-drop is append-first. Dynamic layouts stretch loaded panes over the
// whole client area, so hit-testing alone cannot distinguish "add another"
// from "replace this pane" while an empty slot still exists.
inline int incomingDropTarget(bool hasEmptySlot, int hitPane) {
    return hasEmptySlot ? -1 : hitPane;
}

inline bool supportsExpandedPaneCount(std::size_t activeCount) {
    return activeCount == 3 || activeCount == 5;
}

// Explicit focus is meaningful only for the two supported source counts whose
// existing geometry has one focus cell plus either two stacked panes or a 2x2
// side grid. Preserve it across additions/removals whenever that topology and
// the selected pane both remain valid.
inline int retainedExpandedPane(int expandedPane, const PaneArray<bool>& active) {
    if (expandedPane < 0 || expandedPane >= static_cast<int>(active.size()) ||
        !active[static_cast<std::size_t>(expandedPane)]) {
        return -1;
    }
    const auto activeCount = static_cast<std::size_t>(
        std::count(active.begin(), active.end(), true));
    return supportsExpandedPaneCount(activeCount) ? expandedPane : -1;
}

inline int centeredWindowStart(int ownerStart, int ownerEnd, int windowSize) {
    return ownerStart + std::max(0, (ownerEnd - ownerStart - windowSize) / 2);
}

inline bool sourceHasStarted(double timelineSeconds, double startDelaySeconds) {
    return timelineSeconds + 0.0005 >= std::max(0.0, startDelaySeconds);
}

inline double effectiveTimelineOffset(
    double startDelaySeconds, double adjustmentSeconds, double playbackRate = 1.0) {
    const double rate = std::clamp(playbackRate, 0.25, 4.0);
    return adjustmentSeconds - std::max(0.0, startDelaySeconds) * rate;
}

inline bool sourceIsActive(
    double timelineSeconds, double startDelaySeconds, double adjustmentSeconds = 0.0,
    double playbackRate = 1.0) {
    const double rate = std::clamp(playbackRate, 0.25, 4.0);
    return (timelineSeconds - std::max(0.0, startDelaySeconds)) * rate +
               adjustmentSeconds >= -0.0005;
}

// A linked seek may only move the master clock to (or after) a pane's start
// delay. A positive adjustment means that the pane is already this far into
// its source at that earliest master time, so smaller source targets cannot be
// represented without changing the pane's own adjustment.
inline bool linkedTimelineCanReachSourceTime(
    double sourceSeconds, double syncAdjustmentSeconds) {
    return std::isfinite(sourceSeconds) && std::isfinite(syncAdjustmentSeconds) &&
           std::max(0.0, sourceSeconds) + 0.0005 >= syncAdjustmentSeconds;
}

inline bool timelineMappingChanged(
    double beforeSeconds, double afterSeconds, double toleranceSeconds = 0.0005) {
    if (!std::isfinite(beforeSeconds) || !std::isfinite(afterSeconds)) return true;
    return std::abs(afterSeconds - beforeSeconds) >
           std::max(0.0, toleranceSeconds);
}

inline double sourceTime(
    double timelineSeconds, double startDelaySeconds, double syncAdjustmentSeconds = 0.0,
    double playbackRate = 1.0) {
    const double rate = std::clamp(playbackRate, 0.25, 4.0);
    return std::max(0.0, (timelineSeconds - std::max(0.0, startDelaySeconds)) * rate +
                             syncAdjustmentSeconds);
}

inline double timelineDuration(
    double sourceDuration, double startDelaySeconds, double syncAdjustmentSeconds = 0.0,
    double playbackRate = 1.0) {
    const double delay = std::max(0.0, startDelaySeconds);
    const double rate = std::clamp(playbackRate, 0.25, 4.0);
    return std::max(delay, delay + (std::max(0.0, sourceDuration) - syncAdjustmentSeconds) / rate);
}

// Async stream probing temporarily removes a replacement pane from the set of
// known durations. Keep the last stable deck extent during that interval so
// replacing the longest source cannot look like an immediate end-of-deck.
inline double stableTimelineDuration(
    double previousDuration, double readyDuration, bool sourceOpening) {
    const double previous = std::isfinite(previousDuration)
        ? std::max(0.0, previousDuration) : 0.0;
    const double ready = std::isfinite(readyDuration)
        ? std::max(0.0, readyDuration) : 0.0;
    return sourceOpening ? std::max(previous, ready) : ready;
}

enum class MasterTimelineEndAction { None, Restart, Pause };

inline MasterTimelineEndAction masterTimelineEndAction(
    double position, double duration, bool playing, bool sourceOpening,
    bool masterLoopActive, bool hasRepeatingSource, bool repeatAll) {
    if (sourceOpening || !playing || !std::isfinite(position) ||
        !std::isfinite(duration) || duration <= 0.0 || position < duration ||
        masterLoopActive || hasRepeatingSource) {
        return MasterTimelineEndAction::None;
    }
    return repeatAll ? MasterTimelineEndAction::Restart
                     : MasterTimelineEndAction::Pause;
}

inline double timelineForSourceTime(
    double sourceSeconds, double startDelaySeconds, double syncAdjustmentSeconds = 0.0,
    double playbackRate = 1.0) {
    const double delay = std::max(0.0, startDelaySeconds);
    const double rate = std::clamp(playbackRate, 0.25, 4.0);
    return std::max(delay, delay + (std::max(0.0, sourceSeconds) - syncAdjustmentSeconds) / rate);
}

inline int nextEligiblePane(int current, const PaneArray<bool>& eligible) {
    const int lastPane = static_cast<int>(kMaxPanes - 1);
    const int start = std::clamp(current, 0, lastPane);
    for (std::size_t step = 1; step <= kMaxPanes; ++step) {
        const int candidate = (start + static_cast<int>(step)) % static_cast<int>(kMaxPanes);
        if (eligible[static_cast<std::size_t>(candidate)]) return candidate;
    }
    return -1;
}

inline bool seekPreviewEligible(
    double frameSeconds, double targetSeconds, double maximumLeadSeconds = 15.0) {
    return std::isfinite(frameSeconds) && std::isfinite(targetSeconds) &&
           frameSeconds >= 0.0 && targetSeconds >= 0.0 &&
           frameSeconds <= targetSeconds &&
           targetSeconds - frameSeconds <= std::max(0.0, maximumLeadSeconds);
}

inline bool seekKeyframePreviewEligible(
    bool keyframe, double frameSeconds, double targetSeconds,
    double maximumLeadSeconds = 15.0) {
    return keyframe &&
           seekPreviewEligible(frameSeconds, targetSeconds, maximumLeadSeconds);
}

// A decoded frame represents the half-open presentation interval
// [framePts, framePts + frameDuration). It is still preroll only when that
// interval ends at or before the requested timestamp. A missing duration is
// deliberately conservative: keep discarding earlier timestamps and accept
// the first frame at or after the target instead of guessing a fixed window.
//
// PTS arithmetic stays in stream ticks so mixed frame rates and exact frame
// boundaries are not distorted by an arbitrary seconds tolerance or floating
// point rounding.
inline bool frameEndsAtOrBeforeSeekTarget(
    std::int64_t framePts, std::int64_t frameDuration,
    std::int64_t targetPts) {
    if (framePts >= targetPts) return false;
    if (frameDuration <= 0) return true;
    const auto distance = static_cast<std::uint64_t>(targetPts) -
                          static_cast<std::uint64_t>(framePts);
    return static_cast<std::uint64_t>(frameDuration) <= distance;
}

// Convert a user-visible time to the nearest stream tick. Truncation is not
// safe here: an exactly representable media boundary such as 2.001 seconds in
// a 1/1000 time base can arrive as 2000.9999999999998 after binary floating
// point division and would incorrectly select the preceding presentation
// interval. Invalid inputs map to the stream origin, and extreme values
// saturate before the integer conversion.
inline std::int64_t secondsToStreamPts(
    double seconds, int timeBaseNumerator, int timeBaseDenominator) {
    if (!std::isfinite(seconds) || timeBaseNumerator <= 0 ||
        timeBaseDenominator <= 0) {
        return 0;
    }
    const long double ticks =
        static_cast<long double>(seconds) *
        static_cast<long double>(timeBaseDenominator) /
        static_cast<long double>(timeBaseNumerator);
    if (!std::isfinite(ticks)) {
        return ticks < 0.0L ? std::numeric_limits<std::int64_t>::min()
                            : std::numeric_limits<std::int64_t>::max();
    }
    const long double rounded = std::round(ticks);
    if (rounded >= static_cast<long double>(
                       std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (rounded <= static_cast<long double>(
                       std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(rounded);
}

inline std::int64_t streamTimestampForSeconds(
    double seconds, std::int64_t streamStartPts,
    int timeBaseNumerator, int timeBaseDenominator) {
    const std::int64_t relative = secondsToStreamPts(
        seconds, timeBaseNumerator, timeBaseDenominator);
    constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    if (relative > 0 && streamStartPts > maximum - relative) return maximum;
    if (relative < 0 && streamStartPts < minimum - relative) return minimum;
    return streamStartPts + relative;
}

// Demuxing and reduced-quality preroll must not aim beyond the video stream
// merely because the container duration includes longer audio or padding.
// The exact presentation gate can retain the original request and use its EOF
// fallback, while decode work targets the final valid stream tick.
inline std::int64_t videoSeekDecodeTargetPts(
    std::int64_t requestedPts, std::int64_t streamStartPts,
    std::int64_t streamDurationPts) {
    if (streamDurationPts <= 0) return requestedPts;
    const std::int64_t durationMinusOne = streamDurationPts - 1;
    const std::int64_t maximum = INT64_MAX;
    const std::int64_t finalStreamPts =
        streamStartPts > maximum - durationMinusOne
            ? maximum
            : streamStartPts + durationMinusOne;
    return std::min(requestedPts, finalStreamPts);
}

inline double videoSeekFastDecodeUntilSeconds(
    std::int64_t decodeTargetPts, std::int64_t streamStartPts,
    double streamTimeBase, double qualityPrerollSeconds = 0.75) {
    if (decodeTargetPts <= streamStartPts || !std::isfinite(streamTimeBase) ||
        streamTimeBase <= 0.0 || !std::isfinite(qualityPrerollSeconds)) {
        return -1.0;
    }
    const auto distance = static_cast<std::uint64_t>(decodeTargetPts) -
                          static_cast<std::uint64_t>(streamStartPts);
    const double targetSeconds = static_cast<double>(distance) * streamTimeBase;
    if (!std::isfinite(targetSeconds)) return -1.0;
    const double preroll = std::max(0.0, qualityPrerollSeconds);
    return targetSeconds > preroll ? targetSeconds - preroll : -1.0;
}

inline bool shouldShowPaneTimeline(
    bool eligible, bool pointerFresh, bool pointerInsidePlayer,
    bool playerForeground, bool dragging) {
    return eligible &&
           (dragging || (pointerFresh && pointerInsidePlayer && playerForeground));
}

inline bool validLoop(double loopA, double loopB) {
    return std::isfinite(loopA) && std::isfinite(loopB) && loopA >= 0.0 && loopB > loopA + 0.001;
}

inline double loopedTime(double seconds, double loopA, double loopB, bool enabled) {
    seconds = std::max(0.0, seconds);
    if (!enabled || !validLoop(loopA, loopB) || seconds < loopB) return seconds;
    return loopA + std::fmod(seconds - loopA, loopB - loopA);
}

// An A-B loop whose B is unset (or not after A) runs from A to the end of
// whatever it loops: the source's duration for a pane loop, the timeline's
// for the master loop. With no usable end either, there is no loop.
inline double loopEnd(double loopA, double loopB, double duration) {
    if (validLoop(loopA, loopB)) return loopB;
    return std::isfinite(duration) ? duration : 0.0;
}

inline bool loopArmed(double loopA, double loopB, double duration) {
    return validLoop(loopA, loopEnd(loopA, loopB, duration));
}

struct RectF {
    float x{};
    float y{};
    float width{};
    float height{};
};

// The extensions the open dialog offers, so that stepping through a folder
// lands on exactly the files that could have been opened by hand.
inline bool isMediaExtension(std::wstring extension) {
    for (wchar_t& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    // The same list Explorer is told about (FileAssociations.hpp).
    return std::any_of(kVideoExtensions.begin(), kVideoExtensions.end(),
                       [&](const wchar_t* candidate) { return extension == candidate; });
}

// The subtitle files the player reads, which are also the ones Explorer is
// told it opens.
inline bool isSubtitleExtension(std::wstring extension) {
    for (wchar_t& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return std::any_of(kSubtitleExtensions.begin(), kSubtitleExtensions.end(),
                       [&](const wchar_t* candidate) { return extension == candidate; });
}

// Stepping past either end wraps, so a folder can be walked in one direction
// without stopping to turn around.
inline int stepFileIndex(int current, int count, int step) {
    if (count <= 0 || current < 0 || current >= count) return -1;
    const int wrapped = (current + step) % count;
    return wrapped < 0 ? wrapped + count : wrapped;
}

// One step of the frame drain performed while a source is held still, after an
// open or a seek: whether to take the queued frame, and whether to stop there.
//
// A source is never drained freely in this state -- sparse or malformed
// timestamps would otherwise let the picture keep moving for seconds after a
// pause -- so the drain stops as soon as it has something appropriate to show.
//
// The subtlety is the frame the decoder marked as a seek's destination. Its PTS
// may precede the target while its presentation interval still covers it, so
// stopping only on the timestamp test works just when the target happens to be
// within a few milliseconds of a frame boundary. Any other time the drain would
// replace it with the following frame, which carries no such mark, and a
// synchronized seek would wait for a frame that had already been consumed.
struct PausedFrameStep {
    bool accept{};
    bool lock{};
};

inline PausedFrameStep pausedFrameStep(
    double candidateSeconds, bool candidateIsExactSeekFrame, bool haveCurrent,
    double currentSeconds, double targetSeconds) {
    // The decoder selected this frame from its PTS interval. Honour that
    // stronger evidence even when a sparse/discontinuous stream starts beyond
    // the generic allowance, or when EOF promotes the same frame that was
    // already shown as the one permitted keyframe preview.
    if (candidateIsExactSeekFrame) return {true, true};
    // A non-exact timestamp that does not advance is treated as malformed.
    if (haveCurrent && candidateSeconds <= currentSeconds + 0.000001) return {false, true};
    // Nothing is shown yet, so a wider window is allowed for the first frame.
    if (candidateSeconds > targetSeconds + (haveCurrent ? 0.003 : 0.050)) return {false, true};
    return {true, candidateSeconds >= targetSeconds - 0.003};
}

// How many decode threads one source may open.
//
// FFmpeg's automatic setting gives a single decoder as many frame threads as
// the machine has cores. Frame threading duplicates the decoder context and
// keeps one in-flight frame buffer per thread, so four sources left on the
// automatic setting allocate dozens of threads and dozens of full-size frame
// buffers on one machine -- for 4K that is a large amount of memory spent on
// oversubscribed threads that the queue's backpressure keeps idle anyway.
// Budget the cores across the panes this player is built for instead.
//
// Hardware decoding does its work on the GPU and frame threads buy nothing
// there, but only strict Hardware mode guarantees no CPU frame is ever
// produced. Automatic keeps the software budget: it prefers D3D11VA yet still
// accepts software frames when the GPU cannot handle a profile, and starving
// that fallback of threads costs far more than a few unused threads do.
// Frame threads spend much of their life blocked on inter-frame dependencies,
// so a budget of one thread per core divided between the panes leaves the
// decoders idle rather than merely unhurried. Measured on four 4K sources in
// software mode, that budget held decoding to about two thirds of the source
// frame rate with every queue sitting empty. Allowing two threads per core
// before dividing keeps the panes from starving without returning to FFmpeg's
// automatic setting, which would give each of four decoders the whole machine.
inline int decodeThreadBudget(
    unsigned hardwareConcurrency, bool hardwareOnly,
    int simultaneousSources = static_cast<int>(kMaxPanes)) {
    if (hardwareOnly) return 1;
    const unsigned cores = std::max(1U, hardwareConcurrency);
    const unsigned sources = static_cast<unsigned>(std::max(1, simultaneousSources));
    return static_cast<int>(std::clamp(2U * cores / sources, 4U, 16U));
}

struct SurfaceSize {
    int width{};
    int height{};
    friend bool operator==(const SurfaceSize& left, const SurfaceSize& right) {
        return left.width == right.width && left.height == right.height;
    }
};

// Size of the intermediate surface a pane decodes or converts into.
//
// Both the hardware and the software path used to clamp this to a fixed
// 1920x1080, so a 4K source shown full-window on a 4K display was scaled down
// to 1080p and then magnified back by the sampler. The intermediate is now
// sized to what the pane actually displays.
//
// Two properties matter to the callers. The source aspect ratio is preserved
// exactly, because the renderer derives Fit and Fill geometry from these
// dimensions rather than from the decoded frame. And the scale is snapped to a
// small number of steps, so dragging a window edge cannot recreate the video
// processor and its textures on every pixel of movement.
// allowMagnify lets the intermediate grow past the source, in sixteenths so a
// window drag still recreates it rarely. That is only worth doing when the
// video processor's upscaler is better than the sampler's bilinear filter --
// with Super Resolution on, the processor must be the one to reach the
// pane's pixels or there is nothing for it to enhance.
inline SurfaceSize presentationSize(
    int sourceWidth, int sourceHeight, double targetWidth, double targetHeight,
    int maximumDimension = 3840, int scaleSteps = 8, bool allowMagnify = false) {
    if (sourceWidth <= 0 || sourceHeight <= 0) return {};
    scaleSteps = std::max(1, scaleSteps);
    const auto width = static_cast<double>(sourceWidth);
    const auto height = static_cast<double>(sourceHeight);

    // Cover the target in both axes; Fill and zoom sample a sub-rectangle, so
    // the larger of the two ratios is what the pane can actually resolve.
    double scale = 1.0;
    if (targetWidth > 0.0 && targetHeight > 0.0 && std::isfinite(targetWidth) &&
        std::isfinite(targetHeight)) {
        scale = std::max(targetWidth / width, targetHeight / height);
    }
    if (allowMagnify && scale > 1.0) {
        constexpr int magnifySteps = 16;
        scale = std::ceil(scale * magnifySteps) / magnifySteps;
    } else {
        // Magnifying into an intermediate copy would only cost bandwidth;
        // leave that to the sampler.
        scale = std::min(1.0, scale);
        scale = std::ceil(scale * scaleSteps) / scaleSteps;
        scale = std::clamp(scale, 1.0 / scaleSteps, 1.0);
    }

    // Bound the surface for sources far beyond the display, keeping the aspect.
    if (maximumDimension > 0) {
        const double limit = std::min(1.0, std::min(maximumDimension / (width * scale),
                                                    maximumDimension / (height * scale)));
        scale *= std::min(1.0, limit);
    }

    SurfaceSize result;
    result.width = std::max(2, static_cast<int>(width * scale)) & ~1;
    result.height = std::max(2, static_cast<int>(height * scale)) & ~1;
    return result;
}

inline RectF fitInside(const RectF& cell, int sourceWidth, int sourceHeight) {
    if (sourceWidth <= 0 || sourceHeight <= 0 || cell.width <= 0 || cell.height <= 0) {
        return cell;
    }
    const float sourceAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    const float cellAspect = cell.width / cell.height;
    RectF result = cell;
    if (sourceAspect > cellAspect) {
        result.height = cell.width / sourceAspect;
        result.y += (cell.height - result.height) * 0.5F;
    } else {
        result.width = cell.height * sourceAspect;
        result.x += (cell.width - result.width) * 0.5F;
    }
    return result;
}

// Where a pane's picture is on screen: the renderer's viewport, fitted inside
// the cell for Fit and the cell for Fill and Stretch. A zoom crops within it
// and does not move it; what lies outside it (Fit's bars) is never picture.
inline RectF visiblePictureRect(const RectF& cell, int sourceWidth, int sourceHeight, ViewMode mode) {
    return mode == ViewMode::Fit ? fitInside(cell, sourceWidth, sourceHeight) : cell;
}

// Where a pane's whole picture lies as the renderer draws it: fitted inside
// the cell for Fit, covering it (and running past it where Fill crops) for
// Fill, the cell itself for Stretch -- then grown about its centre by the
// pane's zoom. Subtitles are placed against this rectangle, as their script
// places them against the video's frame; only its part inside
// visiblePictureRect is on screen.
inline RectF pictureRect(const RectF& cell, int sourceWidth, int sourceHeight, ViewMode mode, float zoom) {
    RectF picture = cell;
    if (sourceWidth > 0 && sourceHeight > 0 && cell.width > 0.0F && cell.height > 0.0F) {
        const float sourceAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
        if (mode == ViewMode::Fit) {
            picture = fitInside(cell, sourceWidth, sourceHeight);
        } else if (mode == ViewMode::Fill) {
            if (sourceAspect > cell.width / cell.height) {
                picture.width = cell.height * sourceAspect;
                picture.x = cell.x + (cell.width - picture.width) * 0.5F;
            } else {
                picture.height = cell.width / sourceAspect;
                picture.y = cell.y + (cell.height - picture.height) * 0.5F;
            }
        }
    }
    const float scale = std::isfinite(zoom) ? std::clamp(zoom, 1.0F, 4.0F) : 1.0F;
    if (scale != 1.0F) {
        const float centreX = picture.x + picture.width * 0.5F;
        const float centreY = picture.y + picture.height * 0.5F;
        picture.width *= scale;
        picture.height *= scale;
        picture.x = centreX - picture.width * 0.5F;
        picture.y = centreY - picture.height * 0.5F;
    }
    return picture;
}

inline PaneArray<RectF> layoutCells(
    float width, float height, LayoutMode mode = LayoutMode::Grid2x2, int solo = -1) {
    PaneArray<RectF> cells{};
    if (solo >= 0 && solo < static_cast<int>(kMaxPanes)) {
        cells[static_cast<std::size_t>(solo)] = {0, 0, width, height};
        return cells;
    }
    if (mode == LayoutMode::SideBySide) {
        const float halfWidth = width * 0.5F;
        cells[0] = {0, 0, halfWidth, height};
        cells[1] = {halfWidth, 0, width - halfWidth, height};
    } else if (mode == LayoutMode::Row4) {
        const float itemWidth = width * 0.25F;
        for (std::size_t index = 0; index < kLegacyPaneCount; ++index) {
            const float left = itemWidth * static_cast<float>(index);
            const float right = index + 1 == kLegacyPaneCount ? width : left + itemWidth;
            cells[index] = {left, 0, right - left, height};
        }
    } else {
        const float halfWidth = width * 0.5F;
        const float halfHeight = height * 0.5F;
        cells[0] = {0, 0, halfWidth, halfHeight};
        cells[1] = {halfWidth, 0, width - halfWidth, halfHeight};
        cells[2] = {0, halfHeight, halfWidth, height - halfHeight};
        cells[3] = {halfWidth, halfHeight, width - halfWidth, height - halfHeight};
    }
    return cells;
}

inline PaneArray<RectF> quadCells(float width, float height, int solo = -1) {
    return layoutCells(width, height, LayoutMode::Grid2x2, solo);
}

// Dynamic GridPlayer-style geometry. LayoutMode is retained for session
// compatibility: Grid2x2 = Auto rows, SideBySide = one row, Row4 = two rows.
inline PaneArray<RectF> activeLayoutCells(
    float width, float height, const PaneArray<bool>& active,
    LayoutMode mode = LayoutMode::Grid2x2, int expandedPane = -1, int solo = -1,
    const PaneArray<float>& aspectRatios = {},
    AutoLayoutFocus autoFocus = AutoLayoutFocus::Dynamic,
    int autoFocusPane = -1) {
    PaneArray<RectF> cells{};
    if (solo >= 0 && solo < static_cast<int>(kMaxPanes) &&
        active[static_cast<std::size_t>(solo)]) {
        cells[static_cast<std::size_t>(solo)] = {0, 0, width, height};
        return cells;
    }
    PaneArray<int> panes{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < active.size(); ++index) {
        if (active[index]) panes[count++] = static_cast<int>(index);
    }
    if (count == 0) return cells;
    if (mode == LayoutMode::Column4) {
        const float itemHeight = height / static_cast<float>(count);
        for (std::size_t item = 0; item < count; ++item) {
            const float top = itemHeight * static_cast<float>(item);
            const float bottom = item + 1 == count ? height : top + itemHeight;
            cells[static_cast<std::size_t>(panes[item])] = {0, top, width, bottom - top};
        }
        return cells;
    }
    const bool oneRow = mode == LayoutMode::SideBySide ||
                        (mode == LayoutMode::Grid2x2 && count <= 2);
    if (oneRow) {
        const float itemWidth = width / static_cast<float>(count);
        for (std::size_t item = 0; item < count; ++item) {
            const float left = itemWidth * static_cast<float>(item);
            const float right = item + 1 == count ? width : left + itemWidth;
            cells[static_cast<std::size_t>(panes[item])] = {left, 0, right - left, height};
        }
        return cells;
    }
    int portraitFocus = -1;
    int landscapeFocus = -1;
    int portraitCount = 0;
    int landscapeCount = 0;
    for (std::size_t item = 0; item < count; ++item) {
        const int pane = panes[item];
        const float aspect = aspectRatios[static_cast<std::size_t>(pane)];
        if (aspect > 0.0F && aspect < 0.90F) {
            ++portraitCount;
            if (portraitFocus < 0) portraitFocus = pane;
        } else if (aspect >= 0.90F) {
            ++landscapeCount;
            if (landscapeFocus < 0) landscapeFocus = pane;
        }
    }
    const bool explicitFocus = supportsExpandedPaneCount(count) &&
                               expandedPane >= 0 &&
                               expandedPane < static_cast<int>(kMaxPanes) &&
                               active[static_cast<std::size_t>(expandedPane)];
    int focus = explicitFocus ? expandedPane : -1;
    bool tallFocus = mode == LayoutMode::PortraitStack;
    bool wideFocus = mode == LayoutMode::LandscapePair;
    if (mode == LayoutMode::Grid2x2 && !explicitFocus) {
        const bool lockedFocus = autoFocusPane >= 0 &&
                                 autoFocusPane < static_cast<int>(kMaxPanes) &&
                                 active[static_cast<std::size_t>(autoFocusPane)];
        if (lockedFocus && autoFocus == AutoLayoutFocus::Tall) {
            focus = autoFocusPane;
            tallFocus = true;
        } else if (lockedFocus && autoFocus == AutoLayoutFocus::Wide) {
            focus = autoFocusPane;
            wideFocus = true;
        } else if (autoFocus == AutoLayoutFocus::Dynamic && portraitCount == 1 &&
                   landscapeCount + portraitCount == static_cast<int>(count)) {
            focus = portraitFocus;
            tallFocus = true;
        } else if (autoFocus == AutoLayoutFocus::Dynamic && landscapeCount == 1 &&
                   landscapeCount + portraitCount == static_cast<int>(count)) {
            focus = landscapeFocus;
            wideFocus = true;
        }
    } else if (tallFocus && focus < 0) {
        focus = portraitFocus >= 0 ? portraitFocus : panes[0];
    } else if (wideFocus && focus < 0) {
        focus = landscapeFocus >= 0 ? landscapeFocus : panes[0];
    }
    if (explicitFocus && !wideFocus) tallFocus = true;

    if (count == 1) {
        cells[static_cast<std::size_t>(panes[0])] = {0, 0, width, height};
    } else if (count == 2) {
        const float halfHeight = height * 0.5F;
        cells[static_cast<std::size_t>(panes[0])] = {0, 0, width, halfHeight};
        cells[static_cast<std::size_t>(panes[1])] = {0, halfHeight, width, height - halfHeight};
    } else if (focus >= 0 && tallFocus) {
        const float focusWidth = width * (explicitFocus ? 0.50F :
                                          (count == 3 ? 0.42F :
                                           (count == 5 ? 0.30F : 0.46F)));
        cells[static_cast<std::size_t>(focus)] = {0, 0, focusWidth, height};
        const std::size_t remaining = count - 1;
        if (remaining == 4) {
            const float sideWidth = width - focusWidth;
            const float halfSideWidth = sideWidth * 0.5F;
            const float halfHeight = height * 0.5F;
            std::size_t grid = 0;
            for (std::size_t item = 0; item < count; ++item) {
                if (panes[item] == focus) continue;
                const bool right = grid % 2 != 0;
                const bool bottom = grid >= 2;
                cells[static_cast<std::size_t>(panes[item])] = {
                    focusWidth + (right ? halfSideWidth : 0.0F),
                    bottom ? halfHeight : 0.0F,
                    right ? sideWidth - halfSideWidth : halfSideWidth,
                    bottom ? height - halfHeight : halfHeight};
                ++grid;
            }
        } else {
            const float itemHeight = height / static_cast<float>(remaining);
            std::size_t row = 0;
            for (std::size_t item = 0; item < count; ++item) {
                if (panes[item] == focus) continue;
                const float top = itemHeight * static_cast<float>(row);
                const float bottom = row + 1 == remaining ? height : top + itemHeight;
                cells[static_cast<std::size_t>(panes[item])] = {
                    focusWidth, top, width - focusWidth, bottom - top};
                ++row;
            }
        }
    } else if (focus >= 0 && wideFocus) {
        const float focusHeight = height * (count == 3 ? 0.58F :
                                            (count == 5 ? 0.42F : 0.54F));
        cells[static_cast<std::size_t>(focus)] = {0, 0, width, focusHeight};
        const std::size_t remaining = count - 1;
        const float itemWidth = width / static_cast<float>(remaining);
        std::size_t column = 0;
        for (std::size_t item = 0; item < count; ++item) {
            if (panes[item] == focus) continue;
            const float left = itemWidth * static_cast<float>(column);
            const float right = column + 1 == remaining ? width : left + itemWidth;
            cells[static_cast<std::size_t>(panes[item])] = {
                left, focusHeight, right - left, height - focusHeight};
            ++column;
        }
    } else if (count == 3) {
        const float halfWidth = width * 0.5F;
        const float halfHeight = height * 0.5F;
        cells[static_cast<std::size_t>(panes[0])] = {0, 0, halfWidth, halfHeight};
        cells[static_cast<std::size_t>(panes[1])] = {halfWidth, 0, width - halfWidth, halfHeight};
        cells[static_cast<std::size_t>(panes[2])] = {0, halfHeight, width, height - halfHeight};
    } else if (count == 4) {
        const float halfWidth = width * 0.5F;
        const float halfHeight = height * 0.5F;
        for (std::size_t item = 0; item < count; ++item) {
            const bool right = item % 2 != 0;
            const bool bottom = item >= 2;
            cells[static_cast<std::size_t>(panes[item])] = {
                right ? halfWidth : 0.0F, bottom ? halfHeight : 0.0F,
                right ? width - halfWidth : halfWidth,
                bottom ? height - halfHeight : halfHeight};
        }
    } else {
        // Five panes use two aspect-aware rows instead of squeezing into a
        // five-column strip. Clamped source aspects keep extreme media useful
        // without allowing one ultrawide clip to starve its neighbours.
        constexpr std::size_t topCount = 3;
        const auto aspectWeight = [&](int pane) {
            const float value = aspectRatios[static_cast<std::size_t>(pane)];
            return std::clamp(value > 0.0F ? value : 1.0F, 0.55F, 2.40F);
        };
        float topWeight = 0.0F;
        float bottomWeight = 0.0F;
        for (std::size_t item = 0; item < count; ++item) {
            (item < topCount ? topWeight : bottomWeight) += aspectWeight(panes[item]);
        }
        const float topIdealHeight = width / std::max(0.1F, topWeight);
        const float bottomIdealHeight = width / std::max(0.1F, bottomWeight);
        const float topHeight = height * std::clamp(
            topIdealHeight / (topIdealHeight + bottomIdealHeight), 0.36F, 0.64F);
        const auto placeRow = [&](std::size_t first, std::size_t last,
                                  float top, float rowHeight, float totalWeight) {
            float left = 0.0F;
            for (std::size_t item = first; item < last; ++item) {
                const float right = item + 1 == last
                    ? width
                    : left + width * aspectWeight(panes[item]) / totalWeight;
                cells[static_cast<std::size_t>(panes[item])] = {
                    left, top, right - left, rowHeight};
                left = right;
            }
        };
        placeRow(0, topCount, 0.0F, topHeight, topWeight);
        placeRow(topCount, count, topHeight, height - topHeight, bottomWeight);
    }
    return cells;
}

// V1-V5 are display positions, not storage-array indices. Visible cells are
// ordered top-to-bottom and then left-to-right; inactive slots follow in their
// internal order so settings remain deterministic before media is loaded.
inline PaneArray<int> panesByDisplayPosition(
    const PaneArray<RectF>& cells) {
    PaneArray<int> order{};
    order.fill(-1);
    std::size_t visible = 0;
    for (std::size_t pane = 0; pane < cells.size(); ++pane) {
        if (cells[pane].width > 0.0F && cells[pane].height > 0.0F) {
            order[visible++] = static_cast<int>(pane);
        }
    }
    std::stable_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(visible),
        [&](int left, int right) {
            const auto& a = cells[static_cast<std::size_t>(left)];
            const auto& b = cells[static_cast<std::size_t>(right)];
            if (std::abs(a.y - b.y) > 0.5F) return a.y < b.y;
            if (std::abs(a.x - b.x) > 0.5F) return a.x < b.x;
            return left < right;
        });
    for (std::size_t pane = 0; pane < cells.size(); ++pane) {
        if (std::find(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(visible),
                      static_cast<int>(pane)) ==
            order.begin() + static_cast<std::ptrdiff_t>(visible)) {
            order[visible++] = static_cast<int>(pane);
        }
    }
    return order;
}

class PlaybackClock {
public:
    using Clock = std::chrono::steady_clock;

    double position() const {
        std::scoped_lock lock(mutex_);
        return positionLocked(Clock::now());
    }

    void play() {
        std::scoped_lock lock(mutex_);
        if (!playing_) {
            anchorWall_ = Clock::now();
            playing_ = true;
        }
    }

    void pause() {
        std::scoped_lock lock(mutex_);
        if (playing_) {
            anchorPosition_ = positionLocked(Clock::now());
            playing_ = false;
        }
    }

    void toggle() {
        if (isPlaying()) {
            pause();
        } else {
            play();
        }
    }

    void seek(double seconds) {
        std::scoped_lock lock(mutex_);
        anchorPosition_ = std::max(0.0, seconds);
        anchorWall_ = Clock::now();
        ++generation_;
    }

    void setRate(double rate) {
        std::scoped_lock lock(mutex_);
        const auto now = Clock::now();
        anchorPosition_ = positionLocked(now);
        anchorWall_ = now;
        rate_ = std::clamp(rate, 0.25, 4.0);
    }

    double rate() const {
        std::scoped_lock lock(mutex_);
        return rate_;
    }

    // A small multiplier on top of rate_, used to track the audio hardware's
    // own crystal. Re-anchors so that changing it never moves the current
    // position, only the speed at which it advances from here.
    void setSlew(double slew) {
        std::scoped_lock lock(mutex_);
        const double clamped = std::clamp(slew, 0.9, 1.1);
        if (clamped == slew_) return;
        const auto now = Clock::now();
        anchorPosition_ = positionLocked(now);
        anchorWall_ = now;
        slew_ = clamped;
    }

    double slew() const {
        std::scoped_lock lock(mutex_);
        return slew_;
    }

    bool isPlaying() const {
        std::scoped_lock lock(mutex_);
        return playing_;
    }

    std::uint64_t generation() const {
        std::scoped_lock lock(mutex_);
        return generation_;
    }

private:
    double positionLocked(Clock::time_point now) const {
        if (!playing_) {
            return anchorPosition_;
        }
        const std::chrono::duration<double> elapsed = now - anchorWall_;
        return anchorPosition_ + elapsed.count() * rate_ * slew_;
    }

    mutable std::mutex mutex_;
    Clock::time_point anchorWall_{Clock::now()};
    double anchorPosition_{};
    double rate_{1.0};
    double slew_{1.0};
    bool playing_{};
    std::uint64_t generation_{};
};

inline int paneAt(float x, float y, float width, float height,
                  LayoutMode mode = LayoutMode::Grid2x2, int solo = -1) {
    if (x < 0 || y < 0 || x >= width || y >= height || width <= 0 || height <= 0) {
        return -1;
    }
    const auto cells = layoutCells(width, height, mode, solo);
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells[index];
        if (cell.width > 0 && cell.height > 0 && x >= cell.x && y >= cell.y &&
            x < cell.x + cell.width && y < cell.y + cell.height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

inline int activePaneAt(float x, float y, float width, float height,
                        const PaneArray<bool>& active,
                        LayoutMode mode = LayoutMode::Grid2x2,
                        int expandedPane = -1, int solo = -1,
                        const PaneArray<float>& aspectRatios = {},
                        AutoLayoutFocus autoFocus = AutoLayoutFocus::Dynamic,
                        int autoFocusPane = -1) {
    if (x < 0 || y < 0 || x >= width || y >= height || width <= 0 || height <= 0) return -1;
    const auto cells = activeLayoutCells(
        width, height, active, mode, expandedPane, solo, aspectRatios,
        autoFocus, autoFocusPane);
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells[index];
        if (cell.width > 0 && cell.height > 0 && x >= cell.x && y >= cell.y &&
            x < cell.x + cell.width && y < cell.y + cell.height) return static_cast<int>(index);
    }
    return -1;
}

}  // namespace quaddeck
