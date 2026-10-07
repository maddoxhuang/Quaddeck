#pragma once

// Synchronization rules, free of Win32, D3D11 and FFmpeg.
//
// The seek barrier and the audio handoff are the two pieces of QuadDeck whose
// behaviour is hardest to reason about and easiest to break, and they used to
// live directly inside App as two dozen parallel member arrays. Expressing them
// here against a plain description of each pane keeps them observable from the
// core test binary, which has neither a window nor a decoder.

#include "Core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace quaddeck {

// Tolerance for deciding that a source has reached its own end. A frame time
// this close to the duration is treated as finished rather than playable.
inline constexpr double kSourceEndEpsilon = 0.030;

// Everything the synchronization rules need to know about one pane.
struct PaneTiming {
    bool loaded{};
    bool ready{};
    bool paused{};
    double pausedTime{};
    double duration{};
    double startDelay{};
    double adjustment{};
    double rate{1.0};
    bool loopEnabled{};
    double loopA{};
    double loopB{};
    bool autoRepeat{};
};

using PaneTimings = PaneArray<PaneTiming>;

// Where a pane's video came from decides which playback rules it follows.
// Manual covers everything opened as part of the deck itself: dropped,
// Open files, the command line, a session. LocalBrowser is an F6 Play or
// Add with its own folder queue. EmbyVideo includes a restored locator still
// being resolved. An Emby photo shows a still and joins neither policy.
enum class PaneOrigin : std::uint8_t { Empty, Manual, LocalBrowser, EmbyVideo, EmbyPhoto };

using PaneOrigins = PaneArray<PaneOrigin>;

// Shared: one master timeline carries every pane (Linked or Independent per
// the saved seek mode). PerPane: any browser-managed pane gives every pane
// its own timeline, queue and end, whatever the saved mode says; the saved
// mode is kept for when the deck is manual again.
enum class DeckTimeline : std::uint8_t { Shared, PerPane };

inline bool browserManaged(PaneOrigin origin) {
    return origin == PaneOrigin::LocalBrowser || origin == PaneOrigin::EmbyVideo;
}

inline std::size_t loadedPaneCount(const PaneOrigins& origins) {
    std::size_t count = 0;
    for (const auto origin : origins) count += origin != PaneOrigin::Empty ? 1 : 0;
    return count;
}

inline DeckTimeline deckTimeline(const PaneOrigins& origins) {
    for (const auto origin : origins) {
        if (browserManaged(origin)) return DeckTimeline::PerPane;
    }
    return DeckTimeline::Shared;
}

// The seek mode actually in force. One video on a shared deck is the master
// timeline whatever was saved; an empty deck shows the saved choice.
inline SeekMode effectiveSeekMode(const PaneOrigins& origins, SeekMode saved) {
    if (deckTimeline(origins) == DeckTimeline::PerPane) return SeekMode::Independent;
    return loadedPaneCount(origins) == 1 ? SeekMode::Linked : saved;
}

// Whether a pane's saved whole-source repeat flag may take effect.
// paneRepeats and mappedPaneTime still require the effective mode to be
// Independent. Manual panes on a per-pane deck keep the rule they had on a
// shared one: their flag counts only among several manual panes under a saved
// Independent mode, so adding a browser pane never suddenly wraps an ended
// manual source. The only video, from wherever it came, ends by "When the
// only video ends", which is also what its repeat chip shows and toggles;
// its own flag waits until another pane joins.
inline bool paneRepeatFlagInForce(const PaneOrigins& origins, std::size_t pane, SeekMode saved) {
    if (pane >= origins.size()) return false;
    const auto origin = origins[pane];
    if (browserManaged(origin) && loadedPaneCount(origins) <= 1) return false;
    if (deckTimeline(origins) == DeckTimeline::PerPane &&
        (origin == PaneOrigin::Manual || origin == PaneOrigin::Empty)) {
        std::size_t manual = 0;
        for (const auto other : origins) manual += other == PaneOrigin::Manual ? 1 : 0;
        return manual > 1 && saved == SeekMode::Independent;
    }
    return true;
}

// The pane whose own time the transport bar and the title show, or -1 when
// they show the master clock. The only video from F6 or Emby keeps its own
// origin under a master clock that went on counting through whatever that
// pane played before it, so the master time is not where this video is. One
// manual video is the master timeline itself, and several share it. A lone
// Emby photo keeps showing its own (empty) time, as it always has.
inline int barTimelinePane(const PaneOrigins& origins) {
    if (loadedPaneCount(origins) != 1) return -1;
    for (std::size_t pane = 0; pane < origins.size(); ++pane) {
        if (browserManaged(origins[pane]) || origins[pane] == PaneOrigin::EmbyPhoto) {
            return static_cast<int>(pane);
        }
    }
    return -1;
}

