#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace quaddeck {

enum class VideoColorMatrix { Bt601, Bt709 };

struct VideoColorDescription {
    VideoColorMatrix matrix{VideoColorMatrix::Bt601};
    bool fullRange{};
    // BT.2020 matrix and PQ (SMPTE ST 2084) transfer: an HDR10 stream. The
    // 8-bit SDR path still approximates it with the 709 matrix; only the
    // 10-bit PQ output mode reads these and passes the stream through.
    bool bt2020{};
    bool pq{};
};

// Both decode paths use the original dimensions, before presentation scaling.
// Untagged/unsupported matrices use BT.709 above SD (720x576), otherwise 601.
// This SDR pipeline supports 601/709 only. BT.2020 (including SDR), other
// matrices and unknown tags deliberately use the same fallback on both paths;
// they are approximations, not colour-managed output. Gamut/transfer conversion
// and HDR tone mapping remain outside the current 8-bit presentation pipeline.
// A subtitle script's colours are matched to the same choice
// (VideoSource's colourMatrix), so this is the one place the rule lives.
inline VideoColorMatrix videoColorMatrixFor(AVColorSpace space, int width, int height) {
    switch (space) {
    case AVCOL_SPC_BT709: return VideoColorMatrix::Bt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return VideoColorMatrix::Bt601;
    default: return width > 720 || height > 576 ? VideoColorMatrix::Bt709 : VideoColorMatrix::Bt601;
    }
}

// The stream's range when it gives one. Without it, RGB and the legacy JPEG
// YUV formats are full range and other YUV is limited.
inline bool videoColorFullRange(AVColorRange range, AVPixelFormat format) {
    if (range == AVCOL_RANGE_JPEG) return true;
    if (range == AVCOL_RANGE_MPEG) return false;
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P: return true;
    default: break;
    }
    const auto* descriptor = av_pix_fmt_desc_get(format);
    return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB);
}

inline VideoColorDescription videoColorDescription(const AVFrame* frame,
                                                   int width, int height) {
    VideoColorDescription color;
    color.matrix = videoColorMatrixFor(frame ? frame->colorspace : AVCOL_SPC_UNSPECIFIED, width, height);
    if (!frame) return color;
    color.bt2020 = frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL;
    color.pq = frame->color_trc == AVCOL_TRC_SMPTE2084;
    color.fullRange = videoColorFullRange(frame->color_range, static_cast<AVPixelFormat>(frame->format));
    return color;
}

// Call after every sws_getCachedContext and before sws_scale: a stream can
// change matrix/range without changing its dimensions or pixel format.
inline int configureSoftwareColorspace(SwsContext* converter, const AVFrame& frame) {
    const auto color = videoColorDescription(&frame, frame.width, frame.height);
    const int space = color.matrix == VideoColorMatrix::Bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    const int* coefficients = sws_getCoefficients(space);
    // BGRA is always full range. Brightness=0 and unity contrast/saturation.
    return sws_setColorspaceDetails(converter, coefficients, color.fullRange ? 1 : 0,
                                   coefficients, 1, 0, 1 << 16, 1 << 16);
}

}  // namespace quaddeck
