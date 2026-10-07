#include "AudioOutput.hpp"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace quaddeck {

namespace {
struct AudioPayload {
    std::vector<float> samples;
};
// Source voices are fed at this rate; the mastering voice may run at whatever
// the device prefers, which is why the two are tracked separately.
constexpr unsigned kSourceSampleRate = 48000;
constexpr unsigned kChannels = 2;
}

class AudioOutput::VoiceCallback final : public IXAudio2VoiceCallback {
public:
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
        delete static_cast<AudioPayload*>(context);
    }
};

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        std::scoped_lock lock(voiceMutexes_[pane]);
        auto*& source = sources_[pane];
        if (!source) continue;
        source->Stop();
        source->FlushSourceBuffers();
        source->DestroyVoice();
        source = nullptr;
    }
    if (mastering_) {
        mastering_->DestroyVoice();
    }
    if (engine_) {
        engine_->Release();
    }
}

bool AudioOutput::initialize() {
    callback_ = std::make_unique<VoiceCallback>();
    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        std::ostringstream out;
        out << "XAudio2Create failed: 0x" << std::hex << static_cast<unsigned long>(hr);
        error_ = out.str();
        return false;
    }
    hr = engine_->CreateMasteringVoice(&mastering_);
    if (FAILED(hr)) {
        error_ = "CreateMasteringVoice failed";
        return false;
    }
    XAUDIO2_VOICE_DETAILS masteringDetails{};
    mastering_->GetVoiceDetails(&masteringDetails);
    if (masteringDetails.InputSampleRate > 0) masteringRate_ = masteringDetails.InputSampleRate;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(kChannels);
    format.nSamplesPerSec = kSourceSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        hr = engine_->CreateSourceVoice(&sources_[pane], &format, 0, 4.0F, callback_.get());
        if (FAILED(hr)) {
            error_ = "CreateSourceVoice failed";
            return false;
        }
        applyPaneVolume(pane);
    }
    mastering_->SetVolume(1.0F);
    return true;
}

AudioSubmitResult AudioOutput::submit(std::size_t pane, AudioChunk&& chunk) {
    if (pane >= sources_.size() || chunk.samples.empty()) {
        return AudioSubmitResult::Error;
    }
    std::scoped_lock lock(voiceMutexes_[pane]);
    // flush() samples the process-wide generation watermark while holding this
    // same lock. Therefore an old submission is linearized either before the
    // flush (and physically removed by XAudio2) or after it (and rejected
    // here); it can never refill a freshly flushed persistent voice.
    if (chunk.generation == 0 ||
        chunk.generation <= rejectedThroughGeneration_[pane]) {
        return AudioSubmitResult::Stale;
    }
    if (!sources_[pane]) return AudioSubmitResult::Error;
    IXAudio2SourceVoice* source = sources_[pane];
    XAUDIO2_VOICE_STATE state{};
    source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued >= 24) {
        return AudioSubmitResult::Backpressure;
    }
    const auto frames = static_cast<std::uint32_t>(chunk.samples.size() / kChannels);
    const double pts = chunk.pts;
    auto* payload = new AudioPayload{std::move(chunk.samples)};
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(payload->samples.size() * sizeof(float));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(payload->samples.data());
    buffer.pContext = payload;
    const HRESULT hr = source->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        {
            std::scoped_lock errorLock(mutex_);
            std::ostringstream out;
            out << "SubmitSourceBuffer failed: 0x" << std::hex
                << static_cast<unsigned long>(hr);
            error_ = out.str();
        }
        delete payload;
        return AudioSubmitResult::Error;
    }
    if (frames > 0) {
        timeline_[pane].push_back({submittedFrames_[pane], frames, pts});
        submittedFrames_[pane] += frames;
    }
    return AudioSubmitResult::Accepted;
}

double AudioOutput::playbackPosition(std::size_t pane) {
    if (pane >= sources_.size()) return -1.0;
    std::scoped_lock lock(voiceMutexes_[pane]);
    if (!sources_[pane]) return -1.0;
    XAUDIO2_VOICE_STATE state{};
    // SamplesPlayed counts samples consumed at the source voice's own input
    // rate, so a pane playing at 2x advances it twice as fast -- which is
    // exactly how its source timestamps advance too.
    sources_[pane]->GetState(&state, 0);
    if (timeline_[pane].empty() || state.SamplesPlayed < playedOrigin_[pane]) return -1.0;
    const std::uint64_t played = state.SamplesPlayed - playedOrigin_[pane];
    while (timeline_[pane].size() > 1) {
        const auto& front = timeline_[pane].front();
        if (played < front.framesBefore + front.frames) break;
        timeline_[pane].pop_front();
    }
    const auto& span = timeline_[pane].front();
    if (played < span.framesBefore) return -1.0;
    const double offset =
        static_cast<double>(played - span.framesBefore) / kSourceSampleRate;
    double seconds = span.pts + offset;
    if (engine_) {
        // SamplesPlayed reflects what the engine has processed, which leads
        // what the speakers emit by the device's buffer depth.
        XAUDIO2_PERFORMANCE_DATA performance{};
        engine_->GetPerformanceData(&performance);
        seconds -= static_cast<double>(performance.CurrentLatencyInSamples) /
                   std::max(1u, masteringRate_);
    }
    return seconds;
}

