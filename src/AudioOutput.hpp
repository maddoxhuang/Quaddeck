#pragma once

#include "VideoSource.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace quaddeck {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool initialize();
    AudioSubmitResult submit(std::size_t pane, AudioChunk&& chunk);
    void play();
    void play(std::size_t pane);
    void pause();
    void pause(std::size_t pane);
    void flush();
    void flush(std::size_t pane);
    // Explicit generation form for callers that disable the producer and
    // schedule its replacement seek before flushing. Buffers from
    // minimumAcceptedGeneration and newer remain eligible after the flush;
    // every older in-flight chunk is stale.
    void flush(std::size_t pane, std::uint64_t minimumAcceptedGeneration);
    void setMuted(bool muted);
    void setVolume(float volume);
    // Per-pane trim, applied on that pane's own source voice. The mastering
    // voice keeps master volume and mute, so the two multiply rather than
    // fight: silencing one pane leaves the others where the user put them.
    void setPaneVolume(std::size_t pane, float volume);
    void setPaneMuted(std::size_t pane, bool muted);
    void swapPaneSettings(std::size_t first, std::size_t second);
    float paneVolume(std::size_t pane) const;
    bool paneMuted(std::size_t pane) const;
    void setRate(std::size_t pane, float rate);
    bool hasQueuedAudio(std::size_t pane) const;
    // Source-local time the device is currently emitting for this pane, with
    // the engine's own latency taken off, or a negative value when it cannot
    // be determined. This is the measurement the master clock tracks.
    double playbackPosition(std::size_t pane);
    bool muted() const { return muted_.load(); }
    float volume() const { return volume_.load(); }
    std::string error() const;

private:
    class VoiceCallback;
    std::unique_ptr<VoiceCallback> callback_;
    IXAudio2* engine_{};
    IXAudio2MasteringVoice* mastering_{};
    PaneArray<IXAudio2SourceVoice*> sources_{};

    // What was handed to each voice, so that the device's running sample count
    // can be turned back into a source timestamp. XAUDIO2_VOICE_STATE also
    // reports the playing buffer's context pointer, but that pointer is owned
    // by the callback that frees it, so the mapping is kept here instead.
    struct PlaybackSpan {
        std::uint64_t framesBefore{};
        std::uint32_t frames{};
        double pts{};
    };
    PaneArray<std::deque<PlaybackSpan>> timeline_{};
    PaneArray<std::uint64_t> submittedFrames_{};
    PaneArray<std::uint64_t> playedOrigin_{};
    // XAudio2 accepts thread-safe voice calls, but flush is a logical
    // transaction spanning Stop, FlushSourceBuffers and timeline rebasing.
    // Serialize the whole transaction with submit/readback for each voice.
    mutable PaneArray<std::mutex> voiceMutexes_{};
    PaneArray<std::uint64_t> rejectedThroughGeneration_{};
    unsigned masteringRate_{48000};

    void applyPaneVolume(std::size_t pane);
    void flushLocked(std::size_t pane, std::uint64_t rejectedThroughGeneration);

    std::atomic<bool> muted_{false};
    std::atomic<float> volume_{1.0F};
    // Written only from the UI thread, which is the only caller that changes
    // them; the voices themselves are thread-safe.
    PaneArray<float> paneVolume_{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    PaneArray<bool> paneMuted_{};
    // XAudio2 voices survive media replacement. Track their Start/Stop state
    // so the render tick can safely ensure a newly-ready pane is audible
    // without issuing Start on an already-running voice every frame.
    PaneArray<bool> playing_{};
    mutable std::mutex mutex_;
    std::string error_;
};

}  // namespace quaddeck
