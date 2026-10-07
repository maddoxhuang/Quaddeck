#pragma once

#include "Core.hpp"
#include "ReadAheadCache.hpp"
#include "Subtitles.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <d3d11.h>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct SwsContext;

namespace quaddeck {

struct HardwareVideoFrame {
    HardwareVideoFrame();
    ~HardwareVideoFrame();
    HardwareVideoFrame(const HardwareVideoFrame&) = delete;
    HardwareVideoFrame& operator=(const HardwareVideoFrame&) = delete;

    double pts{};
    // Effective presentation duration in seconds. The decoder-provided value
    // is preferred; a guessed stream frame rate fills it when that is absent.
    // Zero means neither source was available.
    double duration{};
    int width{};
    int height{};
    std::uint64_t serial{};
    std::uint64_t seekGeneration{};
    bool exactSeekFrame{};
    ID3D11Texture2D* texture{};
    unsigned arraySlice{};
    // Retains original pixels/metadata for both decode paths. CPU frames also
    // keep their existing presentation-sized BGRA fallback below.
    AVFrame* ownerFrame{};
    bool hardware{};
    int stride{};
    std::vector<std::uint8_t> bgra;
};

// One subtitle stream of the container, as the menu lists it. A stream of
// pictures (PGS, VobSub) is listed too, so the menu can say why it is not
// offered: only text is read.
struct SubtitleTrackInfo {
    int streamIndex{-1};
    std::string language;
    std::string title;
    std::string codec;
    bool text{};
    bool isDefault{};
    bool forced{};
};

// How to open the input beyond its path. HTTP streams from a media server
// carry their credentials in request headers so no token sits in the URL a
// log or a session file would keep.
struct SourceOptions {
    std::string httpHeaders;  // "Name: value\r\n" pairs, empty for local files
    // Reads the container's text subtitle streams as the video is read: a
    // local file's, and an Emby file's as the server sends it. Off for a
    // photo, which has none.
    bool readSubtitles{};
    // Picks the subtitle stream shown from the start, -1 for none. It is
    // asked on the decoder's thread the moment the container's streams are
    // known; it must hold what it needs by value.
    std::function<int(const std::vector<SubtitleTrackInfo>&)> chooseSubtitle;
};

// One audio stream of the container, as the menu lists it.
struct AudioTrackInfo {
    int streamIndex{-1};
    std::string language;
    std::string title;
    std::string codec;
    int channels{};
};

struct AudioChunk {
    double pts{};
    int sampleRate{48000};
    int channels{2};
    // Process-wide and monotonically increasing. AudioOutput keeps persistent
    // voices across VideoSource replacement, so a source-local counter would
    // eventually collide with a generation already invalidated by flush().
    std::uint64_t generation{};
    std::vector<float> samples;

    // The latest generation allocated by any VideoSource in this process.
    // AudioOutput samples this while holding a voice's submission lock so a
    // flush rejects every chunk whose decode began before that flush.
    static std::uint64_t generationWatermark() noexcept;
};

enum class AudioSubmitResult : std::uint8_t {
    Accepted,
    Backpressure,
    Stale,
    Error,
};

enum class AudioDecodeState : std::uint8_t {
    Unknown,
    Primed,
    Ended,
    Error,
};

struct AudioDecodeStatus {
    std::uint64_t generation{};
    AudioDecodeState state{AudioDecodeState::Unknown};
};

class VideoSource {
public:
    using AudioSink = std::function<AudioSubmitResult(AudioChunk&&)>;

