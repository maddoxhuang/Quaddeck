#include "VideoSource.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/avutil.h>
}

using namespace quaddeck;
using Microsoft::WRL::ComPtr;

namespace {

struct MediaInfo {
    std::string codec;
    int width{};
    int height{};
    AVRational frameRate{};
    const char* frameRateKind{"unknown"};
    double duration{};
    double precedingKeyframe{-1.0};
    double followingKeyframe{-1.0};
    double maximumKeyframeGap{};
    double maximumKeyframeGapStart{-1.0};
    double maximumKeyframeGapEnd{-1.0};
    bool hasMaximumKeyframeGap{};
    double avSeekMilliseconds{-1.0};
    double target{};
    double exactTarget{};
    double intervalTarget{};
    double exactIntervalTarget{};
    double endIntervalTarget{};
    double streamTimeBase{};
    double firstPacketAtOrAfterTarget{-1.0};
    double firstPacketAtOrAfterExactTarget{-1.0};
    double expectedPacketPtsAtTarget{-1.0};
    double expectedPacketPtsAtExactTarget{-1.0};
    double firstPacketAtOrAfterEndTarget{-1.0};
    double expectedPacketPtsAtEndTarget{-1.0};
    double lastVideoPacketPts{-1.0};
};

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

// Request headers for an HTTP input (a media server's token), from the
// command line; empty for a file.
std::string gHeaders;

bool inspectMedia(const std::wstring& path, double target, MediaInfo& info) {
    AVFormatContext* format = nullptr;
    const std::string utf8 = wideToUtf8(path);
    AVDictionary* options = nullptr;
    if (!gHeaders.empty()) {
        av_dict_set(&options, "headers", gHeaders.c_str(), 0);
        av_dict_set(&options, "user_agent", "QuadDeckSeekProbe", 0);
    }
    const int opened = avformat_open_input(&format, utf8.c_str(), nullptr, options ? &options : nullptr);
    av_dict_free(&options);
    if (opened < 0) return false;
    const auto closeFormat = [&] { avformat_close_input(&format); };
    if (avformat_find_stream_info(format, nullptr) < 0) {
        closeFormat();
        return false;
    }
    const int streamIndex = av_find_best_stream(
        format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        closeFormat();
        return false;
    }

    AVStream* stream = format->streams[streamIndex];
    info.codec = avcodec_get_name(stream->codecpar->codec_id);
    info.width = stream->codecpar->width;
    info.height = stream->codecpar->height;
    if (format->duration != AV_NOPTS_VALUE) {
        info.duration = static_cast<double>(format->duration) / AV_TIME_BASE;
    }
    info.streamTimeBase = av_q2d(stream->time_base);
    info.target = info.duration > 1.0
        ? std::clamp(target, 0.0, info.duration - 0.5)
        : std::max(0.0, target);
    info.exactTarget = info.duration > info.target + 5.5 ? info.target + 5.0 :
        (info.target >= 5.0 ? info.target - 5.0 : info.target);

    const std::int64_t streamStartTimestamp =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    const auto toTimestamp = [&](double seconds) {
        return streamTimestampForSeconds(
            seconds, streamStartTimestamp,
            stream->time_base.num, stream->time_base.den);
    };
    const std::int64_t requestedTargetTimestamp = toTimestamp(info.target);
    const std::int64_t exactTargetTimestamp = toTimestamp(info.exactTarget);
    const std::int64_t endTargetTimestamp = toTimestamp(info.duration);
    info.intervalTarget = static_cast<double>(
        requestedTargetTimestamp - streamStartTimestamp) * info.streamTimeBase;
    info.exactIntervalTarget = static_cast<double>(
        exactTargetTimestamp - streamStartTimestamp) * info.streamTimeBase;
    info.endIntervalTarget = static_cast<double>(
        endTargetTimestamp - streamStartTimestamp) * info.streamTimeBase;
    std::int64_t demuxTargetTimestamp = requestedTargetTimestamp;
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        demuxTargetTimestamp = std::min(
            demuxTargetTimestamp, streamStartTimestamp + stream->duration - 1);
    }
    const auto seekStarted = std::chrono::steady_clock::now();
    const int seekResult = av_seek_frame(
        format, streamIndex, demuxTargetTimestamp, AVSEEK_FLAG_BACKWARD);
    info.avSeekMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - seekStarted).count();
    if (seekResult < 0) {
        closeFormat();
        return false;
    }
    const std::int64_t startTimestamp =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    if (av_seek_frame(format, streamIndex, startTimestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        closeFormat();
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        closeFormat();
        return false;
    }
    std::int64_t guessedPacketDuration = 0;
    const AVRational guessedRate = av_guess_frame_rate(format, stream, nullptr);
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        info.frameRate = stream->avg_frame_rate;
        info.frameRateKind = "average";
    } else if (guessedRate.num > 0 && guessedRate.den > 0) {
        info.frameRate = guessedRate;
        info.frameRateKind = "guessed";
    }
    if (guessedRate.num > 0 && guessedRate.den > 0) {
        guessedPacketDuration = std::max<std::int64_t>(0, av_rescale_q_rnd(
            1, {guessedRate.den, guessedRate.num}, stream->time_base,
            AV_ROUND_UP));
    }
    std::vector<std::int64_t> packetKeyTimestamps;
    double coveringTargetPts = -1.0;
    double coveringExactTargetPts = -1.0;
    double coveringEndTargetPts = -1.0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == streamIndex) {
            const auto timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (timestamp != AV_NOPTS_VALUE) {
                const double seconds = static_cast<double>(
                    timestamp - streamStartTimestamp) *
                    info.streamTimeBase;
                const auto recordFirstFollowing = [&](std::int64_t probeTarget,
                                                      double& first) {
                    if (timestamp >= probeTarget &&
                        (first < 0.0 || seconds < first)) {
                        first = seconds;
                    }
                };
                recordFirstFollowing(
                    requestedTargetTimestamp, info.firstPacketAtOrAfterTarget);
                recordFirstFollowing(
                    exactTargetTimestamp, info.firstPacketAtOrAfterExactTarget);
                recordFirstFollowing(
                    endTargetTimestamp, info.firstPacketAtOrAfterEndTarget);
                const std::int64_t packetDuration = packet->duration > 0
                    ? packet->duration : guessedPacketDuration;
                const auto recordCovering = [&](std::int64_t probeTarget,
                                                double& covering) {
                    const bool startsAtTarget = timestamp == probeTarget;
                    const bool coversTarget = timestamp < probeTarget &&
                        packetDuration > 0 &&
                        !frameEndsAtOrBeforeSeekTarget(
                            timestamp, packetDuration, probeTarget);
                    if ((startsAtTarget || coversTarget) &&
                        (covering < 0.0 || seconds > covering)) {
                        covering = seconds;
                    }
                };
                recordCovering(requestedTargetTimestamp, coveringTargetPts);
                recordCovering(exactTargetTimestamp, coveringExactTargetPts);
                recordCovering(endTargetTimestamp, coveringEndTargetPts);
                info.lastVideoPacketPts = std::max(info.lastVideoPacketPts, seconds);

                if (packet->flags & AV_PKT_FLAG_KEY) {
                    packetKeyTimestamps.push_back(timestamp);
                }
            }
        }
        av_packet_unref(packet);
    }
    // Packet demux order need not be presentation order. Sort exact timestamps
    // before measuring the container's packet-key gaps so B-frame reordering
    // and duplicate key flags cannot overstate the longest observed interval.
    std::sort(packetKeyTimestamps.begin(), packetKeyTimestamps.end());
    packetKeyTimestamps.erase(
        std::unique(packetKeyTimestamps.begin(), packetKeyTimestamps.end()),
        packetKeyTimestamps.end());
    for (std::size_t index = 0; index < packetKeyTimestamps.size(); ++index) {
        const double seconds = static_cast<double>(
            packetKeyTimestamps[index] - streamStartTimestamp) *
            info.streamTimeBase;
        if (seconds <= info.target + 0.0005) {
            info.precedingKeyframe = seconds;
        } else if (info.followingKeyframe < 0.0) {
            info.followingKeyframe = seconds;
        }
        if (index == 0) continue;
        const double previousSeconds = static_cast<double>(
            packetKeyTimestamps[index - 1] - streamStartTimestamp) *
            info.streamTimeBase;
        const double gap = seconds - previousSeconds;
        if (!info.hasMaximumKeyframeGap || gap > info.maximumKeyframeGap) {
            info.maximumKeyframeGap = gap;
            info.maximumKeyframeGapStart = previousSeconds;
            info.maximumKeyframeGapEnd = seconds;
            info.hasMaximumKeyframeGap = true;
        }
    }
    info.expectedPacketPtsAtTarget = coveringTargetPts >= 0.0
        ? coveringTargetPts : info.firstPacketAtOrAfterTarget;
    info.expectedPacketPtsAtExactTarget = coveringExactTargetPts >= 0.0
        ? coveringExactTargetPts : info.firstPacketAtOrAfterExactTarget;
    info.expectedPacketPtsAtEndTarget = coveringEndTargetPts >= 0.0
        ? coveringEndTargetPts : info.firstPacketAtOrAfterEndTarget;
    if (info.expectedPacketPtsAtEndTarget < 0.0) {
        // No interval reaches the container endpoint: VideoSource's EOF path
        // deliberately promotes the final decoded preroll frame.
        info.expectedPacketPtsAtEndTarget = info.lastVideoPacketPts;
    }
    av_packet_free(&packet);
    closeFormat();
    return true;
}

