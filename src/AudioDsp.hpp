#pragma once

// What the sound goes through apart from any device: the effects the whole
// mix passes on its way to the speakers, and the stretch that keeps the
// pitch when a video plays at another speed. Pure: no device, no window,
// no clock.

#include "AudioSettings.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace quaddeck {

namespace audio_dsp_detail {

inline constexpr double kPi = 3.14159265358979323846;

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) { return 20.0 * std::log10(std::max(gain, 1e-12)); }

// One second-order section's coefficients, normalized.
struct Biquad {
    double b0{1.0}, b1{}, b2{}, a1{}, a2{};

    // The cookbook's peaking filter: gainDb at frequency, falling off either
    // side as q says.
    static Biquad peaking(double sampleRate, double frequency, double q, double gainDb) {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * frequency / sampleRate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cosine = std::cos(w0);
        const double a0 = 1.0 + alpha / a;
        Biquad f;
        f.b0 = (1.0 + alpha * a) / a0;
        f.b1 = -2.0 * cosine / a0;
        f.b2 = (1.0 - alpha * a) / a0;
        f.a1 = f.b1;
        f.a2 = (1.0 - alpha / a) / a0;
        return f;
    }
};

// Its memory for one channel, transposed direct form II.
struct BiquadState {
    double z1{}, z2{};
    double run(const Biquad& f, double x) {
        const double y = f.b0 * x + z1;
        z1 = f.b1 * x - f.a1 * y + z2;
        z2 = f.b2 * x - f.a2 * y;
        return y;
    }
};

// The channel a speaker is in, in a buffer whose channels follow the mask's
// bits from the lowest; -1 when the layout has no such speaker.
inline int speakerChannel(unsigned mask, unsigned speaker, std::size_t channels) {
    if ((mask & speaker) == 0) return -1;
    const int index = std::popcount(mask & (speaker - 1U));
    return static_cast<std::size_t>(index) < channels ? index : -1;
}

}  // namespace audio_dsp_detail

// A lookahead limiter: the sound waits 5 ms in a delay while its peaks are
// looked at. The gain each frame needs to stay under the ceiling is held as
// the least over the 5 ms ahead of it and 15 ms after, drops at once and
// comes back over 100 ms, and is smoothed by two moving averages 5 ms long
// between them. What meets a frame leaving the delay is then an average of
// gains none above what that frame needs, so nothing passes the ceiling,
// and the gain moves in an S-curve across the 5 ms rather than in one step:
// a step on a loud wave is a click, and a gain that follows each peak
// flattens its top into a buzz. Below the ceiling, once any gain it took has
// come back, it is a delay and nothing else; the gain rests at exactly 1.
class AudioLimiter {
public:
    static constexpr double kCeiling = 0.98;

