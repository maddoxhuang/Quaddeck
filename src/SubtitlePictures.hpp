#pragma once

// Subtitles that are pictures: Blu-ray's PGS, DVD's VobSub, DVB's. Their
// stream holds no text, only what to paint and where on a canvas the size
// of the disc's frame, so nothing of the subtitle settings' font, size or
// colour applies to them. Until 2026-11-24 they were listed and not shown.
//
// Free of FFmpeg, Win32 and Direct2D: the video worker fills a timeline
// with what its decoder hands over, and the window thread asks it for the
// picture of a moment and lays that over the pane's picture.

#include "AssSubtitles.hpp"
#include "Core.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace quaddeck {

// What a picture subtitle stream shows at one time: pieces on its canvas,
// in the canvas's own pixels.
struct SubtitlePicture {
    int canvasWidth{};
    int canvasHeight{};
    std::vector<SubtitlePiece> pieces;
};

inline std::size_t subtitlePictureBytes(const SubtitlePicture& picture) {
    std::size_t bytes = sizeof(SubtitlePicture);
    for (const auto& piece : picture.pieces) {
        bytes += sizeof(SubtitlePiece) + piece.pixels.size() * sizeof(std::uint32_t);
    }
    return bytes;
}

// The largest piece taken, a side and in all: a Blu-ray's canvas is
// 1920x1080 and a UHD one's 3840x2160.
inline constexpr int kSubtitlePictureSide = 8192;
inline constexpr std::size_t kSubtitlePicturePixels = 3840u * 2160u;

// One rectangle as a decoder hands it over, a palette index for each pixel
// and the palette as 0xAARRGGBB with straight alpha, made the premultiplied
// BGRA the overlay draws. False for a rectangle that is no picture: no
// size, one past the limits, or rows that overlap. An index past the
// palette's end is left clear.
inline bool subtitlePieceFromPalette(const std::uint8_t* indexes, int stride, int width, int height,
                                     const std::uint32_t* palette, int colours, SubtitlePiece& piece) {
    if (!indexes || !palette || width <= 0 || height <= 0 || colours <= 0 || stride < width ||
        width > kSubtitlePictureSide || height > kSubtitlePictureSide ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > kSubtitlePicturePixels) {
        return false;
    }
    std::uint32_t made[256]{};
    const int known = std::min(colours, 256);
    for (int index = 0; index < known; ++index) {
        const std::uint32_t colour = palette[index];
        const std::uint32_t alpha = colour >> 24U;
        const auto scaled = [alpha](std::uint32_t channel) { return (channel * alpha + 127U) / 255U; };
        made[index] = alpha << 24U | scaled((colour >> 16U) & 0xFFU) << 16U |
                      scaled((colour >> 8U) & 0xFFU) << 8U | scaled(colour & 0xFFU);
    }
    piece.width = width;
    piece.height = height;
    piece.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = indexes + static_cast<std::ptrdiff_t>(y) * stride;
        std::uint32_t* out = piece.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) out[x] = made[row[x]];
    }
    return true;
}

// The pictures of one stream as they were read, each from its start to its
// end. A stream shows one picture at a time: whatever it says next, a new
// picture or nothing at all, ends the one before -- Blu-ray's say no end of
// their own and are ended only so. Kept short: the worker reads a little
// ahead of what is shown and a seek starts over, so only the last few are
// ever asked for, and a film's worth of them would be hundreds of megabytes.
class SubtitlePictureTimeline {
public:
    static constexpr std::size_t kMaximumPictures = 64;
    static constexpr std::size_t kMaximumBytes = 96u * 1024u * 1024u;

    void clear() {
        entries_.clear();
        bytes_ = 0;
    }

    // What the stream shows from `start`: `picture`, or nothing when it is
    // null or empty. `end` at or before `start` is an end the stream has
    // yet to say. What was read from this time on before -- the same
    // stretch read again after a failed read -- is replaced.
    void show(double start, double end, std::shared_ptr<const SubtitlePicture> picture) {
        if (!std::isfinite(start)) return;
        while (!entries_.empty() && entries_.back().start >= start) {
            bytes_ -= entries_.back().bytes;
            entries_.pop_back();
        }
        for (auto& entry : entries_) entry.end = std::min(entry.end, start);
        if (!picture || picture->pieces.empty()) return;
        const std::size_t bytes = subtitlePictureBytes(*picture);
        if (bytes > kMaximumBytes) return;
        entries_.push_back({start, std::isfinite(end) && end > start ? end : std::numeric_limits<double>::infinity(),
                            std::move(picture), bytes});
        bytes_ += bytes;
        while (entries_.size() > kMaximumPictures || bytes_ > kMaximumBytes) {
            bytes_ -= entries_.front().bytes;
            entries_.pop_front();
        }
    }

