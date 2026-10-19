#pragma once

// The drawn user interface. Everything the player shows besides the video --
// the transport bar, each pane's label, chips and timeline, notices, the
// empty-window hint -- is painted here with Direct2D and DirectWrite onto the
// swap chain's back buffer, after the frames and before Present. There are
// no child windows: the picture is never covered by a control's background,
// every element can be translucent and fade, and hit-testing is the
// geometry in OverlayLayout.hpp rather than a stack of HWNDs.
//
// App describes one frame's worth of state in an OverlayScene and this class
// only draws it. It owns no player state and makes no decisions.

#include "AssSubtitles.hpp"
#include "Core.hpp"
#include "OverlayLayout.hpp"
#include "PanelMenu.hpp"
#include "SettingsPanel.hpp"
#include "Thumbnails.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace quaddeck {

struct OverlayPaneScene {
    bool active{};
    RectF cell{};
    PaneChromeLayout chrome{};
    // The tag shows on every pane while the pointer moves (chromeAlpha);
    // the rest of the strip -- rail, buttons, time -- on the pane under it
    // (timelineAlpha).
    float chromeAlpha{};
    float timelineAlpha{};
    bool hovered{};
    // Explicit playback destination, independent of pointer and audio.
    bool targeted{};
    // Its place on the screen, 0 for V1: its pill, rails, lit chips, the
    // edge saying it is heard, its outline as the destination or a drop's
    // are in that place's colour, as the browser's marks of what it plays
    // are.
    int position{};
    std::wstring label;
    bool audioOn{};
    bool paused{};
    bool waiting{};
    bool solo{};
    bool repeat{};
    // Repeat is saved for this video but does not apply where it is: the
    // chip is drawn dimmed.
    bool repeatIdle{};
    bool seekable{};
    float position01{};
    // End of the NAS cache's buffered range, 0 when there is none.
    float buffered01{};
    bool railHot{};
    bool railDragging{};
    // The row's time.
    std::wstring timeText;
    // Over the rail while it is pointed at or dragged: the time there and
    // what a seek reaches, centred on `railX`.
    std::wstring railText;
    float railX{};
    // Its subtitle switch: lines shown and one chosen.
    bool subtitlesOn{};
    int hotChip{-1};
    int pressedChip{-1};
    bool pillHot{};
    bool pillPressed{};
    float volume01{1.0F};
    bool volumeHot{};
    bool volumeDragging{};
    bool dragSource{};
    bool dropTarget{};
    // The subtitle line(s) on screen at this frame, empty for none: those
    // at the bottom of the picture, and those their file puts at its top.
    std::wstring subtitle;
    std::wstring subtitleTop;
    // How much of the pane's bottom the transport bar covers right now; the
    // bottom lines stand clear of it.
    float subtitleLift{};
    // The subtitles as libass drew them, its frame's top left corner at
    // (subtitleImageX, subtitleImageY) of the window. When this is set the
    // plain lines above are not drawn.
    std::shared_ptr<const SubtitleBitmap> subtitleImage;
    int subtitleImageX{};
    int subtitleImageY{};
};

struct OverlayPanelScene {
    float alpha{};
    std::wstring title{L"Settings"};
    PanelLayout layout{};
    std::vector<PanelRow> rows;
    // App's own layout and rows, which the scene points at rather than
    // copying a long list every frame: both owned by App and changed only on
    // the window thread, which is also the thread that draws. When set they
    // stand for `layout` and `rows`.
    const PanelLayout* sharedLayout{};
    const std::vector<PanelRow>* sharedRows{};
    const PanelLayout& shownLayout() const { return sharedLayout ? *sharedLayout : layout; }
    const std::vector<PanelRow>& shownRows() const { return sharedRows ? *sharedRows : rows; }
    // The tab strip's captions and the chosen tab; empty for a sheet without tabs.
    std::vector<std::wstring> tabs;
    int tab{-1};
    // The header switches' captions, one per layout.sheetSwitches: the
    // sheet's other faces.
    std::vector<std::wstring> switchCaptions;
    // The pictures the Tiles rows name, owned by App and filled on the
    // window thread, which is also the thread that draws.
    const ThumbnailCache* thumbnails{};
    PanelHitKind hotKind{PanelHitKind::None};
    int hotRow{-1};
    int hotPart{-1};
    PanelHitKind pressedKind{PanelHitKind::None};
    int pressedRow{-1};
    int pressedPart{-1};
    int dragRow{-1};
    bool scrollbarHot{};
    bool scrollbarDragging{};
    // The videos being played, where the list shows them, marked across the
    // scrollbar: sorted by `y`, those of one row side by side.
    std::vector<PanelScrollMark> scrollMarks;
};

