#include "VideoSource.hpp"
#include "AudioSampleTimeline.hpp"
#include "ColorConversion.hpp"
#include "Diagnostics.hpp"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace quaddeck {
namespace {
// A queued hardware frame is not a copy: it holds an AVFrame clone that pins
// one surface of the D3D11VA decoder's fixed pool, and that pool must also
// hold the codec's reference frames. Queueing deeply therefore starves the
// very decoder that fills the queue, and with five sources sharing one device
// the effect multiplies. The queue only needs to be a jitter buffer -- four
// frames is already ~66ms at 60Hz.
//
// Software frames are ordinary CPU buffers with no such coupling, so they can
// be queued generously to absorb decode jitter.
constexpr std::size_t kMaxHardwareFrames = 4;
constexpr std::size_t kMaxSoftwareFrames = 16;
// Beyond the queue, currentFrame_ holds the frame on screen, and one slice of
// slack keeps a decoder from blocking on the exact boundary.
constexpr int kHeldHardwareFrames = 2;
constexpr double kSeekQualityPrerollSeconds = 0.75;
constexpr double kMaximumSeekPreviewLeadSeconds = 15.0;
std::atomic<std::uint64_t> gFrameSerial{0};
// XAudio2 SourceVoices outlive individual VideoSource objects. Audio
// generations therefore have to outlive them too: restarting at one after a
// replacement would collide with a generation that the persistent voice has
// already rejected.
std::atomic<std::uint64_t> gAudioGeneration{0};

std::uint64_t nextAudioGeneration() noexcept {
    return gAudioGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Matroska's index (Cues) normally lists the video's keyframes alone, and
// FFmpeg seeks a stream the index leaves out by reading the file from its
// start up to the target: over a minute for a film on a share, with the
// video's reads queued behind it. The audio worker seeks such a file by the
// video's entries instead, which land in the cluster of a keyframe before
// the target. The demuxer drops the audio block that began before that
// keyframe, so the seek aims this far early, past any audio block's length;
// the sample timeline discards what precedes the target.
constexpr double kAudioSeekByVideoLeadSeconds = 1.0;

struct AudioSeekPoint {
    int stream{};
    std::int64_t timestamp{};
};

// Where the audio worker's demuxer seek for `seconds` (from the audio's
// start) goes: `preroll` before it by the audio stream, or by a Matroska
// file's video stream, at least the lead before it.
AudioSeekPoint audioSeekPoint(AVFormatContext* format, int audioStream, std::int64_t audioStart,
                              double seconds, double preroll) {
    const AVStream* audio = format->streams[audioStream];
    const auto at = [&](double lead) {
        return streamTimestampForSeconds(std::max(0.0, seconds - lead), audioStart,
                                         audio->time_base.num, audio->time_base.den);
    };
    const AudioSeekPoint own{audioStream, at(preroll)};
    if (!std::strstr(format->iformat->name, "matroska")) return own;
    const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0 || (format->streams[video]->disposition & AV_DISPOSITION_ATTACHED_PIC)) return own;
    const AVStream* picture = format->streams[video];
    const std::int64_t target = at(std::max(preroll, kAudioSeekByVideoLeadSeconds));
    // Before the video's first frame there is no keyframe to land on; the
    // file's start is near enough to read through.
    const std::int64_t first = picture->start_time == AV_NOPTS_VALUE ? 0 : picture->start_time;
    if (av_compare_ts(target, audio->time_base, first, picture->time_base) < 0) return own;
    return {video, av_rescale_q(target, audio->time_base, picture->time_base)};
}

void lockD3D(void* opaque) { static_cast<std::recursive_mutex*>(opaque)->lock(); }
void unlockD3D(void* opaque) { static_cast<std::recursive_mutex*>(opaque)->unlock(); }

AVPixelFormat chooseD3D11Format(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* item = formats; *item != AV_PIX_FMT_NONE; ++item) {
        if (*item == AV_PIX_FMT_D3D11) return *item;
    }
    return AV_PIX_FMT_NONE;
}

AVPixelFormat chooseD3D11OrSoftware(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* item = formats; *item != AV_PIX_FMT_NONE; ++item) {
        if (*item == AV_PIX_FMT_D3D11) return *item;
    }
    return formats[0];
}

std::int64_t effectiveVideoFrameDuration(
    AVFormatContext* format, AVStream* stream, AVFrame* frame) {
    if (frame->duration > 0) return frame->duration;
    if (!format || !stream || stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return 0;
    }
    const AVRational rate = av_guess_frame_rate(format, stream, frame);
    if (rate.num <= 0 || rate.den <= 0) return 0;
    const AVRational oneFrame{rate.den, rate.num};
    // A fractional number of stream ticks must cover the whole nominal frame.
    // Rounding 33.333 ticks down to 33 would make a 30fps frame appear to end
    // before it really does and can select the following frame at a seek.
    return std::max<std::int64_t>(0, av_rescale_q_rnd(
        1, oneFrame, stream->time_base, AV_ROUND_UP));
}
}  // namespace

HardwareVideoFrame::HardwareVideoFrame() = default;
HardwareVideoFrame::~HardwareVideoFrame() { av_frame_free(&ownerFrame); }
std::uint64_t AudioChunk::generationWatermark() noexcept {
    return gAudioGeneration.load(std::memory_order_acquire);
}
VideoSource::VideoSource() = default;
VideoSource::~VideoSource() { close(); }

bool VideoSource::open(const std::wstring& path, ID3D11Device* device,
                       ID3D11DeviceContext* deviceContext,
                       std::recursive_mutex* deviceMutex, DecodeMode decodeMode,
                       AudioSink audioSink, CacheMode cacheMode, SourceOptions options) {
    close();
    options_ = std::move(options);
    path_ = path;
    device_ = device;
    deviceContext_ = deviceContext;
    deviceMutex_ = deviceMutex;
    decodeMode_ = decodeMode;
    audioSink_ = std::move(audioSink);
    videoFramesDecoded_.store(0);
    actualVideoFrameLogged_ = false;
    audioChunksDecoded_.store(0);
    duration_.store(0.0);
    videoWidth_.store(0);
    videoHeight_.store(0);
    audioStreamAvailability_.store(0);
    exactSeekReadyGeneration_.store(0);
    eof_.store(false);
    ready_.store(false);
    {
        std::scoped_lock lock(mutex_);
        error_.clear();
        audioTracks_.clear();
        subtitleTracks_.clear();
        fonts_.reset();
        videoSeekPending_ = false;
        audioSeekPending_ = false;
        videoSeekPreviewRequested_ = false;
        videoSeekPreviewPending_ = false;
        videoSeekGeneration_ = 0;
        pendingVideoSeekGeneration_ = 0;
        decodingVideoSeekGeneration_ = 0;
        videoSeekTargetPts_ = 0;
        videoSeekTargetPtsValid_ = false;
        videoSeekFallbackGeneration_ = 0;
        audioSeekGeneration_ = nextAudioGeneration();
        pendingAudioSeekGeneration_ = audioSeekGeneration_;
        audioDecodeState_ = AudioDecodeState::Unknown;
    }
    {
        std::scoped_lock lock(subtitleMutex_);
        subtitleCues_.clear();
        subtitleEvents_.clear();
    }
    colourMatrix_.store(static_cast<int>(SubtitleMatrix::None));
    videoSkipToKeyframe_ = false;
    appendDiagnostic("Opening v1.0.0: " + wideToUtf8(path));
    if (!device_ || !deviceContext_ || !deviceMutex_) {
        setError("Shared D3D11 device is unavailable");
        return false;
    }
    running_.store(true);
    cache_ = createFileCache(path_, cacheMode);
    videoWorker_ = std::thread(&VideoSource::videoWorkerMain, this);
    audioWorker_ = std::thread(&VideoSource::audioWorkerMain, this);
    return true;
}

void VideoSource::close() {
    running_.store(false);
    audioEnabled_.store(false);
    cv_.notify_all();
    if (cache_) cache_->stop();
    if (videoWorker_.joinable()) videoWorker_.join();
    if (audioWorker_.joinable()) audioWorker_.join();
    closeVideoInput();
    audioIo_.reset();
    cache_.reset();
    std::scoped_lock lock(mutex_);
    videoQueue_.clear();
    queuedVideoFrames_.store(0);
    currentFrame_.reset();
    ready_.store(false);
    eof_.store(false);
    videoWidth_.store(0);
    videoHeight_.store(0);
}

std::uint64_t VideoSource::requestSeek(double seconds, bool showKeyframePreview) {
    return requestSeekInternal(seconds, showKeyframePreview, false, 0);
}

std::uint64_t VideoSource::requestKeyframeSeek(double seconds, int direction) {
    return requestSeekInternal(seconds, false, true, direction);
}

std::optional<VideoSource::SeekLanding> VideoSource::takeSeekLanding() {
    std::scoped_lock lock(mutex_);
    auto landing = seekLanding_;
    seekLanding_.reset();
    return landing;
}