    // The picture up at that time, null for none.
    std::shared_ptr<const SubtitlePicture> at(double seconds) const {
        for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
            if (entry->start > seconds) continue;
            return seconds < entry->end ? entry->picture : nullptr;
        }
        return nullptr;
    }

    std::size_t size() const { return entries_.size(); }
    std::size_t bytes() const { return bytes_; }

private:
    struct Entry {
        double start{};
        double end{};
        std::shared_ptr<const SubtitlePicture> picture;
        std::size_t bytes{};
    };
    std::deque<Entry> entries_;
    std::size_t bytes_{};
};

// Where a picture subtitle's canvas lies on screen, given where the pane's
// whole picture lies (pictureRect). A canvas of the video's own shape is
// the video's frame: a DVD's 720x576 over its 720x576 picture, stretched
// with it. Any other is a disc's full frame over a video cut down from it
// -- 1920x1080 over a film stored as 1920x800 -- and is fitted to the
// picture's height with square pixels and centred, as mpv does, so that
// the lines the disc put in its black bars stay inside the picture.
inline RectF subtitleCanvasRect(const RectF& picture, int videoWidth, int videoHeight,
                                int canvasWidth, int canvasHeight) {
    if (canvasWidth <= 0 || canvasHeight <= 0 || videoWidth <= 0 || videoHeight <= 0 ||
        picture.width <= 0.0F || picture.height <= 0.0F) {
        return picture;
    }
    const double canvasShape = static_cast<double>(canvasWidth) / canvasHeight;
    const double videoShape = static_cast<double>(videoWidth) / videoHeight;
    if (std::abs(canvasShape - videoShape) <= 0.02 * videoShape) return picture;
    RectF canvas = picture;
    canvas.width = static_cast<float>(picture.height * canvasShape);
    canvas.x = picture.x + (picture.width - canvas.width) * 0.5F;
    return canvas;
}

namespace subtitle_picture_detail {

// For each of `count` pixels of the scaled row from `first` on, the source
// pixels it is made of and how much of each: the two nearest when the row
// is enlarged, every one it covers when it is reduced.
struct Taps {
    int first{};
    std::vector<float> weights;
};

inline std::vector<Taps> resampleTaps(int source, int scaled, int first, int count) {
    std::vector<Taps> taps(static_cast<std::size_t>(std::max(count, 0)));
    const double step = static_cast<double>(source) / scaled;  // source pixels to one scaled pixel
    for (int index = 0; index < count; ++index) {
        Taps& tap = taps[static_cast<std::size_t>(index)];
        const int at = first + index;
        if (step <= 1.0) {
            const double centre = (at + 0.5) * step - 0.5;
            const double floor = std::floor(centre);
            const float fraction = static_cast<float>(centre - floor);
            const int low = std::clamp(static_cast<int>(floor), 0, source - 1);
            const int high = std::clamp(static_cast<int>(floor) + 1, 0, source - 1);
            tap.first = low;
            if (high == low) tap.weights = {1.0F};
            else tap.weights = {1.0F - fraction, fraction};
            continue;
        }
        const double from = at * step;
        const double to = std::min((at + 1) * step, static_cast<double>(source));
        const int low = std::clamp(static_cast<int>(std::floor(from)), 0, source - 1);
        const int high = std::clamp(static_cast<int>(std::ceil(to)) - 1, low, source - 1);
        tap.first = low;
        tap.weights.resize(static_cast<std::size_t>(high - low + 1));
        double total = 0.0;
        for (int pixel = low; pixel <= high; ++pixel) {
            const double covered = std::min(to, pixel + 1.0) - std::max(from, static_cast<double>(pixel));
            tap.weights[static_cast<std::size_t>(pixel - low)] = static_cast<float>(std::max(covered, 0.0));
            total += std::max(covered, 0.0);
        }
        if (total > 0.0) {
            for (float& weight : tap.weights) weight = static_cast<float>(weight / total);
        } else {
            tap.weights.assign(tap.weights.size(), 1.0F / static_cast<float>(tap.weights.size()));
        }
    }
    return taps;
}

}  // namespace subtitle_picture_detail

