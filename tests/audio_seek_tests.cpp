#include "AudioSampleTimeline.hpp"
#include "VideoSource.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace quaddeck {
// Exercise the real audio worker without a window, GPU or XAudio2 device.
struct AudioSeekTestAccess {
    static void start(VideoSource& source, const std::filesystem::path& path,
                      VideoSource::AudioSink sink, double target, bool cache = false) {
        source.path_ = path.wstring();
        source.audioSink_ = std::move(sink);
        source.running_.store(true);
        if (cache) source.cache_ = createFileCache(path.wstring(), CacheMode::AllFiles);
        source.requestAudioSeek(target);
        source.setAudioEnabled(true);
        source.audioWorker_ = std::thread(&VideoSource::audioWorkerMain, &source);
    }
    // Bytes the cache read from the file; only the audio cursor runs here,
    // and it reads nothing ahead, so this is what the worker asked for.
    static std::uint64_t fetched(const VideoSource& source) {
        return source.cache_ ? source.cache_->stats().fetchedBytes : 0;
    }
};
}

using namespace quaddeck;

static void check(int result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

static std::int16_t signal(int sample, int rate, int channel) {
    // Stereo values encode the exact sample index at 48 kHz. Resampling tests
    // use a smooth known signal, allowing content checks against physical time.
    if (rate == 48000) return static_cast<std::int16_t>((sample % 20000 - 10000) * (channel ? -1 : 1));
    const double seconds = static_cast<double>(sample) / rate;
    return static_cast<std::int16_t>(5000 * std::sin(2 * std::numbers::pi * 137 * seconds) +
        (channel ? -4000 : 4000) * std::sin(2 * std::numbers::pi * 311 * seconds));
}

static void makeAudio(const std::filesystem::path& path, int rate, int total) {
    AVFormatContext* format = nullptr;
    const std::string file = path.string();
    const bool pcm = path.extension() == ".wav";
    check(avformat_alloc_output_context2(&format, nullptr, pcm ? "wav" : "flac", file.c_str()), "allocate output");
    const auto* encoder = avcodec_find_encoder(pcm ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_FLAC);
    if (!encoder) throw std::runtime_error("Audio fixture encoder missing");
    auto* codec = avcodec_alloc_context3(encoder);
    if (!codec) throw std::bad_alloc();
    codec->sample_rate = rate;
    codec->sample_fmt = AV_SAMPLE_FMT_S16;
    codec->time_base = {1, rate};
    av_channel_layout_default(&codec->ch_layout, 2);
    if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(codec, encoder, nullptr), "open encoder");
    const int blockSize = codec->frame_size > 0 ? codec->frame_size : 4096;
    auto* stream = avformat_new_stream(format, nullptr);
    if (!stream) throw std::bad_alloc();
    stream->time_base = codec->time_base;
    check(avcodec_parameters_from_context(stream->codecpar, codec), "copy parameters");
    check(avio_open(&format->pb, file.c_str(), AVIO_FLAG_WRITE), "open fixture");
    check(avformat_write_header(format, nullptr), "write header");
    auto* frame = av_frame_alloc();
    auto* packet = av_packet_alloc();
    if (!frame || !packet) throw std::bad_alloc();
    frame->format = codec->sample_fmt;
    frame->sample_rate = rate;
    frame->nb_samples = blockSize;
    check(av_channel_layout_copy(&frame->ch_layout, &codec->ch_layout), "copy channels");
    check(av_frame_get_buffer(frame, 0), "allocate frame");
    auto drain = [&] {
        int result;
        while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(format, packet), "write packet");
            av_packet_unref(packet);
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "receive packet");
    };
    for (int offset = 0; offset < total; offset += blockSize) {
        check(av_frame_make_writable(frame), "writable frame");
        frame->nb_samples = std::min(blockSize, total - offset);
        frame->pts = offset;
        auto* samples = reinterpret_cast<std::int16_t*>(frame->data[0]);
        for (int index = 0; index < frame->nb_samples; ++index) {
            for (int channel = 0; channel < 2; ++channel) {
                samples[index * 2 + channel] = signal(offset + index, rate, channel);
            }
        }
        check(avcodec_send_frame(codec, frame), "send frame");
        drain();
    }
    check(avcodec_send_frame(codec, nullptr), "flush encoder");
    drain();
    check(av_write_trailer(format), "write trailer");
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avio_closep(&format->pb);
    avformat_free_context(format);
}