std::uint64_t VideoSource::requestSeekInternal(double seconds, bool showKeyframePreview, bool keyframe,
                                               int direction) {
    std::uint64_t generation{};
    {
        std::scoped_lock lock(mutex_);
        generation = ++videoSeekGeneration_;
        pendingVideoSeekGeneration_ = generation;
        audioSeekGeneration_ = nextAudioGeneration();
        pendingAudioSeekGeneration_ = audioSeekGeneration_;
        audioDecodeState_ = audioStreamAvailability_.load() < 0
            ? AudioDecodeState::Error : AudioDecodeState::Unknown;
        videoSeekTarget_ = audioSeekTarget_ = std::max(0.0, seconds);
        videoSeekPending_ = audioSeekPending_ = true;
        videoSeekPreviewRequested_ = showKeyframePreview;
        videoSeekKeyframeRequested_ = keyframe;
        videoSeekDirectionRequested_ = direction;
        seekLanding_.reset();
        videoQueue_.clear();
        queuedVideoFrames_.store(0);
        currentFrame_.reset();
        pausedFrameLocked_ = false;
        // Cancel old I/O before the worker can consume the new seek. Otherwise
        // a late interrupt could cancel the replacement generation itself.
        if (cache_) {
            if (ready_) cache_->interrupt(0);
            if (hasAudioStream()) cache_->interrupt(1);
        }
    }
    eof_.store(false);
    cv_.notify_all();
    return generation;
}

std::uint64_t VideoSource::requestAudioSeek(double seconds) {
    std::uint64_t generation{};
    {
        std::scoped_lock lock(mutex_);
        generation = nextAudioGeneration();
        audioSeekGeneration_ = generation;
        pendingAudioSeekGeneration_ = generation;
        audioDecodeState_ = audioStreamAvailability_.load() < 0
            ? AudioDecodeState::Error : AudioDecodeState::Unknown;
        audioSeekTarget_ = std::max(0.0, seconds);
        audioSeekPending_ = true;
        if (cache_ && hasAudioStream()) cache_->interrupt(1);
    }
    cv_.notify_all();
    return generation;
}

AudioDecodeStatus VideoSource::audioDecodeStatus() const {
    std::scoped_lock lock(mutex_);
    return {audioSeekGeneration_, audioDecodeState_};
}

void VideoSource::setAudioDecodeState(std::uint64_t generation,
                                      AudioDecodeState state) {
    std::scoped_lock lock(mutex_);
    if (audioSeekGeneration_ == generation) audioDecodeState_ = state;
}

bool VideoSource::audioGenerationIsCurrent(std::uint64_t generation) const {
    std::scoped_lock lock(mutex_);
    return !audioSeekPending_ && audioSeekGeneration_ == generation;
}

int VideoSource::interruptCallback(void* opaque) {
    const auto* source = static_cast<const VideoSource*>(opaque);
    return !source || !source->running_.load(std::memory_order_acquire) ? 1 : 0;
}

std::shared_ptr<const HardwareVideoFrame> VideoSource::frameForTime(double seconds, bool advance) {
    std::scoped_lock lock(mutex_);
    if (!advance) {
        // After opening or seeking while paused, advance only until the requested
        // frame is reached and then lock it. A non-increasing timestamp is treated
        // as malformed and locks immediately, preventing post-Pause queue drain.
        while (!pausedFrameLocked_ && !videoQueue_.empty()) {
            const auto& candidate = videoQueue_.front();
            const auto step = pausedFrameStep(
                candidate->pts, candidate->exactSeekFrame, currentFrame_ != nullptr,
                currentFrame_ ? currentFrame_->pts : 0.0, seconds);
            if (!step.accept) {
                pausedFrameLocked_ = true;
                break;
            }
            currentFrame_ = candidate;
            videoQueue_.pop_front();
            queuedVideoFrames_.store(static_cast<unsigned>(videoQueue_.size()));
            pausedFrameLocked_ = step.lock;
        }
        cv_.notify_all();
        return currentFrame_;
    }
    pausedFrameLocked_ = false;
    while (!videoQueue_.empty()) {
        const auto& candidate = videoQueue_.front();
        if (candidate->pts > seconds + 0.003 && currentFrame_) break;
        if (candidate->pts > seconds + 0.050 && !currentFrame_) break;
        currentFrame_ = candidate;
        videoQueue_.pop_front();
        queuedVideoFrames_.store(static_cast<unsigned>(videoQueue_.size()));
    }
    cv_.notify_all();
    return currentFrame_;
}

void VideoSource::setAudioEnabled(bool enabled) {
    audioEnabled_.store(enabled);
    cv_.notify_all();
}

void VideoSource::setPreferredSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    preferredWidth_.store(width, std::memory_order_relaxed);
    preferredHeight_.store(height, std::memory_order_relaxed);
}

std::string VideoSource::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

void VideoSource::setError(std::string message) {
    appendDiagnostic("ERROR: " + message);
    std::scoped_lock lock(mutex_);
    error_ = std::move(message);
}

namespace {

// FFmpeg's HTTP protocol takes its request headers as an option; a local
// file ignores the dictionary. Reconnects cover a server that drops a
// connection left idle while the deck is paused.
int openInputWithOptions(AVFormatContext** context, const std::string& path,
                         const SourceOptions& options) {
    AVDictionary* dictionary = nullptr;
    if (!options.httpHeaders.empty()) {
        av_dict_set(&dictionary, "headers", options.httpHeaders.c_str(), 0);
        av_dict_set(&dictionary, "user_agent", "QuadDeck/1.0.0", 0);
        av_dict_set(&dictionary, "reconnect", "1", 0);
        av_dict_set(&dictionary, "reconnect_on_network_error", "1", 0);
        av_dict_set(&dictionary, "reconnect_delay_max", "4", 0);
    }
    const int result = avformat_open_input(context, path.c_str(), nullptr,
                                           dictionary ? &dictionary : nullptr);
    av_dict_free(&dictionary);
    return result;
}

}  // namespace

std::vector<AudioTrackInfo> VideoSource::audioTracks() const {
    std::scoped_lock lock(mutex_);
    return audioTracks_;
}

std::vector<SubtitleTrackInfo> VideoSource::subtitleTracks() const {
    std::scoped_lock lock(mutex_);
    return subtitleTracks_;
}

void VideoSource::setSubtitleTrack(int streamIndex) {
    std::scoped_lock lock(mutex_);
    subtitleTrackSet_ = true;
    shownSubtitleStream_.store(streamIndex);
}

SubtitleLines VideoSource::subtitleLinesAt(double seconds) const {
    const int shown = shownSubtitleStream_.load();
    std::scoped_lock lock(subtitleMutex_);
    for (const auto& [stream, track] : subtitleCues_) {
        if (stream == shown) return quaddeck::subtitleLinesAt(track, seconds);
    }
    return {};
}

std::size_t VideoSource::subtitleCueCount() const {
    const int shown = shownSubtitleStream_.load();
    std::scoped_lock lock(subtitleMutex_);
    for (const auto& [stream, track] : subtitleCues_) {
        if (stream == shown) return track.cues.size();
    }
    return 0;
}

bool VideoSource::subtitleStreamHeader(int streamIndex, std::string& header, bool& plain) const {
    std::scoped_lock lock(subtitleMutex_);
    for (const auto& stream : subtitleEvents_) {
        if (stream.streamIndex != streamIndex) continue;
        header = stream.header;
        plain = stream.plain;
        return true;
    }
    return false;
}

std::size_t VideoSource::subtitleEventsSince(int streamIndex, std::size_t from,
                                              std::vector<SubtitleEvent>& events) const {
    std::scoped_lock lock(subtitleMutex_);
    for (const auto& stream : subtitleEvents_) {
        if (stream.streamIndex != streamIndex) continue;
        for (std::size_t index = from; index < stream.events.size(); ++index) events.push_back(stream.events[index]);
        return stream.events.size();
    }
    return 0;
}

std::shared_ptr<const std::vector<SubtitleFont>> VideoSource::fonts() const {
    std::scoped_lock lock(mutex_);
    return fonts_;
}

namespace {

// A track of several hours of karaoke effects stays far below this; past it
// the file is not a subtitle any more.
constexpr std::size_t kMaximumSubtitleCues = 200000;
// More text streams than this are not all read; the rest are listed only.
constexpr std::size_t kMaximumSubtitleReaders = 32;
// How far before the keyframe a seek lands on the demuxer starts reading,
// for the lines that were already on screen there. Most speech is shorter.
// What is read on the way is kept to about 24 MiB at the file's average
// bitrate, so that a seek in a high-bitrate file on a share does not wait on
// it: fewer seconds there, never under two. The seek goes back to a
// keyframe, so the distance to that keyframe is read as well.
constexpr double kSubtitlePrerollSeconds = 8.0;
constexpr double kSubtitlePrerollMinimumSeconds = 2.0;
constexpr double kSubtitlePrerollBits = 24.0 * 1024.0 * 1024.0 * 8.0;

bool textSubtitleCodec(AVCodecID id) {
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(id);
    return descriptor && (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0 &&
           avcodec_find_decoder(id) != nullptr;
}

// A fansub release brings its faces along; a few dozen megabytes is usual,
// and what is past this is not read.
constexpr std::size_t kMaximumFontBytes = 256u * 1024u * 1024u;

// An attachment that is a font: by the codec the demuxer gave it, its MIME
// type, or the end of its name (mkvmerge has written several types).
bool fontAttachment(const AVStream* stream) {
    const AVCodecID codec = stream->codecpar->codec_id;
    if (codec == AV_CODEC_ID_TTF || codec == AV_CODEC_ID_OTF) return true;
    if (const AVDictionaryEntry* type = av_dict_get(stream->metadata, "mimetype", nullptr, 0)) {
        std::string value = type->value;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* known : {"application/x-truetype-font", "application/vnd.ms-opentype",
                                  "application/x-font-ttf", "application/x-font-otf", "application/x-font",
                                  "application/font-sfnt", "font/collection", "font/otf", "font/sfnt", "font/ttf"}) {
            if (value == known) return true;
        }
    }
    if (const AVDictionaryEntry* name = av_dict_get(stream->metadata, "filename", nullptr, 0)) {
        std::string value = name->value;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* extension : {".ttf", ".otf", ".ttc", ".otc"}) {
            const std::size_t length = std::strlen(extension);
            if (value.size() > length && value.compare(value.size() - length, length, extension) == 0) return true;
        }
    }
    return false;
}