    // The lookahead, and the sound's delay through it, at a rate.
    static std::size_t delayFrames(double sampleRate) {
        return std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(0.005 * sampleRate)));
    }

    void prepare(double sampleRate, std::size_t channels) {
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_ = std::max<std::size_t>(1, channels);
        delay_ = delayFrames(rate);
        // The averages' lengths less one add up to the delay, so the frames
        // they reach over are the frame leaving and the delay's worth after.
        first_.assign(delay_ / 2 + 1, 1.0);
        second_.assign(delay_ + 2 - first_.size(), 1.0);
        window_ = delay_ + 1 + static_cast<std::size_t>(std::lround(0.015 * rate));
        release_ = std::exp(-1.0 / (0.100 * rate));
        line_.assign(delay_ * channels_, 0.0F);
        leastFrame_.assign(window_ + 1, 0);
        leastGain_.assign(window_ + 1, 1.0);
        reset();
    }

    void reset() {
        std::fill(line_.begin(), line_.end(), 0.0F);
        linePosition_ = 0;
        quietFrames_ = delay_;
        rested_ = false;
        rest();
    }

    std::size_t latency() const { return delay_; }

    // Only silence in the delay: silence is all that would come out.
    bool idle() const { return quietFrames_ >= delay_; }

    // The gain back at 1 at once. Only while idle, when it meets nothing,
    // so the next sound is not met by a gain still coming back up.
    void rest() {
        if (rested_) return;
        leastHead_ = leastCount_ = 0;
        envelope_ = 1.0;
        std::fill(first_.begin(), first_.end(), 1.0);
        std::fill(second_.begin(), second_.end(), 1.0);
        firstPosition_ = secondPosition_ = 0;
        firstSum_ = static_cast<double>(first_.size());
        secondSum_ = static_cast<double>(second_.size());
        firstOnes_ = first_.size();
        secondOnes_ = second_.size();
        sinceSum_ = 0;
        rested_ = true;
    }

    // In place: `frames` interleaved frames, which come out the delay later.
    // A sample that is not a number goes in as silence.
    void process(float* samples, std::size_t frames) {
        const std::size_t slots = window_ + 1;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            float* x = samples + frame * channels_;
            double peak = 0.0;
            bool silent = true;
            for (std::size_t c = 0; c < channels_; ++c) {
                if (!std::isfinite(x[c])) x[c] = 0.0F;
                peak = std::max(peak, static_cast<double>(std::abs(x[c])));
                silent = silent && x[c] == 0.0F;
            }
            const double needed = peak > kCeiling ? kCeiling / peak : 1.0;
            // The least gain any frame in the window needs, by a queue whose
            // gains rise from its head.
            while (leastCount_ > 0 && leastGain_[(leastHead_ + leastCount_ - 1) % slots] >= needed) --leastCount_;
            const std::size_t slot = (leastHead_ + leastCount_) % slots;
            leastFrame_[slot] = frameCount_;
            leastGain_[slot] = needed;
            ++leastCount_;
            while (leastFrame_[leastHead_] + window_ <= frameCount_) {
                leastHead_ = (leastHead_ + 1) % slots;
                --leastCount_;
            }
            const double least = leastGain_[leastHead_];
            envelope_ = least < envelope_ ? least : release_ * envelope_ + (1.0 - release_) * least;
            if (envelope_ > 1.0 - 1e-12) envelope_ = 1.0;
            const double smoothed = average(first_, firstPosition_, firstSum_, firstOnes_, envelope_);
            const double gain = average(second_, secondPosition_, secondSum_, secondOnes_, smoothed);
            // The running sums are worked out afresh now and then, so their
            // rounding cannot build up.
            if (++sinceSum_ == 65536) {
                sinceSum_ = 0;
                firstSum_ = std::accumulate(first_.begin(), first_.end(), 0.0);
                secondSum_ = std::accumulate(second_.begin(), second_.end(), 0.0);
            }
            float* held = line_.data() + linePosition_ * channels_;
            for (std::size_t c = 0; c < channels_; ++c) {
                const float in = x[c];
                x[c] = gain == 1.0 ? held[c] : static_cast<float>(held[c] * gain);
                held[c] = in;
            }
            linePosition_ = (linePosition_ + 1) % delay_;
            quietFrames_ = silent ? std::min(quietFrames_ + 1, delay_) : 0;
            ++frameCount_;
        }
        if (frames > 0) rested_ = false;
    }

private:
    // One moving average: a value in, the mean of the last few out. A run
    // of exact ones the average's length gives exactly one.
    static double average(std::vector<double>& values, std::size_t& position, double& sum,
                          std::size_t& ones, double value) {
        sum += value - values[position];
        values[position] = value;
        position = (position + 1) % values.size();
        ones = value == 1.0 ? std::min(ones + 1, values.size()) : 0;
        if (ones == values.size()) {
            sum = static_cast<double>(values.size());
            return 1.0;
        }
        return sum / static_cast<double>(values.size());
    }

    std::size_t channels_{2};
    std::size_t delay_{2};
    std::size_t window_{3};
    double release_{};
    std::vector<float> line_;
    std::size_t linePosition_{};
    std::size_t quietFrames_{};
    std::vector<std::uint64_t> leastFrame_;
    std::vector<double> leastGain_;
    std::size_t leastHead_{}, leastCount_{};
    std::uint64_t frameCount_{};
    double envelope_{1.0};
    std::vector<double> first_, second_;
    std::size_t firstPosition_{}, secondPosition_{};
    double firstSum_{}, secondSum_{};
    std::size_t firstOnes_{}, secondOnes_{};
    std::size_t sinceSum_{};
    bool rested_{};
};

