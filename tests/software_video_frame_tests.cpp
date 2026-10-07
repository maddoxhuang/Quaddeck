#include "SoftwareVideoFrame.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace quaddeck;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::array<std::uint8_t, 16> yCodes{
    0, 16, 128, 255, 1, 17, 129, 254, 2, 18, 130, 253, 3, 19, 131, 252};
constexpr std::array<std::uint8_t, 4> uCodes{0, 23, 44, 255};
constexpr std::array<std::uint8_t, 4> vCodes{1, 27, 33, 128};
constexpr std::array<std::uint8_t, 8> uvCodes{0, 1, 23, 27, 44, 33, 255, 128};
const std::vector<std::uint8_t> expected{
    0, 16, 128, 255, 1, 17, 129, 254, 2, 18, 130, 253, 3, 19, 131, 252,
    0, 1, 23, 27, 44, 33, 255, 128};

struct Fixture {
    AVFrame frame{};
    std::array<std::vector<std::uint8_t>, 3> storage;

    explicit Fixture(AVPixelFormat format = AV_PIX_FMT_YUV420P) {
        frame.width = 4;
        frame.height = 4;
        frame.format = format;
        frame.colorspace = AVCOL_SPC_UNSPECIFIED;
        frame.color_primaries = AVCOL_PRI_UNSPECIFIED;
        frame.color_trc = AVCOL_TRC_UNSPECIFIED;
        frame.color_range = AVCOL_RANGE_UNSPECIFIED;
        plane(0, 4, 4, 6, false, yCodes.data());
        if (format == AV_PIX_FMT_NV12) {
            plane(1, 4, 2, 8, false, uvCodes.data());
        } else {
            plane(1, 2, 2, 4, false, uCodes.data());
            plane(2, 2, 2, 5, false, vCodes.data());
        }
    }

    void plane(int index, int rowBytes, int rows, int pitch, bool reverse,
               const std::uint8_t* codes) {
        auto& bytes = storage[index];
        bytes.assign(static_cast<std::size_t>(pitch) * rows, 0xEE);
        for (int row = 0; row < rows; ++row) {
            const int physicalRow = reverse ? rows - 1 - row : row;
            for (int x = 0; x < rowBytes; ++x) {
                bytes[static_cast<std::size_t>(physicalRow) * pitch + x] = codes[row * rowBytes + x];
            }
        }
        frame.data[index] = bytes.data() + (reverse ? static_cast<std::size_t>(rows - 1) * pitch : 0);
        frame.linesize[index] = reverse ? -pitch : pitch;
    }
};

void requirePacked(const AVFrame& frame) {
    require(softwareNv12Eligible(frame), "Supported SDR frame must be eligible");
    std::vector<std::uint8_t> output{99};
    int stride = -1;
    require(packSoftwareNv12(frame, output, stride), "Supported SDR frame must pack");
    require(stride == 4, "NV12 stride must equal original width");
    require(output == expected, "NV12 must preserve logical rows and original YUV codes");
}

void requireRejected(const AVFrame& frame) {
    require(!softwareNv12Eligible(frame), "Unsupported frame must not be eligible");
    const std::vector<std::uint8_t> original{7, 8, 9};
    auto output = original;
    int stride = 17;
    require(!packSoftwareNv12(frame, output, stride), "Unsupported frame must be rejected");
    require(output == original && stride == 17, "Rejection must retain existing fallback outputs");
}

void strideAndRangeCases() {
    for (const auto format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}) {
        for (unsigned reverse = 0; reverse < 8; ++reverse) {
            Fixture fixture(format);
            fixture.plane(0, 4, 4, 6, (reverse & 1) != 0, yCodes.data());
            fixture.plane(1, 2, 2, 4, (reverse & 2) != 0, uCodes.data());
            fixture.plane(2, 2, 2, 5, (reverse & 4) != 0, vCodes.data());
            for (const auto range : {AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG}) {
                fixture.frame.color_range = range;
                requirePacked(fixture.frame);
            }
        }
    }
    for (unsigned reverse = 0; reverse < 4; ++reverse) {
        Fixture fixture(AV_PIX_FMT_NV12);
        fixture.plane(0, 4, 4, 6, (reverse & 1) != 0, yCodes.data());
        fixture.plane(1, 4, 2, 8, (reverse & 2) != 0, uvCodes.data());
        requirePacked(fixture.frame);
    }
    // Padded storage is not required: tightly pitched source rows work too.
    Fixture tight;
    tight.plane(0, 4, 4, 4, false, yCodes.data());
    tight.plane(1, 2, 2, 2, false, uCodes.data());
    tight.plane(2, 2, 2, 2, false, vCodes.data());
    requirePacked(tight.frame);
}

