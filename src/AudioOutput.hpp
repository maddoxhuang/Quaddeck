#pragma once

#include "AudioSettings.hpp"
#include "VideoSource.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

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

    // Opens the engine on an output by its Windows id, Windows' default
    // output for an empty one or one that will not open. With
    // deviceChannels the voices take as many channels as the output has, up
    // to eight; otherwise two.
    bool initialize(const std::wstring& deviceId = {}, bool deviceChannels = false);
    // Every voice and the engine go, and new ones come up on the output
    // given. Whatever was queued is dropped and every generation decoded so
    // far is refused from then on, so the caller stops its producers first
    // and seeks each again afterwards, in the voices' new layout. Volumes,
    // mutes, rates and effects carry over.
    bool reopen(const std::wstring& deviceId, bool deviceChannels);
    bool isOpen() const { return mastering_ != nullptr; }
    // The output asked for at the last open, and whether it was the one
    // that opened rather than Windows' default in its place.
    const std::wstring& requestedDevice() const { return requestedDevice_; }
    bool openedRequestedDevice() const { return openedRequested_; }
    // The output's own channels and the layout the voices are fed in.
    unsigned deviceChannelCount() const { return masteringChannels_; }
    unsigned sourceChannels() const { return sourceChannels_; }
    unsigned sourceChannelMask() const { return sourceMask_; }

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
    // 0 to 2: up to 1 on the mastering voice, past it as a gain in the
    // effects ahead of their limiter.
    void setVolume(float volume);
    // The mix's effects; the boost in them is the volume's.
    void setEffects(const AudioEffectSettings& effects);
    const AudioEffectSettings& effects() const { return effects_; }
    // Per-pane trim, applied on that pane's own source voice. The mastering
    // voice keeps master volume and mute, so the two multiply rather than
    // fight: silencing one pane leaves the others where the user put them.
    void setPaneVolume(std::size_t pane, float volume);
    void setPaneMuted(std::size_t pane, bool muted);
    void swapPaneSettings(std::size_t first, std::size_t second);
    float paneVolume(std::size_t pane) const;
    bool paneMuted(std::size_t pane) const;
    // The pane's playback speed. Sound the decoder already stretched to it
    // (AudioChunk::tempo) plays at its own pace, the rest is sped up by the
    // voice.
    void setRate(std::size_t pane, float rate);
    bool hasQueuedAudio(std::size_t pane) const;
    // Source-local time the device is currently emitting for this pane, with
    // the engine's own latency taken off, or a negative value when it cannot
    // be determined. This is the measurement the master clock tracks.
    double playbackPosition(std::size_t pane);
    // Seconds from a sample being handed to a running voice to the speakers
    // emitting it: the engine's latency, which playbackPosition takes off.
    double outputLatency() const;
    bool muted() const { return muted_.load(); }
    float volume() const { return volume_.load(); }
    std::string error() const;
    // The failure XAudio2 stopped on since the last call (the device it plays
    // to went away, for one), or 0. The engine does not start again by
    // itself: every pane is silent until the caller reopens it.
    long takeCriticalError() { return criticalError_.exchange(0); }

private:
    class VoiceCallback;
    class EngineCallback;
    class EffectsXapo;
    std::unique_ptr<VoiceCallback> callback_;
    std::unique_ptr<EngineCallback> engineCallback_;
    std::atomic<long> criticalError_{0};
    IXAudio2* engine_{};
    IXAudio2MasteringVoice* mastering_{};
    EffectsXapo* xapo_{};
    PaneArray<IXAudio2SourceVoice*> sources_{};

    // What was handed to each voice, so that the device's running sample count
    // can be turned back into a source timestamp. XAUDIO2_VOICE_STATE also
    // reports the playing buffer's context pointer, but that pointer is owned
    // by the callback that frees it, so the mapping is kept here instead.
    struct PlaybackSpan {
        std::uint64_t framesBefore{};
        std::uint32_t frames{};
        double pts{};
        // Source seconds per frame are this over the sample rate: above 1
        // for sound the decoder stretched to a faster speed.
        double tempo{1.0};
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
    unsigned masteringChannels_{2};
    unsigned masteringMask_{0x3};
    unsigned sourceChannels_{2};
    unsigned sourceMask_{0x3};
    std::wstring requestedDevice_;
    bool openedRequested_{};

    bool open(const std::wstring& deviceId, bool deviceChannels);
    void close();
    void applyPaneVolume(std::size_t pane);
    void applyRateLocked(std::size_t pane);
    void applyEffects();
    void flushLocked(std::size_t pane, std::uint64_t rejectedThroughGeneration);

    std::atomic<bool> muted_{false};
    std::atomic<float> volume_{1.0F};
    AudioEffectSettings effects_;
    // Written only from the UI thread, which is the only caller that changes
    // them; the voices themselves are thread-safe.
    PaneArray<float> paneVolume_{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    PaneArray<bool> paneMuted_{};
    // The speed each pane plays at and the tempo its queued sound was
    // stretched to; the voice makes up the difference. Under the voice's
    // mutex.
    PaneArray<float> rate_{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    PaneArray<double> streamTempo_{1.0, 1.0, 1.0, 1.0, 1.0};
    // XAudio2 voices survive media replacement. Track their Start/Stop state
    // so the render tick can safely ensure a newly-ready pane is audible
    // without issuing Start on an already-running voice every frame.
    PaneArray<bool> playing_{};
    mutable std::mutex mutex_;
    std::string error_;
};

}  // namespace quaddeck
