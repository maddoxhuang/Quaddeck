#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace quaddeck {

// Resampler output is a continuous sequence of sample frames. Timestamp
// quantization must not create gaps between buffers, and an empty conversion
// must not consume the seek gate. All positions here are in output samples.
class AudioSampleTimeline {
public:
    struct Slice {
        std::int64_t firstSample{};
        int skip{};
        int count{};
    };

    void reset(std::optional<std::int64_t> target = std::nullopt) noexcept {
        discardBefore_ = target;
        nextSample_ = target.value_or(0);
        timestampKnown_ = false;
    }

    // Moves the gate later, never earlier. Before a generation's first
    // buffer, this is how its start follows a clock that kept running while
    // the seek and the decode took their time.
    void raiseGate(std::int64_t sample) noexcept {
        if (!discardBefore_ || *discardBefore_ < sample) discardBefore_ = sample;
    }

    void locate(std::optional<std::int64_t> outputStart,
                std::int64_t timestampTolerance = 1) noexcept {
        if (!outputStart) return;
        // A missing initial PTS falls back to the requested destination (or
        // zero on open), then advances by actual output count. A later known
        // PTS establishes the real clock. Never invent preroll from no PTS.
        if (!timestampKnown_ ||
            std::abs(static_cast<long double>(*outputStart) - nextSample_) >
                std::max<std::int64_t>(1, timestampTolerance)) {
            nextSample_ = *outputStart;
        }
        timestampKnown_ = true;
    }

    // A timestamp has placed the generation; until one has, the count runs
    // from the gate.
    bool located() const noexcept { return timestampKnown_; }
    // Where the next converted sample sits.
    std::int64_t position() const noexcept { return nextSample_; }
    // `samples` more from here would all be cut: they end before the seek's
    // target, or before the end of what was already handed on.
    bool discards(double samples) const noexcept { return discards(nextSample_, samples); }
    // The same for `samples` from `start`.
    bool discards(std::int64_t start, double samples) const noexcept {
        return discardBefore_ &&
            static_cast<long double>(start) + samples <= static_cast<long double>(*discardBefore_);
    }

    Slice consume(int samples) noexcept {
        Slice slice{nextSample_, 0, std::max(0, samples)};
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        if (nextSample_ > maximum - slice.count) {
            slice.count = static_cast<int>(maximum - nextSample_);
        }
        if (slice.count == 0) return slice;
        nextSample_ += slice.count;
        if (discardBefore_ && *discardBefore_ > slice.firstSample) {
            slice.skip = static_cast<int>(std::min<long double>(slice.count,
                static_cast<long double>(*discardBefore_) - slice.firstSample));
            slice.firstSample += slice.skip;
            slice.count -= slice.skip;
        }
        // Once a buffer has been handed to submission, a later PTS must not
        // repeat its interval. This also covers real timestamps arriving after
        // estimated missing-PTS output: discard up to the emitted high-water
        // mark, not just the original seek target. A new generation resets it.
        if (slice.count > 0) discardBefore_ = nextSample_;
        return slice;
    }

private:
    std::optional<std::int64_t> discardBefore_;
    std::int64_t nextSample_{};
    bool timestampKnown_{};
};

// Decoded sound and its timestamps often disagree by a little: stamps
// rounded to a container's millisecond, a packet stamped a frame late, or
// stamps that run at another rate than the samples do. Placing each buffer
// at its stamp cuts or skips samples at every disagreement, and each one is
// a click. Once the first stamp has placed a generation, this follows the
// stamps instead, resampling up to 10% faster or slower so the running
// count meets them smoothly. What is not a small disagreement is not bent
// into the sound. A stamp more than 15 ms off the line the ones before it
// set (15% of a frame over 0.1 s) is held over, a frame or two, until the
// stamps show which line they keep: back on the old one, or on none three
// frames running, they were out of line and are passed over, however far
// out; on a new one, that step is the stamps' own, and the count eases
// onto it at 0.5%, about 9 cents on top of the following, cutting and
// adding nothing and leaving the buffers' times continuous. A step that
// would leave the count more than 100 ms off the stamps, counting what is
// still being eased, is not eased: ahead, it is a gap, filled with silence
// up to 10 s; back, or further ahead, the sound is placed again. So is the
// loop losing the stamps by 100 ms (a fifth of a frame over half a
// second), and so are stamps the count is that far off that are on no line,
// all off it the same way, because each runs further away from it, faster
// than the 10% may follow, or have been on none six frames running. Stamps
// standing still are no clock and passed over however far. All counts are
// in 48 kHz output samples.
class AudioStampFollower {
public:
    enum class Action { Follow, Ease, Fill, Relocate };
    struct Step {
        Action action{Action::Follow};
        // For Fill: the silence to put ahead of the buffer. For Ease: the
        // step the stamps took, eased onto over the frames to come with
        // what is left of any before it (`held`).
        std::int64_t samples{};
        // For Follow and Ease: the samples to add to (or take from) this
        // buffer.
        int compensation{};
    };

