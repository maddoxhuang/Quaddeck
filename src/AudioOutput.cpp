#include "AudioOutput.hpp"

#include "AudioDsp.hpp"

#include <windows.h>
#include <mmreg.h>
#include <xapo.h>
#include <xaudio2.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <sstream>
#include <thread>
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
// The most channels a voice is fed, 7.1; an output with more takes them in
// that layout and leaves its others to XAudio2's mix.
constexpr unsigned kMaxSourceChannels = 8;
constexpr unsigned kSevenOneMask = 0x63F;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelt out rather than linked for.
constexpr GUID kFloatSubtype{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// QuadDeck's own effect; the class id only has to be unique to it.
constexpr GUID kEffectsClass{0x6e1d2a8c, 0x4b7f, 0x4e0a, {0x9d, 0x3c, 0x51, 0xa2, 0xf0, 0xb7, 0xc9, 0x14}};

bool floatFormat(const WAVEFORMATEX* format) {
    if (!format || format->wBitsPerSample != 32) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
    return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat, kFloatSubtype) != FALSE;
}
}  // namespace

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

// Runs on XAudio2's own thread: it only records the failure.
class AudioOutput::EngineCallback final : public IXAudio2EngineCallback {
public:
    explicit EngineCallback(std::atomic<long>& error) : error_(error) {}
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override {
        stopped.store(true);
        error_.store(error);
    }
    // The engine has stopped processing: nothing queued will come back.
    std::atomic<bool> stopped{false};

private:
    std::atomic<long>& error_;
};

// The effects as an XAudio2 effect on the mastering voice, so a change is
// heard at once rather than after what the decoders have queued. In place,
// on XAudio2's thread; settings arrive from the window's thread and are
// picked up at the start of a pass.
class AudioOutput::EffectsXapo final : public IXAPO {
public:
    explicit EffectsXapo(unsigned channelMask) : mask_(channelMask) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IXAPO)) {
            *object = static_cast<IXAPO*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(XAPO_REGISTRATION_PROPERTIES** properties) override {
        if (!properties) return E_POINTER;
        auto* copy = static_cast<XAPO_REGISTRATION_PROPERTIES*>(XAPOAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
        if (!copy) return E_OUTOFMEMORY;
        *copy = {};
        copy->clsid = kEffectsClass;
        wcscpy_s(copy->FriendlyName, L"QuadDeck audio effects");
        wcscpy_s(copy->CopyrightInfo, L"QuadDeck");
        copy->MajorVersion = 1;
        copy->Flags = XAPO_FLAG_CHANNELS_MUST_MATCH | XAPO_FLAG_FRAMERATE_MUST_MATCH |
                      XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
                      XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED;
        copy->MinInputBufferCount = copy->MaxInputBufferCount = 1;
        copy->MinOutputBufferCount = copy->MaxOutputBufferCount = 1;
        *properties = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(const WAVEFORMATEX* output, const WAVEFORMATEX* requested,
                                                     WAVEFORMATEX** supported) override {
        return matchFormat(output, requested, supported);
    }
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(const WAVEFORMATEX* input, const WAVEFORMATEX* requested,
                                                      WAVEFORMATEX** supported) override {
        return matchFormat(input, requested, supported);
    }
    HRESULT STDMETHODCALLTYPE Initialize(const void*, UINT32) override { return S_OK; }
    void STDMETHODCALLTYPE Reset() override { processor_.reset(); }
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 inputCount, const XAPO_LOCKFORPROCESS_PARAMETERS* inputs,
                                             UINT32 outputCount, const XAPO_LOCKFORPROCESS_PARAMETERS* outputs) override {
        if (inputCount != 1 || outputCount != 1 || !inputs || !outputs || !inputs[0].pFormat) return E_INVALIDARG;
        const WAVEFORMATEX* format = inputs[0].pFormat;
        // An output wider than the processor's layouts passes untouched.
        usable_ = format->nChannels >= 1 && format->nChannels <= AudioEffectsProcessor::kMaxChannels;
        processor_.prepare(format->nSamplesPerSec, std::min<std::size_t>(format->nChannels, AudioEffectsProcessor::kMaxChannels), mask_);
        applied_ = ~0U;
        takeSettings();
        return S_OK;
    }
    void STDMETHODCALLTYPE UnlockForProcess() override {}
    void STDMETHODCALLTYPE Process(UINT32 inputCount, const XAPO_PROCESS_BUFFER_PARAMETERS* inputs,
                                   UINT32 outputCount, XAPO_PROCESS_BUFFER_PARAMETERS* outputs, BOOL enabled) override {
        if (inputCount != 1 || outputCount != 1 || !inputs || !outputs) return;
        takeSettings();
        outputs[0].ValidFrameCount = inputs[0].ValidFrameCount;
        outputs[0].BufferFlags = inputs[0].BufferFlags;
        if (!enabled || !usable_ || !inputs[0].pBuffer) return;
        auto* samples = static_cast<float*>(inputs[0].pBuffer);
        if (inputs[0].BufferFlags == XAPO_BUFFER_SILENT) {
            // Silence in, but the limiter's delay may still hold the last
            // 5 ms of sound: it plays out over silence rather than wait for
            // the next sound to push it out ahead of it.
            if (processor_.idle()) {
                processor_.rest();
                return;
            }
            std::fill_n(samples, static_cast<std::size_t>(inputs[0].ValidFrameCount) * processor_.channels(), 0.0F);
            outputs[0].BufferFlags = XAPO_BUFFER_VALID;
        }
        processor_.process(samples, inputs[0].ValidFrameCount);
    }
    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 frames) override { return frames; }
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 frames) override { return frames; }

    // From the window's thread.
    void setSettings(const AudioEffectSettings& settings) {
        std::scoped_lock lock(mutex_);
        pending_ = settings;
        version_.fetch_add(1);
    }