// State of one audio seek generation. Primed means that at least one buffer
// for the destination generation has been accepted by the audio sink. Ended
// means the stream reached EOF before producing destination audio. Error,
// including a source with no usable audio stream, is terminal for the same
// reason: it must not hold otherwise ready video panes until a fail-safe.
enum class SeekAudioState : std::uint8_t {
    Unknown,
    Primed,
    Ended,
    Error,
};

using PaneSeekAudioStates = PaneArray<SeekAudioState>;

inline bool paneSelected(unsigned audioMask, std::size_t pane) {
    return pane < kMaxPanes && (audioMask & (1U << pane)) != 0;
}

// A pane loop with only an A point runs to the source's end and back to A.
// It cannot wrap without a known duration, so until then it is not a repeat.
inline bool paneRepeats(const PaneTiming& pane, SeekMode seek) {
    return (pane.loopEnabled && loopArmed(pane.loopA, pane.loopB, pane.duration)) ||
           (seek == SeekMode::Independent && pane.autoRepeat);
}

inline bool paneIsActive(const PaneTiming& pane, double timeline) {
    return sourceIsActive(timeline, pane.startDelay, pane.adjustment, pane.rate);
}

inline double mappedPaneTime(const PaneTiming& pane, double timeline, SeekMode seek) {
    if (pane.paused) return std::max(0.0, pane.pausedTime);
    const double raw = sourceTime(timeline, pane.startDelay, pane.adjustment, pane.rate);
    if (pane.loopEnabled) {
        return loopedTime(raw, pane.loopA, loopEnd(pane.loopA, pane.loopB, pane.duration), true);
    }
    // Whole-source auto repeat needs a known duration to wrap against.
    if (seek == SeekMode::Independent && pane.autoRepeat && pane.ready && pane.duration > 0.0) {
        return loopedTime(raw, 0.0, pane.duration, true);
    }
    return raw;
}

// A repeating source never runs out, and a source of unknown duration is
// assumed to still have content.
inline bool paneBeforeEnd(const PaneTiming& pane, double timeline, SeekMode seek) {
    if (paneRepeats(pane, seek) || pane.duration <= 0.0) return true;
    return mappedPaneTime(pane, timeline, seek) + kSourceEndEpsilon < pane.duration;
}

inline bool shouldOutputPaneAudio(const PaneTiming& pane, bool audioSelected,
                                  double timeline, bool playing, SeekMode seek) {
    if (!playing || !audioSelected || !pane.loaded || !pane.ready || pane.paused ||
        !paneIsActive(pane, timeline)) {
        return false;
    }
    return paneBeforeEnd(pane, timeline, seek);
}

enum class ReadyAlignmentAction {
    None,
    Wait,
    Clear,
    SeekPane,
    RefreshBarrier,
};

// Opening starts before FFmpeg knows duration, so the first request may use a
// provisional, unwrapped local target. Once metadata settles, choose exactly
// one correction without ever replacing one generation owned by a global
// synchronization barrier.
inline ReadyAlignmentAction readyAlignmentAction(
    bool pending, bool sourceExists, bool ready, bool failed,
    bool barrierActive, double provisionalTarget, double settledTarget) {
    if (!pending) return ReadyAlignmentAction::None;
    if (!sourceExists || failed) return ReadyAlignmentAction::Clear;
    if (!ready) return ReadyAlignmentAction::Wait;
    if (!timelineMappingChanged(provisionalTarget, settledTarget)) {
        return ReadyAlignmentAction::Clear;
    }
    return barrierActive ? ReadyAlignmentAction::RefreshBarrier
                         : ReadyAlignmentAction::SeekPane;
}

// When a selected source finishes before the master timeline does, its audio
// bit moves to the next pane that still has something to play.
struct AudioHandoff {
    int from{-1};
    int to{-1};
    bool valid() const { return from >= 0 && to >= 0; }
};

