// libass, as AssSubtitles drives it, against scripts written for the test:
// where a line lands on the pane, what the viewer's size and raising do and
// do not touch, a plain subtitle standing in the bar below a picture, the
// colours of the picture handed back, the events of a stream added one by
// one. The faces are the system's (DirectWrite), so this runs on Windows.

#include "AssSubtitles.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <windows.h>

using namespace quaddeck;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

struct Box {
    int left{};
    int top{};
    int right{};
    int bottom{};
    double centreX() const { return (left + right) * 0.5; }
    double centreY() const { return (top + bottom) * 0.5; }
    int height() const { return bottom - top; }
};

// Every pixel of every piece, at its frame position.
template <typename Take>
void forEachPixel(const SubtitleBitmap& bitmap, Take&& take) {
    for (const auto& piece : bitmap.pieces) {
        require(piece.pixels.size() == static_cast<std::size_t>(piece.width) * piece.height, "a piece of the wrong size");
        for (int y = 0; y < piece.height; ++y) {
            for (int x = 0; x < piece.width; ++x) {
                take(piece.x + x, piece.y + y, piece.pixels[static_cast<std::size_t>(y) * piece.width + x]);
            }
        }
    }
}

// Where the picture has anything on it, in frame pixels.
Box inkBox(const SubtitleBitmap& bitmap) {
    Box box{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    forEachPixel(bitmap, [&](int x, int y, std::uint32_t pixel) {
        if ((pixel >> 24) == 0) return;
        box.left = std::min(box.left, x);
        box.top = std::min(box.top, y);
        box.right = std::max(box.right, x + 1);
        box.bottom = std::max(box.bottom, y + 1);
    });
    require(box.right > box.left, "a picture with nothing on it");
    return box;
}

// The same of what a render handed back, which must be something.
Box inkBox(const std::shared_ptr<const SubtitleBitmap>& picture) {
    require(picture != nullptr, "nothing was drawn where something should be");
    return inkBox(*picture);
}

std::string script(const std::string& events, const std::string& infoExtra = {}) {
    return "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\nScaledBorderAndShadow: yes\n"
           "YCbCr Matrix: None\n" + infoExtra + "\n"
           "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
           "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, "
           "Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
           "Style: Default,Arial,60,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,2,"
           "20,20,40,1\n\n"
           "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" + events;
}

SubtitleFrame fullFrame(int width, int height) {
    SubtitleFrame frame;
    frame.width = width;
    frame.height = height;
    frame.videoWidth = 1920;
    frame.videoHeight = 1080;
    return frame;
}

void placementFollowsTheScript() {
    auto ass = AssSubtitles::create();
    require(ass != nullptr, "libass could not be set up");
    require(ass->loadScript(script(
                "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(480,270)}Sign\n"
                "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an8}Top line\n"
                "Dialogue: 0,0:00:06.00,0:00:08.00,Default,,0,0,0,,Speech at the bottom\n"),
                            false),
            "the script was not read");
    require(ass->eventCount() == 3, "not every event was read");
    const SubtitleFrame frame = fullFrame(1920, 1080);
    require(ass->render(frame, {}, 500) == nullptr, "a line shown before its time");

    // \pos with \an5 puts the middle of the sign on the point, in script
    // pixels that are the frame's here.
    {
        auto two = AssSubtitles::create();
        two->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(480,270)}Sign\n"),
                        false);
        const auto picture = two->render(frame, {}, 2000);
        require(picture != nullptr, "the positioned sign is not drawn");
        const Box box = inkBox(picture);
        require(std::abs(box.centreX() - 480.0) < 8.0 && std::abs(box.centreY() - 270.0) < 12.0,
                "\\pos did not put the sign where the script says");
    }
    // \an8 at the top, the speech at the bottom above MarginV.
    const auto both = ass->render(frame, {}, 2000);
    require(both != nullptr && inkBox(both).top < 100, "\\an8 did not put the line at the top");
    const auto speech = ass->render(frame, {}, 7000);
    require(speech != nullptr, "the speech is not drawn");
    const Box low = inkBox(speech);
    require(low.bottom > 1080 - 40 - 30 && low.bottom <= 1080 - 40 + 4,
            "the bottom line does not stand on the style's margin");
    require(ass->render(frame, {}, 7016) == speech, "an unchanged moment was composited again");
    require(ass->render(frame, {}, 9000) == nullptr, "a line shown after its time");
}