private:
    // The pass never waits for the window: a change that arrives while it
    // is being written is picked up by the next pass.
    void takeSettings() {
        const unsigned version = version_.load();
        if (version == applied_) return;
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return;
        processor_.configure(pending_);
        applied_ = version;
    }

    static HRESULT matchFormat(const WAVEFORMATEX* other, const WAVEFORMATEX* requested, WAVEFORMATEX** supported) {
        if (!other || !requested) return E_POINTER;
        if (floatFormat(requested) && requested->nChannels == other->nChannels &&
            requested->nSamplesPerSec == other->nSamplesPerSec) {
            return S_OK;
        }
        if (supported) {
            auto* nearest = static_cast<WAVEFORMATEX*>(XAPOAlloc(sizeof(WAVEFORMATEX)));
            if (nearest) {
                *nearest = {};
                nearest->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                nearest->nChannels = other->nChannels;
                nearest->nSamplesPerSec = other->nSamplesPerSec;
                nearest->wBitsPerSample = 32;
                nearest->nBlockAlign = static_cast<WORD>(nearest->nChannels * 4);
                nearest->nAvgBytesPerSec = nearest->nSamplesPerSec * nearest->nBlockAlign;
            }
            *supported = nearest;
        }
        return XAPO_E_FORMAT_UNSUPPORTED;
    }

    std::atomic<ULONG> references_{1};
    const unsigned mask_;
    bool usable_{};
    AudioEffectsProcessor processor_;
    std::mutex mutex_;
    AudioEffectSettings pending_;
    std::atomic<unsigned> version_{0};
    unsigned applied_{~0U};
};

namespace {
std::string failure(const char* call, HRESULT hr) {
    std::ostringstream out;
    out << call << " failed: 0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

using VoiceLocks = std::array<std::unique_lock<std::mutex>, kMaxPanes>;
}  // namespace

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    VoiceLocks locks;
    for (std::size_t pane = 0; pane < locks.size(); ++pane) locks[pane] = std::unique_lock(voiceMutexes_[pane]);
    close();
}

bool AudioOutput::initialize(const std::wstring& deviceId, bool deviceChannels) {
    return reopen(deviceId, deviceChannels);
}

bool AudioOutput::reopen(const std::wstring& deviceId, bool deviceChannels) {
    // Every voice's transaction lock for the whole exchange: a decoder
    // thread still inside submit waits for the new voices, and then finds
    // its generation refused.
    VoiceLocks locks;
    for (std::size_t pane = 0; pane < locks.size(); ++pane) locks[pane] = std::unique_lock(voiceMutexes_[pane]);
    close();
    return open(deviceId, deviceChannels);
}