inline AudioHandoff audioHandoff(
    const PaneTimings& panes, unsigned audioMask, double timeline, SeekMode seek,
    PaneSeekAudioStates currentAudioStates = {}) {
    for (std::size_t current = 0; current < panes.size(); ++current) {
        const auto& finished = panes[current];
        if (!paneSelected(audioMask, current) || !finished.loaded || !finished.ready ||
            !paneIsActive(finished, timeline)) {
            continue;
        }
        // currentAudioStates must describe each source's latest generation;
        // callers map a stale observation to Unknown. An errored generation
        // cannot progress without a fresh seek, but the natural EOF of a
        // repeating pane is temporary: its wrap seek creates that fresh audio
        // generation. Handing the bit away just before the wrap would make the
        // next cycle permanently silent.
        const bool repeats = paneRepeats(finished, seek);
        const bool audioFinished =
            currentAudioStates[current] == SeekAudioState::Error ||
            (currentAudioStates[current] == SeekAudioState::Ended && !repeats);
        const bool videoFinished = !repeats && finished.duration > 0.0 &&
                                   !paneBeforeEnd(finished, timeline, seek);
        if (!audioFinished && !videoFinished) continue;

        PaneArray<bool> eligible{};
        for (std::size_t index = 0; index < panes.size(); ++index) {
            const auto& candidate = panes[index];
            const bool candidateAudioFinished =
                currentAudioStates[index] == SeekAudioState::Ended ||
                currentAudioStates[index] == SeekAudioState::Error;
            if (paneSelected(audioMask, index) || !candidate.loaded || !candidate.ready ||
                candidate.paused || candidateAudioFinished || candidate.duration <= 0.0 ||
                !paneIsActive(candidate, timeline)) {
                continue;
            }
            eligible[index] = paneBeforeEnd(candidate, timeline, seek);
        }
        const int replacement = nextEligiblePane(static_cast<int>(current), eligible);
        if (replacement < 0) continue;
        return {static_cast<int>(current), replacement};
    }
    return {};
}

// Audio clock tracking.
//
// The master clock counts steady_clock time while the sound hardware runs on
// its own crystal. The two disagree by tens of parts per million: inaudible
// over a minute, a visible lip-sync error over an hour. Rather than promoting
// audio to the master clock -- the per-pane offsets, the seek barrier and the
// master A-B loop all rely on the master being free-running -- the clock's
// rate is trimmed so it follows whichever pane is actually being heard.
//
// Proportional control is enough here. Writing e for the error and k for the
// gain, the clock advances at 1 + k*e while the device advances at 1 + d for
// some rate mismatch d, so de/dt = d - k*e. The error decays exponentially
// with time constant 1/k and settles at d/k, which for a 50ppm device and the
// gain below is well under a millisecond. A constant offset -- an unmodelled
// latency, say -- is absorbed exactly once instead of accumulating.
inline constexpr double kMaximumClockSlew = 0.005;    // 0.5%, far below visible
inline constexpr double kClockDriftGain = 0.05;       // ~20s to close an error
inline constexpr double kMaximumCorrectableDrift = 0.5;

struct ClockSlew {
    double factor{1.0};
    bool corrected{};
};

inline ClockSlew audioDriftSlew(
    double audioSeconds, double expectedSeconds, double playbackRate = 1.0,
    double gain = kClockDriftGain, double maximumSlew = kMaximumClockSlew,
    double maximumDrift = kMaximumCorrectableDrift) {
    if (!std::isfinite(audioSeconds) || !std::isfinite(expectedSeconds) ||
        audioSeconds < 0.0) {
        return {};
    }
    // Both times are source-local; dividing by the pane's rate expresses the
    // error in master-clock seconds, which is what is being trimmed.
    const double rate = std::clamp(playbackRate, 0.25, 4.0);
    const double error = (audioSeconds - expectedSeconds) / rate;
    // A discontinuity this large is a seek, a loop wrap or a glitch. Those
    // belong to the seek machinery; correcting for them here would yank the
    // picture instead of nudging it.
    if (std::abs(error) > maximumDrift) return {};
    return {std::clamp(1.0 + gain * error, 1.0 - maximumSlew, 1.0 + maximumSlew), true};
}

// A global seek freezes the master clock until every participating pane has
// presented its own exact destination frame, so the picture and the audio of
// all participating panes resume on the same render pass instead of drifting in one at
// a time.
class SeekBarrier {
public:
    // A source that never delivers its exact frame must not deadlock playback.
    static constexpr long long kTimeoutMs = 30000;

    using AudioState = SeekAudioState;

    // Per-pane facts the barrier cannot know by itself: they live in the
    // decoder and in the audio device.
    struct PaneStatus {
        bool loaded{};
        bool errored{};
        std::uint64_t audioGeneration{};
        AudioState audioState{AudioState::Unknown};
    };

    // Reported once per pane so the caller can log each transition exactly one
    // time without keeping its own bookkeeping.
    struct FrameEvent {
        bool firstFrame{};
        bool exactFrame{};
    };