DecodeMode parseMode(const std::wstring& value) {
    if (value == L"hardware") return DecodeMode::Hardware;
    if (value == L"software") return DecodeMode::Software;
    return DecodeMode::Automatic;
}

const char* modeName(DecodeMode mode) {
    switch (mode) {
    case DecodeMode::Hardware: return "hardware";
    case DecodeMode::Software: return "software";
    default: return "automatic";
    }
}

struct SeekResult {
    struct Observation {
        double pts{};
        double duration{};
        bool exact{};
    };

    bool exact{};
    double firstPts{-1.0};
    double exactPts{-1.0};
    double exactDuration{};
    double firstMilliseconds{-1.0};
    double exactMilliseconds{-1.0};
    std::vector<Observation> observations;
};

SeekResult runSeek(VideoSource& source, double target, bool preview, double timeoutSeconds,
                   int keyframeDirection = 0, bool keyframe = false) {
    SeekResult result;
    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t seekGeneration = keyframe
        ? source.requestKeyframeSeek(target, keyframeDirection)
        : source.requestSeek(target, preview);
    std::uint64_t lastSerial = 0;
    while (std::chrono::duration<double>(
               std::chrono::steady_clock::now() - started).count() < timeoutSeconds) {
        const auto frame = source.frameForTime(target, false);
        if (frame && frame->serial != lastSerial) {
            lastSerial = frame->serial;
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            const bool exact = frame->seekGeneration == seekGeneration &&
                               frame->exactSeekFrame;
            result.observations.push_back({frame->pts, frame->duration, exact});
            if (result.firstPts < 0.0) {
                result.firstPts = frame->pts;
                result.firstMilliseconds = elapsed;
            }
            std::cout << "  visible pts=" << std::fixed << std::setprecision(3)
                      << frame->pts << " duration=" << frame->duration
                      << " end=" << (frame->pts + frame->duration)
                      << " elapsed_ms=" << std::setprecision(1)
                      << elapsed << " serial=" << frame->serial
                      << " exact=" << exact << '\n';
            if (exact) {
                result.exact = true;
                result.exactPts = frame->pts;
                result.exactDuration = frame->duration;
                result.exactMilliseconds = elapsed;
                break;
            }
        }
        if (!source.error().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
}

bool exactObservationRepresentsTarget(
    const SeekResult::Observation& observation, double target,
    double expectedPacketPts, double streamTimeBase) {
    constexpr double numericEpsilon = 0.000001;
    const double timestampMatchEpsilon = std::max(
        0.000000001, std::max(0.0, streamTimeBase) * 0.25);
    if (!std::isfinite(observation.pts) || !std::isfinite(observation.duration)) {
        return false;
    }
    // The full packet scan independently selects the interval that should
    // represent this target (or the first following PTS for a real gap). This
    // catches both arbitrary overshoots and the subtler one-frame-late case.
    if (expectedPacketPts < 0.0 ||
        std::abs(observation.pts - expectedPacketPts) > timestampMatchEpsilon) {
        return false;
    }
    if (observation.pts > target + numericEpsilon) return true;
    if (observation.duration <= 0.0) {
        return observation.pts + numericEpsilon >= target;
    }
    // expectedPacketPts already distinguishes the exact-boundary next frame;
    // this small left tolerance only absorbs seconds-conversion roundoff.
    return observation.pts + observation.duration > target - numericEpsilon;
}

bool validatePreviewSeek(
    const SeekResult& result, double target,
    double expectedPacketPts, double streamTimeBase) {
    if (!result.exact || result.firstPts < 0.0 || result.observations.empty() ||
        !exactObservationRepresentsTarget(
            result.observations.back(), target,
            expectedPacketPts, streamTimeBase) ||
        !result.observations.back().exact) {
        return false;
    }
    std::size_t preExactFrames = 0;
    for (const auto& observation : result.observations) {
        if (!observation.exact) ++preExactFrames;
    }
    if (preExactFrames > 1) return false;
    if (preExactFrames == 1 &&
        (!seekPreviewEligible(result.observations.front().pts, target) ||
         result.observations.front().exact)) {
        return false;
    }
    return true;
}

bool validateExactSeek(
    const SeekResult& result, double target,
    double expectedPacketPts, double streamTimeBase) {
    return result.exact && result.observations.size() == 1 &&
           result.observations.front().exact &&
           exactObservationRepresentsTarget(
               result.observations.front(), target,
               expectedPacketPts, streamTimeBase);
}

bool validateEndSeek(
    const SeekResult& result, double expectedPacketPts, double streamTimeBase) {
    if (!result.exact || result.observations.size() != 1 ||
        !result.observations.front().exact || expectedPacketPts < 0.0) {
        return false;
    }
    const double epsilon = std::max(
        0.000000001, std::max(0.0, streamTimeBase) * 0.25);
    return std::abs(result.observations.front().pts - expectedPacketPts) <= epsilon;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "Usage: QuadDeckSeekProbe <video> [automatic|hardware|software] "
                     "[target-seconds] [timeout-seconds] [cache|direct|network] "
                     "[exact|keyframe|keyframe-forward] [http-headers]\n";
        return 2;
    }
    const std::wstring path = argv[1];
    const DecodeMode mode = argc >= 3 ? parseMode(argv[2]) : DecodeMode::Automatic;
    const double requestedTarget = argc >= 4 ? std::wcstod(argv[3], nullptr) : 30.0;
    const double timeout = argc >= 5 ? std::max(1.0, std::wcstod(argv[4], nullptr)) : 60.0;
    const CacheMode cacheMode = argc >= 6 && std::wstring(argv[5]) == L"cache" ? CacheMode::AllFiles
        : argc >= 6 && std::wstring(argv[5]) == L"direct" ? CacheMode::Off : CacheMode::Network;
    // Keyframe landing, the player's single-video seek: the first frame
    // decoded is the frame; the audio is re-aimed at it.
    const std::wstring landing = argc >= 7 ? argv[6] : L"exact";
    const bool keyframeLanding = landing == L"keyframe" || landing == L"keyframe-forward";
    const int keyframeDirection = landing == L"keyframe-forward" ? 1 : 0;
    if (argc >= 8) gHeaders = wideToUtf8(argv[7]);

    MediaInfo media;
    if (!inspectMedia(path, requestedTarget, media)) {
        std::cerr << "Could not inspect input media.\n";
        return 3;
    }
    const double target = media.target;
    std::cout << "media codec=" << media.codec
              << " size=" << media.width << 'x' << media.height;
    if (media.frameRate.num > 0 && media.frameRate.den > 0) {
        std::cout << " frame_rate=" << media.frameRate.num << '/'
                  << media.frameRate.den
                  << " frame_rate_fps=" << std::fixed << std::setprecision(3)
                  << av_q2d(media.frameRate);
    } else {
        std::cout << " frame_rate=unknown frame_rate_fps=unknown";
    }
    std::cout << " frame_rate_kind=" << media.frameRateKind
              << " duration=" << std::fixed << std::setprecision(3) << media.duration
              << " target=" << target
              << " interval_target=" << media.intervalTarget
              << " key_before=" << media.precedingKeyframe
              << " key_after=" << media.followingKeyframe;
    if (media.hasMaximumKeyframeGap) {
        std::cout << " max_packet_key_gap_s=" << media.maximumKeyframeGap
                  << " max_packet_key_gap_start_s="
                  << media.maximumKeyframeGapStart
                  << " max_packet_key_gap_end_s="
                  << media.maximumKeyframeGapEnd;
    } else {
        std::cout << " max_packet_key_gap_s=unknown"
                     " max_packet_key_gap_start_s=unknown"
                     " max_packet_key_gap_end_s=unknown";
    }
    std::cout << " first_packet_at_or_after=" << media.firstPacketAtOrAfterTarget
              << " expected_packet_pts=" << media.expectedPacketPtsAtTarget
              << " av_seek_ms=" << media.avSeekMilliseconds
              << " mode=" << modeName(mode) << '\n';

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    constexpr D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL actual{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT deviceResult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &actual, &context);
    if (FAILED(deviceResult)) {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex
                  << static_cast<unsigned long>(deviceResult) << '\n';
        return 4;
    }

    std::recursive_mutex deviceMutex;
    VideoSource source;
    SourceOptions sourceOptions;
    sourceOptions.httpHeaders = gHeaders;
    if (!source.open(path, device.Get(), context.Get(), &deviceMutex, mode, {}, cacheMode, sourceOptions)) {
        std::cerr << "VideoSource::open failed: " << source.error() << '\n';
        return 5;
    }
    const auto readyStarted = std::chrono::steady_clock::now();
    while (!source.ready() && source.error().empty() &&
           std::chrono::duration<double>(
               std::chrono::steady_clock::now() - readyStarted).count() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!source.ready()) {
        std::cerr << "Source did not become ready: " << source.error() << '\n';
        return 6;
    }

    if (keyframeLanding) {
        // The keyframe seek: where it landed, how long it took, and that the
        // audio was re-aimed there. No exactness to validate: landing near
        // the target is the point.
        std::cout << "keyframe seek target=" << target << " direction=" << keyframeDirection << ":\n";
        const SeekResult landingResult = runSeek(source, target, false, timeout, keyframeDirection, true);
        const auto landed = source.takeSeekLanding();
        const auto audio = source.audioDecodeStatus();
        std::cout << "  first_ms=" << landingResult.firstMilliseconds
                  << " exact_ms=" << landingResult.exactMilliseconds
                  << " landed_pts=" << landingResult.exactPts
                  << " reported=" << (landed ? landed->seconds : -1.0)
                  << " audio_generation=" << (landed ? landed->audioGeneration : 0)
                  << " audio_state_generation=" << audio.generation
                  << " key_before=" << media.precedingKeyframe
                  << " key_after=" << media.followingKeyframe << '\n';
        const bool landedOnKey = landingResult.exact && landed &&
            std::abs(landed->seconds - landingResult.exactPts) < 0.001 &&
            landed->audioGeneration == audio.generation &&
            (keyframeDirection > 0
                 ? std::abs(landingResult.exactPts - media.followingKeyframe) < 0.05 ||
                       std::abs(landingResult.exactPts - media.precedingKeyframe) < 0.05
                 : std::abs(landingResult.exactPts - media.precedingKeyframe) < 0.05);
        std::cout << "  result=" << (landedOnKey ? "PASS" : "FAIL") << '\n';
        source.close();
        return landedOnKey ? 0 : 7;
    }

    std::cout << "preview seek:\n";
    const SeekResult previewResult = runSeek(source, target, true, timeout);
    const bool previewPassed = validatePreviewSeek(
        previewResult, media.intervalTarget, media.expectedPacketPtsAtTarget,
        media.streamTimeBase);
    std::cout << "  first_ms=" << previewResult.firstMilliseconds
              << " exact_ms=" << previewResult.exactMilliseconds
              << " exact_pts=" << previewResult.exactPts
              << " exact_duration=" << previewResult.exactDuration
              << " exact_end=" << (previewResult.exactPts + previewResult.exactDuration)
              << " result=" << (previewPassed ? "PASS" : "FAIL") << '\n';

    const double exactTarget = media.exactTarget;
    std::cout << "exact-only seek target=" << exactTarget << ":\n";
    const SeekResult exactResult = runSeek(source, exactTarget, false, timeout);
    const bool exactPassed = validateExactSeek(
        exactResult, media.exactIntervalTarget,
        media.expectedPacketPtsAtExactTarget,
        media.streamTimeBase);
    std::cout << "  first_ms=" << exactResult.firstMilliseconds
              << " exact_ms=" << exactResult.exactMilliseconds
              << " exact_pts=" << exactResult.exactPts
              << " exact_duration=" << exactResult.exactDuration
              << " exact_end=" << (exactResult.exactPts + exactResult.exactDuration)
              << " result=" << (exactPassed ? "PASS" : "FAIL") << '\n';

    bool endPassed = true;
    if (media.duration > 0.0 && media.lastVideoPacketPts >= 0.0) {
        std::cout << "end seek target=" << media.duration << ":\n";
        const SeekResult endResult = runSeek(source, media.duration, false, timeout);
        endPassed = validateEndSeek(
            endResult, media.expectedPacketPtsAtEndTarget,
            media.streamTimeBase);
        std::cout << "  first_ms=" << endResult.firstMilliseconds
                  << " exact_ms=" << endResult.exactMilliseconds
                  << " exact_pts=" << endResult.exactPts
                  << " exact_duration=" << endResult.exactDuration
                  << " exact_end=" << (endResult.exactPts + endResult.exactDuration)
                  << " last_packet_pts=" << media.lastVideoPacketPts
                  << " expected_packet_pts=" << media.expectedPacketPtsAtEndTarget
                  << " result=" << (endPassed ? "PASS" : "FAIL") << '\n';
    }

    const auto cache = source.cacheStats();
    std::cout << "cache enabled=" << cache.enabled << " resident_bytes=" << cache.residentBytes
              << " ahead_bytes=" << cache.aheadBytes << " fetched_bytes=" << cache.fetchedBytes
              << " hits=" << cache.hits << " misses=" << cache.misses << " retries=" << cache.retries
              << " queue=" << source.queuedVideoFrames() << '/' << source.videoQueueCapacity() << '\n';
    const auto closeStarted = std::chrono::steady_clock::now();
    source.close();
    std::cout << "close_ms=" << std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - closeStarted).count() << '\n';
    if (cacheMode == CacheMode::AllFiles && !cache.enabled) return 8;
    return previewPassed && exactPassed && endPassed ? 0 : 7;
}