// A submenu that is open, beside the line `from` of the card before it.
// Its items are that line's children.
struct OverlayPanelSubmenuScene {
    int from{-1};
    PanelMenuLayout layout{};
    int highlighted{-1};
    int pressed{-1};
    float alpha{};
};

// The menu that is open (PanelMenu.hpp), the sheet's or one over the
// picture; no items when none is.
struct OverlayPanelMenuScene {
    std::vector<PanelMenuItem> items;
    PanelMenuLayout layout{};
    // What it hangs from and is kept inside, and how, for a fixture that
    // measures its text again.
    OverlayRect anchor{};
    OverlayRect bounds{};
    PanelMenuPlacement placement{PanelMenuPlacement::Below};
    PanelMenuMetrics metrics{};
    // Under the pointer or the keyboard, and pressed.
    int highlighted{-1};
    int pressed{-1};
    // Fading in -- with the sheet's own alpha, for its drop-down -- and the
    // distance it still has to slide to its place, in pixels.
    float alpha{};
    float offset{};
    // Outermost first.
    std::vector<OverlayPanelSubmenuScene> submenus;
    // The place of the video it is about (0 for V1), whose colour its
    // ticks, switches and chosen segments are lit in; -1 for none.
    int position{-1};
};

struct OverlayScene {
    float width{};
    float height{};
    float scale{1.0F};
    bool anyLoaded{};

    TransportBarLayout bar{};
    float barAlpha{};
    std::wstring scopeText;
    bool playing{};
    bool muted{};
    bool fullscreen{};
    bool seekable{};
    float volume01{1.0F};
    bool volumeOpen{};
    bool volumeDragging{};
    float seek01{};
    // The place of the video the rail is the timeline of, whose colour it
    // is filled in; -1 while it is all of them.
    int seekPosition{-1};
    bool seekHot{};
    bool seekDragging{};
    // Pointer x over the rail for the time bubble; negative when absent.
    float seekHoverX{-1.0F};
    std::wstring seekHoverText;
    std::wstring timeText;
    int hotBarItem{-1};
    int pressedBarItem{-1};

    // The window caption: empty in fullscreen. It shows with the bar and
    // with the sheet, whichever is further in.
    CaptionLayout caption{};
    float captionAlpha{};
    std::wstring captionTitle;
    bool maximized{};
    int hotCaptionItem{-1};
    int pressedCaptionItem{-1};

    PaneArray<OverlayPaneScene> panes{};
    // Accent border only means something when there is more than one pane.
    int activePaneCount{};
    // Subtitles: their size as a share of the default, how far the bottom
    // lines stand above a pane's bottom edge in pane heights, whether a
    // dark box is drawn behind them, and whether the bar's button is lit as
    // on or greyed as off.
    float subtitleSize{1.0F};
    float subtitlePosition{};
    bool subtitleBackground{};
    bool subtitlesOn{};
    // The language the lines are most likely in, as DirectWrite names it
    // ("zh-Hans", "ja-JP"), empty for none: Chinese, Japanese and Korean
    // share their characters but not the shapes they are drawn in, and the
    // font that stands in for Segoe UI is chosen by this.
    std::wstring subtitleLocale;

    std::wstring notice;
    float noticeAlpha{};

    std::wstring tooltip;
    OverlayRect tooltipAnchor{};

    bool showEmptyHint{};
    // Where the hint's lines and its Open file and Open Emby buttons stand,
    // how far the buttons show (they fade under the sheet) and their
    // pointer states.
    EmptyHintLayout emptyHint{};
    float emptyButtonsAlpha{};
    bool emptyOpenHot{};
    bool emptyOpenPressed{};
    bool emptyEmbyHot{};
    bool emptyEmbyPressed{};

    OverlayPanelScene panel;
    OverlayPanelMenuScene panelMenu;
};

enum class OverlayTextStyle { Body, BodyBold, Small, Title, Icon, MediaTitle, MediaTitleCompact, CaptionIcon };

class Overlay {
public:
    Overlay() = default;
    ~Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Builds the Direct2D device on the renderer's D3D11 device. Call again
    // after the renderer recovers from a lost device.
    bool initialize(ID3D11Device* device);
    void release();
    // Lets go of everything made on the D3D device, the Direct2D device
    // first, before the renderer rebuilds it; text is still measured until
    // initialize runs again on the new one.
    void releaseDeviceResources();
    bool ready() const { return context_ != nullptr; }
    // True when an icon face is installed; otherwise buttons show words.
    bool hasIconFont() const { return !iconFamily_.empty(); }

    // Width in pixels of `text` at this scale, for laying out pills.
    float measureText(const std::wstring& text, OverlayTextStyle style, float scale);