// The effects on the mix, in the order a signal meets them: what happens to
// left and right, the voice brought forward, the equalizer, the headphone
// crossfeed, the widening, night mode's compressor, the normalizer, the
// volume past 100%, and a limiter that keeps all of that from clipping.
// The limiter is always there, so turning the rest on and off never moves
// the sound in time; with nothing else to do, and the mix under its
// ceiling once any gain it took has come back, the samples come out exactly
// as they came, its 5 ms later. A mix
// over it -- several panes at once, a hot master, a downmix -- is limited
// whether anything else is on or not.
class AudioEffectsProcessor {
public:
    static constexpr std::size_t kMaxChannels = 8;
    // Speaker bits as Windows' channel masks have them.
    static constexpr unsigned kFrontLeft = 0x1, kFrontRight = 0x2, kFrontCenter = 0x4,
                              kBackLeft = 0x10, kBackRight = 0x20, kSideLeft = 0x200, kSideRight = 0x400;

    // The format the samples will come in. A mask of 0 reads two channels as
    // left and right.
    void prepare(double sampleRate, std::size_t channels, unsigned channelMask) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_ = std::clamp<std::size_t>(channels, 1, kMaxChannels);
        using audio_dsp_detail::speakerChannel;
        if (channelMask == 0 && channels_ >= 2) channelMask = kFrontLeft | kFrontRight;
        pairCount_ = 0;
        const std::array<std::array<unsigned, 2>, 3> pairs{{
            {kFrontLeft, kFrontRight}, {kBackLeft, kBackRight}, {kSideLeft, kSideRight}}};
        for (const auto& pair : pairs) {
            const int left = speakerChannel(channelMask, pair[0], channels_);
            const int right = speakerChannel(channelMask, pair[1], channels_);
            if (left >= 0 && right >= 0) pairs_[pairCount_++] = {left, right};
        }
        center_ = speakerChannel(channelMask, kFrontCenter, channels_);
        limiter_.prepare(sampleRate_, channels_);
        configure(settings_);
        reset();
    }

    void configure(const AudioEffectSettings& requested) {
        using namespace audio_dsp_detail;
        const AudioEffectSettings settings = clampAudioEffects(requested);
        const bool stereo = pairCount_ > 0;
        eqBandCount_ = 0;
        if (settings.equalizer) {
            for (std::size_t band = 0; band < kEqualizerBands; ++band) {
                if (settings.bands[band] == 0.0F || kEqualizerFrequencies[band] >= sampleRate_ * 0.45) {
                    // A band left out forgets what it held, as a stage
                    // switched off does, so moving it off 0 starts clean.
                    for (auto& channel : eqState_) channel[band] = {};
                    continue;
                }
                eqFilters_[band] = Biquad::peaking(sampleRate_, kEqualizerFrequencies[band], 1.414, settings.bands[band]);
                eqBands_[eqBandCount_++] = band;
            }
        }
        preampGain_ = settings.equalizer ? dbToGain(settings.preamp) : 1.0;
        const bool equalizer = eqBandCount_ > 0 || preampGain_ != 1.0;
        voiceFilter_ = Biquad::peaking(sampleRate_, 1500.0, 0.7, 6.0);
        configureCrossfeed();
        const double attack = std::exp(-1.0 / (0.005 * sampleRate_));
        const double release = std::exp(-1.0 / (0.200 * sampleRate_));
        compressorAttack_ = attack;
        compressorRelease_ = release;
        if (settings.compression == 1) compressor_ = {-24.0, 2.0, 6.0, 5.0};
        else compressor_ = {-30.0, 4.0, 8.0, 10.0};
        normalizerMeasure_ = std::exp(-1.0 / (0.4 * sampleRate_));
        normalizerFall_ = std::exp(-static_cast<double>(kNormalizerBlock) / (0.3 * sampleRate_));
        normalizerRise_ = std::exp(-static_cast<double>(kNormalizerBlock) / (3.0 * sampleRate_));
        widening_ = settings.widening == 1 ? 1.4 : settings.widening == 2 ? 1.9 : 1.0;
        boostTarget_ = settings.boost;

        // A stage switched off forgets what it held, so switching it on
        // again starts clean.
        if (!equalizer) clearEqualizer();
        if (!settings.voiceEmphasis) voiceState_ = {};
        if (!settings.crossfeed) crossfeed_ = {};
        if (settings.compression == 0) compressorEnvelope_ = 0.0;
        if (!settings.normalize) clearNormalizer();

        settings_ = settings;
        equalizerOn_ = equalizer;
        stereoOn_ = stereo && settings.stereo != AudioStereoMode::Stereo;
        crossfeedOn_ = stereo && settings.crossfeed;
        wideningOn_ = stereo && settings.widening > 0;
        voiceOn_ = settings.voiceEmphasis && (stereo || center_ >= 0);
        active_ = equalizerOn_ || stereoOn_ || crossfeedOn_ || wideningOn_ || voiceOn_ ||
                  settings.compression > 0 || settings.normalize || settings.boost > 1.0F ||
                  boostCurrent_ > 1.0;
    }

    const AudioEffectSettings& settings() const { return settings_; }
    // Whether anything but the limiter is at work.
    bool active() const { return active_; }
    std::size_t channels() const { return channels_; }
    // How many frames later the sound comes out: the limiter's lookahead.
    std::size_t latency() const { return limiter_.latency(); }
    // Silence in, and only silence left in the limiter's delay: the pass
    // can be skipped, and the limiter set at rest for what comes next.
    bool idle() const { return limiter_.idle(); }
    void rest() { limiter_.rest(); }

    // In place: `frames` frames of interleaved floats in the prepared format.
    void process(float* samples, std::size_t frames) {
        if (frames == 0) return;
        if (active_) applyEffects(samples, frames);
        limiter_.process(samples, frames);
    }

    void reset() {
        resetEffects();
        limiter_.reset();
    }