// Under every voice's lock.
void AudioOutput::close() {
    const std::uint64_t watermark = AudioChunk::generationWatermark();
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        rejectedThroughGeneration_[pane] = std::max(rejectedThroughGeneration_[pane], watermark);
        if (!sources_[pane]) continue;
        sources_[pane]->Stop();
        sources_[pane]->FlushSourceBuffers();
    }
    // The flushed buffers come back through OnBufferEnd, which frees them,
    // on the engine's next pass or two; let that happen before the voices
    // go. An engine that stopped on an error sends nothing back.
    const bool stopped = engineCallback_ && engineCallback_->stopped.load();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (std::size_t pane = 0; pane < sources_.size() && !stopped; ++pane) {
        while (sources_[pane]) {
            XAUDIO2_VOICE_STATE state{};
            sources_[pane]->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0 || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (sources_[pane]) sources_[pane]->DestroyVoice();
        sources_[pane] = nullptr;
        playing_[pane] = false;
        timeline_[pane].clear();
        submittedFrames_[pane] = 0;
        playedOrigin_[pane] = 0;
        streamTempo_[pane] = 1.0;
    }
    if (mastering_) mastering_->DestroyVoice();
    mastering_ = nullptr;
    if (xapo_) xapo_->Release();
    xapo_ = nullptr;
    if (engine_) {
        if (engineCallback_) engine_->UnregisterForCallbacks(engineCallback_.get());
        engine_->Release();
    }
    engine_ = nullptr;
}

// Under every voice's lock, after close.
bool AudioOutput::open(const std::wstring& deviceId, bool deviceChannels) {
    requestedDevice_ = deviceId;
    openedRequested_ = false;
    const auto fail = [&](const char* call, HRESULT hr) {
        {
            std::scoped_lock lock(mutex_);
            error_ = failure(call, hr);
        }
        close();
        return false;
    };
    if (!callback_) callback_ = std::make_unique<VoiceCallback>();
    if (!engineCallback_) engineCallback_ = std::make_unique<EngineCallback>(criticalError_);
    engineCallback_->stopped.store(false);
    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        engine_ = nullptr;
        return fail("XAudio2Create", hr);
    }
    engine_->RegisterForCallbacks(engineCallback_.get());
    if (!deviceId.empty()) {
        hr = engine_->CreateMasteringVoice(&mastering_, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0,
                                           deviceId.c_str());
        if (FAILED(hr)) {
            mastering_ = nullptr;
            std::scoped_lock lock(mutex_);
            error_ = failure("CreateMasteringVoice on the chosen output", hr);
        }
    }
    if (!mastering_) {
        hr = engine_->CreateMasteringVoice(&mastering_);
        if (FAILED(hr)) {
            mastering_ = nullptr;
            return fail("CreateMasteringVoice", hr);
        }
        openedRequested_ = deviceId.empty();
    } else {
        openedRequested_ = true;
    }
    XAUDIO2_VOICE_DETAILS masteringDetails{};
    mastering_->GetVoiceDetails(&masteringDetails);
    masteringRate_ = masteringDetails.InputSampleRate > 0 ? masteringDetails.InputSampleRate : 48000;
    masteringChannels_ = std::max(1U, static_cast<unsigned>(masteringDetails.InputChannels));
    DWORD mask = 0;
    if (FAILED(mastering_->GetChannelMask(&mask))) mask = 0;
    masteringMask_ = mask;

    // The voices are fed stereo, or the output's own layout when it has
    // more channels and the user wants them.
    if (deviceChannels && masteringChannels_ > 2) {
        if (masteringChannels_ <= kMaxSourceChannels &&
            static_cast<unsigned>(std::popcount(masteringMask_)) == masteringChannels_) {
            sourceChannels_ = masteringChannels_;
            sourceMask_ = masteringMask_;
        } else {
            sourceChannels_ = kMaxSourceChannels;
            sourceMask_ = kSevenOneMask;
        }
    } else {
        sourceChannels_ = 2;
        sourceMask_ = 0x3;
    }

    xapo_ = new EffectsXapo(masteringMask_);
    XAUDIO2_EFFECT_DESCRIPTOR descriptor{static_cast<IXAPO*>(xapo_), TRUE, masteringChannels_};
    XAUDIO2_EFFECT_CHAIN chain{1, &descriptor};
    hr = mastering_->SetEffectChain(&chain);
    if (FAILED(hr)) {
        // The sound plays on without its effects.
        xapo_->Release();
        xapo_ = nullptr;
        std::scoped_lock lock(mutex_);
        error_ = failure("SetEffectChain", hr);
    }

    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = sourceChannels_ > 2 ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_IEEE_FLOAT;
    format.Format.nChannels = static_cast<WORD>(sourceChannels_);
    format.Format.nSamplesPerSec = kSourceSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * format.Format.wBitsPerSample / 8);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    if (sourceChannels_ > 2) {
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = sourceMask_;
        format.SubFormat = kFloatSubtype;
    }
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        hr = engine_->CreateSourceVoice(&sources_[pane], &format.Format, 0, 4.0F, callback_.get());
        if (FAILED(hr)) {
            sources_[pane] = nullptr;
            return fail("CreateSourceVoice", hr);
        }
        sources_[pane]->SetVolume(paneMuted_[pane] ? 0.0F : paneVolume_[pane]);
        applyRateLocked(pane);
    }
    mastering_->SetVolume(muted_.load() ? 0.0F : std::min(volume_.load(), 1.0F));
    applyEffects();
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
    // Decoded for another layout than the voices now take: its producer is
    // about to be sought again in the new one.
    if (chunk.channels != static_cast<int>(sourceChannels_)) return AudioSubmitResult::Stale;
    IXAudio2SourceVoice* source = sources_[pane];
    XAUDIO2_VOICE_STATE state{};
    source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued >= 24) {
        return AudioSubmitResult::Backpressure;
    }
    const auto frames = static_cast<std::uint32_t>(chunk.samples.size() / sourceChannels_);
    const double pts = chunk.pts;
    const double tempo = chunk.tempo > 0.0 ? chunk.tempo : 1.0;
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
    // A generation stretched to another tempo than the last starts on a
    // flushed voice, so the ratio changes before any of it plays.
    if (tempo != streamTempo_[pane]) {
        streamTempo_[pane] = tempo;
        applyRateLocked(pane);
    }
    if (frames > 0) {
        timeline_[pane].push_back({submittedFrames_[pane], frames, pts, tempo});
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
    // exactly how its source timestamps advance too. Sound stretched by the
    // decoder plays at the voice's own pace instead, each frame worth its
    // tempo in source time.
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
        static_cast<double>(played - span.framesBefore) * span.tempo / kSourceSampleRate;
    // SamplesPlayed reflects what the engine has processed, which leads what
    // the speakers emit by the device's buffer depth.
    return span.pts + offset - outputLatency();
}