// The matrix a video's colours are decoded with, for the colour correction
// of a subtitle script. A stream that does not say is taken as VSFilter's
// successors take it, by its size. RGB, and the wide gamut of BT.2020 that
// no script was coloured against, are left alone.
SubtitleMatrix videoColourMatrix(const AVCodecParameters* parameters) {
    const bool full = parameters->color_range == AVCOL_RANGE_JPEG;
    switch (parameters->color_space) {
    case AVCOL_SPC_RGB:
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
    case AVCOL_SPC_ICTCP:
        return SubtitleMatrix::None;
    case AVCOL_SPC_BT709: return full ? SubtitleMatrix::Bt709Pc : SubtitleMatrix::Bt709Tv;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return full ? SubtitleMatrix::Bt601Pc : SubtitleMatrix::Bt601Tv;
    case AVCOL_SPC_SMPTE240M: return full ? SubtitleMatrix::Smpte240mPc : SubtitleMatrix::Smpte240mTv;
    case AVCOL_SPC_FCC: return full ? SubtitleMatrix::FccPc : SubtitleMatrix::FccTv;
    default: break;
    }
    const bool high = parameters->width >= 1280 || parameters->height > 576;
    if (high) return full ? SubtitleMatrix::Bt709Pc : SubtitleMatrix::Bt709Tv;
    return full ? SubtitleMatrix::Bt601Pc : SubtitleMatrix::Bt601Tv;
}

}  // namespace

void VideoSource::closeSubtitleDecoders() {
    for (auto& reader : subtitleReaders_) avcodec_free_context(&reader.codec);
    subtitleReaders_.clear();
}

void VideoSource::openSubtitleDecoders() {
    closeSubtitleDecoders();
    if (!options_.readSubtitles || !videoFormat_) return;
    std::vector<std::pair<int, SubtitleTrack>> cues;
    std::vector<SubtitleStreamEvents> events;
    for (unsigned index = 0; index < videoFormat_->nb_streams; ++index) {
        AVStream* stream = videoFormat_->streams[index];
        if (!stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE ||
            !textSubtitleCodec(stream->codecpar->codec_id)) {
            continue;
        }
        if (subtitleReaders_.size() >= kMaximumSubtitleReaders) break;
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        SubtitleReader reader;
        reader.streamIndex = static_cast<int>(index);
        reader.codec = avcodec_alloc_context3(decoder);
        if (!reader.codec || avcodec_parameters_to_context(reader.codec, stream->codecpar) < 0) {
            avcodec_free_context(&reader.codec);
            continue;
        }
        reader.codec->pkt_timebase = stream->time_base;
        const int result = avcodec_open2(reader.codec, decoder, nullptr);
        if (result < 0) {
            appendDiagnostic("Subtitle decoder unavailable for stream " + std::to_string(index) + ": " +
                             ffmpegError(result));
            avcodec_free_context(&reader.codec);
            continue;
        }
        // Every text decoder hands its lines over as ASS events, under a
        // header that names the styles they use: the stream's own for ASS
        // and SSA, a stand-in of FFmpeg's for the rest.
        SubtitleStreamEvents streamText;
        streamText.streamIndex = reader.streamIndex;
        if (reader.codec->subtitle_header && reader.codec->subtitle_header_size > 0) {
            streamText.header.assign(reinterpret_cast<const char*>(reader.codec->subtitle_header),
                                        static_cast<std::size_t>(reader.codec->subtitle_header_size));
            reader.header = parseAssHeader(streamText.header);
        }
        const AVCodecID id = stream->codecpar->codec_id;
        reader.plain = id != AV_CODEC_ID_ASS && id != AV_CODEC_ID_SSA;
        streamText.plain = reader.plain;
        stream->discard = AVDISCARD_DEFAULT;
        cues.emplace_back(reader.streamIndex, SubtitleTrack{});
        events.push_back(std::move(streamText));
        subtitleReaders_.push_back(std::move(reader));
    }
    if (!subtitleReaders_.empty()) {
        appendDiagnostic(std::to_string(subtitleReaders_.size()) +
                         " text subtitle stream(s) are read with the video");
    }
    std::scoped_lock lock(subtitleMutex_);
    subtitleCues_ = std::move(cues);
    subtitleEvents_ = std::move(events);
}

void VideoSource::decodeSubtitlePacket(const AVPacket* packet) {
    const auto reader = std::find_if(subtitleReaders_.begin(), subtitleReaders_.end(),
        [&](const SubtitleReader& candidate) { return candidate.streamIndex == packet->stream_index; });
    if (reader == subtitleReaders_.end()) return;
    AVSubtitle subtitle{};
    int got = 0;
    if (avcodec_decode_subtitle2(reader->codec, &subtitle, &got, packet) < 0 || !got) return;
    const std::int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (pts != AV_NOPTS_VALUE) {
        const AVStream* stream = videoFormat_->streams[packet->stream_index];
        // Source time is the video's: counted from where its stream starts.
        const double origin = static_cast<double>(videoStartPts_) *
                              av_q2d(videoFormat_->streams[videoStream_]->time_base);
        const double base = static_cast<double>(pts) * av_q2d(stream->time_base) - origin;
        double start = base + static_cast<double>(subtitle.start_display_time) / 1000.0;
        double end = start;
        if (subtitle.end_display_time != 0 && subtitle.end_display_time != UINT32_MAX) {
            end = base + static_cast<double>(subtitle.end_display_time) / 1000.0;
        } else if (packet->duration > 0) {
            end = start + static_cast<double>(packet->duration) * av_q2d(stream->time_base);
        }
        // A line a moment before the video's first frame still belongs to it.
        if (start < 0.0 && end > 0.0) start = 0.0;
        std::vector<SubtitleCue> cues;
        std::vector<SubtitleEvent> events;
        for (unsigned index = 0; index < subtitle.num_rects; ++index) {
            const AVSubtitleRect* rect = subtitle.rects[index];
            if (!rect) continue;
            if (rect->type == SUBTITLE_ASS && rect->ass) {
                if (auto cue = cueFromDecodedAss(reader->header, rect->ass, start, end)) {
                    cues.push_back(std::move(*cue));
                }
                // For libass, the event as it came -- a drawing included --
                // once. A seek back passes the same packet again. An ASS
                // stream's event is told by its read order too: a script may
                // write the same line twice on purpose, to draw it stronger.
                const std::string_view chunk(rect->ass);
                const auto comma = chunk.find(',');
                if (comma == std::string_view::npos || start < 0.0) continue;
                SubtitleEvent event;
                event.startMs = std::llround(start * 1000.0);
                event.durationMs = end > start ? std::llround((end - start) * 1000.0) : 2000;
                std::string key = std::to_string(event.startMs) + ':' + std::to_string(event.durationMs) + ':';
                key.append(reader->plain ? chunk.substr(comma) : chunk);
                if (!reader->seen.insert(std::move(key)).second) continue;
                // The decoder of a plain stream numbers its events from 0
                // again after every seek; libass takes a number it has seen
                // for an event it already has.
                event.chunk = reader->plain ? std::to_string(reader->readOrder++) + std::string(chunk.substr(comma))
                                            : std::string(chunk);
                events.push_back(std::move(event));
            } else if (rect->type == SUBTITLE_TEXT && rect->text) {
                subtitle_detail::addCue(cues, start, end,
                                        subtitle_detail::parseMarkup(utf8ToWideText(rect->text), false).text);
            }
        }
        if (!cues.empty() || !events.empty()) {
            std::scoped_lock lock(subtitleMutex_);
            for (auto& [streamIndex, track] : subtitleCues_) {
                if (streamIndex != packet->stream_index) continue;
                for (auto& cue : cues) {
                    if (track.cues.size() >= kMaximumSubtitleCues) break;
                    insertSubtitleCue(track, std::move(cue));
                }
            }
            for (auto& text : subtitleEvents_) {
                if (text.streamIndex != packet->stream_index) continue;
                for (auto& event : events) {
                    if (text.events.size() >= kMaximumSubtitleCues) break;
                    text.events.push_back(std::move(event));
                }
            }
        }
    }
    avsubtitle_free(&subtitle);
}

