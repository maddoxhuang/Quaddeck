#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace quaddeck {

// The video processor's SDR route handles 601/709 only. Keep unsupported
// gamut/transfer metadata on the existing BGRA fallback rather than tagging it
// as ordinary SDR. Missing tags retain the established SDR fallback policy.
inline bool softwareNv12Eligible(const AVFrame& frame) noexcept {
    if (frame.width <= 0 || frame.height <= 0 ||
        (frame.width & 1) || (frame.height & 1)) return false;
    switch (frame.format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12: break;
    default: return false;
    }
    switch (frame.colorspace) {
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: break;
    default: return false;
    }
    switch (frame.color_primaries) {
    case AVCOL_PRI_UNSPECIFIED:
    case AVCOL_PRI_BT709:
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE170M: break;
    default: return false;
    }
    switch (frame.color_trc) {
    case AVCOL_TRC_UNSPECIFIED:
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_GAMMA22: break;
    default: return false;
    }
    const auto hasPlane = [&](int plane, int rowBytes) {
        const auto pitch = static_cast<std::int64_t>(frame.linesize[plane]);
        return frame.data[plane] && (pitch < 0 ? -pitch : pitch) >= rowBytes;
    };
    if (!hasPlane(0, frame.width)) return false;
    if (frame.format == AV_PIX_FMT_NV12) return hasPlane(1, frame.width);
    return hasPlane(1, frame.width / 2) && hasPlane(2, frame.width / 2);
}

// Pack the original code values into tightly pitched NV12: full Y plane,
// followed by interleaved UV. AVFrame data points at the logical first row;
// signed linesizes therefore also support vertically reversed storage. No
// matrix, range, transfer or size conversion occurs. Rejection leaves both
// outputs untouched; the caller can continue using its existing BGRA fallback.
inline bool packSoftwareNv12(const AVFrame& frame,
                             std::vector<std::uint8_t>& bytes, int& stride) {
    if (!softwareNv12Eligible(frame)) return false;
    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    if (width > std::numeric_limits<std::size_t>::max() / height) return false;
    const auto yBytes = width * height;
    if (yBytes > bytes.max_size() || yBytes / 2 > bytes.max_size() - yBytes) return false;
    // All validation is complete. Reuse the renderer's per-pane upload buffer
    // instead of allocating another full-size image for every decoded frame.
    bytes.resize(yBytes + yBytes / 2);
    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(bytes.data() + static_cast<std::size_t>(y) * width,
            frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0], width);
    }
    for (int y = 0; y < frame.height / 2; ++y) {
        auto* destination = bytes.data() + yBytes + static_cast<std::size_t>(y) * width;
        const auto* u = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
        if (frame.format == AV_PIX_FMT_NV12) {
            std::memcpy(destination, u, width);
        } else {
            const auto* v = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
            for (int x = 0; x < frame.width / 2; ++x) {
                destination[2 * x] = u[x];
                destination[2 * x + 1] = v[x];
            }
        }
    }
    stride = frame.width;
    return true;
}

