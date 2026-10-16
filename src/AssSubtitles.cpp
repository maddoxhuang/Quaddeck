#include "AssSubtitles.hpp"

#include "Diagnostics.hpp"

#include <ass/ass.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>

namespace quaddeck {

namespace {

// libass speaks of every face it falls back from, and a script that names
// faces this machine lacks would fill the log with it: errors and warnings
// only, and only so many in a run.
constexpr int kLoggedMessageLimit = 40;
std::atomic<int> loggedMessages{0};

void logLibassMessage(int level, const char* format, va_list arguments, void*) {
    if (level > 2) return;
    if (loggedMessages.fetch_add(1) >= kLoggedMessageLimit) return;
    char text[512]{};
    std::vsnprintf(text, sizeof(text), format, arguments);
    appendDiagnostic(std::string("libass: ") + text);
}

// The matrix a script's colours were picked against. A script that does not
// say was coloured in VSFilter, which took every video for BT.601; one that
// says something unreadable is left as it is.
SubtitleMatrix scriptMatrix(int value) {
    switch (value) {
    case YCBCR_NONE:
    case YCBCR_UNKNOWN: return SubtitleMatrix::None;
    case YCBCR_BT601_PC: return SubtitleMatrix::Bt601Pc;
    case YCBCR_BT709_TV: return SubtitleMatrix::Bt709Tv;
    case YCBCR_BT709_PC: return SubtitleMatrix::Bt709Pc;
    case YCBCR_SMPTE240M_TV: return SubtitleMatrix::Smpte240mTv;
    case YCBCR_SMPTE240M_PC: return SubtitleMatrix::Smpte240mPc;
    case YCBCR_FCC_TV: return SubtitleMatrix::FccTv;
    case YCBCR_FCC_PC: return SubtitleMatrix::FccPc;
    default: return SubtitleMatrix::Bt601Tv;
    }
}

// Glyph caches for one pane: libass's defaults are sized for one video on
// screen, and there may be five.
constexpr int kCacheMegabytes = 64;

// value / 255, rounded, for value up to 255 * 255: what blending divides by
// for every pixel, without the division.
inline std::uint32_t divide255(std::uint32_t value) {
    value += 128U;
    return (value + (value >> 8)) >> 8;
}

// One coverage mask in one colour laid over premultiplied BGRA.
void blendCoverage(const unsigned char* coverage, int stride, int width, int height, std::uint32_t rgb,
                   std::uint32_t opacity, std::uint32_t* out, int outStride) {
    const std::uint32_t red = (rgb >> 16) & 0xFFU;
    const std::uint32_t green = (rgb >> 8) & 0xFFU;
    const std::uint32_t blue = rgb & 0xFFU;
    const std::uint32_t solid = 0xFF000000U | (red << 16) | (green << 8) | blue;
    for (int row = 0; row < height; ++row) {
        const unsigned char* mask = coverage + static_cast<std::ptrdiff_t>(row) * stride;
        std::uint32_t* line = out + static_cast<std::ptrdiff_t>(row) * outStride;
        for (int column = 0; column < width; ++column) {
            const std::uint32_t cover = mask[column];
            if (cover == 0) continue;
            const std::uint32_t alpha = opacity == 255U ? cover : divide255(cover * opacity);
            if (alpha == 255U) {
                line[column] = solid;
                continue;
            }
            if (alpha == 0) continue;
            const std::uint32_t keep = 255U - alpha;
            const std::uint32_t under = line[column];
            const std::uint32_t outB = divide255(blue * alpha + (under & 0xFFU) * keep);
            const std::uint32_t outG = divide255(green * alpha + ((under >> 8) & 0xFFU) * keep);
            const std::uint32_t outR = divide255(red * alpha + ((under >> 16) & 0xFFU) * keep);
            const std::uint32_t outA = alpha + divide255((under >> 24) * keep);
            line[column] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
        }
    }
}

}  // namespace

// A place on the pane that has lines, in frame pixels.
struct AssSubtitles::PieceRect {
    int left{};
    int top{};
    int right{};
    int bottom{};
    bool meets(const PieceRect& other) const {
        return left < other.right && other.left < right && top < other.bottom && other.top < bottom;
    }
    bool holds(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
    PieceRect joined(const PieceRect& other) const {
        return {std::min(left, other.left), std::min(top, other.top), std::max(right, other.right),
                std::max(bottom, other.bottom)};
    }
};

std::unique_ptr<AssSubtitles> AssSubtitles::create() {
    std::unique_ptr<AssSubtitles> made(new AssSubtitles());
    made->library_ = ass_library_init();
    if (!made->library_) return nullptr;
    ass_set_message_cb(made->library_, logLibassMessage, nullptr);
    // A script may carry its faces in a [Fonts] section.
    ass_set_extract_fonts(made->library_, 1);
    made->renderer_ = ass_renderer_init(made->library_);
    if (!made->renderer_) return nullptr;
    ass_set_cache_limits(made->renderer_, 0, kCacheMegabytes);
    // The viewer's size applies to speech, not to a sign the script placed
    // on a wall in the picture.
    ass_set_selective_style_override_enabled(made->renderer_, ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);
    made->setFallbackFont("Segoe UI");
    return made;
}

AssSubtitles::~AssSubtitles() {
    clear();
    if (renderer_) ass_renderer_done(renderer_);
    if (library_) ass_library_done(library_);
}

void AssSubtitles::setFallbackFont(const std::string& family) {
    if (!renderer_ || (!fallbackFont_.empty() && family == fallbackFont_)) return;
    fallbackFont_ = family;
    // DirectWrite on Windows: the system's faces, found as they are asked for.
    ass_set_fonts(renderer_, nullptr, family.c_str(), ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    dirty_ = true;
}

void AssSubtitles::addFonts(const std::vector<SubtitleFont>& fonts) {
    if (!library_ || !renderer_) return;
    bool added = false;
    for (const auto& font : fonts) {
        if (font.data.empty() || font.data.size() > static_cast<std::size_t>(INT_MAX)) continue;
        ass_add_font(library_, font.name.c_str(), font.data.data(), static_cast<int>(font.data.size()));
        added = true;
    }
    if (!added) return;
    // libass takes up a font added after it has drawn, but keeps the face
    // it already chose for a name the new font answers to -- the stand-in,
    // when the video's own fonts arrive after the script's first line. Its
    // font lookup is begun again.
    ass_set_fonts(renderer_, nullptr, fallbackFont_.c_str(), ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    dirty_ = true;
}

void AssSubtitles::setPlain(bool plain) {
    plain_ = plain;
    scriptMatrix_ = track_ ? scriptMatrix(track_->YCbCrMatrix) : SubtitleMatrix::None;
    // The margins and the storage size depend on it: set again at the next
    // render.
    configured_ = false;
    dirty_ = true;
}

bool AssSubtitles::loadScript(std::string script, bool plain) {
    clear();
    if (!library_ || script.empty()) return false;
    track_ = ass_read_memory(library_, script.data(), script.size(), nullptr);
    if (!track_ || track_->n_events <= 0) {
        clear();
        return false;
    }
    setPlain(plain);
    return true;
}

bool AssSubtitles::startStream(const std::string& header, bool plain) {
    clear();
    if (!library_) return false;
    track_ = ass_new_track(library_);
    if (!track_) return false;
    if (!header.empty() && header.size() < static_cast<std::size_t>(INT_MAX)) {
        ass_process_codec_private(track_, header.data(), static_cast<int>(header.size()));
    }
    setPlain(plain);
    return true;
}

void AssSubtitles::addEvent(const SubtitleEvent& event) {
    if (!track_ || event.chunk.empty() || event.chunk.size() >= static_cast<std::size_t>(INT_MAX)) return;
    ass_process_chunk(track_, event.chunk.data(), static_cast<int>(event.chunk.size()), event.startMs,
                      event.durationMs);
    dirty_ = true;
}

void AssSubtitles::clear() {
    if (track_) ass_free_track(track_);
    track_ = nullptr;
    last_.reset();
    dirty_ = true;
}

int AssSubtitles::eventCount() const {
    return track_ ? track_->n_events : 0;
}

std::shared_ptr<const SubtitleBitmap> AssSubtitles::render(const SubtitleFrame& frame, const SubtitleLook& look,
                                                           long long milliseconds) {
    if (!renderer_ || !track_ || frame.width <= 0 || frame.height <= 0) return nullptr;
    // Each change of these empties libass's caches; they change when the
    // pane is resized, not every frame.
    if (!configured_ || !(frame == frame_)) {
        ass_set_frame_size(renderer_, frame.width, frame.height);
        ass_set_margins(renderer_, frame.top, frame.bottom, frame.left, frame.right);
        // A plain subtitle may sit in the bar below a wide picture, as it
        // did before libass drew it; a script's speech stays on the picture
        // it was placed on -- unless the picture is cropped (Fill, a zoom),
        // where speech placed against the whole picture would fall off the
        // pane: then it is laid out in the pane, and only what the script
        // places itself goes with the picture.
        const bool cropped = frame.top < 0 || frame.bottom < 0 || frame.left < 0 || frame.right < 0;
        ass_set_use_margins(renderer_, plain_ || cropped ? 1 : 0);
        // Without a storage size libass takes pixels as square: a plain
        // subtitle is not stretched with a stretched picture. A script's
        // lines are part of the picture and are.
        ass_set_storage_size(renderer_, plain_ ? 0 : std::max(0, frame.videoWidth),
                             plain_ ? 0 : std::max(0, frame.videoHeight));
        frame_ = frame;
        dirty_ = true;
    }
    if (!configured_ || look.fontScale != look_.fontScale) ass_set_font_scale(renderer_, look.fontScale);
    if (!configured_ || look.raise != look_.raise) {
        ass_set_line_position(renderer_, std::clamp(look.raise, 0.0, 100.0));
    }
    if (!configured_ || !(look == look_)) dirty_ = true;
    look_ = look;
    configured_ = true;

    int change = 0;
    const ASS_Image* images = ass_render_frame(renderer_, track_, milliseconds, &change);
    if (!images) {
        last_.reset();
        return nullptr;
    }
    if (change == 0 && !dirty_ && last_) return last_;
    dirty_ = false;

    // The places that have lines: the images' rectangles, merged where they
    // meet, so that every image falls inside exactly one piece and pieces
    // never overlap.
    std::vector<PieceRect> places;
    for (const ASS_Image* image = images; image; image = image->next) {
        if (image->w <= 0 || image->h <= 0) continue;
        PieceRect merged{image->dst_x, image->dst_y, image->dst_x + image->w, image->dst_y + image->h};
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t index = 0; index < places.size();) {
                if (places[index].meets(merged)) {
                    merged = merged.joined(places[index]);
                    places.erase(places.begin() + static_cast<std::ptrdiff_t>(index));
                    grew = true;
                } else {
                    ++index;
                }
            }
        }
        places.push_back(merged);
    }
    if (places.empty()) {
        last_.reset();
        return nullptr;
    }
    auto bitmap = std::make_shared<SubtitleBitmap>();
    bitmap->pieces.resize(places.size());
    for (std::size_t index = 0; index < places.size(); ++index) {
        SubtitlePiece& piece = bitmap->pieces[index];
        piece.x = places[index].left;
        piece.y = places[index].top;
        piece.width = places[index].right - places[index].left;
        piece.height = places[index].bottom - places[index].top;
        piece.pixels.assign(static_cast<std::size_t>(piece.width) * static_cast<std::size_t>(piece.height), 0U);
    }

    // libass's images are coverage masks in one colour each, to be laid
    // over one another in the order they come: fill over outline over
    // shadow, layer over layer.
    std::uint32_t lastColour = 0xFFFFFFFFU;
    std::uint32_t lastShown = 0;
    for (const ASS_Image* image = images; image; image = image->next) {
        if (image->w <= 0 || image->h <= 0) continue;
        const std::uint32_t opacity = 255U - (image->color & 0xFFU);
        if (opacity == 0) continue;
        const std::uint32_t colour = image->color >> 8;
        if (colour != lastColour) {
            lastColour = colour;
            lastShown = subtitleColourForVideo(colour, scriptMatrix_, look.videoMatrix);
        }
        std::size_t place = 0;
        while (place + 1 < places.size() && !places[place].holds(image->dst_x, image->dst_y)) ++place;
        SubtitlePiece& piece = bitmap->pieces[place];
        blendCoverage(image->bitmap, image->stride, image->w, image->h, lastShown, opacity,
                      piece.pixels.data() + static_cast<std::size_t>(image->dst_y - piece.y) * piece.width +
                          static_cast<std::size_t>(image->dst_x - piece.x),
                      piece.width);
    }
    last_ = std::move(bitmap);
    return last_;
}

}  // namespace quaddeck