    // Measure wrapped detail text before settingsPanelLayout. rowWidth is
    // the sheet's inner width in pixels (after its two padding margins).
    // Without a ready renderer, the pure layout keeps its safe fallback.
    void measureMediaDetail(PanelRow& row, float rowWidth, float scale);

    // Opt-in wrapped notes use the same layout for measurement and paint.
    void measureWrappedNote(PanelRow& row, float rowWidth, float scale);

    // A drop-down's text widths by item, in the styles it is drawn in, for
    // panelMenuLayout; 0 where no font is ready.
    void measurePanelMenu(const std::vector<PanelMenuItem>& items, float scale,
                          std::vector<float>& labelWidths, std::vector<float>& shortcutWidths);

    // Paints the scene onto the swap chain's current back buffer. Called by
    // the renderer inside its device lock, between the last pane and Present.
    // With transparentLayer the surface is not the back buffer but an empty
    // BGRA layer the renderer composites itself (its 10-bit PQ output mode):
    // it is cleared first and drawn with premultiplied alpha.
    void draw(IDXGISurface* surface, const OverlayScene& scene, bool transparentLayer = false);

private:
    struct Formats {
        float scale{};
        Microsoft::WRL::ComPtr<IDWriteTextFormat> body;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyBold;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> smallText;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> title;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> icon;
        // The window buttons' glyphs, at Windows' own 10 px.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> captionIcon;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaTitle;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaTitleCompact;
    };
    bool ensureFormats(float scale);
    IDWriteTextFormat* format(OverlayTextStyle style) const;
    void text(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
              D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER,
              bool clip = false, bool wrap = false);
    // The layout `text` draws `value` with in a box of that size, or with
    // `media` the wrapped one an information page and a wrapped note are
    // measured and drawn with, at that line height; from textLayouts_,
    // made on first use. Null for empty text or an empty box.
    IDWriteTextLayout* textLayout(const std::wstring& value, OverlayTextStyle style, float width, float height,
                                  DWRITE_TEXT_ALIGNMENT align, bool wrap, bool media, float lineHeight);
    IDWriteTextLayout* mediaTextLayout(const std::wstring& value, OverlayTextStyle style, float width,
                                       float height, float lineHeight);
    void trimTextCaches();
    void clearTextCaches();
    void mediaText(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
                   float lineHeight, D2D1_COLOR_F color);
    // On, it is lit in the accent, or in the colour of the video at place
    // `video` (0 for V1) when it is about one.
    void toggleSwitch(const OverlayRect& box, bool on, bool hot, float alpha, float scale, int video = -1);
    // A one-line box: its label, the search's magnifier, the text (dots for
    // a password) or its grey placeholder, an input method's composition,
    // the selection, the caret and the clear button, all placed by
    // panelTextFieldTextBox.
    void drawTextField(const PanelRow& row, const PanelRowGeometry& geometry, bool hot, bool clearHot,
                       bool clearPressed, float alpha, float scale);
    void glyph(wchar_t code, const wchar_t* fallback, const OverlayRect& box, D2D1_COLOR_F color);
    void fillRound(const OverlayRect& box, float radius, D2D1_COLOR_F color);
    void strokeRound(const OverlayRect& box, float radius, D2D1_COLOR_F color, float width);
    // `toggled` lights a switch that is on in the accent, or in the colour
    // of the video at place `video` when it is one of its; `dimmed` greys
    // the glyph of one that is off.
    void button(const OverlayRect& box, wchar_t code, const wchar_t* fallback,
                bool hot, bool pressed, bool toggled, float alpha, float scale, bool dimmed = false,
                int video = -1);
    // Filled to the playhead in the accent, or in the video's colour.
    void rail(const OverlayRect& band, float inset, float thickness, float position01,
              float buffered01, bool showThumb, float thumbRadius, float alpha, int video = -1);
    // A rail standing up: 0 at the bottom, 1 at the top. The volume popup.
    void verticalRail(const OverlayRect& band, float inset, float thickness, float position01,
                      float thumbRadius, float alpha);
    // Subtitle text at the bottom of a pane or at its top, sized to the
    // pane, outlined in black so it reads over any picture, on a dark box
    // where `background` asks for one. `margin` is the distance from that
    // edge in pixels.
    void drawSubtitle(const OverlayRect& cell, const std::wstring& value, float scale, bool top,
                      float size, float margin, bool background);
    // A pane's libass picture, pixel for pixel, clipped to the pane.
    void drawSubtitleImage(std::size_t pane, const OverlayPaneScene& scene, const OverlayRect& cell);
    // The Direct2D bitmap of a cached picture, decoded on first use and
    // scaled down to the width its key names; null while the bytes are not
    // there yet, failed to decode or are too large to (refused from the
    // size in their header), or this frame's decode budget is spent.
    ID2D1Bitmap* bitmapFor(const std::string& key, const ThumbnailCache& cache);
    void trimBitmaps();
    void drawTile(const PanelRow& row, std::size_t part, const OverlayRect& box, bool hot, bool pressed,
                  float alpha, float scale, const ThumbnailCache* cache);
    void drawMediaBackdrop(const PanelMediaDetail& detail, const OverlayRect& box,
                           float alpha, const ThumbnailCache* cache);
    void drawMediaDetail(const PanelRow& row, const PanelRowGeometry& geometry,
                         int hotPart, int pressedPart, float alpha, float scale,
                         const ThumbnailCache* cache);