    VideoSource();
    ~VideoSource();
    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    bool open(const std::wstring& path, ID3D11Device* device,
              ID3D11DeviceContext* deviceContext,
              std::recursive_mutex* deviceMutex, DecodeMode decodeMode,
              AudioSink audioSink = {}, CacheMode cacheMode = CacheMode::Network,
              SourceOptions options = {});
    void close();
    std::uint64_t requestSeek(double seconds, bool showKeyframePreview = false);
    std::uint64_t requestAudioSeek(double seconds);
    // A seek that lands on a keyframe instead of decoding forward to the
    // exact target: as quick as the demuxer's seek, at the cost of landing
    // up to a keyframe interval away. `direction` > 0 takes the keyframe at
    // or after the target (a forward step), otherwise the one at or before
    // it. The frame it lands on is the exact frame, the audio is re-aimed at
    // that frame's time, and takeSeekLanding() says where it landed.
    std::uint64_t requestKeyframeSeek(double seconds, int direction);
    struct SeekLanding {
        std::uint64_t generation{};
        double seconds{};
        std::uint64_t audioGeneration{};
    };
    std::optional<SeekLanding> takeSeekLanding();
    std::shared_ptr<const HardwareVideoFrame> frameForTime(double seconds, bool advance = true);
    void setAudioEnabled(bool enabled);
    void setAudioOutputPane(std::size_t pane) { audioOutputPane_.store(pane); }
    std::size_t audioOutputPane() const { return audioOutputPane_.load(); }
    // Hint from the renderer: how many pixels this pane currently occupies.
    // Software frames are converted at that size instead of a fixed 1080p.
    void setPreferredSize(int width, int height);

    bool ready() const { return ready_.load(); }
    bool eof() const { return eof_.load(); }
    double duration() const { return duration_.load(); }
    int videoWidth() const { return videoWidth_.load(); }
    int videoHeight() const { return videoHeight_.load(); }
    bool audioStreamKnown() const { return audioStreamAvailability_.load() != 0; }
    bool hasAudioStream() const { return audioStreamAvailability_.load() > 0; }
    AudioDecodeStatus audioDecodeStatus() const;
    bool exactSeekReady(std::uint64_t generation) const {
        return generation != 0 && exactSeekReadyGeneration_.load() >= generation;
    }
    std::uint64_t videoFramesDecoded() const { return videoFramesDecoded_.load(); }
    // Queue occupancy separates a decoder that cannot keep up from one that is
    // being held back: a queue sitting empty means the renderer is starved,
    // while a queue sitting at capacity means the decoder is throttled and has
    // headroom to spare.
    unsigned queuedVideoFrames() const { return queuedVideoFrames_.load(); }
    unsigned videoQueueCapacity() const;
    std::uint64_t videoCapacityWaits() const { return videoCapacityWaits_.load(); }
    std::uint64_t audioChunksDecoded() const { return audioChunksDecoded_.load(); }
    const std::wstring& path() const { return path_; }
    std::string error() const;
    CacheStats cacheStats() const { return cache_ ? cache_->stats() : CacheStats{}; }
    // The container's audio streams, known once the video input is open.
    std::vector<AudioTrackInfo> audioTracks() const;
    // Chooses the audio stream by container index. Before open() it decides
    // the first stream decoded; afterwards the audio worker switches at its
    // next seek, so follow it with requestAudioSeek.
    void setAudioTrack(int streamIndex) { requestedAudioStream_.store(streamIndex); }
    int requestedAudioTrack() const { return requestedAudioStream_.load(); }
    // The stream the audio worker is actually decoding, -1 before it opens.
    int activeAudioTrack() const { return activeAudioStream_.load(); }
    // The container's subtitle streams, known once the video input is open.
    std::vector<SubtitleTrackInfo> subtitleTracks() const;
    // Chooses the subtitle stream whose lines subtitleLinesAt answers with,
    // -1 for none. Every text stream is gathered as the demuxer passes its
    // packets -- nothing is read twice, which on a share would be the whole
    // file -- so a change of stream shows at once what was read of it, and
    // what lies ahead arrives as the video reaches it. A seek reads from a
    // little before where it lands, for the line being spoken there.
    void setSubtitleTrack(int streamIndex);
    int subtitleTrack() const { return shownSubtitleStream_.load(); }
    // The chosen stream's lines at a source time, of the cues read so far.
    SubtitleLines subtitleLinesAt(double seconds) const;
    std::size_t subtitleCueCount() const;
    // A text stream as libass is to be given it: the header its decoder
    // hands over, and whether the stream is plain (anything but ASS or SSA,
    // so the header is FFmpeg's stand-in rather than the stream's own).
    // False for a stream that is not read.
    bool subtitleStreamHeader(int streamIndex, std::string& header, bool& plain) const;
    // The events of that stream read so far, from the `from`th on, in the
    // order the demuxer passed them; each once, however often a seek reads
    // it again. Returns how many there are in all.
    std::size_t subtitleEventsSince(int streamIndex, std::size_t from, std::vector<SubtitleEvent>& events) const;
    // The fonts the container brought (Matroska attachments), known once
    // the video input is open; null before.
    std::shared_ptr<const std::vector<SubtitleFont>> fonts() const;
    // The matrix the video's colours are decoded with, as far as a
    // subtitle's colours care: what its stream says, or what its size
    // suggests when it says nothing.
    SubtitleMatrix colourMatrix() const { return static_cast<SubtitleMatrix>(colourMatrix_.load()); }

private:
    friend struct AudioSeekTestAccess;
    friend struct SubtitleStreamTestAccess;
    friend struct AppRegressionTests;
    void videoWorkerMain();
    void audioWorkerMain();
    bool openVideoInput();
    int openAudioInput(AVFormatContext**, AVCodecContext**, AVFrame**, int*, std::int64_t*);
    // Opens the decoder for one audio stream of an already opened input and
    // discards every other stream; the seek path uses it to switch tracks.
    int openAudioCodec(AVFormatContext* format, int streamIndex, AVCodecContext** codec,
                       std::int64_t* startPts);
    void closeVideoInput();
    bool handleVideoSeek(double seconds, bool showKeyframePreview, bool keyframe, int direction);
    std::uint64_t requestSeekInternal(double seconds, bool showKeyframePreview, bool keyframe, int direction);
    bool decodeVideoPacket(const AVPacket* packet);
    // The video worker's: a decoder for each text subtitle stream, and a
    // packet's lines added to its stream's cues.
    void openSubtitleDecoders();
    void closeSubtitleDecoders();
    void decodeSubtitlePacket(const AVPacket* packet);
    bool pushVideoFrame(AVFrame* decoded, bool endOfStreamFallback = false);
    bool waitForVideoCapacity();
    void setError(std::string message);
    static int interruptCallback(void* opaque);
    void setAudioDecodeState(std::uint64_t generation, AudioDecodeState state);
    bool audioGenerationIsCurrent(std::uint64_t generation) const;

