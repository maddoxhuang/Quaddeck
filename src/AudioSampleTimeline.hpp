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

}  // namespace quaddeck