private:
    static constexpr std::size_t kNormalizerBlock = 64;
    static constexpr double kNormalizerTargetDb = -20.0;
    static constexpr double kNormalizerRangeDb = 12.0;
    static constexpr double kNormalizerGateDb = -60.0;

    struct Compressor {
        double threshold, ratio, knee, makeup;
    };
    struct CrossfeedState {
        std::array<double, 2> low{}, high{}, previous{};
    };

    void applyEffects(float* samples, std::size_t frames) {
        using namespace audio_dsp_detail;
        const std::size_t channels = channels_;
        const double boostStep = (boostTarget_ - boostCurrent_) / static_cast<double>(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            float* x = samples + frame * channels;
            if (stereoOn_) applyStereoMode(x);
            if (voiceOn_) emphasizeVoice(x);
            if (equalizerOn_) {
                for (std::size_t c = 0; c < channels; ++c) {
                    double value = x[c] * preampGain_;
                    for (std::size_t i = 0; i < eqBandCount_; ++i) {
                        const std::size_t band = eqBands_[i];
                        value = eqState_[c][band].run(eqFilters_[band], value);
                    }
                    x[c] = static_cast<float>(value);
                }
            }
            if (crossfeedOn_) crossfeed(x);
            if (wideningOn_) {
                for (std::size_t pair = 0; pair < pairCount_; ++pair) {
                    float& l = x[pairs_[pair][0]];
                    float& r = x[pairs_[pair][1]];
                    const double mid = (l + r) * 0.5;
                    const double side = (l - r) * 0.5 * widening_;
                    l = static_cast<float>(mid + side);
                    r = static_cast<float>(mid - side);
                }
            }
            double gain = 1.0;
            if (settings_.compression > 0) gain *= compress(peak(x));
            if (settings_.normalize) gain *= normalize(x);
            boostCurrent_ += boostStep;
            gain *= boostCurrent_;
            for (std::size_t c = 0; c < channels; ++c) x[c] = static_cast<float>(x[c] * gain);
        }
        boostCurrent_ = boostTarget_;
        // Back at 100% with nothing else on: only the limiter is left, and
        // it keeps what it holds.
        if (boostCurrent_ <= 1.0 && !effectsOn()) {
            active_ = false;
            resetEffects();
        }
    }

    void resetEffects() {
        clearEqualizer();
        voiceState_ = {};
        crossfeed_ = {};
        compressorEnvelope_ = 0.0;
        clearNormalizer();
        boostCurrent_ = boostTarget_;
    }

    bool effectsOn() const {
        return equalizerOn_ || stereoOn_ || crossfeedOn_ || wideningOn_ || voiceOn_ ||
               settings_.compression > 0 || settings_.normalize;
    }

    double peak(const float* x) const {
        double level = 0.0;
        for (std::size_t c = 0; c < channels_; ++c) level = std::max(level, static_cast<double>(std::abs(x[c])));
        return level;
    }

    void clearEqualizer() {
        for (auto& channel : eqState_) channel = {};
    }

    // The level heard, the gain reached and the glide to it, all of it: a
    // gain left behind would play the first block after it at that gain.
    void clearNormalizer() {
        normalizerPower_ = 0.0;
        normalizerBlockPower_ = 0.0;
        normalizerGainDb_ = 0.0;
        normalizerCount_ = 0;
        normalizerApplied_ = 1.0;
        normalizerStep_ = 0.0;
    }

    void applyStereoMode(float* x) const {
        for (std::size_t pair = 0; pair < pairCount_; ++pair) {
            float& l = x[pairs_[pair][0]];
            float& r = x[pairs_[pair][1]];
            switch (settings_.stereo) {
            case AudioStereoMode::Swapped: std::swap(l, r); break;
            case AudioStereoMode::LeftOnly: r = l; break;
            case AudioStereoMode::RightOnly: l = r; break;
            case AudioStereoMode::Mono: l = r = (l + r) * 0.5F; break;
            case AudioStereoMode::VoiceRemoval: l = r = (l - r) * 0.70710678F; break;
            case AudioStereoMode::Stereo: break;
            }
        }
        // A centre speaker carries the dialogue of a surround mix by itself.
        if (settings_.stereo == AudioStereoMode::VoiceRemoval && center_ >= 0) x[center_] = 0.0F;
    }

    // What is common to both sides -- where a voice sits -- lifted around
    // 1.5 kHz, and the centre speaker with it.
    void emphasizeVoice(float* x) {
        if (pairCount_ > 0) {
            float& l = x[pairs_[0][0]];
            float& r = x[pairs_[0][1]];
            const double mid = (l + r) * 0.5;
            const double lift = voiceState_[0].run(voiceFilter_, mid) - mid;
            l = static_cast<float>(l + lift);
            r = static_cast<float>(r + lift);
        }
        if (center_ >= 0) x[center_] = static_cast<float>(voiceState_[1].run(voiceFilter_, x[center_]));
    }

    // Bauer's stereo-to-binaural crossfeed (bs2b, 700 Hz and 4.5 dB): each
    // ear hears the other side's lows a little later and softer, as from
    // speakers in front of it.
    void configureCrossfeed() {
        const double cutLow = 700.0;
        const double feed = 4.5;
        const double gainLowDb = feed * -5.0 / 6.0 - 3.0;
        const double gainHighDb = feed / 6.0 - 3.0;
        const double gainLow = std::pow(10.0, gainLowDb / 20.0);
        const double gainHigh = 1.0 - std::pow(10.0, gainHighDb / 20.0);
        const double cutHigh = cutLow * std::pow(2.0, (gainLowDb - 20.0 * std::log10(gainHigh)) / 12.0);
        double x = std::exp(-2.0 * audio_dsp_detail::kPi * cutLow / sampleRate_);
        lowB1_ = x;
        lowA0_ = gainLow * (1.0 - x);
        x = std::exp(-2.0 * audio_dsp_detail::kPi * cutHigh / sampleRate_);
        highB1_ = x;
        highA0_ = 1.0 - gainHigh * (1.0 - x);
        highA1_ = -x;
        crossfeedGain_ = 1.0 / (1.0 - gainHigh + gainLow);
    }

    void crossfeed(float* x) {
        float& l = x[pairs_[0][0]];
        float& r = x[pairs_[0][1]];
        const std::array<double, 2> in{l, r};
        auto& s = crossfeed_;
        for (std::size_t side = 0; side < 2; ++side) {
            s.low[side] = lowA0_ * in[side] + lowB1_ * s.low[side];
            s.high[side] = highA0_ * in[side] + highA1_ * s.previous[side] + highB1_ * s.high[side];
            s.previous[side] = in[side];
        }
        l = static_cast<float>((s.high[0] + s.low[1]) * crossfeedGain_);
        r = static_cast<float>((s.high[1] + s.low[0]) * crossfeedGain_);
    }

    // Night mode: the loud parts brought down towards the quiet ones, and
    // the whole lifted back. A soft knee, every channel together.
    double compress(double level) {
        using namespace audio_dsp_detail;
        const auto& c = compressor_;
        const double in = gainToDb(level);
        const double over = in - c.threshold;
        double out = in;
        if (2.0 * over >= c.knee) {
            out = c.threshold + over / c.ratio;
        } else if (2.0 * over > -c.knee) {
            const double into = over + c.knee * 0.5;
            out = in + (1.0 / c.ratio - 1.0) * into * into / (2.0 * c.knee);
        }
        const double reduction = out - in;
        const double coefficient = reduction < compressorEnvelope_ ? compressorAttack_ : compressorRelease_;
        compressorEnvelope_ = coefficient * compressorEnvelope_ + (1.0 - coefficient) * reduction;
        return dbToGain(compressorEnvelope_ + c.makeup);
    }

    // The normalizer: the level of the last fraction of a second held near
    // one loudness, coming down fast and going up slowly, and left alone
    // while the block just played was silent -- a pause is not something
    // quiet to bring up. Gains are worked out each block and glided across
    // it.
    double normalize(const float* x) {
        using namespace audio_dsp_detail;
        double power = 0.0;
        for (std::size_t c = 0; c < channels_; ++c) power = std::max(power, static_cast<double>(x[c]) * x[c]);
        normalizerPower_ = normalizerMeasure_ * normalizerPower_ + (1.0 - normalizerMeasure_) * power;
        if (normalizerCount_ == 0) {
            const double levelDb = 10.0 * std::log10(std::max(normalizerPower_, 1e-12));
            const double blockDb = 10.0 * std::log10(std::max(normalizerBlockPower_ / kNormalizerBlock, 1e-12));
            normalizerBlockPower_ = 0.0;
            if (levelDb > kNormalizerGateDb && blockDb > kNormalizerGateDb) {
                const double wanted = std::clamp(kNormalizerTargetDb - levelDb, -kNormalizerRangeDb, kNormalizerRangeDb);
                const double coefficient = wanted < normalizerGainDb_ ? normalizerFall_ : normalizerRise_;
                normalizerGainDb_ = coefficient * normalizerGainDb_ + (1.0 - coefficient) * wanted;
            }
            normalizerStep_ = (dbToGain(normalizerGainDb_) - normalizerApplied_) / kNormalizerBlock;
        }
        normalizerBlockPower_ += power;
        normalizerCount_ = (normalizerCount_ + 1) % kNormalizerBlock;
        normalizerApplied_ += normalizerStep_;
        return normalizerApplied_;
    }

    double sampleRate_{48000.0};
    std::size_t channels_{2};
    std::array<std::array<int, 2>, 3> pairs_{{{0, 1}}};
    std::size_t pairCount_{1};
    int center_{-1};
    AudioEffectSettings settings_;
    bool active_{};
    bool equalizerOn_{};
    bool stereoOn_{};
    bool crossfeedOn_{};
    bool wideningOn_{};
    bool voiceOn_{};

    std::array<audio_dsp_detail::Biquad, kEqualizerBands> eqFilters_{};
    // The bands the equalizer runs, the first eqBandCount_ of them: a fixed
    // array, since configure fills it on the audio thread.
    std::array<std::size_t, kEqualizerBands> eqBands_{};
    std::size_t eqBandCount_{};
    std::array<std::array<audio_dsp_detail::BiquadState, kEqualizerBands>, kMaxChannels> eqState_{};
    double preampGain_{1.0};

    audio_dsp_detail::Biquad voiceFilter_{};
    std::array<audio_dsp_detail::BiquadState, 2> voiceState_{};

    double lowA0_{}, lowB1_{}, highA0_{}, highA1_{}, highB1_{}, crossfeedGain_{1.0};
    CrossfeedState crossfeed_{};

    double widening_{1.0};

    Compressor compressor_{-24.0, 2.0, 6.0, 5.0};
    double compressorAttack_{}, compressorRelease_{}, compressorEnvelope_{};

    double normalizerMeasure_{}, normalizerFall_{}, normalizerRise_{};
    double normalizerPower_{}, normalizerBlockPower_{}, normalizerGainDb_{}, normalizerApplied_{1.0}, normalizerStep_{};
    std::size_t normalizerCount_{};

    double boostTarget_{1.0};
    double boostCurrent_{1.0};
    AudioLimiter limiter_;
};

