#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace quaddeck {

enum class VideoColorMatrix { Bt601, Bt709 };

struct VideoColorDescription {
    VideoColorMatrix matrix{VideoColorMatrix::Bt601};
    bool fullRange{};
    // PQ (SMPTE ST 2084) transfer: an HDR10 stream, taken as BT.2020 whatever
    // its matrix tag says. The renderer passes it through to a 10-bit PQ
    // output, or brings it down to SDR itself (its tone-map pass) where the
    // output is SDR. BT.2020 without PQ is still approximated with the 709
    // matrix on the SDR path.
    bool bt2020{};
    bool pq{};
    // HLG (ARIB STD-B67) transfer: the other HDR, BT.2020 as well. The
    // renderer makes PQ of it for the PQ output and brings it down to SDR
    // where the output is SDR, as it does an HDR10 picture.
    bool hlg{};
    // The 4:2:0 chroma sits on the luma row (top-left), as UHD discs have
    // it, rather than between two rows (left).
    bool chromaTopLeft{};
    bool hdr() const { return pq || hlg; }
};

// Both decode paths use the original dimensions, before presentation scaling.
// Untagged/unsupported matrices use BT.709 above SD (720x576), otherwise 601.
// This SDR pipeline supports 601/709 only. BT.2020 SDR, other matrices and
// unknown tags deliberately use the same fallback on both paths; they are
// approximations, not colour-managed output. An HDR picture (PQ or HLG) is
// the exception: it is decoded with the BT.2020 matrix on both paths and the
// renderer converts its transfer and gamut.
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
    color.hlg = frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
    color.chromaTopLeft = frame->chroma_location == AVCHROMA_LOC_TOPLEFT;
    color.fullRange = videoColorFullRange(frame->color_range, static_cast<AVPixelFormat>(frame->format));
    return color;
}

// A stream can name its colour only in its container (Matroska's Colour, an
// MP4 colr box) while its bitstream names nothing; the decoder then hands
// out frames that name nothing, and an HDR10 film is taken for SDR. What a
// frame leaves unsaid is taken from the container; what it says stands.
inline void inheritContainerColor(AVFrame& frame, AVColorTransferCharacteristic transfer,
                                  AVColorPrimaries primaries, AVColorSpace matrix) {
    if (frame.color_trc == AVCOL_TRC_UNSPECIFIED) frame.color_trc = transfer;
    if (frame.color_primaries == AVCOL_PRI_UNSPECIFIED) frame.color_primaries = primaries;
    if (frame.colorspace == AVCOL_SPC_UNSPECIFIED) frame.colorspace = matrix;
}

// SDR white in an HDR10 picture, in nits (BT.2408's reference white): what
// the tone-map pass brings to the SDR output's white.
inline constexpr float kHdrReferenceWhiteNits = 203.0F;

// How bright an HDR10 picture is said to go, in nits, for the curve that
// brings it down to SDR: the stream's content light level (MaxCLL) when it
// gives one, else its mastering display's peak, else 1000, what most HDR10 is
// graded for. Never below SDR white, where there is nothing to bring down.
inline float hdrSourcePeakNits(const AVFrame* frame) {
    double peak = 0.0;
    if (frame) {
        if (const auto* light = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
            peak = reinterpret_cast<const AVContentLightMetadata*>(light->data)->MaxCLL;
        }
        if (!(peak > 0.0)) {
            if (const auto* display = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
                const auto* mastering = reinterpret_cast<const AVMasteringDisplayMetadata*>(display->data);
                if (mastering->has_luminance && mastering->max_luminance.den != 0) {
                    peak = av_q2d(mastering->max_luminance);
                }
            }
        }
    }
    if (!(peak > 0.0)) peak = 1000.0;
    return static_cast<float>(std::clamp(peak, static_cast<double>(kHdrReferenceWhiteNits), 10000.0));
}

// The display an HLG picture is made PQ for: BT.2100's nominal 1000 nits,
// where its system gamma is 1.2. Also the peak its way down to SDR starts at.
inline constexpr float kHlgPeakNits = 1000.0F;

// How bright an HDR picture is said to go: an HDR10 one by its metadata, an
// HLG one by the display it is rendered for.
inline float hdrPicturePeakNits(const AVFrame* frame) {
    return frame && frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ? kHlgPeakNits : hdrSourcePeakNits(frame);
}