// A film as fansub releases mux it: Matroska whose index lists only the
// video's keyframes (FFmpeg's muxer, like mkvmerge, indexes video alone), and
// beside the video the 48 kHz audio of signal(). Noise frames make the video
// the bulk of the file, so reading it through shows in the bytes fetched.
static void makeMovie(const std::filesystem::path& path, int total) {
    constexpr int kFrameRate = 10;
    AVFormatContext* format = nullptr;
    const std::string file = path.string();
    check(avformat_alloc_output_context2(&format, nullptr, "matroska", file.c_str()), "allocate movie");
    const auto* videoEncoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const auto* audioEncoder = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!videoEncoder || !audioEncoder) throw std::runtime_error("Movie fixture encoder missing");
    auto* video = avcodec_alloc_context3(videoEncoder);
    auto* audio = avcodec_alloc_context3(audioEncoder);
    if (!video || !audio) throw std::bad_alloc();
    video->width = 160;
    video->height = 120;
    video->pix_fmt = AV_PIX_FMT_YUV420P;
    video->time_base = {1, kFrameRate};
    video->framerate = {kFrameRate, 1};
    video->gop_size = kFrameRate;  // a keyframe a second
    video->max_b_frames = 0;
    video->flags |= AV_CODEC_FLAG_QSCALE;
    video->global_quality = FF_QP2LAMBDA * 2;
    audio->sample_rate = 48000;
    audio->sample_fmt = AV_SAMPLE_FMT_S16;
    audio->time_base = {1, 48000};
    av_channel_layout_default(&audio->ch_layout, 2);
    for (auto* codec : {video, audio}) {
        if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    check(avcodec_open2(video, videoEncoder, nullptr), "open video encoder");
    check(avcodec_open2(audio, audioEncoder, nullptr), "open audio encoder");
    auto* videoStream = avformat_new_stream(format, nullptr);
    auto* audioStream = avformat_new_stream(format, nullptr);
    if (!videoStream || !audioStream) throw std::bad_alloc();
    videoStream->time_base = video->time_base;
    audioStream->time_base = audio->time_base;
    check(avcodec_parameters_from_context(videoStream->codecpar, video), "copy video parameters");
    check(avcodec_parameters_from_context(audioStream->codecpar, audio), "copy audio parameters");
    check(avio_open(&format->pb, file.c_str(), AVIO_FLAG_WRITE), "open movie");
    check(avformat_write_header(format, nullptr), "write movie header");
    auto* picture = av_frame_alloc();
    auto* sound = av_frame_alloc();
    auto* packet = av_packet_alloc();
    if (!picture || !sound || !packet) throw std::bad_alloc();
    picture->format = video->pix_fmt;
    picture->width = video->width;
    picture->height = video->height;
    check(av_frame_get_buffer(picture, 0), "allocate picture");
    const int block = audio->frame_size > 0 ? audio->frame_size : 4608;
    sound->format = audio->sample_fmt;
    sound->sample_rate = audio->sample_rate;
    sound->nb_samples = block;
    check(av_channel_layout_copy(&sound->ch_layout, &audio->ch_layout), "copy movie channels");
    check(av_frame_get_buffer(sound, 0), "allocate sound");
    auto drain = [&](AVCodecContext* codec, AVStream* stream) {
        int result;
        while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(format, packet), "write movie packet");
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "receive movie packet");
    };
    const int frames = static_cast<int>(static_cast<std::int64_t>(total) * kFrameRate / 48000);
    std::uint32_t noise = 1;
    int offset = 0;
    for (int index = 0; index <= frames; ++index) {
        // The audio up to this frame's end goes first, so the two streams
        // interleave in the file as a muxer lays them out.
        const int audioEnd = static_cast<int>(std::min<std::int64_t>(
            total, static_cast<std::int64_t>(index + 1) * 48000 / kFrameRate));
        for (; offset < audioEnd; offset += sound->nb_samples) {
            check(av_frame_make_writable(sound), "writable sound");
            sound->nb_samples = std::min(block, total - offset);
            sound->pts = offset;
            auto* samples = reinterpret_cast<std::int16_t*>(sound->data[0]);
            for (int sample = 0; sample < sound->nb_samples; ++sample) {
                for (int channel = 0; channel < 2; ++channel) {
                    samples[sample * 2 + channel] = signal(offset + sample, 48000, channel);
                }
            }
            check(avcodec_send_frame(audio, sound), "send sound");
            drain(audio, audioStream);
        }
        if (index == frames) break;
        check(av_frame_make_writable(picture), "writable picture");
        for (int plane = 0; plane < 3; ++plane) {
            const int rows = plane ? picture->height / 2 : picture->height;
            for (int row = 0; row < rows; ++row) {
                auto* line = picture->data[plane] + static_cast<std::ptrdiff_t>(row) * picture->linesize[plane];
                for (int column = 0; column < picture->linesize[plane]; ++column) {
                    noise = noise * 1664525u + 1013904223u;
                    line[column] = static_cast<std::uint8_t>(noise >> 24);
                }
            }
        }
        picture->pts = index;
        check(avcodec_send_frame(video, picture), "send picture");
        drain(video, videoStream);
    }
    check(avcodec_send_frame(audio, nullptr), "flush audio encoder");
    drain(audio, audioStream);
    check(avcodec_send_frame(video, nullptr), "flush video encoder");
    drain(video, videoStream);
    check(av_write_trailer(format), "write movie trailer");
    av_packet_free(&packet);
    av_frame_free(&sound);
    av_frame_free(&picture);
    avcodec_free_context(&audio);
    avcodec_free_context(&video);
    avio_closep(&format->pb);
    avformat_free_context(format);
}