double AudioOutput::outputLatency() const {
    if (!engine_) return 0.0;
    XAUDIO2_PERFORMANCE_DATA performance{};
    engine_->GetPerformanceData(&performance);
    // The effects' limiter holds the sound back by its lookahead too, which
    // the engine's own figure does not know of.
    const std::uint64_t lookahead = xapo_ && masteringChannels_ <= AudioEffectsProcessor::kMaxChannels
        ? AudioLimiter::delayFrames(masteringRate_) : 0;
    return static_cast<double>(performance.CurrentLatencyInSamples + lookahead) /
           std::max(1u, masteringRate_);
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
    if (mastering_) mastering_->SetVolume(muted ? 0.0F : std::min(volume_.load(), 1.0F));
}

void AudioOutput::setVolume(float volume) {
    const float previous = volume_.load();
    volume_.store(std::isfinite(volume) ? std::clamp(volume, 0.0F, 2.0F) : 1.0F);
    if (mastering_ && !muted_.load()) mastering_->SetVolume(std::min(volume_.load(), 1.0F));
    if (std::max(previous, 1.0F) != std::max(volume_.load(), 1.0F)) applyEffects();
}

void AudioOutput::setEffects(const AudioEffectSettings& effects) {
    effects_ = clampAudioEffects(effects);
    effects_.boost = 1.0F;
    applyEffects();
}

void AudioOutput::applyEffects() {
    if (!xapo_) return;
    AudioEffectSettings settings = effects_;
    settings.boost = std::max(volume_.load(), 1.0F);
    xapo_->setSettings(settings);
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
    rate_[pane] = std::clamp(rate, 0.25F, 4.0F);
    applyRateLocked(pane);
}

void AudioOutput::applyRateLocked(std::size_t pane) {
    if (!sources_[pane]) return;
    const double ratio = static_cast<double>(rate_[pane]) / std::max(streamTempo_[pane], 0.25);
    sources_[pane]->SetFrequencyRatio(static_cast<float>(std::clamp(ratio, 0.25, 4.0)));
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