void theViewerSizesAndRaisesSpeechOnly() {
    auto ass = AssSubtitles::create();
    require(ass->loadScript(script(
                "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,Speech\n"
                "Dialogue: 0,0:00:06.00,0:00:08.00,Default,,0,0,0,,{\\an5\\pos(960,300)}Sign\n"),
                            false),
            "the script was not read");
    const SubtitleFrame frame = fullFrame(1920, 1080);
    const Box normal = inkBox(ass->render(frame, {}, 2000));
    SubtitleLook bigger;
    bigger.fontScale = 2.0;
    const Box large = inkBox(ass->render(frame, bigger, 2000));
    require(large.height() > normal.height() * 1.7, "the size did not reach the speech");
    const Box sign = inkBox(ass->render(frame, {}, 7000));
    const Box signLarge = inkBox(ass->render(frame, bigger, 7000));
    require(std::abs(signLarge.height() - sign.height()) <= 2, "the size reached a sign the script placed");

    SubtitleLook raised;
    raised.raise = 50.0;
    const Box up = inkBox(ass->render(frame, raised, 2000));
    require(normal.bottom - up.bottom > 400, "raising did not move the speech up");
    const Box signRaised = inkBox(ass->render(frame, raised, 7000));
    require(std::abs(signRaised.centreY() - sign.centreY()) < 2.0, "raising moved a sign the script placed");
}

void thePictureIsWhatTheScriptIsPlacedOn() {
    // A 16:9 picture in a 4:3 pane: 180 pixels of bar above and below.
    SubtitleFrame frame = fullFrame(1920, 1440);
    frame.top = 180;
    frame.bottom = 180;
    auto ass = AssSubtitles::create();
    ass->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(960,540)}Middle\n"
                           "Dialogue: 0,0:00:06.00,0:00:08.00,Default,,0,0,0,,Speech\n"),
                    false);
    const Box middle = inkBox(ass->render(frame, {}, 2000));
    require(std::abs(middle.centreY() - 720.0) < 12.0, "a position was measured against the pane, not the picture");
    const Box speech = inkBox(ass->render(frame, {}, 7000));
    require(speech.bottom <= 1440 - 180, "a script's speech went into the bar below the picture");

    // A plain subtitle stands in that bar, as it always did.
    auto plain = AssSubtitles::create();
    const std::string srt = "1\n00:00:01,000 --> 00:00:05,000\nPlain line\n";
    require(plain->loadScript(plainSubtitleScript(srt, {}), true), "the plain script was not read");
    const Box bar = inkBox(plain->render(frame, {}, 2000));
    require(bar.bottom > 1440 - 180, "a plain subtitle did not use the bar below the picture");

    // Cropped (Fill): the picture runs past the pane on both sides.
    SubtitleFrame cropped = fullFrame(1080, 1080);
    cropped.left = -420;
    cropped.right = -420;
    auto crop = AssSubtitles::create();
    crop->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(960,540)}Middle\n"),
                     false);
    const Box centre = inkBox(crop->render(cropped, {}, 2000));
    require(std::abs(centre.centreX() - 540.0) < 10.0, "a cropped picture's middle is not the pane's");
}