static void waitEnded(VideoSource& source) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = source.audioDecodeStatus().state;
        if (state == AudioDecodeState::Ended) return;
        if (state == AudioDecodeState::Error) throw std::runtime_error("Audio worker failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Audio worker timeout");
}

// Returns the bytes the cache fetched, zero without it.
static std::uint64_t verifySeek(const std::filesystem::path& path, int rate, int total,
                                double target, bool backpressure = false, bool cache = false) {
    VideoSource source;
    std::vector<AudioChunk> chunks;
    int attempts = 0;
    AudioChunk retried;
    AudioSeekTestAccess::start(source, path, [&](AudioChunk&& chunk) {
        if (backpressure && attempts++ < 2) {
            if (attempts == 1) retried = chunk;
            else assert(chunk.pts == retried.pts && chunk.samples == retried.samples);
            return AudioSubmitResult::Backpressure;
        }
        if (backpressure && chunks.empty()) {
            assert(chunk.generation == retried.generation);
            assert(chunk.pts == retried.pts && chunk.samples == retried.samples);
        }
        chunks.push_back(std::move(chunk));
        return AudioSubmitResult::Accepted;
    }, target, cache);
    waitEnded(source);
    const auto fetched = AudioSeekTestAccess::fetched(source);
    source.close();
    const auto first = static_cast<std::int64_t>(std::llround(target * 48000));
    const auto end = static_cast<std::int64_t>(std::ceil(static_cast<double>(total) * 48000 / rate));
    if (first >= end) {
        assert(chunks.empty());
        return fetched;
    }
    assert(!chunks.empty());
    assert(std::abs(chunks.front().pts * 48000 - first) < 1e-6);
    std::int64_t position = first;
    std::size_t checked = 0;
    for (const auto& chunk : chunks) {
        assert(chunk.channels == 2 && chunk.sampleRate == 48000);
        assert(!chunk.samples.empty() && chunk.samples.size() % 2 == 0);
        assert(std::abs(chunk.pts * 48000 - position) < 1e-6);
        for (std::size_t offset = 0; offset < chunk.samples.size() / 2; ++offset) {
            const auto sample = position + static_cast<std::int64_t>(offset);
            for (int channel = 0; channel < 2; ++channel) {
                const float actual = chunk.samples[offset * 2 + channel];
                if (rate == 48000) {
                    assert(actual == static_cast<float>(signal(static_cast<int>(sample), rate, channel)) / 32768);
                } else if (sample > 32 && sample + 32 < end) {
                    const double seconds = static_cast<double>(sample) / 48000;
                    const double expected = (5000 * std::sin(2 * std::numbers::pi * 137 * seconds) +
                        (channel ? -4000 : 4000) * std::sin(2 * std::numbers::pi * 311 * seconds)) / 32768;
                    // Resets can change the fractional 44.1 kHz resampling
                    // phase by <= half an output sample, but never a block.
                    assert(std::abs(actual - expected) < 0.0045);
                    ++checked;
                }
            }
        }
        position += static_cast<std::int64_t>(chunk.samples.size() / 2);
    }
    assert(std::abs(position - end) <= (rate == 48000 ? 0 : 1));
    if (rate != 48000 && end - first > 64) assert(checked > 0);
    std::cout << "rate=" << rate << " target=" << target << " first=" << chunks.front().pts
              << " samples=" << position - first << " chunks=" << chunks.size() << '\n';
    return fetched;
}