// Plays a stream faster or slower without changing its pitch (WSOLA, as
// VLC's and mpv's scaletempo): the input is cut into overlapping pieces a
// fixed 30 ms of output long, each taken from where the speed has got to,
// nudged by up to 14 ms to where it best continues the one before, and
// cross-faded into it.
class TimeStretch {
public:
    void configure(std::size_t channels, double sampleRate = 48000.0) {
        channels_ = std::max<std::size_t>(1, channels);
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        stride_ = static_cast<std::size_t>(std::lround(0.030 * rate));
        overlap_ = std::max<std::size_t>(1, stride_ / 5);
        search_ = static_cast<std::size_t>(std::lround(0.014 * rate));
        window_.resize(overlap_);
        for (std::size_t i = 0; i < overlap_; ++i) {
            window_[i] = static_cast<float>(static_cast<double>(i) * static_cast<double>(overlap_ - i));
        }
        reset(tempo_);
    }

    // Starts over at this speed, forgetting what was queued.
    void reset(double tempo) {
        tempo_ = std::clamp(tempo, 0.25, 4.0);
        queue_.clear();
        head_ = 0;
        carry_ = 0.0;
        first_ = true;
        pushed_ = 0;
        produced_ = 0;
        tail_.assign(overlap_ * channels_, 0.0F);
    }