// The frame and margins the player hands libass (subtitlePlacement over
// pictureRect and visiblePictureRect): speech stays on the pane however the
// picture is cropped or zoomed, what the script places goes with the
// picture, nothing is drawn on Fit's bars, and plain text is not stretched.
void speechStaysOnThePane() {
    const std::string events =
        "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,Speech at the bottom\n"
        "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(960,270)}Sign\n";
    const auto place = [](const RectF& cell, int width, int height, ViewMode mode, float zoom, bool plain) {
        return subtitlePlacement(cell, visiblePictureRect(cell, width, height, mode),
                                 pictureRect(cell, width, height, mode, zoom), width, height, plain);
    };
    const auto speechAndSign = [&](const SubtitlePlacement& placement, Box& speech, Box& sign) {
        auto ass = AssSubtitles::create();
        ass->loadScript(script(events), false);
        auto two = AssSubtitles::create();
        two->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,Speech at the bottom\n"), false);
        speech = inkBox(two->render(placement.frame, {}, 2000));
        auto three = AssSubtitles::create();
        three->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(960,270)}Sign\n"),
                          false);
        sign = inkBox(three->render(placement.frame, {}, 2000));
        require(ass->render(placement.frame, {}, 2000) != nullptr, "the two lines together are not drawn");
    };
    const RectF wide{0, 0, 1920, 1080};
    Box speech, sign;

    // A 4:3 video filling a 16:9 pane: 180 pixels of picture cropped above
    // and below. The speech is whole on the pane; the sign is where the
    // picture puts it (270 of 1080 is 360 of the 1440 drawn, less 180).
    auto filled = place(wide, 1440, 1080, ViewMode::Fill, 1.0F, false);
    require(filled.frame.top == -180 && filled.frame.bottom == -180, "Fill's crop is not given as margins");
    speechAndSign(filled, speech, sign);
    require(speech.top >= 900 && speech.bottom <= 1080, "speech fell off a cropped pane");
    require(std::abs(sign.centreY() - 180.0) < 12.0, "a placed sign did not go with the cropped picture");

    // Zoomed twice: the speech stays at the pane's bottom, the sign moves
    // out with the picture (960,270 of the script is the pane's 960,0).
    auto zoomed = place(wide, 1920, 1080, ViewMode::Fit, 2.0F, false);
    speechAndSign(zoomed, speech, sign);
    require(speech.top >= 900 && speech.bottom <= 1080, "speech fell off a zoomed pane");
    require(sign.top <= 2 && std::abs(sign.centreX() - 960.0) < 12.0, "a placed sign did not move with the zoom");

    // A 16:9 video zoomed in a 4:3 pane: the frame is the picture on
    // screen, not the bars, so a sign the zoom pushes below the picture is
    // not drawn on the bar there.
    const RectF square{0, 0, 1920, 1440};
    auto barred = place(square, 1920, 1080, ViewMode::Fit, 1.5F, false);
    require(barred.originY == 180 && barred.frame.height == 1080, "a script's frame took in Fit's bars");
    auto low = AssSubtitles::create();
    low->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an5\\pos(960,1000)}Low sign\n"),
                    false);
    if (const auto picture = low->render(barred.frame, {}, 2000)) {
        require(inkBox(picture).bottom <= barred.frame.height, "a sign was drawn outside the picture on screen");
    }
    // A plain subtitle's frame is the whole pane, bars included.
    const auto plainBarred = place(square, 1920, 1080, ViewMode::Fit, 1.0F, true);
    require(plainBarred.originY == 0 && plainBarred.frame.height == 1440 && plainBarred.frame.bottom == 0,
            "a plain subtitle's frame is not the pane");

    // Stretched: a plain line keeps its proportions.
    const std::string srt = "1\n00:00:01,000 --> 00:00:05,000\nThe same plain line\n";
    const auto plainWidth = [&](ViewMode mode) {
        auto plain = AssSubtitles::create();
        plain->loadScript(plainSubtitleScript(srt, {}), true);
        const Box box = inkBox(plain->render(place(wide, 1920, 804, mode, 1.0F, true).frame, {}, 2000));
        return box.right - box.left;
    };
    require(std::abs(plainWidth(ViewMode::Stretch) - plainWidth(ViewMode::Fit)) <= 2,
            "a plain line was stretched with a stretched picture");
}