// An HDR (PQ or HLG) frame decoded on the CPU, deeper than 8 bits: planar
// little-endian YUV of 10 to 16 bits, 4:2:0, 4:2:2 or 4:4:4. The video
// processor takes it as P010 and keeps ten of its bits, where the BGRA copy
// keeps eight and bands. An 8-bit or SDR frame stays on the routes above.
inline bool softwareP010Eligible(const AVFrame& frame) noexcept {
    if (frame.width <= 0 || frame.height <= 0 || (frame.width & 1) || (frame.height & 1)) return false;
    if (frame.color_trc != AVCOL_TRC_SMPTE2084 && frame.color_trc != AVCOL_TRC_ARIB_STD_B67) return false;
    const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
    if (!descriptor || descriptor->nb_components < 3) return false;
    constexpr std::uint64_t refused = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL |
        AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BAYER;
    if (!(descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) || (descriptor->flags & refused)) return false;
    if (descriptor->log2_chroma_w > 1 || descriptor->log2_chroma_h > 1) return false;
    for (int component = 0; component < 3; ++component) {
        const auto& part = descriptor->comp[component];
        // One plane each, a whole 16-bit word a sample, its bits at the bottom.
        if (part.plane != component || part.step != 2 || part.offset != 0 || part.shift != 0 ||
            part.depth < 10 || part.depth > 16 || part.depth != descriptor->comp[0].depth) return false;
    }
    const int chromaWidth = frame.width >> descriptor->log2_chroma_w;
    const auto hasPlane = [&](int plane, int samples) {
        const auto pitch = static_cast<std::int64_t>(frame.linesize[plane]);
        return frame.data[plane] && (pitch < 0 ? -pitch : pitch) >= static_cast<std::int64_t>(samples) * 2;
    };
    return hasPlane(0, frame.width) && hasPlane(1, chromaWidth) && hasPlane(2, chromaWidth);
}

// Pack such a frame into tightly pitched P010: the Y plane, then interleaved
// UV at half size each way, every sample a 16-bit word with its bits at the
// top. Code values are kept (a deeper sample is cut to sixteen bits' worth of
// top, which P010 readers take ten of); chroma finer than 4:2:0 is averaged
// down to it. No matrix, range or transfer conversion occurs. `stride` is the
// row pitch in bytes. Rejection leaves both outputs untouched.
inline bool packSoftwareP010(const AVFrame& frame, std::vector<std::uint8_t>& bytes, int& stride) {
    if (!softwareP010Eligible(frame)) return false;
    const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
    const int shift = 16 - descriptor->comp[0].depth;
    // Source chroma samples for each one of P010's, across and down.
    const int across = descriptor->log2_chroma_w == 1 ? 1 : 2;
    const int down = descriptor->log2_chroma_h == 1 ? 1 : 2;
    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    if (width > std::numeric_limits<std::size_t>::max() / height / 3) return false;
    const auto yBytes = width * height * 2;
    if (yBytes > bytes.max_size() || yBytes / 2 > bytes.max_size() - yBytes) return false;
    bytes.resize(yBytes + yBytes / 2);
    const auto word = [](const std::uint8_t* row, int x) {
        std::uint16_t value;
        std::memcpy(&value, row + static_cast<std::ptrdiff_t>(x) * 2, sizeof(value));
        return static_cast<unsigned>(value);
    };
    const auto store = [](std::uint8_t* row, std::size_t x, unsigned value) {
        const auto sample = static_cast<std::uint16_t>(value);
        std::memcpy(row + x * 2, &sample, sizeof(sample));
    };
    for (int y = 0; y < frame.height; ++y) {
        const auto* source = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        auto* destination = bytes.data() + static_cast<std::size_t>(y) * width * 2;
        for (int x = 0; x < frame.width; ++x) store(destination, static_cast<std::size_t>(x), word(source, x) << shift);
    }
    const unsigned count = static_cast<unsigned>(across * down);
    for (int y = 0; y < frame.height / 2; ++y) {
        auto* destination = bytes.data() + yBytes + static_cast<std::size_t>(y) * width * 2;
        for (int plane = 1; plane <= 2; ++plane) {
            const auto* first = frame.data[plane] + static_cast<std::ptrdiff_t>(y * down) * frame.linesize[plane];
            const auto* second = down == 2 ? first + frame.linesize[plane] : first;
            for (int x = 0; x < frame.width / 2; ++x) {
                unsigned sum = word(first, x * across);
                if (across == 2) sum += word(first, x * across + 1);
                if (down == 2) {
                    sum += word(second, x * across);
                    if (across == 2) sum += word(second, x * across + 1);
                }
                store(destination, static_cast<std::size_t>(x) * 2 + static_cast<std::size_t>(plane - 1),
                      ((sum + count / 2) / count) << shift);
            }
        }
    }
    stride = frame.width * 2;
    return true;
}

}  // namespace quaddeck