    double tempo() const { return tempo_; }
    std::size_t channels() const { return channels_; }
    std::size_t queuedFrames() const { return queue_.size() / channels_ - head_; }

    void push(const float* samples, std::size_t frames) {
        queue_.insert(queue_.end(), samples, samples + frames * channels_);
        pushed_ += frames;
    }

    // Appends every piece the queued input is enough for; the number of
    // frames appended.
    std::size_t pull(std::vector<float>& out) {
        std::size_t produced = 0;
        for (;;) {
            const double advance = static_cast<double>(stride_) * tempo_ + carry_;
            const auto whole = static_cast<std::size_t>(advance);
            const std::size_t needed = std::max(search_ + stride_ + overlap_, whole + 1);
            if (queuedFrames() < needed) break;
            const float* queued = queue_.data() + head_ * channels_;
            const std::size_t offset = first_ ? 0 : bestOffset(queued);
            const float* piece = queued + offset * channels_;
            if (first_) {
                out.insert(out.end(), piece, piece + stride_ * channels_);
            } else {
                for (std::size_t i = 0; i < overlap_; ++i) {
                    const float in = (static_cast<float>(i) + 0.5F) / static_cast<float>(overlap_);
                    for (std::size_t c = 0; c < channels_; ++c) {
                        const std::size_t at = i * channels_ + c;
                        out.push_back(tail_[at] * (1.0F - in) + piece[at] * in);
                    }
                }
                out.insert(out.end(), piece + overlap_ * channels_, piece + stride_ * channels_);
            }
            std::copy(piece + stride_ * channels_, piece + (stride_ + overlap_) * channels_, tail_.begin());
            head_ += whole;
            carry_ = advance - static_cast<double>(whole);
            first_ = false;
            produced += stride_;
            produced_ += stride_;
        }
        compact();
        return produced;
    }