void metadataCases() {
    Fixture fixture;
    for (const auto matrix : {AVCOL_SPC_UNSPECIFIED, AVCOL_SPC_BT709, AVCOL_SPC_BT470BG, AVCOL_SPC_SMPTE170M}) {
        fixture.frame.colorspace = matrix;
        for (const auto primaries : {AVCOL_PRI_UNSPECIFIED, AVCOL_PRI_BT709, AVCOL_PRI_BT470BG, AVCOL_PRI_SMPTE170M}) {
            fixture.frame.color_primaries = primaries;
            for (const auto transfer : {AVCOL_TRC_UNSPECIFIED, AVCOL_TRC_BT709, AVCOL_TRC_SMPTE170M, AVCOL_TRC_GAMMA22}) {
                fixture.frame.color_trc = transfer;
                requirePacked(fixture.frame);
            }
        }
    }
    fixture.frame.colorspace = AVCOL_SPC_BT709;
    fixture.frame.color_primaries = AVCOL_PRI_BT709;
    fixture.frame.color_trc = AVCOL_TRC_BT709;
    for (const auto matrix : {AVCOL_SPC_BT2020_NCL, AVCOL_SPC_BT2020_CL, AVCOL_SPC_SMPTE240M, AVCOL_SPC_RGB}) {
        fixture.frame.colorspace = matrix;
        requireRejected(fixture.frame);
    }
    fixture.frame.colorspace = AVCOL_SPC_BT709;
    for (const auto primaries : {AVCOL_PRI_BT2020, AVCOL_PRI_SMPTE431, AVCOL_PRI_SMPTE432, AVCOL_PRI_BT470M}) {
        fixture.frame.color_primaries = primaries;
        requireRejected(fixture.frame);
    }
    fixture.frame.color_primaries = AVCOL_PRI_BT709;
    for (const auto transfer : {AVCOL_TRC_SMPTE2084, AVCOL_TRC_ARIB_STD_B67, AVCOL_TRC_LINEAR,
                               AVCOL_TRC_BT2020_10, AVCOL_TRC_BT2020_12,
                               AVCOL_TRC_GAMMA28, AVCOL_TRC_IEC61966_2_1}) {
        fixture.frame.color_trc = transfer;
        requireRejected(fixture.frame);
    }
}

void unsupportedStorageCases() {
    Fixture fixture;
    for (const auto format : {AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV422P,
                             AV_PIX_FMT_BGRA, AV_PIX_FMT_D3D11}) {
        auto frame = fixture.frame;
        frame.format = format;
        requireRejected(frame);
    }
    constexpr std::array<std::array<int, 2>, 5> invalidDimensions{{
        {3, 4}, {4, 3}, {0, 4}, {4, 0}, {-4, 4}}};
    for (const auto dimensions : invalidDimensions) {
        auto frame = fixture.frame;
        frame.width = dimensions[0];
        frame.height = dimensions[1];
        requireRejected(frame);
    }
    for (int plane = 0; plane < 3; ++plane) {
        auto frame = fixture.frame;
        frame.data[plane] = nullptr;
        requireRejected(frame);
        for (const auto stride : {0, 1, -1}) {
            frame = fixture.frame;
            frame.linesize[plane] = stride;
            requireRejected(frame);
        }
    }
    Fixture nv12(AV_PIX_FMT_NV12);
    nv12.frame.linesize[1] = 3;
    requireRejected(nv12.frame);
    nv12.frame.linesize[1] = -3;
    requireRejected(nv12.frame);
}
}  // namespace

int main() {
    try {
        strideAndRangeCases();
        metadataCases();
        unsupportedStorageCases();
        std::cout << "Software video frame tests PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Software video frame tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