    static constexpr double kMaxStretch = 0.10;
    static constexpr double kEase = 0.005;
    static constexpr std::int64_t kGap = 4800;
    static constexpr std::int64_t kFillLimit = 10 * 48000;

    // A seek, or the sound placed again: the count sits on a stamp, and the
    // rate the stamps run at is kept.
    void restart() noexcept {
        smoothed_ = 0.0;
        primed_ = false;
        carry_ = 0.0;
        stretch_ = 0.0;
        expecting_ = false;
        pending_ = 0;
        passed_ = 0;
        held_ = 0.0;
    }
    // Another stream.
    void reset() noexcept {
        restart();
        rate_ = 0.0;
    }

    // `error`: where the stamp puts the buffer about to be converted, less
    // where the running count has it. `frames`: what the buffer holds at the
    // nominal rate.
    Step observe(std::int64_t error, double frames) noexcept {
        frames = std::max(frames, 0.0);
        const auto measured = static_cast<double>(error);
        // How far this stamp is off the line the ones before it set: where
        // the last stamp in line, the rate and what was added put it, or,
        // after a placement, where the placement did. Neither what is still
        // being eased nor the loop's own lag is in it.
        const double deviation = measured - (expecting_ ? expected_ : rate_ * frames);
        const auto agrees = [frames](double a, double b) { return std::abs(a - b) <= tolerance(frames); };
        // The loop holds the stamps within 100 ms of the count, what is
        // still being eased aside, or has lost them. On frames over half a
        // second, a fifth of the frame: the loop moves the count at most
        // 10% of a frame each frame, so while it learns a rate it can trail
        // the stamps by more than 100 ms.
        const double lost = std::max(static_cast<double>(kGap), 2.0 * kMaxStretch * frames);
        const bool adrift = std::abs(measured - held_) > lost;
        double taken = 0.0;
        if (pending_ == 0) {
            if (!agrees(deviation, 0.0) || adrift) {
                lines_[0] = deviation;
                pending_ = 1;
                return hold(measured - deviation, frames);
            }
        } else {
            // A stamp held over only because the loop had lost it, though on
            // the line, is no line of its own: one that agrees with it is on
            // the line too, and the loop is found lost below.
            const bool online = agrees(deviation, 0.0) || (agrees(lines_[0], 0.0) && agrees(deviation, lines_[0]));
            const bool kept = !online &&
                (agrees(deviation, lines_[0]) || (pending_ == 2 && agrees(deviation, lines_[1])));
            if (!online && !kept) {
                // Off both lines: held over once more, to see which is kept.
                if (pending_ == 1) {
                    lines_[1] = deviation;
                    pending_ = 2;
                    return hold(measured - deviation, frames);
                }
                // Off every line three frames running, the stamps are only
                // uneven: this one is passed over as well, and the next is
                // judged afresh against the line. Not if the count is more
                // than 100 ms off them and all three are off the line the
                // same way, running away from it -- each further off,
                // faster than the loop may follow -- or six frames running
                // off every line: the loop has lost them. Stamps that stand
                // still, or go back, each less than a fifth of a frame past
                // the last, are no clock to place the sound by: passed over.
                pending_ = 0;
                const double away = deviation > 0.0 ? 1.0 : -1.0;
                const bool oneSide = away * lines_[0] > 0.0 && away * lines_[1] > 0.0;
                const bool runningAway = oneSide && away * lines_[1] > away * lines_[0] &&
                                         away * deviation > away * lines_[1];
                // (The line moves on by the frame and the rate each frame.)
                const double still = -(0.8 + rate_) * frames;
                const bool standing = lines_[1] - lines_[0] < still && deviation - lines_[1] < still;
                ++passed_;
                if (standing || std::abs(measured - held_) <= lost || !oneSide || (!runningAway && passed_ < 2)) {
                    return hold(measured - deviation, frames);
                }
                restart();
                if (error > 0 && error <= kFillLimit) return {Action::Fill, error, 0};
                return {Action::Relocate, 0, 0};
            }
            pending_ = 0;
            // Back on the line, the stamps held over were out of it and are
            // passed over. On a line held over, the step is the stamps': left
            // to the easing, out of the loop's sight. Either way, not if the
            // count would be left more than 100 ms off them, or the loop has
            // lost them.
            const double total = held_ + (kept ? deviation : 0.0);
            if (std::abs(total) > kGap || std::abs(measured - total) > lost) {
                restart();
                if (error > 0 && error <= kFillLimit) return {Action::Fill, error, 0};
                return {Action::Relocate, 0, 0};
            }
            if (kept) {
                taken = deviation;
                held_ = total;
            }
        }
        passed_ = 0;
        // A late or early stamp counts for its share of 0.1 s; a lasting
        // difference is closed over about half a second, and one that keeps
        // coming back is learnt as a rate within a few seconds. However long
        // the frame, one observation closes at most half the difference and
        // learns at most a quarter of it, or the loop would overshoot by more
        // each frame on codecs with frames near a second long.
        const double offset = measured - held_;
        const double weight = 1.0 - std::exp(-frames / kSmoothing);
        smoothed_ = primed_ ? smoothed_ + weight * (offset - smoothed_) : offset;
        primed_ = true;
        if (frames > 0.0) {
            const double closing = std::min(frames / kProportional, 0.5) / frames;
            const double learning = std::min(frames * frames / (kIntegral * kIntegral), 0.25) / frames;
            const double rate = std::clamp(rate_ + smoothed_ * learning, -kMaxStretch, kMaxStretch);
            const double wanted = smoothed_ * closing + rate;
            stretch_ = std::clamp(wanted, -kMaxStretch, kMaxStretch);
            // Nothing is learnt while the stretch is pinned at its limit the
            // way the difference pulls, so a long one is not overlearnt.
            if (wanted == stretch_ || (wanted > 0.0) != (smoothed_ > 0.0)) rate_ = rate;
            ease(frames);
        }
        const int compensation = compensate(measured, frames);
        if (taken != 0.0) return {Action::Ease, std::llround(taken), compensation};
        return {Action::Follow, 0, compensation};
    }