// HDR10's static metadata as a swap chain is told it (DXGI_HDR_METADATA_HDR10
// has these fields in these units): chromaticities in 1/50000, the mastering
// display's peak in nits and its black in 1/10000 nit, the content's
// brightest pixel and brightest frame average in nits. `present` is false
// for a stream that gives neither a mastering display nor a light level;
// nothing is then said for it. What a stream leaves out is BT.2020 and D65,
// and zero for a level, which means unknown.
struct HdrStaticMetadata {
    bool present{};
    std::uint16_t red[2]{35400, 14600};
    std::uint16_t green[2]{8500, 39850};
    std::uint16_t blue[2]{6550, 2300};
    std::uint16_t white[2]{15635, 16450};
    std::uint32_t maxMasteringNits{};
    std::uint32_t minMasteringTenThousandths{};
    std::uint16_t maxContentNits{};
    std::uint16_t maxFrameAverageNits{};
    bool operator==(const HdrStaticMetadata&) const = default;
};

inline HdrStaticMetadata hdrStaticMetadata(const AVFrame* frame) {
    HdrStaticMetadata metadata;
    if (!frame) return metadata;
    const auto scaled = [](AVRational value, double scale, double limit) {
        if (value.den == 0) return 0.0;
        return std::clamp(std::round(av_q2d(value) * scale), 0.0, limit);
    };
    if (const auto* display = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        const auto* mastering = reinterpret_cast<const AVMasteringDisplayMetadata*>(display->data);
        if (mastering->has_primaries) {
            std::uint16_t* const primaries[] = {metadata.red, metadata.green, metadata.blue};
            for (int colour = 0; colour < 3; ++colour) {
                for (int axis = 0; axis < 2; ++axis) {
                    primaries[colour][axis] = static_cast<std::uint16_t>(
                        scaled(mastering->display_primaries[colour][axis], 50000.0, 65535.0));
                }
            }
            for (int axis = 0; axis < 2; ++axis) {
                metadata.white[axis] = static_cast<std::uint16_t>(
                    scaled(mastering->white_point[axis], 50000.0, 65535.0));
            }
            metadata.present = true;
        }
        if (mastering->has_luminance) {
            metadata.maxMasteringNits = static_cast<std::uint32_t>(scaled(mastering->max_luminance, 1.0, 10000.0));
            metadata.minMasteringTenThousandths =
                static_cast<std::uint32_t>(scaled(mastering->min_luminance, 10000.0, 100000000.0));
            metadata.present = true;
        }
    }
    if (const auto* light = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        const auto* level = reinterpret_cast<const AVContentLightMetadata*>(light->data);
        metadata.maxContentNits = static_cast<std::uint16_t>(std::min<unsigned>(level->MaxCLL, 65535U));
        metadata.maxFrameAverageNits = static_cast<std::uint16_t>(std::min<unsigned>(level->MaxFALL, 65535U));
        metadata.present = metadata.present || level->MaxCLL > 0 || level->MaxFALL > 0;
    }
    return metadata;
}

// A Dolby Vision stream whose base layer is not HDR10, HLG or SDR (profile 5,
// and the later profiles that say they are compatible with nothing) is coded
// in Dolby's own colour space and needs its per-frame reshaping to be shown;
// this player has neither, so its colours come out wrong (green and purple).
// The other profiles carry a picture any player can show, and are played as
// that. `profile` and `compatibility` are the stream's configuration record
// (dv_profile, dv_bl_signal_compatibility_id).
inline bool dolbyVisionNeedsItsOwnDecoding(int profile, int compatibility) {
    return profile == 5 || (profile >= 8 && compatibility == 0);
}

// Call after every sws_getCachedContext and before sws_scale: a stream can
// change matrix/range without changing its dimensions or pixel format.
inline int configureSoftwareColorspace(SwsContext* converter, const AVFrame& frame) {
    const auto color = videoColorDescription(&frame, frame.width, frame.height);
    // An HDR picture keeps its PQ or HLG codes through swscale; with its own
    // matrix they are the BT.2020 R'G'B' the renderer's passes expect.
    const int space = color.hdr() ? SWS_CS_BT2020
                    : color.matrix == VideoColorMatrix::Bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    const int* coefficients = sws_getCoefficients(space);
    // BGRA is always full range. Brightness=0 and unity contrast/saturation.
    return sws_setColorspaceDetails(converter, coefficients, color.fullRange ? 1 : 0,
                                   coefficients, 1, 0, 1 << 16, 1 << 16);
}

}  // namespace quaddeck
