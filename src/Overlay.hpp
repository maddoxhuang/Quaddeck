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
    // 0 hides the chrome; the pill, chips and rail fade together.
    float chromeAlpha{};
    bool hovered{};
    // Explicit playback destination, independent of pointer and audio.
    bool targeted{};
    std::wstring label;
    bool audioOn{};
    bool paused{};
    bool waiting{};
    bool solo{};
    bool repeat{};
    bool seekable{};
    float position01{};
    // End of the NAS cache's buffered range, 0 when there is none.
    float buffered01{};
    bool railHot{};
    bool railDragging{};
    std::wstring timeText;
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
    // The tab strip's captions and the chosen tab; empty for a sheet without tabs.
    std::vector<std::wstring> tabs;
    int tab{-1};
    // The header switch's caption: the other face of the sheet.
    std::wstring switchCaption;
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
};

struct OverlayScene {
    float width{};
    float height{};
    float scale{1.0F};
    bool anyLoaded{};

    TransportBarLayout bar{};
    float barAlpha{};
    bool playing{};
    bool muted{};
    bool fullscreen{};
    bool pinned{};
    bool seekable{};
    float volume01{1.0F};
    bool volumeOpen{};
    bool volumeDragging{};
    float seek01{};
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
    // dark box is drawn behind them, and whether the bar's button shows
    // them as on.
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

    OverlayPanelScene panel;
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
    bool mediaTextLayout(const std::wstring& value, OverlayTextStyle style, float width,
                         float height, float lineHeight,
                         Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout);
    void mediaText(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
                   float lineHeight, D2D1_COLOR_F color);
    void toggleSwitch(const OverlayRect& box, bool on, bool hot, float alpha, float scale);
    void glyph(wchar_t code, const wchar_t* fallback, const OverlayRect& box, D2D1_COLOR_F color);
    void fillRound(const OverlayRect& box, float radius, D2D1_COLOR_F color);
    void strokeRound(const OverlayRect& box, float radius, D2D1_COLOR_F color, float width);
    void button(const OverlayRect& box, wchar_t code, const wchar_t* fallback,
                bool hot, bool pressed, bool toggled, float alpha, float scale);
    void rail(const OverlayRect& band, float inset, float thickness, float position01,
              float buffered01, bool showThumb, float thumbRadius, float alpha);
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
    // The Direct2D bitmap of a cached picture, decoded on first use; null
    // while the bytes are not there yet, failed to decode, or this frame's
    // decode budget is spent.
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
    // Decoded browser pictures by cache key, dropped least recently drawn
    // first; an entry with no bitmap remembers bytes that would not decode.
    struct CachedBitmap {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t used{};
    };
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    std::unordered_map<std::string, CachedBitmap> bitmaps_;
    std::uint64_t frame_{};
    int decodesThisFrame_{};
};

}  // namespace quaddeck