    // The dark gradient the bottom bar and the caption stand on, from
    // `clear` (transparent) to `dark`.
    void fillScrim(const OverlayRect& band, float clear, float dark, float alpha);
    void drawPanes(const OverlayScene& scene);
    void drawBar(const OverlayScene& scene);
    // The caption's scrim lies under the sheet; its title and buttons over it.
    void drawCaptionBand(const OverlayScene& scene);
    void drawCaption(const OverlayScene& scene);
    void drawNotice(const OverlayScene& scene);
    void drawTooltip(const OverlayScene& scene);
    void drawEmptyHint(const OverlayScene& scene);
    void drawPanel(const OverlayScene& scene);
    void drawPanelMenu(const OverlayScene& scene);
    // One card of it: `opened` is the line whose submenu is open.
    void drawPanelMenuCard(const std::vector<PanelMenuItem>& items, const PanelMenuLayout& layout, int highlighted,
                           int pressed, int opened, float alpha, float offset, float scale, int video);

    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> scrim_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> mediaScrim_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    std::wstring iconFamily_;
    Formats formats_;
    // Subtitle formats by pixel size; a pane's size changes rarely. All are
    // of one locale and go when it changes.
    std::map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> subtitleFormats_;
    std::wstring subtitleLocale_;
    // DirectWrite layouts and widths, made once and used again while the
    // text, its style and its box's size stay the same: a frame draws a few
    // hundred strings, nearly all as they were the frame before. Where the
    // box lies is no part of a layout, so a scroll finds them too. Kept
    // while used; one unused for a while goes (trimTextCaches), and all go
    // with the formats -- a new scale -- and with the factory.
    struct TextLayoutKey {
        std::wstring text;
        OverlayTextStyle style{};
        float width{};
        float height{};
        DWRITE_TEXT_ALIGNMENT align{};
        bool wrap{};
        bool media{};
        float lineHeight{};
        bool operator==(const TextLayoutKey&) const = default;
    };
    struct TextLayoutKeyHash {
        std::size_t operator()(const TextLayoutKey& key) const noexcept;
    };
    struct CachedTextLayout {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::uint64_t used{};
    };
    struct TextWidthKey {
        std::wstring text;
        OverlayTextStyle style{};
        bool operator==(const TextWidthKey&) const = default;
    };
    struct TextWidthKeyHash {
        std::size_t operator()(const TextWidthKey& key) const noexcept;
    };
    struct CachedTextWidth {
        float width{};
        std::uint64_t used{};
    };
    std::unordered_map<TextLayoutKey, CachedTextLayout, TextLayoutKeyHash> textLayouts_;
    std::unordered_map<TextWidthKey, CachedTextWidth, TextWidthKeyHash> textWidths_;
    // Each pane's libass picture as bitmaps, one a piece, made again only
    // when libass hands over a new picture; refilled in place where a piece
    // keeps its size.
    struct SubtitlePieceSurface {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        int width{};
        int height{};
    };
    struct SubtitleSurface {
        std::shared_ptr<const SubtitleBitmap> source;
        std::vector<SubtitlePieceSurface> pieces;
    };
    PaneArray<SubtitleSurface> subtitleSurfaces_{};
    // Decoded browser pictures by cache key; an entry with no bitmap
    // remembers bytes that would not decode. After every frame, shown sheet
    // or not, the least recently drawn go while they are past
    // kBitmapBudget bytes of pixels (or kBitmapsLimit entries) -- never one
    // this frame drew -- so a closed sheet keeps one screenful for its next
    // reveal and no more.
    static constexpr std::size_t kBitmapBudget = 96u * 1024u * 1024u;
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t used{};
        std::size_t bytes{};
    };
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    std::unordered_map<std::string, CachedBitmap> bitmaps_;
    std::uint64_t frame_{};
    int decodesThisFrame_{};
};

}  // namespace quaddeck