void coloursComeBackAsTheScriptSaysThem() {
    auto ass = AssSubtitles::create();
    // &H0000FF& is red (ASS writes BGR); no outline, no shadow.
    ass->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\c&H0000FF&\\fs120}HH\n"), false);
    const auto picture = ass->render(fullFrame(1920, 1080), {}, 2000);
    require(picture != nullptr, "the red line is not drawn");
    bool solid = false;
    forEachPixel(*picture, [&](int, int, std::uint32_t pixel) {
        if ((pixel >> 24) != 255) return;
        solid = true;
        require(((pixel >> 16) & 0xFF) == 255 && ((pixel >> 8) & 0xFF) == 0 && (pixel & 0xFF) == 0,
                "a red line came back another colour");
    });
    require(solid, "no pixel of the line is opaque");

    // A script that does not say its matrix was coloured as if every video
    // were BT.601; on a BT.709 video its red is corrected.
    auto old = AssSubtitles::create();
    std::string legacy = script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\c&H0000FF&\\fs120}HH\n");
    legacy.erase(legacy.find("YCbCr Matrix: None\n"), std::string("YCbCr Matrix: None\n").size());
    old->loadScript(legacy, false);
    SubtitleLook hd;
    hd.videoMatrix = SubtitleMatrix::Bt709Tv;
    const auto corrected = old->render(fullFrame(1920, 1080), hd, 2000);
    const std::uint32_t expected = subtitleColourForVideo(0xFF0000U, SubtitleMatrix::Bt601Tv, SubtitleMatrix::Bt709Tv);
    require(expected != 0xFF0000U, "the correction changed nothing to test");
    bool checked = false;
    forEachPixel(*corrected, [&](int, int, std::uint32_t pixel) {
        if ((pixel >> 24) != 255) return;
        checked = true;
        require((pixel & 0xFFFFFFU) == expected, "a VSFilter-era colour was not corrected to the video");
    });
    require(checked, "no pixel of the corrected line is opaque");
}

void aStreamTakesItsEventsOneByOne() {
    auto ass = AssSubtitles::create();
    const std::string header = script("");
    require(ass->startStream(header, false), "the stream's header was not taken");
    ass->addEvent({"0,0,Default,,0,0,0,,{\\an5\\pos(960,540)}First", 1000, 2000});
    ass->addEvent({"1,0,Default,,0,0,0,,Second", 5000, 1000});
    // A seek back hands the same event over again: libass keeps one.
    ass->addEvent({"0,0,Default,,0,0,0,,{\\an5\\pos(960,540)}First", 1000, 2000});
    require(ass->eventCount() == 2, "an event read twice was kept twice");
    const SubtitleFrame frame = fullFrame(1920, 1080);
    require(ass->render(frame, {}, 1500) != nullptr, "the first event is not drawn");
    require(ass->render(frame, {}, 5500) != nullptr, "the second event is not drawn");
    require(ass->render(frame, {}, 4000) == nullptr, "a moment between events is not empty");
    ass->clear();
    require(!ass->loaded() && ass->render(frame, {}, 1500) == nullptr, "clear left something shown");
    require(!ass->loadScript("not a script at all", false), "nothing at all was taken for a script");
}

void plainScriptsAreDrawn() {
    // What a plain file becomes: its tags as overrides, its own {\an8}, a
    // box when asked for.
    const std::string srt =
        "1\n00:00:01,000 --> 00:00:03,000\n<i>Italic</i> and <font color=\"#00FF00\">green</font>\n\n"
        "2\n00:00:04,000 --> 00:00:06,000\n{\\an8}At the top\n";
    PlainSubtitleStyle boxed;
    boxed.box = true;
    auto ass = AssSubtitles::create();
    require(ass->loadScript(plainSubtitleScript(srt, boxed), true), "the plain script was not read");
    require(ass->eventCount() == 2, "a cue of the plain file was lost");
    const SubtitleFrame frame = fullFrame(1920, 1080);
    const auto first = ass->render(frame, {}, 2000);
    require(first != nullptr, "the first cue is not drawn");
    bool green = false;
    forEachPixel(*first, [&](int, int, std::uint32_t pixel) {
        const std::uint32_t alpha = pixel >> 24;
        if (alpha == 255 && ((pixel >> 8) & 0xFF) == 255 && ((pixel >> 16) & 0xFF) == 0) green = true;
    });
    require(green, "<font color> did not colour the word");
    // The box reaches past the letters on every side, translucent black
    // where there is no letter: its corner is such a pixel.
    auto open = AssSubtitles::create();
    open->loadScript(plainSubtitleScript(srt, {}), true);
    const Box letters = inkBox(open->render(frame, {}, 2000));
    const Box withBox = inkBox(first);
    require(withBox.left + 8 <= letters.left && withBox.right >= letters.right + 8 &&
                withBox.top + 8 <= letters.top && withBox.bottom >= letters.bottom + 8,
            "the box asked for is not behind the line");
    bool corner = false;
    forEachPixel(*first, [&](int x, int y, std::uint32_t pixel) {
        if (x == withBox.left + 1 && y == withBox.top + 1) {
            corner = (pixel >> 24) > 100 && (pixel >> 24) < 160 && (pixel & 0xFFFFFFU) == 0;
        }
    });
    require(corner, "the box is not translucent black");
    require(inkBox(ass->render(frame, {}, 5000)).top < 200, "the plain file's {\\an8} is not at the top");
}