static void verifySuperseded(const std::filesystem::path& path, bool cache = false) {
    VideoSource source;
    std::uint64_t replacement = 0;
    std::vector<AudioChunk> chunks;
    AudioSeekTestAccess::start(source, path, [&](AudioChunk&& chunk) {
        if (!replacement) {
            replacement = source.requestAudioSeek(1.5);
            return AudioSubmitResult::Backpressure;
        }
        assert(chunk.generation == replacement);
        chunks.push_back(std::move(chunk));
        return AudioSubmitResult::Accepted;
    }, 1.0, cache);
    waitEnded(source);
    source.close();
    assert(!chunks.empty() && chunks.front().pts == 1.5);
    assert(chunks.front().samples.front() == static_cast<float>(signal(72000, 48000, 0)) / 32768);
}

static void verifyTimeline() {
    AudioSampleTimeline timeline;
    timeline.reset(48000);
    timeline.locate(46080);
    assert(timeline.consume(0).count == 0);
    const auto within = timeline.consume(4608);
    assert(within.skip == 1920 && within.count == 2688 && within.firstSample == 48000);
    timeline.reset(4608);
    timeline.locate(0);
    assert(timeline.consume(4608).count == 0);
    const auto boundary = timeline.consume(4608);
    assert(boundary.skip == 0 && boundary.firstSample == 4608);
    timeline.reset(1000);
    timeline.locate(std::nullopt);
    assert(timeline.consume(0).count == 0);
    const auto missing = timeline.consume(10);
    assert(missing.firstSample == 1000 && missing.count == 10);
    assert(timeline.consume(10).firstSample == 1010);
    timeline.locate(1021, 48); // later known PTS anchors the missing-PTS fallback
    assert(timeline.consume(10).firstSample == 1021);
    timeline.locate(1060, 48); // millisecond PTS quantization stays continuous
    assert(timeline.consume(10).firstSample == 1031);
    timeline.locate(2000, 48); // a genuine discontinuity still has a new PTS
    assert(timeline.consume(10).firstSample == 2000);
    timeline.reset(100);
    timeline.locate(-100);
    assert(timeline.consume(150).count == 0);
    assert(timeline.consume(0).count == 0);
    const auto delayed = timeline.consume(100);
    assert(delayed.skip == 50 && delayed.count == 50 && delayed.firstSample == 100);
    timeline.reset(100);
    assert(timeline.consume(10).firstSample == 100);
    timeline.locate(0); // real PTS arriving after an estimated buffer still gates preroll
    assert(timeline.consume(100).count == 0);
    assert(timeline.consume(10).count == 0); // [100,110) was already emitted
    assert(timeline.consume(10).firstSample == 110);
    timeline.locate(105); // backward known PTS also cannot duplicate queued PCM
    const auto overlap = timeline.consume(20);
    assert(overlap.skip == 15 && overlap.count == 5 && overlap.firstSample == 120);
    timeline.locate(200);
    assert(timeline.consume(10).firstSample == 200); // preserve forward gaps
    timeline.reset(100); // a replacement generation permits a real backward seek
    timeline.locate(100);
    assert(timeline.consume(10).firstSample == 100);
    timeline.reset(std::numeric_limits<std::int64_t>::max());
    assert(timeline.consume(10).count == 0); // no representable end; never repeat saturated PTS
    timeline.reset(std::numeric_limits<std::int64_t>::max() - 5);
    assert(timeline.consume(10).count == 5);
    assert(timeline.consume(10).count == 0);
}

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    try {
        verifyTimeline();
        const auto directory = std::filesystem::temp_directory_path() /
            ("QuadDeck-audio-seek-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const auto native = directory / "sample-index-48000-stereo.flac";
        const auto resampled = directory / "analytic-44100-stereo.flac";
        const auto shortClip = directory / "short-44100-stereo.wav";
        makeAudio(native, 48000, 96101);
        makeAudio(resampled, 44100, 88301);
        makeAudio(shortClip, 44100, 441);
        for (const double target : {0.0, 1.0 / 48000, 0.096, 1.0, 1.03, 1.04, 1.05, 1.06, 1.08, 1.09, 2.0, 96101.0 / 48000, 2.002125}) {
            verifySeek(native, 48000, 96101, target, target == 1.0);
        }
        for (const double target : {0.0, 4608.0 / 44100, 1.0, 1.04, 1.05, 1.08, 2.0}) {
            verifySeek(resampled, 44100, 88301, target);
        }
        verifySeek(shortClip, 44100, 441, 0.0);
        verifySeek(shortClip, 44100, 441, 470.0 / 48000); // all retained PCM comes from swr EOF flush
        verifySuperseded(native);
        verifySuperseded(native, true);
        verifySeek(native, 48000, 96101, 1.0, true, true);
        verifySeek(resampled, 44100, 88301, 1.08, false, true);
        // A minute of film: the audio lands on its sample however the seek
        // reaches it, and near the end it reads the end, not the whole file.
        const auto movie = directory / "video-indexed-movie.mkv";
        constexpr int kMovieSamples = 2880101;
        makeMovie(movie, kMovieSamples);
        for (const double target : {0.0, 0.5, 1.0, 30.0, 30.05, 57.3}) {
            verifySeek(movie, 48000, kMovieSamples, target);
        }
        const auto movieBytes = std::filesystem::file_size(movie);
        const auto fetched = verifySeek(movie, 48000, kMovieSamples, 57.3, false, true);
        std::cout << "movie bytes=" << movieBytes << " fetched for 57.3 s=" << fetched << '\n';
        assert(fetched > 0 && fetched < movieBytes / 2);
        for (const auto& path : {native, resampled, shortClip, movie}) std::filesystem::remove(path);
        std::filesystem::remove(directory);
        std::cout << "QuadDeck audio seek tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