// A picture as the overlay draws it: its canvas laid over `canvas` on
// screen (subtitleCanvasRect, window pixels), each piece scaled to its
// place there and cut to `clip`, the pane's cell -- a zoomed pane's canvas
// is several times the window. The pieces' places are the window's.
// Premultiplied colour is averaged as it is, so an edge neither darkens
// nor fringes.
inline std::shared_ptr<const SubtitleBitmap> placeSubtitlePicture(const SubtitlePicture& picture,
                                                                  const RectF& canvas, const RectF& clip) {
    auto bitmap = std::make_shared<SubtitleBitmap>();
    if (picture.canvasWidth <= 0 || picture.canvasHeight <= 0 || canvas.width <= 0.0F || canvas.height <= 0.0F) {
        return bitmap;
    }
    const double scaleX = static_cast<double>(canvas.width) / picture.canvasWidth;
    const double scaleY = static_cast<double>(canvas.height) / picture.canvasHeight;
    const long long clipLeft = std::llround(std::floor(clip.x));
    const long long clipTop = std::llround(std::floor(clip.y));
    const long long clipRight = std::llround(std::ceil(clip.x + clip.width));
    const long long clipBottom = std::llround(std::ceil(clip.y + clip.height));
    for (const SubtitlePiece& piece : picture.pieces) {
        if (piece.width <= 0 || piece.height <= 0 ||
            piece.pixels.size() < static_cast<std::size_t>(piece.width) * static_cast<std::size_t>(piece.height)) {
            continue;
        }
        const long long left = std::llround(canvas.x + piece.x * scaleX);
        const long long top = std::llround(canvas.y + piece.y * scaleY);
        const long long width = std::max(1LL, std::llround(piece.width * scaleX));
        const long long height = std::max(1LL, std::llround(piece.height * scaleY));
        const long long fromX = std::max(left, clipLeft);
        const long long fromY = std::max(top, clipTop);
        const long long toX = std::min(left + width, clipRight);
        const long long toY = std::min(top + height, clipBottom);
        if (toX <= fromX || toY <= fromY || width > (1LL << 24) || height > (1LL << 24)) continue;
        const int outWidth = static_cast<int>(toX - fromX);
        const int outHeight = static_cast<int>(toY - fromY);
        const auto columns = subtitle_picture_detail::resampleTaps(
            piece.width, static_cast<int>(width), static_cast<int>(fromX - left), outWidth);
        const auto rows = subtitle_picture_detail::resampleTaps(
            piece.height, static_cast<int>(height), static_cast<int>(fromY - top), outHeight);
        // Rows first, only those a kept row is made of; then down.
        const int rowFirst = rows.front().first;
        const int rowLast = rows.back().first + static_cast<int>(rows.back().weights.size()) - 1;
        const int rowCount = rowLast - rowFirst + 1;
        std::vector<float> across(static_cast<std::size_t>(rowCount) * static_cast<std::size_t>(outWidth) * 4U);
        for (int y = 0; y < rowCount; ++y) {
            const std::uint32_t* in = piece.pixels.data() +
                static_cast<std::size_t>(rowFirst + y) * static_cast<std::size_t>(piece.width);
            float* out = across.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) * 4U;
            for (int x = 0; x < outWidth; ++x) {
                const auto& tap = columns[static_cast<std::size_t>(x)];
                float sum[4]{};
                for (std::size_t index = 0; index < tap.weights.size(); ++index) {
                    const std::uint32_t pixel = in[static_cast<std::size_t>(tap.first) + index];
                    const float weight = tap.weights[index];
                    sum[0] += weight * static_cast<float>(pixel & 0xFFU);
                    sum[1] += weight * static_cast<float>((pixel >> 8U) & 0xFFU);
                    sum[2] += weight * static_cast<float>((pixel >> 16U) & 0xFFU);
                    sum[3] += weight * static_cast<float>(pixel >> 24U);
                }
                std::copy(sum, sum + 4, out + static_cast<std::size_t>(x) * 4U);
            }
        }
        SubtitlePiece placed;
        placed.x = static_cast<int>(fromX);
        placed.y = static_cast<int>(fromY);
        placed.width = outWidth;
        placed.height = outHeight;
        placed.pixels.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight));
        bool any = false;
        for (int y = 0; y < outHeight; ++y) {
            const auto& tap = rows[static_cast<std::size_t>(y)];
            std::uint32_t* out =
                placed.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth);
            for (int x = 0; x < outWidth; ++x) {
                float sum[4]{};
                for (std::size_t index = 0; index < tap.weights.size(); ++index) {
                    const float* in = across.data() +
                        (static_cast<std::size_t>(tap.first - rowFirst) + index) *
                            static_cast<std::size_t>(outWidth) * 4U +
                        static_cast<std::size_t>(x) * 4U;
                    const float weight = tap.weights[index];
                    for (int channel = 0; channel < 4; ++channel) sum[channel] += weight * in[channel];
                }
                const auto byte = [](float value) {
                    return static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 255L));
                };
                const std::uint32_t alpha = byte(sum[3]);
                // Rounding must not leave a colour above its alpha.
                const auto under = [&](float value) { return std::min(byte(value), alpha); };
                out[x] = alpha << 24U | under(sum[2]) << 16U | under(sum[1]) << 8U | under(sum[0]);
                any = any || alpha != 0;
            }
        }
        if (any) bitmap->pieces.push_back(std::move(placed));
    }
    return bitmap;
}

}  // namespace quaddeck