// A video's font answers for the face a script names, also when it arrives
// after the script's first line was drawn with a stand-in. The font is a
// system face's file under a name no system face has.
void aVideosFontIsUsedWhenItArrives() {
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::ifstream in(std::filesystem::path(windows) / L"Fonts" / L"segoesc.ttf", std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::puts("skipped: no Segoe Script on this system");
        return;
    }
    const auto rename = [&](const std::string& from, const std::string& to) {
        for (std::size_t at = 0; (at = data.find(from, at)) != std::string::npos; at += to.size()) {
            data.replace(at, from.size(), to);
        }
    };
    std::string wideFrom, wideTo;
    for (const char c : std::string("Segoe Script")) { wideFrom += '\0'; wideFrom += c; }
    for (const char c : std::string("QDTest Face!")) { wideTo += '\0'; wideTo += c; }
    rename(wideFrom, wideTo);
    rename("Segoe Script", "QDTest Face!");
    rename("SegoeScript", "QDTestFace!");
    const std::string sign = script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
                                    "{\\an7\\pos(10,10)\\fnQDTest Face!}Handwriting\n");
    const SubtitleFrame frame = fullFrame(1920, 1080);
    auto withFont = AssSubtitles::create();
    withFont->addFonts({{"face.ttf", data}});
    withFont->loadScript(sign, false);
    const Box wanted = inkBox(withFont->render(frame, {}, 2000));
    auto without = AssSubtitles::create();
    without->loadScript(sign, false);
    const Box standIn = inkBox(without->render(frame, {}, 2000));
    require(wanted.right - wanted.left != standIn.right - standIn.left, "the attached face drew like the stand-in");
    without->addFonts({{"face.ttf", data}});
    const Box late = inkBox(without->render(frame, {}, 2000));
    require(late.left == wanted.left && late.right == wanted.right && late.bottom == wanted.bottom,
            "a font that arrived after the first line was not used");
}

// Signs in two corners are two pieces, and no two pieces overlap.
void piecesKeepApart() {
    auto ass = AssSubtitles::create();
    ass->loadScript(script("Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an7\\pos(20,20)}Left\n"
                           "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\an9\\pos(1900,20)}Right\n"
                           "Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\\bord4\\shad3}Speech\n"),
                    false);
    const auto picture = ass->render(fullFrame(1920, 1080), {}, 2000);
    require(picture && picture->pieces.size() == 3, "far-apart lines were not drawn as pieces of their own");
    std::size_t area = 0;
    for (std::size_t a = 0; a < picture->pieces.size(); ++a) {
        const auto& one = picture->pieces[a];
        area += one.pixels.size();
        for (std::size_t b = a + 1; b < picture->pieces.size(); ++b) {
            const auto& two = picture->pieces[b];
            require(one.x + one.width <= two.x || two.x + two.width <= one.x || one.y + one.height <= two.y ||
                        two.y + two.height <= one.y,
                    "two pieces overlap");
        }
    }
    require(area < 1920u * 1080u / 10u, "the pieces cover far more than the lines");
}

}  // namespace

int main() {
    placementFollowsTheScript();
    theViewerSizesAndRaisesSpeechOnly();
    thePictureIsWhatTheScriptIsPlacedOn();
    speechStaysOnThePane();
    coloursComeBackAsTheScriptSaysThem();
    aStreamTakesItsEventsOneByOne();
    plainScriptsAreDrawn();
    piecesKeepApart();
    aVideosFontIsUsedWhenItArrives();
    std::puts("ass render tests passed");
    return 0;
}
