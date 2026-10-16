#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
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

}  // namespace quaddeck