bool VideoSource::openVideoInput() {
    const std::string path = wideToUtf8(path_);
    videoFormat_ = avformat_alloc_context();
    if (!videoFormat_) {
        setError("Cannot allocate video format context");
        return false;
    }
    videoFormat_->interrupt_callback.callback = &VideoSource::interruptCallback;
    videoFormat_->interrupt_callback.opaque = this;
    videoIo_ = std::make_unique<CachedAvio>(cache_, 0);
    const int attached = videoIo_->attach(videoFormat_);
    if (attached < 0) {
        if (running_) setError("Cannot open NAS cache input: " + ffmpegError(attached));
        return false;
    }
    int result = openInputWithOptions(&videoFormat_, path, options_);
    if (result < 0) {
        if (running_.load()) setError("Cannot open video input: " + ffmpegError(result));
        return false;
    }
    result = avformat_find_stream_info(videoFormat_, nullptr);
    if (result < 0) {
        if (running_.load()) setError("Cannot read stream info: " + ffmpegError(result));
        return false;
    }
    videoStream_ = av_find_best_stream(videoFormat_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream_ < 0) { setError("No video stream found"); return false; }
    AVStream* stream = videoFormat_->streams[videoStream_];
    {
        // What the audio-track menu lists. The audio worker opens its own
        // input; the container's stream indices are the same on both.
        std::vector<AudioTrackInfo> tracks;
        for (unsigned index = 0; index < videoFormat_->nb_streams; ++index) {
            const AVStream* candidate = videoFormat_->streams[index];
            if (!candidate->codecpar || candidate->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            AudioTrackInfo info;
            info.streamIndex = static_cast<int>(index);
            if (const AVDictionaryEntry* language = av_dict_get(candidate->metadata, "language", nullptr, 0)) {
                info.language = language->value;
            }
            if (const AVDictionaryEntry* title = av_dict_get(candidate->metadata, "title", nullptr, 0)) {
                info.title = title->value;
            }
            if (const char* codecName = avcodec_get_name(candidate->codecpar->codec_id)) info.codec = codecName;
            info.channels = candidate->codecpar->ch_layout.nb_channels;
            tracks.push_back(std::move(info));
        }
        std::scoped_lock lock(mutex_);
        audioTracks_ = std::move(tracks);
    }
    {
        // What the subtitle menu lists, and the stream shown from the
        // start when the owner left the choice to its options.
        std::vector<SubtitleTrackInfo> tracks;
        std::size_t textStreams = 0;
        for (unsigned index = 0; index < videoFormat_->nb_streams; ++index) {
            const AVStream* candidate = videoFormat_->streams[index];
            if (!candidate->codecpar || candidate->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
            SubtitleTrackInfo info;
            info.streamIndex = static_cast<int>(index);
            if (const AVDictionaryEntry* language = av_dict_get(candidate->metadata, "language", nullptr, 0)) {
                info.language = language->value;
            }
            if (const AVDictionaryEntry* title = av_dict_get(candidate->metadata, "title", nullptr, 0)) {
                info.title = title->value;
            }
            if (const char* codecName = avcodec_get_name(candidate->codecpar->codec_id)) info.codec = codecName;
            // Only as many text streams are read as there are readers for;
            // one past them is listed like a stream that cannot be shown.
            info.text = textSubtitleCodec(candidate->codecpar->codec_id) &&
                        textStreams++ < kMaximumSubtitleReaders;
            info.isDefault = (candidate->disposition & AV_DISPOSITION_DEFAULT) != 0;
            info.forced = (candidate->disposition & AV_DISPOSITION_FORCED) != 0;
            tracks.push_back(std::move(info));
        }
        const int chosen = options_.chooseSubtitle && !tracks.empty() ? options_.chooseSubtitle(tracks) : -1;
        std::scoped_lock lock(mutex_);
        subtitleTracks_ = std::move(tracks);
        if (!subtitleTrackSet_ && chosen >= 0) shownSubtitleStream_.store(chosen);
    }
    {
        // The faces a fansub script names, which a release brings along as
        // attachments. Matroska reads them with the header; nothing more is
        // read for them. Kept for a server's stream too: the files beside
        // the video come from the server, their fonts only from here.
        auto fonts = std::make_shared<std::vector<SubtitleFont>>();
        std::size_t total = 0;
        for (unsigned index = 0; index < videoFormat_->nb_streams; ++index) {
            const AVStream* candidate = videoFormat_->streams[index];
            if (!candidate->codecpar || candidate->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT ||
                !candidate->codecpar->extradata || candidate->codecpar->extradata_size <= 0 ||
                !fontAttachment(candidate)) {
                continue;
            }
            const auto size = static_cast<std::size_t>(candidate->codecpar->extradata_size);
            if (total + size > kMaximumFontBytes) break;
            total += size;
            SubtitleFont font;
            if (const AVDictionaryEntry* name = av_dict_get(candidate->metadata, "filename", nullptr, 0)) {
                font.name = name->value;
            }
            font.data.assign(reinterpret_cast<const char*>(candidate->codecpar->extradata), size);
            fonts->push_back(std::move(font));
        }
        if (!fonts->empty()) {
            appendDiagnostic(std::to_string(fonts->size()) + " font(s) attached to the video, " +
                             std::to_string(total / 1024) + " KiB");
        }
        colourMatrix_.store(static_cast<int>(videoColourMatrix(stream->codecpar)));
        std::scoped_lock lock(mutex_);
        fonts_ = std::move(fonts);
    }
    for (unsigned index = 0; index < videoFormat_->nb_streams; ++index) {
        videoFormat_->streams[index]->discard =
            static_cast<int>(index) == videoStream_ ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    openSubtitleDecoders();
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { setError("No FFmpeg decoder for this codec"); return false; }

    bool codecOffersD3D11 = false;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
        if (!config) break;
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            codecOffersD3D11 = true;
            break;
        }
    }
    usingHardware_ = decodeMode_ != DecodeMode::Software && codecOffersD3D11;
    if (decodeMode_ == DecodeMode::Hardware && !codecOffersD3D11) {
        setError("This codec does not offer a D3D11VA decoder");
        return false;
    }
    if (decodeMode_ != DecodeMode::Software && !codecOffersD3D11) {
        // Automatic silently used software here, which looks identical from
        // the outside to hardware decoding that is merely slow.
        appendDiagnostic(std::string("No D3D11VA configuration for ") + codec->name +
                         "; Auto is using software decode");
    }
    if (usingHardware_) {
        hardwareDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hardwareDevice_) { setError("Cannot allocate D3D11VA device context"); return false; }
        auto* base = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice_->data);
        auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(base->hwctx);
        d3d->device = device_;
        d3d->device->AddRef();
        d3d->device_context = deviceContext_;
        d3d->device_context->AddRef();
        d3d->lock = lockD3D;
        d3d->unlock = unlockD3D;
        d3d->lock_ctx = deviceMutex_;
        d3d->BindFlags = D3D11_BIND_DECODER;
        result = av_hwdevice_ctx_init(hardwareDevice_);
        if (result < 0) {
            if (decodeMode_ == DecodeMode::Hardware) {
                setError("Cannot initialize shared D3D11VA device: " + ffmpegError(result));
                return false;
            }
            appendDiagnostic("D3D11VA initialization failed; Auto is using software decode");
            av_buffer_unref(&hardwareDevice_);
            usingHardware_ = false;
        }
    }

    videoCodec_ = avcodec_alloc_context3(codec);
    if (!videoCodec_ || avcodec_parameters_to_context(videoCodec_, stream->codecpar) < 0) {
        setError("Cannot allocate video codec context"); return false;
    }
    if (usingHardware_) {
        videoCodec_->hw_device_ctx = av_buffer_ref(hardwareDevice_);
        videoCodec_->get_format = decodeMode_ == DecodeMode::Hardware
            ? chooseD3D11Format : chooseD3D11OrSoftware;
    }
    videoCodec_->thread_count = decodeThreadBudget(
        std::thread::hardware_concurrency(), decodeMode_ == DecodeMode::Hardware);
    if (usingHardware_) {
        // A D3D11VA pool is one texture array of a fixed size, decided when the
        // decoder initializes it and sized for the codec's own reference
        // frames. Every frame this class queues pins one of those slices, so
        // without telling the decoder how many the application intends to hold,
        // a queue deep enough to be useful starves the decoder that fills it.
        // Declaring them here has the pool allocated large enough instead, which
        // is what lets the queue stay a real jitter buffer rather than being
        // shrunk until it stops competing with the decoder.
        videoCodec_->extra_hw_frames =
            static_cast<int>(kMaxHardwareFrames) + kHeldHardwareFrames;
    }
    result = avcodec_open2(videoCodec_, codec, nullptr);
    if (result < 0) {
        setError(std::string(usingHardware_ ? "D3D11VA" : "Software") +
                 " decoder unavailable: " + ffmpegError(result));
        return false;
    }
    decodeFrame_ = av_frame_alloc();
    if (!decodeFrame_) { setError("Cannot allocate video frame"); return false; }
    videoSeekFallbackFrame_ = av_frame_alloc();
    if (!videoSeekFallbackFrame_) {
        setError("Cannot allocate final seek fallback frame");
        return false;
    }
    {
        // This is setup, not proof of the path used by returned frames:
        // Automatic may fall back after D3D11VA was initially selected.
        std::ostringstream summary;
        summary << "Decoder setup " << codec->name << ' ' << videoCodec_->width << 'x'
                 << videoCodec_->height << ' '
                 << (usingHardware_ ? (decodeMode_ == DecodeMode::Hardware
                         ? "D3D11VA-required" : "D3D11VA-preferred") : "software")
                 << " threads=" << videoCodec_->thread_count;
        appendDiagnostic(summary.str());
    }
    videoStartPts_ = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    videoWidth_.store(std::max(0, videoCodec_->width));
    videoHeight_.store(std::max(0, videoCodec_->height));
    if (videoFormat_->duration != AV_NOPTS_VALUE) {
        duration_.store(static_cast<double>(videoFormat_->duration) / AV_TIME_BASE);
    }
    return true;
}

void VideoSource::closeVideoInput() {
    av_frame_free(&decodeFrame_);
    av_frame_free(&videoSeekFallbackFrame_);
    closeSubtitleDecoders();
    avcodec_free_context(&videoCodec_);
    av_buffer_unref(&hardwareDevice_);
    sws_freeContext(sws_);
    sws_ = nullptr;
    if (videoFormat_) avformat_close_input(&videoFormat_);
    videoIo_.reset();
    videoStream_ = -1;
}