void AudioOutput::play() {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) play(pane);
}

void AudioOutput::play(std::size_t pane) {
    if (pane >= sources_.size()) return;
    std::scoped_lock lock(voiceMutexes_[pane]);
    if (!sources_[pane] || playing_[pane]) return;
    if (SUCCEEDED(sources_[pane]->Start())) playing_[pane] = true;
}

void AudioOutput::pause() {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) pause(pane);
}

void AudioOutput::pause(std::size_t pane) {
    if (pane >= sources_.size()) return;
    std::scoped_lock lock(voiceMutexes_[pane]);
    if (!sources_[pane] || !playing_[pane]) return;
    if (SUCCEEDED(sources_[pane]->Stop())) playing_[pane] = false;
}

void AudioOutput::flush() {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) flush(pane);
}

void AudioOutput::flush(std::size_t pane) {
    if (pane >= sources_.size()) return;
    std::scoped_lock lock(voiceMutexes_[pane]);
    flushLocked(pane, AudioChunk::generationWatermark());
}

void AudioOutput::flush(std::size_t pane,
                        std::uint64_t minimumAcceptedGeneration) {
    if (pane >= sources_.size()) return;
    std::scoped_lock lock(voiceMutexes_[pane]);
    const std::uint64_t rejectedGeneration = minimumAcceptedGeneration == 0
        ? 0 : minimumAcceptedGeneration - 1;
    flushLocked(pane, rejectedGeneration);
}

void AudioOutput::flushLocked(std::size_t pane,
                              std::uint64_t rejectedGeneration) {
    rejectedThroughGeneration_[pane] = std::max(
        rejectedThroughGeneration_[pane], rejectedGeneration);
    if (!sources_[pane]) return;
    sources_[pane]->Stop();
    playing_[pane] = false;
    sources_[pane]->FlushSourceBuffers();
    // Everything recorded for this voice described buffers that will now never
    // play. Re-base the accounting on the device's current count so the next
    // submission maps correctly whether or not the flush reset it.
    XAUDIO2_VOICE_STATE state{};
    sources_[pane]->GetState(&state, 0);
    timeline_[pane].clear();
    submittedFrames_[pane] = 0;
    playedOrigin_[pane] = state.SamplesPlayed;
}

void AudioOutput::setMuted(bool muted) {
    muted_.store(muted);
    if (mastering_) mastering_->SetVolume(muted ? 0.0F : volume_.load());
}

void AudioOutput::setVolume(float volume) {
    volume_.store(std::clamp(volume, 0.0F, 1.0F));
    if (mastering_ && !muted_.load()) mastering_->SetVolume(volume_.load());
}

void AudioOutput::applyPaneVolume(std::size_t pane) {
    if (pane >= sources_.size()) return;
    std::scoped_lock lock(voiceMutexes_[pane]);
    if (!sources_[pane]) return;
    sources_[pane]->SetVolume(paneMuted_[pane] ? 0.0F : paneVolume_[pane]);
}

void AudioOutput::setPaneVolume(std::size_t pane, float volume) {
    if (pane >= sources_.size()) return;
    paneVolume_[pane] = std::clamp(volume, 0.0F, 1.0F);
    applyPaneVolume(pane);
}

void AudioOutput::setPaneMuted(std::size_t pane, bool muted) {
    if (pane >= sources_.size()) return;
    paneMuted_[pane] = muted;
    applyPaneVolume(pane);
}

void AudioOutput::swapPaneSettings(std::size_t first, std::size_t second) {
    if (first >= sources_.size() || second >= sources_.size() || first == second) return;
    std::swap(paneVolume_[first], paneVolume_[second]);
    std::swap(paneMuted_[first], paneMuted_[second]);
    applyPaneVolume(first);
    applyPaneVolume(second);
}

float AudioOutput::paneVolume(std::size_t pane) const {
    return pane < paneVolume_.size() ? paneVolume_[pane] : 1.0F;
}

bool AudioOutput::paneMuted(std::size_t pane) const {
    return pane < paneMuted_.size() && paneMuted_[pane];
}

void AudioOutput::setRate(std::size_t pane, float rate) {
    if (pane >= sources_.size()) return;
    std::scoped_lock lock(voiceMutexes_[pane]);
    if (!sources_[pane]) return;
    sources_[pane]->SetFrequencyRatio(std::clamp(rate, 0.25F, 4.0F));
}

bool AudioOutput::hasQueuedAudio(std::size_t pane) const {
    if (pane >= sources_.size()) return false;
    std::scoped_lock lock(voiceMutexes_[pane]);
    if (!sources_[pane]) return false;
    XAUDIO2_VOICE_STATE state{};
    sources_[pane]->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued > 0;
}

std::string AudioOutput::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

}  // namespace quaddeck
