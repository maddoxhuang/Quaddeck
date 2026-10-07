// Subtitle streams inside a container, read by the real video worker as it
// demuxes: no window, GPU or audio device. The fixture is a small Matroska
// file written here -- a video stream, a SubRip stream, an ASS stream and a
// stream of pictures that is listed but never read.

#include "VideoSource.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace quaddeck {
// Runs the video worker in software, which needs no D3D11 device.
struct SubtitleStreamTestAccess {
    static void start(VideoSource& source, const std::filesystem::path& path, SourceOptions options) {
        source.path_ = path.wstring();
        source.options_ = std::move(options);
        source.decodeMode_ = DecodeMode::Software;
        source.running_.store(true);
        source.videoWorker_ = std::thread(&VideoSource::videoWorkerMain, &source);
    }
};
}

using namespace quaddeck;

namespace {

constexpr int kFrameRate = 25;
constexpr int kFrames = 250;  // ten seconds
// Stands in for a font's bytes: what is attached comes back as it went in.
const std::string kFontBytes = std::string("\0\1\0\0", 4) + std::string(3000, 'F');

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check(int result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

struct Line {
    int stream;
    int startMs;
    int durationMs;
    std::string text;
};

AVStream* addSubtitleStream(AVFormatContext* format, AVCodecID codec, const char* language, const char* title,
                            bool isDefault, const std::string& header = {}) {
    AVStream* stream = avformat_new_stream(format, nullptr);
    if (!stream) throw std::bad_alloc();
    stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    stream->codecpar->codec_id = codec;
    stream->time_base = {1, 1000};
    if (language) av_dict_set(&stream->metadata, "language", language, 0);
    if (title) av_dict_set(&stream->metadata, "title", title, 0);
    if (isDefault) stream->disposition |= AV_DISPOSITION_DEFAULT;
    if (!header.empty()) {
        stream->codecpar->extradata = static_cast<std::uint8_t*>(
            av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!stream->codecpar->extradata) throw std::bad_alloc();
        std::memcpy(stream->codecpar->extradata, header.data(), header.size());
        stream->codecpar->extradata_size = static_cast<int>(header.size());
    }
    return stream;
}

// Returns the stream indexes: video, SubRip, ASS, pictures.
std::vector<int> makeFixture(const std::filesystem::path& path) {
    AVFormatContext* format = nullptr;
    const std::string file = path.string();
    check(avformat_alloc_output_context2(&format, nullptr, "matroska", file.c_str()), "allocate output");
    const AVCodec* encoder = nullptr;
    for (const AVCodecID id : {AV_CODEC_ID_MPEG4, AV_CODEC_ID_MJPEG, AV_CODEC_ID_FFV1}) {
        encoder = avcodec_find_encoder(id);
        if (encoder) break;
    }
    if (!encoder) throw std::runtime_error("No video encoder for the fixture");
    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    if (!codec) throw std::bad_alloc();
    codec->width = 160;
    codec->height = 120;
    codec->pix_fmt = encoder->id == AV_CODEC_ID_MJPEG ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    codec->time_base = {1, kFrameRate};
    codec->framerate = {kFrameRate, 1};
    codec->gop_size = kFrameRate;  // a keyframe a second, so a seek lands near
    codec->max_b_frames = 0;
    codec->bit_rate = 200000;
    // Said outright, though the size would suggest BT.601: what the stream
    // says is what a subtitle's colours are corrected to.
    codec->colorspace = AVCOL_SPC_BT709;
    if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(codec, encoder, nullptr), "open video encoder");
    AVStream* video = avformat_new_stream(format, nullptr);
    if (!video) throw std::bad_alloc();
    video->time_base = codec->time_base;
    check(avcodec_parameters_from_context(video->codecpar, codec), "copy video parameters");

    const std::string assHeader =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 160\nPlayResY: 120\n\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
        "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
        "MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n"
        "Style: Sign,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,8,10,10,10,1\n\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    AVStream* srt = addSubtitleStream(format, AV_CODEC_ID_SUBRIP, "chi", "SC", true);
    AVStream* ass = addSubtitleStream(format, AV_CODEC_ID_ASS, "eng", "Styled", false, assHeader);
    AVStream* pictures = addSubtitleStream(format, AV_CODEC_ID_HDMV_PGS_SUBTITLE, "jpn", nullptr, false);
    // What a fansub release attaches: the faces its script names, and a
    // cover picture that is not a font.
    const auto attach = [&](AVCodecID id, const char* name, const char* type, const std::string& bytes) {
        AVStream* stream = avformat_new_stream(format, nullptr);
        if (!stream) throw std::bad_alloc();
        stream->codecpar->codec_type = AVMEDIA_TYPE_ATTACHMENT;
        stream->codecpar->codec_id = id;
        av_dict_set(&stream->metadata, "filename", name, 0);
        av_dict_set(&stream->metadata, "mimetype", type, 0);
        stream->codecpar->extradata = static_cast<std::uint8_t*>(av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!stream->codecpar->extradata) throw std::bad_alloc();
        std::memcpy(stream->codecpar->extradata, bytes.data(), bytes.size());
        stream->codecpar->extradata_size = static_cast<int>(bytes.size());
    };
    attach(AV_CODEC_ID_TTF, "Test Face.ttf", "application/x-truetype-font", kFontBytes);
    attach(AV_CODEC_ID_NONE, "cover.jpg", "image/jpeg", "not a font");

    check(avio_open(&format->pb, file.c_str(), AVIO_FLAG_WRITE), "open fixture");
    check(avformat_write_header(format, nullptr), "write header");

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) throw std::bad_alloc();
    const std::vector<Line> lines{
        {srt->index, 200, 800, "First"},
        {srt->index, 2000, 1000, "<i>Second</i>"},
        {srt->index, 2000, 1000, "{\\an8}Up top"},
        {srt->index, 5300, 2500, "Carried over"},
        {srt->index, 8000, 1000, "Late"},
        {ass->index, 2000, 1000, "0,0,Sign,,0,0,0,,Sign"},
        {ass->index, 2000, 1000, "1,0,Default,,0,0,0,,Speech {\\i1}two{\\i0}"},
        {ass->index, 2000, 1000, "2,1,Default,,0,0,0,,Speech two"},
        // The same line written twice on purpose, to draw it stronger.
        {ass->index, 2000, 1000, "3,1,Default,,0,0,0,,Speech two"},
    };
    for (const auto& line : lines) {
        check(av_new_packet(packet, static_cast<int>(line.text.size())), "allocate subtitle packet");
        std::memcpy(packet->data, line.text.data(), line.text.size());
        packet->stream_index = line.stream;
        packet->pts = packet->dts = line.startMs;
        packet->duration = line.durationMs;
        av_packet_rescale_ts(packet, AVRational{1, 1000}, format->streams[line.stream]->time_base);
        check(av_interleaved_write_frame(format, packet), "write subtitle packet");
    }
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    check(av_frame_get_buffer(frame, 0), "allocate frame");
    const auto drain = [&] {
        int result;
        while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, video->time_base);
            packet->stream_index = video->index;
            check(av_interleaved_write_frame(format, packet), "write video packet");
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "receive video packet");
    };
    for (int index = 0; index < kFrames; ++index) {
        check(av_frame_make_writable(frame), "writable frame");
        std::memset(frame->data[0], 16 + index % 200, static_cast<std::size_t>(frame->linesize[0]) * frame->height);
        std::memset(frame->data[1], 128, static_cast<std::size_t>(frame->linesize[1]) * frame->height / 2);
        std::memset(frame->data[2], 128, static_cast<std::size_t>(frame->linesize[2]) * frame->height / 2);
        frame->pts = index;
        check(avcodec_send_frame(codec, frame), "send frame");
        drain();
    }
    check(avcodec_send_frame(codec, nullptr), "flush video encoder");
    drain();
    check(av_write_trailer(format), "write trailer");
    const std::vector<int> streams{video->index, srt->index, ass->index, pictures->index};
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avio_closep(&format->pb);
    avformat_free_context(format);
    return streams;
}