void VideoSource::videoWorkerMain() {
    if (!openVideoInput()) {
        running_.store(false);
        if (cache_) cache_->stop();
        cv_.notify_all();
        return;
    }
    ready_.store(true);
    bool firstPacket = true;
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        setError("Cannot allocate video packet");
        running_.store(false);
        if (cache_) cache_->stop();
        cv_.notify_all();
        return;
    }
    while (running_.load()) {
        double target = 0;
        bool seek = false;
        bool showKeyframePreview = false;
        bool keyframe = false;
        int direction = 0;
        std::uint64_t seekGeneration{};
        {
            std::scoped_lock lock(mutex_);
            if (videoSeekPending_) {
                target = videoSeekTarget_;
                showKeyframePreview = videoSeekPreviewRequested_;
                keyframe = videoSeekKeyframeRequested_;
                direction = videoSeekDirectionRequested_;
                seekGeneration = pendingVideoSeekGeneration_;
                videoSeekPending_ = false;
                seek = true;
            }
        }
        if (seek) {
            decodingVideoSeekGeneration_ = seekGeneration;
            videoIo_->clearError();
            if (!handleVideoSeek(target, showKeyframePreview, keyframe, direction)) break;
        }
        if (!waitForVideoCapacity()) break;
        const int result = av_read_frame(videoFormat_, packet);
        {
            std::scoped_lock lock(mutex_);
            if (videoSeekPending_) { av_packet_unref(packet); continue; }
        }
        if (result == AVERROR_EOF) {
            if (!decodeVideoPacket(nullptr)) break;
            // A duration endpoint is just outside the final half-open frame
            // interval. It may also be later than the last video timestamp
            // when the container duration includes audio or padding. Promote
            // the last hidden preroll frame so an end seek still completes.
            if (videoSeekTargetPtsValid_ && videoSeekFallbackFrame_ &&
                videoSeekFallbackFrame_->format >= 0 &&
                videoSeekFallbackGeneration_ == decodingVideoSeekGeneration_ &&
                !pushVideoFrame(videoSeekFallbackFrame_, true)) {
                break;
            }
            eof_.store(true);
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !running_.load() || videoSeekPending_; });
            continue;
        }
        if (result < 0) {
            if (running_.load()) setError("Video read error: " + ffmpegError(result));
            break;
        }
        if (packet->stream_index == videoStream_ && firstPacket && cache_) {
            auto bitrate = videoFormat_->bit_rate;
            if (bitrate <= 0 && duration_ > 0 && cache_->size() > 0) {
                const long double estimate = static_cast<long double>(cache_->size()) * 8 / duration_.load();
                const auto maximum = std::numeric_limits<std::int64_t>::max();
                bitrate = estimate >= static_cast<long double>(maximum)
                    ? maximum : static_cast<std::int64_t>(estimate);
            }
            cache_->configure(bitrate);
            cache_->prime();
            std::scoped_lock lock(mutex_);
            if (videoSeekPending_) { av_packet_unref(packet); continue; }
            firstPacket = false;
        }
        if (packet->stream_index == videoStream_ && videoSkipToKeyframe_) {
            // Read ahead of the keyframe the seek lands on, for the lines
            // already on screen there: the video before it is passed over.
            const auto timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (!(packet->flags & AV_PKT_FLAG_KEY) || timestamp == AV_NOPTS_VALUE ||
                timestamp < videoSkipUntilPts_) {
                av_packet_unref(packet);
                continue;
            }
            videoSkipToKeyframe_ = false;
        }
        if (packet->stream_index == videoStream_ && !decodeVideoPacket(packet)) {
            av_packet_unref(packet);
            break;
        }
        if (packet->stream_index != videoStream_ && !subtitleReaders_.empty()) decodeSubtitlePacket(packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (!eof_.load()) {
        running_.store(false);
        if (cache_) cache_->stop();
        cv_.notify_all();
    }
}

unsigned VideoSource::videoQueueCapacity() const {
    return static_cast<unsigned>(usingHardware_ ? kMaxHardwareFrames : kMaxSoftwareFrames);
}

bool VideoSource::waitForVideoCapacity() {
    // usingHardware_ is settled by openVideoInput() before this worker starts
    // reading packets, and only this thread touches it. Automatic mode may
    // still deliver software frames under this flag; the smaller queue is the
    // safe side of that, costing a little jitter tolerance rather than risking
    // a starved surface pool.
    const std::size_t capacity = usingHardware_ ? kMaxHardwareFrames : kMaxSoftwareFrames;
    std::unique_lock lock(mutex_);
    if (videoQueue_.size() >= capacity) videoCapacityWaits_.fetch_add(1);
    cv_.wait(lock, [&] {
        return !running_.load() || videoSeekPending_ || videoQueue_.size() < capacity;
    });
    return running_.load();
}

bool VideoSource::handleVideoSeek(double seconds, bool showKeyframePreview, bool keyframe, int direction) {
    AVStream* stream = videoFormat_->streams[videoStream_];
    const double timeBase = av_q2d(stream->time_base);
    const auto target = streamTimestampForSeconds(
        seconds, videoStartPts_, stream->time_base.num, stream->time_base.den);
    // Seek from at most the last video-stream tick: some demuxers reject EOF
    // itself, and the requested container position may be later because audio
    // or padding outlives video. The exact gate below deliberately retains the
    // original target so EOF can promote the true final frame.
    auto demuxTarget = videoSeekDecodeTargetPts(
        target, videoStartPts_, stream->duration);
    if (demuxTarget == target && duration_.load() > 0.0 &&
        seconds >= duration_.load() - timeBase) {
        demuxTarget = std::max(videoStartPts_, demuxTarget - 1);
    }
    // A forward keyframe step takes the keyframe at or after the target;
    // past the last one, the one before it.
    const int flags = keyframe && direction > 0 ? 0 : AVSEEK_FLAG_BACKWARD;
    int result = av_seek_frame(videoFormat_, videoStream_, demuxTarget, flags);
    if (result < 0 && flags == 0) {
        result = av_seek_frame(videoFormat_, videoStream_, demuxTarget, AVSEEK_FLAG_BACKWARD);
    }
    if (result < 0) {
        { std::scoped_lock lock(mutex_); if (videoSeekPending_) return true; }
        if (running_.load()) setError("Video seek failed: " + ffmpegError(result));
        return false;
    }
    av_frame_unref(videoSeekFallbackFrame_);
    videoSeekFallbackGeneration_ = 0;
    avcodec_flush_buffers(videoCodec_);
    for (auto& reader : subtitleReaders_) avcodec_flush_buffers(reader.codec);
    videoSkipToKeyframe_ = false;
    if (!subtitleReaders_.empty() && std::strstr(videoFormat_->iformat->name, "matroska")) {
        // Matroska keeps a subtitle line in the cluster of its first moment,
        // so a seek that starts reading at a keyframe never sees the line
        // being spoken there. Where the container's index says which
        // keyframe the seek landed on, start a few seconds before it and
        // pass the video over until that keyframe comes; the subtitle
        // packets on the way are read. Without the index the seek stands
        // as it is.
        const int entry = av_index_search_timestamp(stream, demuxTarget, flags);
        const AVIndexEntry* indexed = entry >= 0 ? avformat_index_get_entry(stream, entry) : nullptr;
        // Taken by value: the entry is FFmpeg's only until the next call on
        // the stream, and the next call is the seek below.
        const std::int64_t landed = indexed ? indexed->timestamp : AV_NOPTS_VALUE;
        if (landed != AV_NOPTS_VALUE) {
            double lead = kSubtitlePrerollSeconds;
            if (videoFormat_->bit_rate > 0) {
                lead = std::clamp(kSubtitlePrerollBits / static_cast<double>(videoFormat_->bit_rate),
                                  kSubtitlePrerollMinimumSeconds, kSubtitlePrerollSeconds);
            }
            const std::int64_t preroll = static_cast<std::int64_t>(lead / timeBase);
            const std::int64_t early = std::max(videoStartPts_, landed - preroll);
            // Nothing earlier to read from when it landed on the first frame.
            if (early < landed) {
                if (av_seek_frame(videoFormat_, videoStream_, early, AVSEEK_FLAG_BACKWARD) >= 0) {
                    videoSkipToKeyframe_ = true;
                    // The keyframe is known by its time; a rounding of that
                    // time between the index and the packet -- Matroska
                    // counts in milliseconds -- must not let it pass.
                    videoSkipUntilPts_ = landed - static_cast<std::int64_t>(0.002 / timeBase);
                } else if (av_seek_frame(videoFormat_, videoStream_, demuxTarget, flags) < 0) {
                    av_seek_frame(videoFormat_, videoStream_, demuxTarget, AVSEEK_FLAG_BACKWARD);
                }
            }
        }
    }
    videoSeekLandingPending_ = keyframe;
    if (keyframe) {
        // Landing on the keyframe itself: no gate and no preroll; the first
        // frame decoded is the frame, wherever it is.
        videoSeekTargetPtsValid_ = false;
        videoSeekPreviewPending_ = false;
        videoFastDecodeUntil_ = -1.0;
        videoCodec_->skip_frame = AVDISCARD_DEFAULT;
        videoCodec_->skip_idct = AVDISCARD_DEFAULT;
        videoCodec_->skip_loop_filter = AVDISCARD_DEFAULT;
        eof_.store(false);
        return true;
    }
    // FFmpeg seeks to the preceding keyframe. Decode those dependency frames,
    // but never publish them: otherwise the renderer visibly fast-forwards
    // from the keyframe (sometimes the beginning of the file) to the target.
    // The exact gate is the target PTS itself. pushVideoFrame uses each frame's
    // presentation duration to decide whether its interval has reached it;
    // unlike the old fixed 50ms gate, this is correct for both low and high
    // frame rates.
    videoSeekTargetPts_ = target;
    videoSeekTargetPtsValid_ = true;
    videoSeekPreviewPending_ = showKeyframePreview;
    videoFastDecodeUntil_ = -1.0;
    videoCodec_->skip_frame = AVDISCARD_DEFAULT;
    videoCodec_->skip_idct = AVDISCARD_DEFAULT;
    videoCodec_->skip_loop_filter = AVDISCARD_DEFAULT;
    videoFastDecodeUntil_ = videoSeekFastDecodeUntilSeconds(
        demuxTarget, videoStartPts_, timeBase, kSeekQualityPrerollSeconds);
    if (videoFastDecodeUntil_ >= 0.0) {
        // None of these frames is rendered, and a non-reference frame by
        // definition reconstructs nothing that follows it, so the whole
        // preroll can skip them. Full quality is restored shortly before the
        // destination so the frame that is actually shown is decoded from a
        // complete stream.
        //
        // This is the dominant cost of a seek. The distance from the preceding
        // keyframe, not the speed of any one frame, is what makes a seek slow:
        // on 4K H.264 with a ten-second keyframe gap, hardware decoding still
        // needed almost three seconds to reach the target.
        // Use the clamped video target, not the container request. Otherwise a
        // container whose audio/padding extends beyond video can leave
        // AVDISCARD_NONREF enabled all the way to EOF and make the fallback a
        // stale reference frame instead of the true last presentation frame.
        videoCodec_->skip_frame = AVDISCARD_NONREF;
        if (!usingHardware_) {
            // Both stages run on the CPU, so they mean nothing to a hardware
            // decoder; skipping a frame there keeps the packet off the GPU
            // decoder entirely, which is where the saving comes from.
            videoCodec_->skip_idct = AVDISCARD_NONREF;
            videoCodec_->skip_loop_filter = AVDISCARD_ALL;
        }
    }
    eof_.store(false);
    return true;
}