    // The input's end: what is queued is stretched out with silence behind
    // it, cut where the input is used up, and the stretch starts over. All
    // told the output is the input's length over the speed.
    std::size_t drain(std::vector<float>& out) {
        const std::size_t before = out.size();
        pull(out);
        const auto target = static_cast<std::size_t>(std::llround(static_cast<double>(pushed_) / tempo_));
        if (pushed_ > 0) {
            const std::vector<float> silence((stride_ * 5 + search_ + overlap_) * channels_, 0.0F);
            while (produced_ < target) {
                push(silence.data(), silence.size() / channels_);
                pull(out);
            }
        }
        const std::size_t appended = (out.size() - before) / channels_;
        const std::size_t excess = produced_ > target ? std::min(produced_ - target, appended) : 0;
        out.resize(out.size() - excess * channels_);
        reset(tempo_);
        return appended - excess;
    }
    // How far, on average, the input heard at an output frame is ahead of
    // that frame's number times the speed, in input frames: half the search,
    // and the drift across a piece, which plays at the input's own pace.
    double meanLeadFrames() const {
        return static_cast<double>(search_) * 0.5 + (static_cast<double>(stride_) - 1.0) * (1.0 - tempo_) * 0.5;
    }

private:
    // Where in the search range the queued input best continues the last
    // piece: the highest correlation of the two over the overlap, weighted
    // to its middle and against the candidate's own energy.
    std::size_t bestOffset(const float* queued) {
        mono_.resize(search_ + overlap_);
        for (std::size_t i = 0; i < mono_.size(); ++i) {
            float sum = 0.0F;
            for (std::size_t c = 0; c < channels_; ++c) sum += queued[i * channels_ + c];
            mono_[i] = sum;
        }
        reference_.resize(overlap_);
        for (std::size_t i = 0; i < overlap_; ++i) {
            float sum = 0.0F;
            for (std::size_t c = 0; c < channels_; ++c) sum += tail_[i * channels_ + c];
            reference_[i] = sum * window_[i];
        }
        double energy = 0.0;
        for (std::size_t i = 0; i < overlap_; ++i) energy += static_cast<double>(mono_[i]) * mono_[i];
        std::size_t best = 0;
        double bestScore = -1e300;
        for (std::size_t offset = 0; offset < search_; ++offset) {
            double correlation = 0.0;
            for (std::size_t i = 0; i < overlap_; ++i) correlation += static_cast<double>(reference_[i]) * mono_[offset + i];
            const double score = correlation / std::sqrt(energy + 1e-9);
            if (score > bestScore) {
                bestScore = score;
                best = offset;
            }
            const double leaving = mono_[offset];
            const double entering = mono_[offset + overlap_];
            energy = std::max(0.0, energy - leaving * leaving + entering * entering);
        }
        return best;
    }

    void compact() {
        if (head_ == 0) return;
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(std::min(head_ * channels_, queue_.size())));
        head_ = 0;
    }

    std::size_t channels_{2};
    std::size_t stride_{1440};
    std::size_t overlap_{288};
    std::size_t search_{672};
    double tempo_{1.0};
    double carry_{};
    bool first_{true};
    std::size_t head_{};
    // Frames in and out since the last start.
    std::size_t pushed_{};
    std::size_t produced_{};
    std::vector<float> queue_;
    std::vector<float> tail_;
    std::vector<float> window_;
    std::vector<float> mono_;
    std::vector<float> reference_;
};

}  // namespace quaddeck