    std::wstring path_;
    std::shared_ptr<ReadAheadCache> cache_;
    std::unique_ptr<CachedAvio> videoIo_;
    std::unique_ptr<CachedAvio> audioIo_;
    SourceOptions options_;
    AudioSink audioSink_;
    DecodeMode decodeMode_{DecodeMode::Automatic};
    ID3D11Device* device_{};
    ID3D11DeviceContext* deviceContext_{};
    std::recursive_mutex* deviceMutex_{};
    std::thread videoWorker_;
    std::thread audioWorker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> audioEnabled_{false};
    std::atomic<std::size_t> audioOutputPane_{0};
    std::atomic<double> duration_{0.0};
    std::atomic<int> videoWidth_{0};
    std::atomic<int> videoHeight_{0};
    std::atomic<int> audioStreamAvailability_{0};
    // Seeded with the previous fixed ceiling so frames decoded before the
    // first layout hint arrives are no larger than they used to be.
    std::atomic<int> preferredWidth_{1920};
    std::atomic<int> preferredHeight_{1080};
    std::atomic<std::uint64_t> exactSeekReadyGeneration_{0};
    std::atomic<std::uint64_t> videoFramesDecoded_{0};
    bool actualVideoFrameLogged_{};  // Video worker only; reset before open.
    std::atomic<unsigned> queuedVideoFrames_{0};
    std::atomic<std::uint64_t> videoCapacityWaits_{0};
    std::atomic<std::uint64_t> audioChunksDecoded_{0};
    std::atomic<int> requestedAudioStream_{-1};
    std::atomic<int> activeAudioStream_{-1};
    std::atomic<int> shownSubtitleStream_{-1};
    // The video worker's own: a decoder per text subtitle stream.
    struct SubtitleReader {
        int streamIndex{-1};
        AVCodecContext* codec{};
        AssHeader header;
        bool plain{};
        // What its events were, so that a seek back adds none twice; and the
        // read order handed to libass for a plain stream, whose decoder
        // counts from 0 again after every seek.
        std::unordered_set<std::string> seen;
        int readOrder{};
    };
    std::vector<SubtitleReader> subtitleReaders_;
    // The cues read so far of each of those streams, under their own lock:
    // the window thread asks for the lines of every frame.
    mutable std::mutex subtitleMutex_;
    std::vector<std::pair<int, SubtitleTrack>> subtitleCues_;
    // The same streams for libass: header, plainness and events. Under the
    // same lock.
    struct SubtitleStreamEvents {
        int streamIndex{-1};
        std::string header;
        bool plain{};
        std::vector<SubtitleEvent> events;
    };
    std::vector<SubtitleStreamEvents> subtitleEvents_;
    std::atomic<int> colourMatrix_{static_cast<int>(SubtitleMatrix::None)};
    // While a seek reads ahead of the keyframe it is to land on, for the
    // subtitle lines that began before it: video packets are passed over
    // until the keyframe at this timestamp. Worker-owned.
    bool videoSkipToKeyframe_{};
    std::int64_t videoSkipUntilPts_{};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<HardwareVideoFrame>> videoQueue_;
    std::shared_ptr<HardwareVideoFrame> currentFrame_;
    bool pausedFrameLocked_{};
    bool videoSeekPending_{};
    bool audioSeekPending_{};
    bool videoSeekPreviewRequested_{};
    bool videoSeekKeyframeRequested_{};
    int videoSeekDirectionRequested_{};
    // Worker-owned: the next decoded frame is where a keyframe seek lands.
    bool videoSeekLandingPending_{};
    std::optional<SeekLanding> seekLanding_;
    bool videoSeekPreviewPending_{};
    std::uint64_t videoSeekGeneration_{};
    std::uint64_t pendingVideoSeekGeneration_{};
    std::uint64_t decodingVideoSeekGeneration_{};
    std::uint64_t audioSeekGeneration_{};
    std::uint64_t pendingAudioSeekGeneration_{};
    AudioDecodeState audioDecodeState_{AudioDecodeState::Unknown};
    double videoSeekTarget_{};
    double audioSeekTarget_{};
    std::int64_t videoSeekTargetPts_{};
    bool videoSeekTargetPtsValid_{};
    std::uint64_t videoSeekFallbackGeneration_{};
    double videoFastDecodeUntil_{-1.0};
    std::string error_;
    std::vector<AudioTrackInfo> audioTracks_;
    std::vector<SubtitleTrackInfo> subtitleTracks_;
    std::shared_ptr<const std::vector<SubtitleFont>> fonts_;
    // Set once the owner has chosen a subtitle stream, or none: the open no
    // longer chooses for it.
    bool subtitleTrackSet_{};
    AVFormatContext* videoFormat_{};
    AVCodecContext* videoCodec_{};
    AVFrame* decodeFrame_{};
    // The most recent hidden preroll frame. If a seek lands at/past the final
    // presentation interval, EOF promotes this one frame instead of leaving
    // the exact-seek generation unresolved (or stranded on its keyframe preview).
    AVFrame* videoSeekFallbackFrame_{};
    AVBufferRef* hardwareDevice_{};
    SwsContext* sws_{};
    bool usingHardware_{};
    int videoStream_{-1};
    std::int64_t videoStartPts_{};
};

}  // namespace quaddeck