bool VideoSource::decodeVideoPacket(const AVPacket* packet) {
    if (packet && videoFastDecodeUntil_ >= 0.0) {
        const auto timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if (timestamp == AV_NOPTS_VALUE) {
            videoFastDecodeUntil_ = -1.0;
        } else {
            const double seconds = static_cast<double>(timestamp - videoStartPts_) *
                av_q2d(videoFormat_->streams[videoStream_]->time_base);
            if (seconds >= videoFastDecodeUntil_) videoFastDecodeUntil_ = -1.0;
        }
        if (videoFastDecodeUntil_ < 0.0) {
            videoCodec_->skip_frame = AVDISCARD_DEFAULT;
            videoCodec_->skip_idct = AVDISCARD_DEFAULT;
            videoCodec_->skip_loop_filter = AVDISCARD_DEFAULT;
        }
    }
    int result = avcodec_send_packet(videoCodec_, packet);
    if (result < 0 && result != AVERROR_EOF) { setError("Video packet rejected: " + ffmpegError(result)); return false; }
    while (running_.load()) {
        result = avcodec_receive_frame(videoCodec_, decodeFrame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
        if (result < 0) { setError("Video decode failed: " + ffmpegError(result)); return false; }
        if (!actualVideoFrameLogged_) {
            actualVideoFrameLogged_ = true;
            const bool hardware = decodeFrame_->format == AV_PIX_FMT_D3D11 && decodeFrame_->data[0];
            auto pixelFormat = static_cast<AVPixelFormat>(decodeFrame_->format);
            if (hardware && decodeFrame_->hw_frames_ctx) {
                const auto* frames = reinterpret_cast<const AVHWFramesContext*>(decodeFrame_->hw_frames_ctx->data);
                pixelFormat = frames->sw_format;
            }
            const auto* descriptor = av_pix_fmt_desc_get(pixelFormat);
            const auto nameOrUnknown = [](const char* name) { return name ? name : "unknown"; };
            std::ostringstream summary;
            summary << "Actual video frame decode=" << (hardware ? "D3D11VA" : "CPU")
                    << " format=" << nameOrUnknown(av_get_pix_fmt_name(static_cast<AVPixelFormat>(decodeFrame_->format)))
                    << " pixel-format=" << nameOrUnknown(av_get_pix_fmt_name(pixelFormat))
                    << " size=" << decodeFrame_->width << 'x' << decodeFrame_->height
                    << " depth=" << (descriptor && descriptor->nb_components ? descriptor->comp[0].depth : 0)
                    << " matrix=" << nameOrUnknown(av_color_space_name(decodeFrame_->colorspace))
                    << '(' << decodeFrame_->colorspace << ')'
                    << " transfer=" << nameOrUnknown(av_color_transfer_name(decodeFrame_->color_trc))
                    << '(' << decodeFrame_->color_trc << ')'
                    << " primaries=" << nameOrUnknown(av_color_primaries_name(decodeFrame_->color_primaries))
                    << '(' << decodeFrame_->color_primaries << ')'
                    << " range=" << nameOrUnknown(av_color_range_name(decodeFrame_->color_range))
                    << '(' << decodeFrame_->color_range << ')';
            appendDiagnostic(summary.str());
        }
        if (!pushVideoFrame(decodeFrame_)) { av_frame_unref(decodeFrame_); return false; }
        videoFramesDecoded_.fetch_add(1);
        av_frame_unref(decodeFrame_);
    }
    return false;
}

bool VideoSource::pushVideoFrame(AVFrame* decoded, bool endOfStreamFallback) {
    {
        std::scoped_lock lock(mutex_);
        if (videoSeekPending_) return true;
    }
    const auto timestamp = decoded->best_effort_timestamp == AV_NOPTS_VALUE
        ? decoded->pts : decoded->best_effort_timestamp;
    AVStream* stream = videoFormat_->streams[videoStream_];
    const double timeBase = av_q2d(stream->time_base);
    const std::int64_t frameDuration = effectiveVideoFrameDuration(
        videoFormat_, stream, decoded);
    double seconds = 0.0;
    if (timestamp != AV_NOPTS_VALUE) {
        seconds = static_cast<double>(timestamp - videoStartPts_) * timeBase;
    }
    bool exactSeekFrame = endOfStreamFallback && decodingVideoSeekGeneration_ != 0;
    bool landed = false;
    if (!endOfStreamFallback && videoSeekLandingPending_ && decodingVideoSeekGeneration_ != 0) {
        // A keyframe seek: this first frame is where it landed.
        videoSeekLandingPending_ = false;
        videoSeekTargetPtsValid_ = false;
        videoSeekPreviewPending_ = false;
        av_frame_unref(videoSeekFallbackFrame_);
        videoSeekFallbackGeneration_ = 0;
        exactSeekFrame = true;
        landed = true;
    } else if (endOfStreamFallback) {
        videoSeekTargetPtsValid_ = false;
        videoSeekPreviewPending_ = false;
        videoSeekFallbackGeneration_ = 0;
    } else if (timestamp != AV_NOPTS_VALUE) {
        const bool seeking = videoSeekTargetPtsValid_ && decodingVideoSeekGeneration_ != 0;
        if (seeking && frameEndsAtOrBeforeSeekTarget(
                           timestamp, frameDuration, videoSeekTargetPts_)) {
            // Keep only a cheap AVFrame reference, not a converted CPU frame.
            // One retained D3D11 surface is covered by extra_hw_frames and is
            // released as soon as a newer preroll or the exact frame arrives.
            av_frame_unref(videoSeekFallbackFrame_);
            videoSeekFallbackGeneration_ = 0;
            const int fallbackResult = av_frame_ref(videoSeekFallbackFrame_, decoded);
            if (fallbackResult < 0) {
                setError("Cannot retain final seek preroll frame: " +
                         ffmpegError(fallbackResult));
                return false;
            }
            videoSeekFallbackGeneration_ = decodingVideoSeekGeneration_;
            if (!videoSeekPreviewPending_) {
                return true;
            }
            // The preview opportunity belongs only to the first decoded
            // keyframe reached by the backward seek. If that keyframe is too
            // old, reject the preview entirely; do not later expose an
            // arbitrary dependency frame merely because it entered the
            // 15-second window.
            videoSeekPreviewPending_ = false;
            const bool keyframe = (decoded->flags & AV_FRAME_FLAG_KEY) != 0;
            const double targetSeconds = static_cast<double>(
                videoSeekTargetPts_ - videoStartPts_) * timeBase;
            if (!seekKeyframePreviewEligible(
                    keyframe, seconds, targetSeconds,
                    kMaximumSeekPreviewLeadSeconds)) {
                return true;
            }
        } else if (seeking) {
            exactSeekFrame = true;
            videoSeekTargetPtsValid_ = false;
            videoSeekPreviewPending_ = false;
            av_frame_unref(videoSeekFallbackFrame_);
            videoSeekFallbackGeneration_ = 0;
        }
    } else if (videoSeekTargetPtsValid_ && decodingVideoSeekGeneration_ != 0) {
        // With no timestamp there is no interval to compare. Preserve the
        // established fail-open behaviour, but consume the gate exactly once.
        exactSeekFrame = true;
        videoSeekTargetPtsValid_ = false;
        videoSeekPreviewPending_ = false;
        av_frame_unref(videoSeekFallbackFrame_);
        videoSeekFallbackGeneration_ = 0;
    }
    auto frame = std::make_shared<HardwareVideoFrame>();
    if (decoded->format == AV_PIX_FMT_D3D11 && decoded->data[0]) {
        frame->ownerFrame = av_frame_clone(decoded);
        if (!frame->ownerFrame) { setError("Cannot retain D3D11VA surface"); return false; }
        frame->texture = reinterpret_cast<ID3D11Texture2D*>(decoded->data[0]);
        frame->arraySlice = static_cast<unsigned>(reinterpret_cast<std::intptr_t>(decoded->data[1]));
        frame->width = decoded->width;
        frame->height = decoded->height;
        frame->hardware = true;
    } else {
        if (decodeMode_ == DecodeMode::Hardware) {
            setError("Decoder returned a CPU frame while Hardware mode is selected");
            return false;
        }
        frame->ownerFrame = av_frame_clone(decoded);
        if (!frame->ownerFrame) { setError("Cannot retain software video frame"); return false; }
        const auto requested = presentationSize(
            decoded->width, decoded->height,
            preferredWidth_.load(std::memory_order_relaxed),
            preferredHeight_.load(std::memory_order_relaxed));
        frame->width = requested.width;
        frame->height = requested.height;
        frame->stride = frame->width * 4;
        frame->bgra.resize(static_cast<std::size_t>(frame->stride) * frame->height);
        sws_ = sws_getCachedContext(sws_, decoded->width, decoded->height,
            static_cast<AVPixelFormat>(decoded->format), frame->width, frame->height,
            AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) { setError("Cannot create software pixel converter"); return false; }
        if (configureSoftwareColorspace(sws_, *decoded) < 0) {
            setError("Cannot configure software color conversion");
            return false;
        }
        std::uint8_t* destination[] = {frame->bgra.data()};
        int destinationStride[] = {frame->stride};
        if (sws_scale(sws_, decoded->data, decoded->linesize, 0, decoded->height,
                      destination, destinationStride) <= 0) {
            setError("Software pixel conversion failed");
            return false;
        }
    }
    frame->pts = seconds;
    frame->duration = frameDuration > 0
        ? static_cast<double>(frameDuration) * timeBase : 0.0;
    frame->seekGeneration = decodingVideoSeekGeneration_;
    frame->exactSeekFrame = exactSeekFrame;
    if (endOfStreamFallback) {
        // Hardware frames have been cloned and software pixels copied above,
        // so the worker no longer needs its retained fallback reference.
        av_frame_unref(videoSeekFallbackFrame_);
        videoSeekFallbackGeneration_ = 0;
    }
    {
        std::scoped_lock lock(mutex_);
        // A new seek can arrive while a 4K software frame is being converted.
        // Recheck under the enqueue lock so work from the superseded request
        // cannot become visible after requestSeek() cleared the old queue.
        if (videoSeekPending_) return true;
        if (exactSeekFrame) {
            exactSeekReadyGeneration_.store(
                decodingVideoSeekGeneration_, std::memory_order_release);
        }
        if (landed) {
            // The audio starts over from where the picture landed, under a
            // generation of its own; the owner learns both from the landing.
            audioSeekGeneration_ = nextAudioGeneration();
            pendingAudioSeekGeneration_ = audioSeekGeneration_;
            audioDecodeState_ = audioStreamAvailability_.load() < 0
                ? AudioDecodeState::Error : AudioDecodeState::Unknown;
            audioSeekTarget_ = seconds;
            audioSeekPending_ = true;
            if (cache_ && hasAudioStream()) cache_->interrupt(1);
            seekLanding_ = SeekLanding{decodingVideoSeekGeneration_, seconds, audioSeekGeneration_};
        }
        frame->serial = gFrameSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        videoQueue_.push_back(std::move(frame));
        queuedVideoFrames_.store(static_cast<unsigned>(videoQueue_.size()));
    }
    cv_.notify_all();
    return true;
}

int VideoSource::openAudioInput(AVFormatContext** format, AVCodecContext** codec,
                                AVFrame** frame, int* streamIndex, std::int64_t* startPts) {
    const std::string path = wideToUtf8(path_);
    *format = avformat_alloc_context();
    if (!*format) return AVERROR(ENOMEM);
    (*format)->interrupt_callback.callback = &VideoSource::interruptCallback;
    (*format)->interrupt_callback.opaque = this;
    audioIo_ = std::make_unique<CachedAvio>(cache_, 1);
    const int attached = audioIo_->attach(*format);
    if (attached < 0) return attached;
    int result = openInputWithOptions(format, path, options_);
    if (result < 0) return result;
    result = avformat_find_stream_info(*format, nullptr);
    if (result < 0) return result;
    // A requested track wins when it names an audio stream of this container;
    // otherwise FFmpeg's notion of the best stream, as before.
    const int wanted = requestedAudioStream_.load();
    *streamIndex = -1;
    if (wanted >= 0 && wanted < static_cast<int>((*format)->nb_streams) &&
        (*format)->streams[wanted]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        *streamIndex = wanted;
    } else {
        *streamIndex = av_find_best_stream(*format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (*streamIndex < 0) return *streamIndex;
    result = openAudioCodec(*format, *streamIndex, codec, startPts);
    if (result < 0) return result;
    *frame = av_frame_alloc();
    if (!*frame) return AVERROR(ENOMEM);
    activeAudioStream_.store(*streamIndex);
    return 0;
}

int VideoSource::openAudioCodec(AVFormatContext* format, int streamIndex, AVCodecContext** codec,
                                std::int64_t* startPts) {
    AVStream* stream = format->streams[streamIndex];
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        format->streams[index]->discard =
            static_cast<int>(index) == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;
    *codec = avcodec_alloc_context3(decoder);
    if (!*codec) return AVERROR(ENOMEM);
    int result = avcodec_parameters_to_context(*codec, stream->codecpar);
    if (result < 0) return result;
    result = avcodec_open2(*codec, decoder, nullptr);
    if (result < 0) return result;
    *startPts = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    return 0;
}

void VideoSource::audioWorkerMain() {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    int streamIndex = -1;
    std::int64_t startPts = 0;
    AudioSampleTimeline audioTimeline;
    const int openResult = openAudioInput(
        &format, &codec, &frame, &streamIndex, &startPts);
    if (openResult < 0) {
        audioStreamAvailability_.store(-1);
        const auto status = audioDecodeStatus();
        setAudioDecodeState(status.generation, AudioDecodeState::Error);
        if (running_.load()) {
            appendDiagnostic("No usable audio stream: " + wideToUtf8(path_) +
                             " (" + ffmpegError(openResult) + ")");
        }
        if (format) avformat_close_input(&format);
        avcodec_free_context(&codec);
        av_frame_free(&frame);
        return;
    }
    audioStreamAvailability_.store(1);
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        audioStreamAvailability_.store(-1);
        const auto status = audioDecodeStatus();
        setAudioDecodeState(status.generation, AudioDecodeState::Error);
        if (running_.load()) {
            appendDiagnostic("Cannot allocate audio packet: " + wideToUtf8(path_));
        }
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return;
    }

    enum class ReceiveResult {
        NeedInput,
        Ended,
        Superseded,
        Failed,
    };

    std::uint64_t decodingGeneration{};
    {
        std::scoped_lock lock(mutex_);
        decodingGeneration = audioSeekGeneration_;
    }
    bool terminal = false;
    bool generationPrimed = false;

    auto failGeneration = [&](const std::string& message) {
        if (running_.load() && audioGenerationIsCurrent(decodingGeneration)) {
            appendDiagnostic(message + ": " + wideToUtf8(path_));
            setAudioDecodeState(decodingGeneration, AudioDecodeState::Error);
        }
        terminal = true;
    };

    auto submitConverted = [&](AudioChunk& chunk, int converted) -> ReceiveResult {
        if (!audioGenerationIsCurrent(decodingGeneration)) {
            return ReceiveResult::Superseded;
        }
        if (converted < 0) {
            failGeneration("Audio resample failed (" + ffmpegError(converted) + ")");
            return ReceiveResult::Failed;
        }
        const auto slice = audioTimeline.consume(converted);
        if (slice.count == 0) return ReceiveResult::NeedInput;
        chunk.samples.resize(static_cast<std::size_t>(slice.skip + slice.count) * 2);
        chunk.samples.erase(chunk.samples.begin(),
                            chunk.samples.begin() + slice.skip * 2);
        chunk.pts = static_cast<double>(slice.firstSample) / 48000;
        chunk.generation = decodingGeneration;
        while (running_.load() && audioEnabled_.load() && audioSink_) {
            if (!audioGenerationIsCurrent(decodingGeneration)) {
                return ReceiveResult::Superseded;
            }
            const auto submitResult = audioSink_(std::move(chunk));
            if (submitResult == AudioSubmitResult::Accepted) {
                audioChunksDecoded_.fetch_add(1);
                if (!generationPrimed) {
                    setAudioDecodeState(decodingGeneration, AudioDecodeState::Primed);
                    generationPrimed = true;
                }
                return ReceiveResult::NeedInput;
            }
            if (submitResult == AudioSubmitResult::Stale) {
                return ReceiveResult::Superseded;
            }
            if (submitResult == AudioSubmitResult::Error) {
                failGeneration("Audio output submission failed");
                return ReceiveResult::Failed;
            }
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return !running_.load() || !audioEnabled_.load() || audioSeekPending_;
            });
        }
        return ReceiveResult::Superseded;
    };

    auto finishGeneration = [&]() {
        // Decoder EOF does not drain libswresample's filter delay. Flush it
        // through the same trim/submission path, especially for short clips
        // whose seek destination lies entirely in that final delayed output.
        while (swr && running_.load() && audioEnabled_.load()) {
            if (!audioGenerationIsCurrent(decodingGeneration)) return;
            const int capacity = swr_get_out_samples(swr, 0);
            if (capacity < 0) {
                failGeneration("Cannot size audio resampler flush");
                return;
            }
            AudioChunk chunk;
            chunk.samples.resize(static_cast<std::size_t>(std::max(1, capacity)) * 2);
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(chunk.samples.data())};
            const int converted = swr_convert(swr, output, std::max(1, capacity), nullptr, 0);
            const auto submitted = submitConverted(chunk, converted);
            if (submitted == ReceiveResult::Failed ||
                submitted == ReceiveResult::Superseded) return;
            if (converted == 0) break;
        }
        if (!running_.load() || !audioEnabled_.load() ||
            !audioGenerationIsCurrent(decodingGeneration)) return;
        setAudioDecodeState(decodingGeneration, AudioDecodeState::Ended);
        terminal = true;
    };

    auto receiveFrames = [&]() -> ReceiveResult {
        while (running_.load() && audioEnabled_.load()) {
            const int result = avcodec_receive_frame(codec, frame);
            if (result == AVERROR(EAGAIN)) return ReceiveResult::NeedInput;
            if (result == AVERROR_EOF) return ReceiveResult::Ended;
            if (result < 0) {
                failGeneration("Audio decode failed (" + ffmpegError(result) + ")");
                return ReceiveResult::Failed;
            }
            if (!audioGenerationIsCurrent(decodingGeneration)) {
                av_frame_unref(frame);
                return ReceiveResult::Superseded;
            }
            if (frame->sample_rate <= 0) {
                av_frame_unref(frame);
                failGeneration("Audio frame has no valid sample rate");
                return ReceiveResult::Failed;
            }
            if (!swr) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                const int setup = swr_alloc_set_opts2(
                    &swr, &stereo, AV_SAMPLE_FMT_FLT, 48000,
                    &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
                    frame->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&stereo);
                if (setup < 0 || !swr || swr_init(swr) < 0) {
                    av_frame_unref(frame);
                    swr_free(&swr);
                    failGeneration("Audio resampler setup failed");
                    return ReceiveResult::Failed;
                }
            }
            const int capacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(swr, frame->sample_rate) + frame->nb_samples,
                48000, frame->sample_rate, AV_ROUND_UP));
            const auto timestamp = frame->best_effort_timestamp == AV_NOPTS_VALUE
                ? frame->pts : frame->best_effort_timestamp;
            if (timestamp != AV_NOPTS_VALUE) {
                const auto timeBase = format->streams[streamIndex]->time_base;
                // The next PCM buffer starts at the input timestamp minus
                // samples already held in swr. A common multiple gives exact
                // delay units for both input and output rates, avoiding the
                // one-input-sample rounding at 44.1 -> 48 kHz.
                const auto delayBase = static_cast<std::int64_t>(frame->sample_rate) * 48000;
                const double outputStart =
                    (static_cast<double>(timestamp) - static_cast<double>(startPts)) *
                        av_q2d(timeBase) -
                    static_cast<double>(swr_get_delay(swr, delayBase)) / delayBase;
                const auto samplePts = streamTimestampForSeconds(outputStart, 0, 1, 48000);
                const auto tolerance = static_cast<std::int64_t>(
                    std::ceil(48000 * av_q2d(timeBase))) + 1;
                audioTimeline.locate(samplePts, tolerance);
            }
            AudioChunk chunk;
            chunk.samples.resize(static_cast<std::size_t>(std::max(0, capacity)) * 2);
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(chunk.samples.data())};
            const int converted = capacity > 0
                ? swr_convert(swr, output, capacity, frame->extended_data, frame->nb_samples)
                : 0;
            av_frame_unref(frame);
            const auto submitted = submitConverted(chunk, converted);
            if (submitted != ReceiveResult::NeedInput) return submitted;
        }
        return ReceiveResult::Superseded;
    };

    while (running_.load()) {
        double seekTarget = 0.0;
        bool seek = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return !running_.load() ||
                       (audioEnabled_.load() && (!terminal || audioSeekPending_));
            });
            if (!running_.load()) break;
            if (audioSeekPending_) {
                seekTarget = audioSeekTarget_;
                decodingGeneration = pendingAudioSeekGeneration_;
                audioSeekPending_ = false;
                seek = true;
                terminal = false;
                generationPrimed = false;
            }
        }
        if (seek) {
            // A track change rides on the seek that App issues with it: the
            // demuxer is repositioned anyway, so the decoder is swapped first.
            const int wantedStream = requestedAudioStream_.load();
            if (wantedStream >= 0 && wantedStream != streamIndex &&
                wantedStream < static_cast<int>(format->nb_streams) &&
                format->streams[wantedStream]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                AVCodecContext* replacement = nullptr;
                std::int64_t replacementStart = 0;
                const int switched = openAudioCodec(format, wantedStream, &replacement, &replacementStart);
                if (switched < 0) {
                    avcodec_free_context(&replacement);
                    for (unsigned index = 0; index < format->nb_streams; ++index) {
                        format->streams[index]->discard =
                            static_cast<int>(index) == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
                    }
                    // Give the request up rather than fail this and every
                    // later seek on it; the current track keeps playing.
                    requestedAudioStream_.store(streamIndex);
                    appendDiagnostic("Audio track switch to stream " + std::to_string(wantedStream) +
                                     " failed (" + ffmpegError(switched) + "); keeping stream " +
                                     std::to_string(streamIndex) + ": " + wideToUtf8(path_));
                } else {
                    avcodec_free_context(&codec);
                    codec = replacement;
                    streamIndex = wantedStream;
                    startPts = replacementStart;
                    activeAudioStream_.store(streamIndex);
                    appendDiagnostic("Audio track switched to stream " + std::to_string(streamIndex) +
                                     ": " + wideToUtf8(path_));
                }
            }
            AVStream* stream = format->streams[streamIndex];
            audioIo_->clearError();
            // Even PCM demuxers may seek directly to the requested sample.
            // Prime the resampling filter before that point so near-EOF seeks
            // have enough input to produce their short final output. Respect
            // codecs with a longer declared convergence preroll as well.
            const double preroll = std::max(0.050, codec->sample_rate > 0
                ? static_cast<double>(stream->codecpar->seek_preroll) / codec->sample_rate
                : 0.0);
            const auto point = audioSeekPoint(format, streamIndex, startPts, seekTarget, preroll);
            const int seekResult = av_seek_frame(
                format, point.stream, point.timestamp, AVSEEK_FLAG_BACKWARD);
            if (seekResult < 0) {
                if (!running_.load()) break;
                if (!audioGenerationIsCurrent(decodingGeneration)) continue;
                failGeneration("Audio seek failed (" + ffmpegError(seekResult) + ")");
                continue;
            }
            avcodec_flush_buffers(codec);
            swr_free(&swr);
            audioTimeline.reset(streamTimestampForSeconds(seekTarget, 0, 1, 48000));
            if (!audioGenerationIsCurrent(decodingGeneration)) continue;
        }
        const int readResult = av_read_frame(format, packet);
        if (!audioGenerationIsCurrent(decodingGeneration)) {
            av_packet_unref(packet);
            continue;
        }
        if (readResult == AVERROR(EAGAIN)) {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return !running_.load() || !audioEnabled_.load() ||
                       audioSeekPending_;
            });
            continue;
        }
        if (readResult == AVERROR_EOF) {
            int sendResult = avcodec_send_packet(codec, nullptr);
            if (sendResult == AVERROR(EAGAIN)) {
                const ReceiveResult pending = receiveFrames();
                if (pending == ReceiveResult::Failed ||
                    pending == ReceiveResult::Superseded) {
                    continue;
                }
                if (pending == ReceiveResult::Ended) {
                    finishGeneration();
                    continue;
                }
                sendResult = avcodec_send_packet(codec, nullptr);
            }
            if (sendResult < 0 && sendResult != AVERROR_EOF) {
                failGeneration("Audio decoder flush failed (" +
                               ffmpegError(sendResult) + ")");
                continue;
            }
            const ReceiveResult received = sendResult == AVERROR_EOF
                ? ReceiveResult::Ended : receiveFrames();
            if (received == ReceiveResult::Failed ||
                received == ReceiveResult::Superseded) {
                continue;
            }
            finishGeneration();
            continue;
        }
        if (readResult < 0) {
            if (!running_.load()) break;
            failGeneration("Audio read failed (" + ffmpegError(readResult) + ")");
            continue;
        }
        if (packet->stream_index != streamIndex) { av_packet_unref(packet); continue; }
        if (!audioGenerationIsCurrent(decodingGeneration)) {
            av_packet_unref(packet);
            continue;
        }
        int result = avcodec_send_packet(codec, packet);
        if (result == AVERROR(EAGAIN)) {
            const ReceiveResult received = receiveFrames();
            if (received == ReceiveResult::Failed ||
                received == ReceiveResult::Superseded) {
                av_packet_unref(packet);
                continue;
            }
            result = avcodec_send_packet(codec, packet);
        }
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR_EOF) {
            failGeneration("Audio packet rejected (" + ffmpegError(result) + ")");
            continue;
        }
        const ReceiveResult received = result == AVERROR_EOF
            ? ReceiveResult::Ended : receiveFrames();
        if (received == ReceiveResult::Ended) {
            finishGeneration();
        }
    }
    av_packet_free(&packet);
    swr_free(&swr);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
}

}  // namespace quaddeck
