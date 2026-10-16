#pragma once

// Subtitles drawn as their script is written, by libass: the faces it names
// and those the video brought, colours and transparency, outlines, shadows
// and boxes, positions and movement, rotation, scaling, blur, fades, clips,
// drawings and karaoke. One AssSubtitles belongs to one pane and to one
// video's media: a libass library holding that video's fonts, a renderer
// sized to the pane, and the script shown. It is used on the window thread
// only.
//
// What it hands back is a picture -- the lines of one moment composited into
// premultiplied BGRA, in pieces that cover only the parts of the pane that
// have anything on them -- which the Overlay draws over the video like the
// rest of the interface.

#include "Core.hpp"
#include "Subtitles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ass_library;
struct ass_renderer;
struct ass_track;

namespace quaddeck {

// One part of a moment's subtitles: the lines of one place on the pane.
struct SubtitlePiece {
    // Within the frame the lines were drawn for, in pixels.
    int x{};
    int y{};
    int width{};
    int height{};
    std::vector<std::uint32_t> pixels;  // premultiplied BGRA, row after row
};

// One moment's subtitles: a piece for each place that has lines, so that a
// sign in one corner and the speech at the bottom are two small pictures,
// not one the size of the pane to clear, fill and hand to the GPU.
struct SubtitleBitmap {
    std::vector<SubtitlePiece> pieces;
};

// The pane libass draws into, and where in it the picture lies.
struct SubtitleFrame {
    int width{};
    int height{};
    // From each edge of the picture to the pane's, in pixels: positive where
    // the pane shows bars beside the picture, negative where it crops it.
    int top{};
    int bottom{};
    int left{};
    int right{};
    // The video's own size. A script's blur and its 3D rotation are measured
    // against it, and a stretched picture's pixels are not square.
    int videoWidth{};
    int videoHeight{};
    friend bool operator==(const SubtitleFrame&, const SubtitleFrame&) = default;
};

// The frame a pane's subtitles are drawn into, and its top-left corner in
// the window. A plain script's frame is the whole pane with no picture in it:
// its lines are sized and placed against the pane, bars included, as the
// plain lines always were (and as mpv sizes them by its window), whatever
// the picture's shape. A script's frame is the picture as it is on screen
// (`visible`), so that nothing it draws lands on Fit's bars; the whole
// picture (`picture`, grown by the zoom or by Fill's crop) is given against
// it as margins, so its coordinates are the picture's.
struct SubtitlePlacement {
    SubtitleFrame frame;
    int originX{};
    int originY{};
};

inline SubtitlePlacement subtitlePlacement(const RectF& cell, const RectF& visible, const RectF& picture,
                                           int videoWidth, int videoHeight, bool plain) {
    const RectF& area = plain ? cell : visible;
    const auto at = [](float value) { return static_cast<int>(std::lround(value)); };
    SubtitlePlacement placement;
    placement.originX = at(area.x);
    placement.originY = at(area.y);
    const int right = at(area.x + area.width);
    const int bottom = at(area.y + area.height);
    placement.frame.width = std::max(0, right - placement.originX);
    placement.frame.height = std::max(0, bottom - placement.originY);
    placement.frame.videoWidth = videoWidth;
    placement.frame.videoHeight = videoHeight;
    if (plain) return placement;
    placement.frame.left = at(picture.x) - placement.originX;
    placement.frame.top = at(picture.y) - placement.originY;
    placement.frame.right = right - at(picture.x + picture.width);
    placement.frame.bottom = bottom - at(picture.y + picture.height);
    return placement;
}

// What the viewer and the player make of the script.
struct SubtitleLook {
    // The size against what the script asks for. Lines the script places
    // itself (\pos, \move, a clip) keep theirs.
    double fontScale{1.0};
    // How far the bottom lines the script does not place are raised, from 0
    // where their style puts them to 100 at the top (libass's line position).
    double raise{};
    // The matrix the video is decoded with: a script's colours are corrected
    // to it (subtitleColourForVideo).
    SubtitleMatrix videoMatrix{SubtitleMatrix::None};
    friend bool operator==(const SubtitleLook&, const SubtitleLook&) = default;
};

class AssSubtitles {
public:
    // Null when libass cannot be set up.
    static std::unique_ptr<AssSubtitles> create();
    ~AssSubtitles();
    AssSubtitles(const AssSubtitles&) = delete;
    AssSubtitles& operator=(const AssSubtitles&) = delete;

    // The face drawn where a script names one this machine has not got.
    void setFallbackFont(const std::string& family);
    // Fonts the video brought with it; libass takes them up on the next
    // render. They stay for as long as this object.
    void addFonts(const std::vector<SubtitleFont>& fonts);
    // A whole script in place of what was shown. `plain` for one made from
    // an SRT or WebVTT: its lines may stand in the bars below the picture,
    // and are not stretched with a stretched picture. False, and nothing
    // shown, when libass finds no event in it.
    bool loadScript(std::string script, bool plain);
    // A stream's header in place of what was shown; its events follow, one
    // by one, as the video is read.
    bool startStream(const std::string& header, bool plain);
    void addEvent(const SubtitleEvent& event);
    void clear();
    bool loaded() const { return track_ != nullptr; }
    int eventCount() const;

    // The lines on screen at `milliseconds` of the video: the same picture
    // as last time while nothing about it changed, null when nothing is on
    // screen.
    std::shared_ptr<const SubtitleBitmap> render(const SubtitleFrame& frame, const SubtitleLook& look,
                                                 long long milliseconds);

private:
    struct PieceRect;
    AssSubtitles() = default;
    void setPlain(bool plain);

    ass_library* library_{};
    ass_renderer* renderer_{};
    ass_track* track_{};
    bool plain_{};
    std::string fallbackFont_;
    SubtitleMatrix scriptMatrix_{SubtitleMatrix::None};
    // What the renderer was last set to; a change empties libass's caches.
    SubtitleFrame frame_{};
    SubtitleLook look_{};
    bool configured_{};
    // Something about the picture changed that libass does not report: the
    // script, the fonts, the colour correction.
    bool dirty_{true};
    std::shared_ptr<const SubtitleBitmap> last_;
};

}  // namespace quaddeck