template <typename Done>
void waitFor(Done&& done, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(std::string("Timed out: ") + what);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Shows the frames from `from` to `to` as a player would, which is what
// lets the worker read on: it stops when its queue is full.
void play(VideoSource& source, double from, double to) {
    for (double time = from; time <= to; time += 1.0 / kFrameRate) {
        waitFor([&] {
            const auto frame = source.frameForTime(time);
            return (frame && frame->pts >= time - 1.5 / kFrameRate) || source.eof() || !source.error().empty();
        }, "a frame to show");
    }
    require(source.error().empty(), "The source failed while playing");
}

void run(const std::filesystem::path& path, const std::vector<int>& streams) {
    const int srt = streams[1], ass = streams[2], pictures = streams[3];
    VideoSource source;
    SourceOptions options;
    options.readSubtitles = true;
    std::vector<SubtitleTrackInfo> offered;
    options.chooseSubtitle = [&offered, srt](const std::vector<SubtitleTrackInfo>& tracks) {
        offered = tracks;
        return srt;
    };
    SubtitleStreamTestAccess::start(source, path, options);
    waitFor([&] { return source.ready() || !source.error().empty(); }, "the source to open");
    require(source.error().empty(), "The fixture did not open");

    // What the menu lists: every subtitle stream, with what tells them
    // apart, and which of them is text.
    const auto tracks = source.subtitleTracks();
    require(tracks.size() == 3 && offered.size() == 3, "The subtitle streams were not listed");
    require(tracks[0].streamIndex == srt && tracks[0].text && tracks[0].language == "chi" &&
            tracks[0].title == "SC" && tracks[0].isDefault, "The SubRip stream is described wrongly");
    require(tracks[1].streamIndex == ass && tracks[1].text && tracks[1].language == "eng" &&
            tracks[1].title == "Styled" && !tracks[1].isDefault, "The ASS stream is described wrongly");
    require(tracks[2].streamIndex == pictures && !tracks[2].text, "The picture stream is described wrongly");
    require(source.subtitleTrack() == srt, "The stream chosen at the open was not taken");

    // The faces the release attached are handed over for libass, the cover
    // is not; the video's matrix is what its stream says.
    const auto fonts = source.fonts();
    require(fonts && fonts->size() == 1 && fonts->front().name == "Test Face.ttf" &&
            fonts->front().data == kFontBytes, "The attached font was not handed over as it was");
    require(source.colourMatrix() == SubtitleMatrix::Bt709Tv, "The video's matrix was not read from its stream");

    // libass's view of the streams: the ASS stream's own header, a plain
    // stand-in for the SubRip one.
    std::string header;
    bool plain = false;
    require(source.subtitleStreamHeader(ass, header, plain) && !plain &&
            header.find("Style: Sign,Arial,16") != std::string::npos, "The ASS stream's header was not kept");
    require(source.subtitleStreamHeader(srt, header, plain) && plain, "The SubRip stream was not taken as plain");
    require(!source.subtitleStreamHeader(pictures, header, plain), "A stream of pictures has a header for libass");

    // The lines come as the video is read: those played past are there,
    // those further on are not yet.
    source.requestSeek(0.0);
    play(source, 0.0, 3.5);
    waitFor([&] { return source.subtitleCueCount() >= 3; }, "the first lines");
    require(source.subtitleLinesAt(0.5).bottom == L"First" && source.subtitleLinesAt(0.1).empty() &&
            source.subtitleLinesAt(1.0).empty(), "The first line is wrong or wrongly timed");
    auto lines = source.subtitleLinesAt(2.5);
    require(lines.bottom == L"Second" && lines.top == L"Up top", "Markup or placement of a SubRip line is wrong");
    require(source.subtitleLinesAt(1.999).empty() && !source.subtitleLinesAt(2.0).empty() &&
            !source.subtitleLinesAt(2.999).empty() && source.subtitleLinesAt(3.0).empty(),
            "A line's times are not the container's");
    require(source.subtitleCueCount() == 3 && source.subtitleLinesAt(6.0).empty() &&
            source.subtitleLinesAt(8.5).empty(), "A line was read before the video reached it");
    // The same lines as events for libass, in the order they were read:
    // the SubRip ones under read orders of the source's own, its markup as
    // ASS overrides; the ASS ones as they were written, layered copies too.
    std::vector<SubtitleEvent> events;
    require(source.subtitleEventsSince(srt, 0, events) == 3 && events.size() == 3, "The SubRip events are missing");
    require(events[0].chunk.rfind("0,", 0) == 0 && events[0].chunk.find("First") != std::string::npos &&
            events[0].startMs == 200 && events[0].durationMs == 800, "The first SubRip event is wrong");
    require(events[1].chunk.rfind("1,", 0) == 0 && events[1].chunk.find("{\\i1}Second") != std::string::npos,
            "A SubRip event lost its markup or its read order");
    events.clear();
    require(source.subtitleEventsSince(srt, 2, events) == 3 && events.size() == 1, "Events were handed over twice");
    events.clear();
    require(source.subtitleEventsSince(ass, 0, events) == 4 && events[0].chunk == "0,0,Sign,,0,0,0,,Sign" &&
            events[2].chunk == "2,1,Default,,0,0,0,,Speech two" && events[0].startMs == 2000 &&
            events[0].durationMs == 1000, "The ASS events did not come as they were written");
    require(events[3].chunk == "3,1,Default,,0,0,0,,Speech two", "A line written twice on purpose was kept once");

    // A seek into the middle of a line shows it: the line began two
    // keyframes before the one the seek lands on, and the demuxer starts
    // reading early enough to pass it. What was read before is kept.
    source.requestSeek(7.5);
    play(source, 7.5, 8.6);
    waitFor([&] { return source.subtitleCueCount() >= 5; }, "the lines around the seek");
    require(source.subtitleLinesAt(7.5).bottom == L"Carried over", "The line being spoken where a seek landed is missing");
    require(source.subtitleLinesAt(8.5).bottom == L"Late" && source.subtitleLinesAt(0.5).bottom == L"First",
            "A seek lost lines or did not read the new ones");
    // The same after a seek that lands on a keyframe instead of a frame.
    source.requestKeyframeSeek(7.6, 0);
    waitFor([&] { return source.takeSeekLanding().has_value(); }, "the keyframe seek to land");
    require(source.subtitleLinesAt(7.5).bottom == L"Carried over", "A keyframe seek lost the line");
    // A seek back passes the same packets again: no line is held twice.
    source.requestSeek(0.0);
    play(source, 0.0, 3.0);
    require(source.subtitleCueCount() == 5, "Lines read a second time were added a second time");
    events.clear();
    require(source.subtitleEventsSince(srt, 0, events) == 5 && source.subtitleEventsSince(ass, 0, events) == 4,
            "Events read a second time were added a second time");

    // Another stream: it was read alongside, so its lines are there at once.
    // Layered copies of a line are one line.
    source.setSubtitleTrack(ass);
    lines = source.subtitleLinesAt(2.5);
    require(lines.top == L"Sign" && lines.bottom == L"Speech two", "An ASS line's style or markup was not followed");
    require(source.subtitleCueCount() == 2 && source.subtitleLinesAt(0.5).empty(),
            "A line layered twice was held twice, or the other stream's lines are shown");
    source.setSubtitleTrack(srt);
    require(source.subtitleLinesAt(0.5).bottom == L"First", "Coming back to a stream lost its lines");

    // A stream of pictures is not read, and no stream shows nothing.
    source.setSubtitleTrack(pictures);
    require(source.subtitleTrack() == pictures && source.subtitleCueCount() == 0 &&
            source.subtitleLinesAt(2.5).empty(), "A picture stream produced lines");
    source.setSubtitleTrack(-1);
    require(source.subtitleLinesAt(2.5).empty(), "No stream still shows lines");
    source.close();
}

// A source not asked to read subtitles lists them and reads none, and a
// stream named before the open -- or none at all -- stands against the
// chooser.
void runChoices(const std::filesystem::path& path, const std::vector<int>& streams) {
    {
        VideoSource source;
        SourceOptions options;
        options.chooseSubtitle = [&](const std::vector<SubtitleTrackInfo>&) { return streams[1]; };
        SubtitleStreamTestAccess::start(source, path, options);
        waitFor([&] { return source.ready() || !source.error().empty(); }, "the source to open");
        source.requestSeek(0.0);
        play(source, 0.0, 3.0);
        require(source.subtitleTrack() == streams[1] && source.subtitleCueCount() == 0 &&
                source.subtitleLinesAt(2.5).empty() && source.subtitleTracks().size() == 3,
                "A source not asked to read subtitles read them");
        source.close();
    }
    {
        VideoSource source;
        SourceOptions options;
        options.readSubtitles = true;
        options.chooseSubtitle = [&](const std::vector<SubtitleTrackInfo>&) { return streams[1]; };
        source.setSubtitleTrack(-1);
        SubtitleStreamTestAccess::start(source, path, options);
        waitFor([&] { return source.ready() || !source.error().empty(); }, "the source to open");
        source.requestSeek(0.0);
        play(source, 0.0, 3.0);
        require(source.subtitleTrack() == -1 && source.subtitleLinesAt(2.5).empty(),
                "The open chose over the owner's own choice of none");
        source.setSubtitleTrack(streams[1]);
        require(source.subtitleLinesAt(2.5).bottom == L"Second", "A stream chosen later has no lines");
        source.close();
    }
}

}  // namespace

int main() {
    // The stream of pictures has no packet to be measured by; FFmpeg says
    // so at every open, which is this fixture and not a finding.
    av_log_set_level(AV_LOG_ERROR);
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("QuadDeck-subtitle-streams-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const auto path = directory / "subtitled.mkv";
        const auto streams = makeFixture(path);
        run(path, streams);
        runChoices(path, streams);
        std::error_code code;
        std::filesystem::remove_all(directory, code);
        std::cout << "QuadDeck subtitle stream tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