    // How much faster than nominal the sound goes now, and the part of it
    // learnt as the stream's own rate.
    double stretch() const noexcept { return stretch_; }
    double rate() const noexcept { return rate_; }
    // What of the steps taken is still to be eased onto.
    double held() const noexcept { return held_; }

    // How far a stamp may be off the line and still be on it: 15 ms, or
    // 15% of a frame over 0.1 s, more than a rate not yet learnt moves a
    // frame's stamp.
    static double tolerance(double frames) noexcept {
        return std::max(kStepFloor, kStepShare * std::max(frames, 0.0));
    }

    // The output samples swr_set_compensation spreads a frame's
    // compensation over: the frame's own length, and always more than the
    // samples added. A frame under half an output sample (one sample at
    // 192 kHz) rounds to none, and at a distance of only the samples added
    // libswresample's step through its input is zero, which it divides by.
    static int compensationDistance(int compensation, double frames) noexcept {
        if (compensation == 0) return 0;
        return std::max({1, compensation + 1, static_cast<int>(std::lround(std::max(frames, 0.0))) + compensation});
    }

private:
    static constexpr double kSmoothing = 0.1 * 48000;
    static constexpr double kProportional = 0.5 * 48000;
    static constexpr double kIntegral = 1.0 * 48000;
    static constexpr double kStepFloor = 720;
    static constexpr double kStepShare = 0.15;

    // A step taken is eased onto within what the 10% leaves.
    void ease(double frames) noexcept {
        if (frames <= 0.0) return;
        const double easing = std::clamp(held_ / frames, -kEase, kEase);
        const double total = std::clamp(stretch_ + easing, -kMaxStretch, kMaxStretch);
        held_ -= (total - stretch_) * frames;
        stretch_ = total;
    }

    // A stamp held over: the frame goes at the rate, easing on, and the
    // next stamp is looked for on the line it is off.
    Step hold(double line, double frames) noexcept {
        stretch_ = rate_;
        ease(frames);
        return {Action::Follow, 0, compensate(line, frames)};
    }

    // The frame's samples to add at the stretch, carrying what rounds off,
    // and where that and the rate put the next stamp from this one's place
    // on the line.
    int compensate(double line, double frames) noexcept {
        const double wanted = stretch_ * frames + carry_;
        const auto compensation = static_cast<int>(std::lround(wanted));
        carry_ = wanted - compensation;
        expected_ = line + rate_ * frames - compensation;
        expecting_ = true;
        return compensation;
    }

    double smoothed_{};
    bool primed_{};
    double rate_{};
    double carry_{};
    double stretch_{};
    double expected_{};
    bool expecting_{};
    // Stamps held over, each one's deviation from the line, and how many
    // times running three were passed over.
    int pending_{};
    double lines_[2]{};
    int passed_{};
    double held_{};
};

}  // namespace quaddeck