    // Returns true when at least one pane has to be waited for. A barrier that
    // nobody waits on should be completed immediately by the caller.
    bool begin(double target, bool resume, const PaneTimings& panes,
               unsigned audioMask, SeekMode seek) {
        reset();
        active_ = true;
        resume_ = resume;
        target_ = target;
        bool anyWait = false;
        for (std::size_t pane = 0; pane < panes.size(); ++pane) {
            if (!panes[pane].loaded) continue;
            auto& state = panes_[pane];
            state.target = mappedPaneTime(panes[pane], target, seek);
            state.wait = !panes[pane].paused && paneBeforeEnd(panes[pane], target, seek) &&
                         paneIsActive(panes[pane], target);
            state.audioWait = resume && paneSelected(audioMask, pane) && state.wait;
            anyWait = anyWait || state.wait;
        }
        return anyWait;
    }

    void reset() {
        active_ = false;
        resume_ = false;
        timedOut_ = false;
        target_ = 0.0;
        panes_.fill(PaneWait{});
    }

    // Replacing one source invalidates only its seek ticket. Other panes
    // still wait on the generations recorded by the same global seek.
    void detachPane(std::size_t pane) {
        if (pane < panes_.size()) panes_[pane] = PaneWait{};
    }

    bool active() const { return active_; }
    bool resume() const { return resume_; }
    void setResume(bool value) { resume_ = value; }
    double target() const { return target_; }
    bool timedOut() const { return timedOut_; }

    bool waiting(std::size_t pane) const { return pane < panes_.size() && panes_[pane].wait; }
    bool audioWaiting(std::size_t pane) const {
        return pane < panes_.size() && panes_[pane].audioWait;
    }
    void setAudioWaiting(std::size_t pane, bool value) {
        if (pane < panes_.size()) panes_[pane].audioWait = value;
    }
    double paneTarget(std::size_t pane) const {
        return pane < panes_.size() ? panes_[pane].target : 0.0;
    }
    void setGeneration(std::size_t pane, std::uint64_t generation) {
        if (pane < panes_.size()) panes_[pane].generation = generation;
    }
    void setAudioGeneration(std::size_t pane, std::uint64_t generation) {
        if (pane < panes_.size()) panes_[pane].audioGeneration = generation;
    }

    void noteElapsed(long long elapsedMs) {
        if (active_ && elapsedMs >= kTimeoutMs) timedOut_ = true;
    }

    // Only a frame carrying this pane's recorded generation and marked exact
    // satisfies the barrier: a frame decoded for a superseded request, or a
    // keyframe published as a seek preview, must not release it.
    FrameEvent observeFrame(std::size_t pane, bool haveFrame,
                            std::uint64_t frameGeneration, bool exactFrame) {
        FrameEvent event;
        if (pane >= panes_.size() || !panes_[pane].wait) return event;
        auto& state = panes_[pane];
        if (haveFrame && !state.firstFrameLogged) {
            state.firstFrameLogged = true;
            event.firstFrame = true;
        }
        if (haveFrame && exactFrame && frameGeneration == state.generation) {
            state.frameReady = true;
            if (!state.exactLogged) {
                state.exactLogged = true;
                event.exactFrame = true;
            }
        }
        return event;
    }

    bool ready(long long elapsedMs, const PaneArray<PaneStatus>& status) const {
        if (!active_) return false;
        if (elapsedMs >= kTimeoutMs) return true;
        for (std::size_t pane = 0; pane < panes_.size(); ++pane) {
            if (!panes_[pane].wait || !status[pane].loaded) continue;
            // A pane that has failed cannot contribute a frame; waiting for one
            // would hold the other panes hostage.
            if (status[pane].errored) continue;
            if (!panes_[pane].frameReady) return false;
            if (panes_[pane].audioWait) {
                // Audio state is meaningful only for the seek that established
                // this barrier. A terminal or primed result from an older seek
                // must not release a newer one after its buffers were flushed.
                if (panes_[pane].audioGeneration == 0 ||
                    status[pane].audioGeneration != panes_[pane].audioGeneration ||
                    status[pane].audioState == AudioState::Unknown) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    struct PaneWait {
        bool wait{};
        bool audioWait{};
        bool frameReady{};
        bool firstFrameLogged{};
        bool exactLogged{};
        double target{};
        std::uint64_t generation{};
        std::uint64_t audioGeneration{};
    };

    bool active_{};
    bool resume_{};
    bool timedOut_{};
    double target_{};
    PaneArray<PaneWait> panes_{};
};

}  // namespace quaddeck
