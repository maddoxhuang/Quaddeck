#include "Overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

namespace quaddeck {
namespace {

// Segoe Fluent Icons / MDL2 Assets code points, identical in both faces.
constexpr wchar_t kGlyphPlay = L'\xE768';
constexpr wchar_t kGlyphPause = L'\xE769';
constexpr wchar_t kGlyphStop = L'\xE71A';
constexpr wchar_t kGlyphVolume = L'\xE767';
constexpr wchar_t kGlyphMute = L'\xE74F';
constexpr wchar_t kGlyphMenu = L'\xE712';
constexpr wchar_t kGlyphFullscreen = L'\xE740';
constexpr wchar_t kGlyphWindowed = L'\xE73F';
constexpr wchar_t kGlyphSettings = L'\xE713';
constexpr wchar_t kGlyphRepeat = L'\xE8EE';
constexpr wchar_t kGlyphClose = L'\xE711';
constexpr wchar_t kGlyphPin = L'\xE840';
constexpr wchar_t kGlyphPrevious = L'\xE892';
constexpr wchar_t kGlyphNext = L'\xE893';
constexpr wchar_t kGlyphSubtitles = L'\xED1E';
constexpr wchar_t kGlyphLayout = L'\xE8A9';   // ViewAll
constexpr wchar_t kGlyphChromeMinimize = L'\xE921';
constexpr wchar_t kGlyphChromeMaximize = L'\xE922';
constexpr wchar_t kGlyphChromeRestore = L'\xE923';
constexpr wchar_t kGlyphChromeClose = L'\xE8BB';

D2D1_COLOR_F rgba(int r, int g, int b, float a) {
    return D2D1::ColorF(r / 255.0F, g / 255.0F, b / 255.0F, a);
}
const D2D1_COLOR_F kAccent = rgba(63, 132, 232, 1.0F);
const D2D1_COLOR_F kText = rgba(240, 243, 248, 1.0F);
const D2D1_COLOR_F kTextDim = rgba(190, 196, 208, 1.0F);
const D2D1_COLOR_F kPillFill = rgba(10, 12, 16, 0.74F);
const D2D1_COLOR_F kPillEdge = rgba(255, 255, 255, 0.10F);

D2D1_COLOR_F withAlpha(D2D1_COLOR_F color, float alpha) {
    color.a *= std::clamp(alpha, 0.0F, 1.0F);
    return color;
}

D2D1_RECT_F toRect(const OverlayRect& box) {
    return D2D1::RectF(box.x, box.y, box.right(), box.bottom());
}

}  // namespace

bool Overlay::initialize(ID3D11Device* device) {
    release();
    if (!device) return false;
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                   &options, reinterpret_cast<void**>(factory_.GetAddressOf()));
    if (FAILED(hr)) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) { release(); return false; }
    hr = factory_->CreateDevice(dxgiDevice.Get(), &device_);
    if (FAILED(hr)) { release(); return false; }
    hr = device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_);
    if (FAILED(hr)) { release(); return false; }
    // Pixels, not DIPs: every metric is already scaled by OverlayLayout.
    context_->SetDpi(96.0F, 96.0F);
    context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    hr = context_->CreateSolidColorBrush(kText, &brush_);
    if (FAILED(hr)) { release(); return false; }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(hr)) { release(); return false; }

    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts))) {
        for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(face, &index, &exists)) && exists) {
                iconFamily_ = face;
                break;
            }
        }
    }
    formats_ = {};
    // Pictures are optional: without WIC the browser's tiles show titles.
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic_.ReleaseAndGetAddressOf())))) {
        wic_.Reset();
    }
    return true;
}

void Overlay::release() {
    formats_ = {};
    subtitleFormats_.clear();
    subtitleLocale_.clear();
    subtitleSurfaces_ = {};
    bitmaps_.clear();
    wic_.Reset();
    scrim_.Reset();
    mediaScrim_.Reset();
    brush_.Reset();
    context_.Reset();
    device_.Reset();
    factory_.Reset();
    dwrite_.Reset();
    iconFamily_.clear();
}

bool Overlay::ensureFormats(float scale) {
    if (!dwrite_) return false;
    if (formats_.body && std::abs(formats_.scale - scale) < 0.001F) return true;
    Formats formats;
    formats.scale = scale;
    const auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size,
                          Microsoft::WRL::ComPtr<IDWriteTextFormat>& target) {
        const HRESULT hr = dwrite_->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            std::max(1.0F, size * scale), L"", &target);
        if (SUCCEEDED(hr)) {
            target->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return SUCCEEDED(hr);
    };
    if (!make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 14.0F, formats.body)) return false;
    if (!make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0F, formats.bodyBold)) return false;
    if (!make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 12.0F, formats.smallText)) return false;
    if (!make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0F, formats.title)) return false;
    if (!make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 32.0F, formats.mediaTitle)) return false;
    if (!make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 26.0F, formats.mediaTitleCompact)) return false;
    if (!iconFamily_.empty()) {
        if (!make(iconFamily_.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 16.0F, formats.icon)) {
            formats.icon.Reset();
        }
        if (!make(iconFamily_.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 10.0F, formats.captionIcon)) {
            formats.captionIcon.Reset();
        }
    }
    formats_ = std::move(formats);
    return true;
}

IDWriteTextFormat* Overlay::format(OverlayTextStyle style) const {
    switch (style) {
    case OverlayTextStyle::BodyBold: return formats_.bodyBold.Get();
    case OverlayTextStyle::Small: return formats_.smallText.Get();
    case OverlayTextStyle::Title: return formats_.title.Get();
    case OverlayTextStyle::Icon: return formats_.icon ? formats_.icon.Get() : formats_.body.Get();
    case OverlayTextStyle::CaptionIcon:
        return formats_.captionIcon ? formats_.captionIcon.Get() : formats_.smallText.Get();
    case OverlayTextStyle::MediaTitle: return formats_.mediaTitle.Get();
    case OverlayTextStyle::MediaTitleCompact: return formats_.mediaTitleCompact.Get();
    default: return formats_.body.Get();
    }
}

float Overlay::measureText(const std::wstring& value, OverlayTextStyle style, float scale) {
    if (!ensureFormats(scale) || value.empty()) return 0.0F;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                         format(style), 4096.0F, 256.0F, &layout))) {
        return 0.0F;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0F;
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

bool Overlay::mediaTextLayout(const std::wstring& value, OverlayTextStyle style, float width,
                              float height, float lineHeight,
                              Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout) {
    if (value.empty() || !dwrite_ || !format(style) || width <= 0.0F || height <= 0.0F) return false;
    if (FAILED(dwrite_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                        format(style), width, height, &layout))) return false;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    // Match measurement and painting, including explicit paragraphs and
    // emergency wrapping of long unbroken titles at narrow dock widths.
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, lineHeight * 0.8F);
    return true;
}

void Overlay::measureMediaDetail(PanelRow& row, float rowWidth, float scale) {
    if (row.kind != PanelRowKind::MediaDetail) return;
    auto& detail = row.mediaDetail;
    detail.measuredTextHeights = {};
    detail.measuredTextWidth = 0.0F;
    detail.measuredScale = 0.0F;
    if (!std::isfinite(rowWidth) || !std::isfinite(scale) || rowWidth <= 0.0F || scale <= 0.0F ||
        !ensureFormats(scale)) return;
    const auto geometry = panelMediaDetailLayout(row, {0.0F, 0.0F, rowWidth, 0.0F}, scale);
    const std::array<const std::wstring*, 4> values{
        &detail.title, &detail.metadata, &detail.mediaInfo, &detail.overview};
    const std::array<OverlayTextStyle, 4> styles{
        geometry.stacked ? OverlayTextStyle::MediaTitleCompact : OverlayTextStyle::MediaTitle,
        OverlayTextStyle::Body, OverlayTextStyle::Small, OverlayTextStyle::Body};
    const std::array<float, 4> lineHeights{geometry.stacked ? 34.0F : 42.0F, 22.0F, 20.0F, 22.0F};
    for (std::size_t i = 0; i < values.size(); ++i) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (!mediaTextLayout(*values[i], styles[i], geometry.title.width, 1000000.0F,
                             lineHeights[i] * scale, layout)) continue;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics)) && std::isfinite(metrics.height)) {
            detail.measuredTextHeights[i] = std::ceil(metrics.height);
        }
    }
    detail.measuredTextWidth = geometry.title.width;
    detail.measuredScale = scale;
}

void Overlay::measureWrappedNote(PanelRow& row, float rowWidth, float scale) {
    if (row.kind != PanelRowKind::Note || !row.wrapNote) return;
    row.measuredNoteHeight = row.measuredNoteWidth = row.measuredNoteScale = 0.0F;
    if (!std::isfinite(rowWidth) || !std::isfinite(scale) || rowWidth <= 0.0F || scale <= 0.0F ||
        !ensureFormats(scale)) return;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (!mediaTextLayout(row.label, OverlayTextStyle::Small, rowWidth, 1000000.0F,
                         kPanelWrappedNoteLineHeight * scale, layout)) return;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)) || !std::isfinite(metrics.height) || metrics.height <= 0.0F) return;
    row.measuredNoteHeight = std::ceil(metrics.height);
    row.measuredNoteWidth = rowWidth;
    row.measuredNoteScale = scale;
}

void Overlay::mediaText(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
                        float lineHeight, D2D1_COLOR_F color) {
    if (!box.visible()) return;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (!mediaTextLayout(value, style, box.width, box.height, lineHeight, layout)) return;
    brush_->SetColor(color);
    context_->PushAxisAlignedClip(toRect(box), D2D1_ANTIALIAS_MODE_ALIASED);
    context_->DrawTextLayout(D2D1::Point2F(box.x, box.y), layout.Get(), brush_.Get());
    context_->PopAxisAlignedClip();
}

void Overlay::text(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
                   D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align, bool clip, bool wrap) {
    if (value.empty() || !box.visible()) return;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                         format(style), box.width, box.height, &layout))) {
        return;
    }
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                       : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    brush_->SetColor(color);
    if (clip) context_->PushAxisAlignedClip(toRect(box), D2D1_ANTIALIAS_MODE_ALIASED);
    context_->DrawTextLayout(D2D1::Point2F(box.x, box.y), layout.Get(), brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (clip) context_->PopAxisAlignedClip();
}

void Overlay::glyph(wchar_t code, const wchar_t* fallback, const OverlayRect& box,
                    D2D1_COLOR_F color) {
    if (formats_.icon) {
        text(std::wstring(1, code), OverlayTextStyle::Icon, box, color);
    } else {
        text(fallback, OverlayTextStyle::Small, box, color, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
}

void Overlay::fillRound(const OverlayRect& box, float radius, D2D1_COLOR_F color) {
    if (!box.visible() || color.a <= 0.0F) return;
    brush_->SetColor(color);
    context_->FillRoundedRectangle(D2D1::RoundedRect(toRect(box), radius, radius), brush_.Get());
}

void Overlay::strokeRound(const OverlayRect& box, float radius, D2D1_COLOR_F color, float width) {
    if (!box.visible() || color.a <= 0.0F) return;
    brush_->SetColor(color);
    const float inset = width * 0.5F;
    const D2D1_RECT_F rect = D2D1::RectF(box.x + inset, box.y + inset,
                                         box.right() - inset, box.bottom() - inset);
    context_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), width);
}

void Overlay::button(const OverlayRect& box, wchar_t code, const wchar_t* fallback,
                     bool hot, bool pressed, bool toggled, float alpha, float scale) {
    if (!box.visible()) return;
    const float radius = 8.0F * scale;
    if (toggled) {
        fillRound(box, radius, withAlpha(rgba(63, 132, 232, 0.32F), alpha));
        strokeRound(box, radius, withAlpha(rgba(63, 132, 232, 0.9F), alpha), std::max(1.0F, scale));
    }
    if (pressed) fillRound(box, radius, withAlpha(rgba(255, 255, 255, 0.22F), alpha));
    else if (hot) fillRound(box, radius, withAlpha(rgba(255, 255, 255, 0.14F), alpha));
    glyph(code, fallback, box, withAlpha(toggled ? rgba(190, 216, 255, 1.0F) : kText, alpha));
}

void Overlay::rail(const OverlayRect& band, float inset, float thickness, float position01,
                   float buffered01, bool showThumb, float thumbRadius, float alpha) {
    if (!band.visible()) return;
    const float left = band.x + inset;
    const float right = band.right() - inset;
    if (right <= left) return;
    const float centerY = band.y + band.height * 0.5F;
    const float half = thickness * 0.5F;
    const OverlayRect track{left, centerY - half, right - left, thickness};
    fillRound(track, half, withAlpha(rgba(255, 255, 255, 0.28F), alpha));
    const float playhead = left + (right - left) * std::clamp(position01, 0.0F, 1.0F);
    if (buffered01 > position01) {
        const float bufferedX = left + (right - left) * std::clamp(buffered01, 0.0F, 1.0F);
        fillRound({playhead, centerY - half, bufferedX - playhead, thickness}, half,
                  withAlpha(rgba(255, 255, 255, 0.28F), alpha));
    }
    fillRound({left, centerY - half, playhead - left, thickness}, half, withAlpha(kAccent, alpha));
    if (showThumb && thumbRadius > 0.0F) {
        brush_->SetColor(withAlpha(kText, alpha));
        context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(playhead, centerY), thumbRadius, thumbRadius),
                              brush_.Get());
    }
}

void Overlay::verticalRail(const OverlayRect& band, float inset, float thickness, float position01,
                           float thumbRadius, float alpha) {
    if (!band.visible()) return;
    const float top = band.y + inset;
    const float bottom = band.bottom() - inset;
    if (bottom <= top) return;
    const float centerX = band.x + band.width * 0.5F;
    const float half = thickness * 0.5F;
    fillRound({centerX - half, top, thickness, bottom - top}, half, withAlpha(rgba(255, 255, 255, 0.28F), alpha));
    const float level = bottom - (bottom - top) * std::clamp(position01, 0.0F, 1.0F);
    fillRound({centerX - half, level, thickness, bottom - level}, half, withAlpha(kAccent, alpha));
    if (thumbRadius > 0.0F) {
        brush_->SetColor(withAlpha(kText, alpha));
        context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, level), thumbRadius, thumbRadius),
                              brush_.Get());
    }
}

void Overlay::draw(IDXGISurface* surface, const OverlayScene& scene, bool transparentLayer) {
    if (!context_ || !surface || !ensureFormats(scene.scale)) return;
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          transparentLayer ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        96.0F, 96.0F);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
    if (FAILED(context_->CreateBitmapFromDxgiSurface(surface, &properties, &target))) return;
    context_->SetTarget(target.Get());
    ++frame_;
    decodesThisFrame_ = 0;
    context_->BeginDraw();
    // Text is already grayscale-antialiased (initialize), which is what a
    // transparent target needs; ClearType would leave alpha wrong.
    if (transparentLayer) context_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
    drawPanes(scene);
    drawBar(scene);
    drawCaptionBand(scene);
    drawEmptyHint(scene);
    drawPanel(scene);
    drawCaption(scene);
    drawNotice(scene);
    drawTooltip(scene);
    const HRESULT hr = context_->EndDraw();
    context_->SetTarget(nullptr);
    if (hr == D2DERR_RECREATE_TARGET) {
        // The device behind the context is gone; the renderer will report the
        // same loss and App re-initialises us after recovery.
        release();
    }
}

void Overlay::drawSubtitle(const OverlayRect& cell, const std::wstring& value, float scale, bool top,
                           float sizeFactor, float margin, bool background) {
    if (!dwrite_ || !context_ || !brush_ || value.empty() || !cell.visible()) return;
    // A share of the pane's height, so the lines keep their proportion to
    // the picture in a grid of five as on one full screen.
    const int size = static_cast<int>(std::clamp(
        cell.height * 0.048F * std::clamp(sizeFactor, 0.25F, 4.0F), 10.0F * scale, 200.0F));
    // A pane resized by dragging asks for a size per pixel of height.
    if (subtitleFormats_.size() > 64) subtitleFormats_.clear();
    auto& format = subtitleFormats_[size];
    if (!format) {
        if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             static_cast<float>(size), subtitleLocale_.c_str(), &format))) {
            subtitleFormats_.erase(size);
            return;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    const float maxWidth = std::max(1.0F, cell.width * 0.9F);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), format.Get(),
                                         maxWidth, cell.height, &layout))) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;
    const float pad = static_cast<float>(size) * 0.35F;
    const float x = cell.x + (cell.width - maxWidth) * 0.5F;
    // Lines too tall for the place asked for stay inside the pane: the
    // first of them is what must not be cut off.
    const float y = top
        ? cell.y + margin + pad * 0.5F
        : std::max(cell.y + pad * 0.5F, cell.bottom() - margin - metrics.height);
    const float boxWidth = metrics.width + pad * 2.0F;
    // Nothing of it may reach a neighbouring pane.
    context_->PushAxisAlignedClip(toRect(cell), D2D1_ANTIALIAS_MODE_ALIASED);
    if (background) {
        fillRound({cell.x + (cell.width - boxWidth) * 0.5F, y - pad * 0.5F, boxWidth, metrics.height + pad},
                  pad * 0.5F, rgba(0, 0, 0, 0.45F));
    }
    // The outline is the letters drawn again in black, a step off in eight
    // directions. Without the box it is all that parts white letters from
    // a white picture, so it is drawn solid and a little wider.
    const float edge = std::max(1.0F, static_cast<float>(size) * (background ? 0.06F : 0.075F));
    const float slant = edge * 0.7071F;
    brush_->SetColor(rgba(0, 0, 0, background ? 0.85F : 1.0F));
    for (const auto& [dx, dy] : std::array<std::pair<float, float>, 8>{{
             {edge, 0.0F}, {-edge, 0.0F}, {0.0F, edge}, {0.0F, -edge},
             {slant, slant}, {-slant, slant}, {slant, -slant}, {-slant, -slant}}}) {
        context_->DrawTextLayout(D2D1::Point2F(x + dx, y + dy), layout.Get(), brush_.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    brush_->SetColor(rgba(255, 255, 255, 1.0F));
    context_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    context_->PopAxisAlignedClip();
}

void Overlay::drawSubtitleImage(std::size_t pane, const OverlayPaneScene& scene, const OverlayRect& cell) {
    const auto& image = scene.subtitleImage;
    if (!context_ || pane >= subtitleSurfaces_.size() || !image) return;
    auto& surface = subtitleSurfaces_[pane];
    if (surface.source != image) {
        surface.source = image;
        surface.pieces.resize(image->pieces.size());
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        for (std::size_t index = 0; index < image->pieces.size(); ++index) {
            const SubtitlePiece& piece = image->pieces[index];
            SubtitlePieceSurface& held = surface.pieces[index];
            if (piece.width <= 0 || piece.height <= 0 ||
                piece.pixels.size() < static_cast<std::size_t>(piece.width) * static_cast<std::size_t>(piece.height)) {
                held = {};
                continue;
            }
            const UINT32 pitch = static_cast<UINT32>(piece.width) * 4U;
            if (held.bitmap && held.width == piece.width && held.height == piece.height &&
                SUCCEEDED(held.bitmap->CopyFromMemory(nullptr, piece.pixels.data(), pitch))) {
                continue;
            }
            held = {};
            if (SUCCEEDED(context_->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(piece.width), static_cast<UINT32>(piece.height)),
                    piece.pixels.data(), pitch, properties, &held.bitmap))) {
                held.width = piece.width;
                held.height = piece.height;
            }
        }
    }
    context_->PushAxisAlignedClip(toRect(cell), D2D1_ANTIALIAS_MODE_ALIASED);
    for (std::size_t index = 0; index < image->pieces.size() && index < surface.pieces.size(); ++index) {
        const SubtitlePieceSurface& held = surface.pieces[index];
        if (!held.bitmap) continue;
        const SubtitlePiece& piece = image->pieces[index];
        const float left = static_cast<float>(scene.subtitleImageX + piece.x);
        const float top = static_cast<float>(scene.subtitleImageY + piece.y);
        context_->DrawBitmap(held.bitmap.Get(),
                             D2D1::RectF(left, top, left + static_cast<float>(piece.width),
                                         top + static_cast<float>(piece.height)),
                             1.0F, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    context_->PopAxisAlignedClip();
}

void Overlay::drawPanes(const OverlayScene& scene) {
    const float s = scene.scale;
    if (scene.subtitleLocale != subtitleLocale_) {
        subtitleLocale_ = scene.subtitleLocale;
        subtitleFormats_.clear();
    }
    for (std::size_t index = 0; index < scene.panes.size(); ++index) {
        const auto& pane = scene.panes[index];
        if (!pane.active) continue;
        const OverlayRect cell = overlayRect(pane.cell);
        if (!cell.visible()) continue;

        if (pane.dragSource) {
            fillRound(cell, 0.0F, rgba(0, 0, 0, 0.35F));
        }
        if (pane.dropTarget) {
            strokeRound(cell, 0.0F, rgba(63, 132, 232, 0.95F), std::max(2.0F, 3.0F * s));
        } else if (pane.audioOn && scene.activePaneCount > 1) {
            // Which pane is being heard, without a label: a fine accent edge
            // that stays even when the chrome has faded.
            strokeRound(cell, 0.0F, rgba(63, 132, 232, 0.55F), std::max(1.0F, 2.0F * s));
        }
        if (pane.targeted && scene.activePaneCount > 1) {
            // The playback destination is independent of the blue audio
            // edge. Keep its quiet inner outline when chrome fades away.
            const float inset = 4.0F * s;
            strokeRound({cell.x + inset, cell.y + inset, cell.width - 2.0F * inset,
                         cell.height - 2.0F * inset}, 0.0F, rgba(230, 237, 248, 0.72F), std::max(1.0F, s));
        }

        // Subtitles belong to the picture, not the chrome: they stay while
        // the pill and chips have faded. libass's picture, when it drew
        // them; otherwise the plain lines, the bottom ones at their own
        // height plus what the viewer raised them by, or clear of the
        // transport bar while it is up, whichever is higher.
        if (pane.subtitleImage) drawSubtitleImage(index, pane, cell);
        if (!pane.subtitle.empty()) {
            constexpr float kPlainSubtitleHeight = 0.06F;
            const float margin = std::max({cell.height * (kPlainSubtitleHeight + scene.subtitlePosition), 4.0F * s,
                                           pane.subtitleLift > 0.0F ? pane.subtitleLift + 8.0F * s : 0.0F});
            drawSubtitle(cell, pane.subtitle, s, false, scene.subtitleSize, margin, scene.subtitleBackground);
        }
        if (!pane.subtitleTop.empty()) {
            drawSubtitle(cell, pane.subtitleTop, s, true, scene.subtitleSize,
                         std::max(cell.height * 0.05F, 8.0F * s), scene.subtitleBackground);
        }

        const float alpha = std::clamp(pane.chromeAlpha, 0.0F, 1.0F);
        if (alpha <= 0.0F) continue;
        const auto& chrome = pane.chrome;
        if (chrome.pill.visible()) {
            const float radius = chrome.pill.height * 0.5F;
            fillRound(chrome.pill, radius,
                      withAlpha(pane.pillPressed ? rgba(30, 34, 44, 0.9F)
                                : pane.pillHot ? rgba(24, 28, 36, 0.86F) : kPillFill, alpha));
            strokeRound(chrome.pill, radius,
                        withAlpha(pane.targeted ? rgba(230, 237, 248, 0.72F) : kPillEdge, alpha), std::max(1.0F, s));
            OverlayRect textBox = chrome.pill;
            textBox.x += 12.0F * s;
            textBox.width = std::max(0.0F, textBox.width - 24.0F * s);
            text(pane.label, OverlayTextStyle::BodyBold, textBox, withAlpha(kText, alpha),
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
        struct Chip { wchar_t code; const wchar_t* fallback; bool toggled; };
        const std::array<Chip, kPaneChipCount> chips{{
            {pane.audioOn ? kGlyphVolume : kGlyphMute, L"Aud", pane.audioOn},
            {pane.paused ? kGlyphPlay : kGlyphPause, pane.paused ? L"Go" : L"||", pane.paused},
            {pane.solo ? kGlyphWindowed : kGlyphFullscreen, L"Solo", pane.solo},
            {kGlyphRepeat, L"Rpt", pane.repeat},
            {kGlyphClose, L"X", false},
        }};
        for (std::size_t chip = 0; chip < chips.size(); ++chip) {
            const auto& box = chrome.chips[chip];
            if (!box.visible()) continue;
            fillRound(box, box.height * 0.5F, withAlpha(kPillFill, alpha));
            button(box, chips[chip].code, chips[chip].fallback,
                   pane.hotChip == static_cast<int>(chip),
                   pane.pressedChip == static_cast<int>(chip), chips[chip].toggled, alpha, s);
        }
        if (chrome.volume.visible()) {
            fillRound(chrome.volume, chrome.volume.height * 0.5F, withAlpha(kPillFill, alpha));
            const bool emphasised = pane.volumeHot || pane.volumeDragging;
            rail(chrome.volume, 12.0F * s, (emphasised ? 5.0F : 4.0F) * s, pane.volume01, 0.0F,
                 true, 6.0F * s, alpha);
        }

        if (chrome.timeline.visible() && pane.seekable) {
            const bool emphasised = pane.railHot || pane.railDragging;
            rail(chrome.timeline, 12.0F * s, (emphasised ? 6.0F : 3.0F) * s,
                 pane.position01, pane.buffered01, emphasised, 7.0F * s, alpha);
            if (emphasised && chrome.timeLabel.visible() && !pane.timeText.empty()) {
                const float labelWidth = std::min(
                    chrome.timeLabel.width,
                    measureText(pane.timeText, OverlayTextStyle::Small, s) + 16.0F * s);
                const OverlayRect label{chrome.timeLabel.x, chrome.timeLabel.y,
                                        labelWidth, chrome.timeLabel.height};
                fillRound(label, label.height * 0.5F, withAlpha(kPillFill, alpha));
                text(pane.timeText, OverlayTextStyle::Small, label, withAlpha(kText, alpha));
            }
        } else if (chrome.timeline.visible() && (pane.waiting || !pane.seekable)) {
            rail(chrome.timeline, 12.0F * s, 3.0F * s, 0.0F, 0.0F, false, 0.0F, alpha * 0.5F);
        }
    }
}

void Overlay::fillScrim(const OverlayRect& band, float clear, float dark, float alpha) {
    if (!band.visible() || alpha <= 0.0F) return;
    if (!scrim_) {
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0F, rgba(0, 0, 0, 0.0F)}, {0.45F, rgba(0, 0, 0, 0.42F)}, {1.0F, rgba(0, 0, 0, 0.72F)}};
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(context_->CreateGradientStopCollection(stops, 3, &collection))) {
            context_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)),
                collection.Get(), &scrim_);
        }
    }
    if (!scrim_) return;
    scrim_->SetStartPoint(D2D1::Point2F(0.0F, clear));
    scrim_->SetEndPoint(D2D1::Point2F(0.0F, dark));
    scrim_->SetOpacity(alpha);
    context_->FillRectangle(toRect(band), scrim_.Get());
}

void Overlay::drawBar(const OverlayScene& scene) {
    const float alpha = std::clamp(scene.barAlpha, 0.0F, 1.0F);
    if (alpha <= 0.0F) return;
    const float s = scene.scale;
    const auto& bar = scene.bar;
    fillScrim(bar.band, bar.band.y, bar.band.bottom(), alpha);
    const auto hot = [&](BarItem item) { return scene.hotBarItem == static_cast<int>(item); };
    const auto pressed = [&](BarItem item) { return scene.pressedBarItem == static_cast<int>(item); };
    if (scene.activePaneCount > 1 && bar.scopeLabel.visible()) {
        fillRound(bar.scopeLabel, 9.0F * s, withAlpha(kPillFill, alpha));
        text(L"All", OverlayTextStyle::Small, bar.scopeLabel, withAlpha(kTextDim, alpha),
             DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
    if (bar[BarItem::Previous].visible()) {
        button(bar[BarItem::Previous], kGlyphPrevious, L"<<", hot(BarItem::Previous),
               pressed(BarItem::Previous), false, alpha, s);
    }
    button(bar[BarItem::Play], scene.playing ? kGlyphPause : kGlyphPlay,
           scene.playing ? L"Pause" : L"Play", hot(BarItem::Play), pressed(BarItem::Play),
           false, alpha, s);
    if (bar[BarItem::Next].visible()) {
        button(bar[BarItem::Next], kGlyphNext, L">>", hot(BarItem::Next),
               pressed(BarItem::Next), false, alpha, s);
    }
    button(bar[BarItem::Stop], kGlyphStop, L"Stop", hot(BarItem::Stop), pressed(BarItem::Stop),
           false, alpha, s);
    text(scene.timeText, OverlayTextStyle::Body, bar[BarItem::Time], withAlpha(kText, alpha),
         DWRITE_TEXT_ALIGNMENT_CENTER, true);
    if (bar[BarItem::Seek].visible()) {
        const bool emphasised = scene.seekHot || scene.seekDragging;
        rail(bar[BarItem::Seek], 8.0F * s, (emphasised ? 6.0F : 4.0F) * s,
             scene.seekable ? scene.seek01 : 0.0F, 0.0F, scene.seekable && emphasised,
             7.0F * s, scene.seekable ? alpha : alpha * 0.45F);
        if (emphasised && scene.seekHoverX >= 0.0F && !scene.seekHoverText.empty()) {
            const float width = measureText(scene.seekHoverText, OverlayTextStyle::Small, s) + 16.0F * s;
            const float height = 22.0F * s;
            OverlayRect bubble{scene.seekHoverX - width * 0.5F,
                               bar[BarItem::Seek].y - height - 4.0F * s, width, height};
            bubble.x = std::clamp(bubble.x, 0.0F, std::max(0.0F, scene.width - width));
            fillRound(bubble, height * 0.5F, withAlpha(kPillFill, alpha));
            text(scene.seekHoverText, OverlayTextStyle::Small, bubble, withAlpha(kText, alpha));
        }
    }
    button(bar[BarItem::Audio], scene.muted ? kGlyphMute : kGlyphVolume,
           scene.muted ? L"Muted" : L"Audio", hot(BarItem::Audio), pressed(BarItem::Audio),
           scene.muted, alpha, s);
    if (bar[BarItem::Volume].visible()) {
        // The popup above the speaker: a pill with the rail standing in it.
        const auto& popup = bar[BarItem::Volume];
        const OverlayRect pill{popup.x, popup.y, popup.width, std::max(0.0F, popup.height - 4.0F * s)};
        fillRound(pill, popup.width * 0.5F, withAlpha(kPillFill, alpha));
        verticalRail(pill, 12.0F * s, 4.0F * s, scene.volume01, 6.0F * s, alpha);
    }
    button(bar[BarItem::Subtitles], kGlyphSubtitles, L"CC", hot(BarItem::Subtitles),
           pressed(BarItem::Subtitles), scene.subtitlesOn, alpha, s);
    if (bar[BarItem::Layout].visible()) {
        button(bar[BarItem::Layout], kGlyphLayout, L"Grid", hot(BarItem::Layout),
               pressed(BarItem::Layout), false, alpha, s);
    }
    button(bar[BarItem::Menu], kGlyphMenu, L"Menu", hot(BarItem::Menu), pressed(BarItem::Menu),
           false, alpha, s);
    button(bar[BarItem::Fullscreen], scene.fullscreen ? kGlyphWindowed : kGlyphFullscreen,
           scene.fullscreen ? L"Exit" : L"Full", hot(BarItem::Fullscreen),
           pressed(BarItem::Fullscreen), scene.fullscreen, alpha, s);
    button(bar[BarItem::Settings], scene.pinned ? kGlyphPin : kGlyphSettings,
           scene.pinned ? L"Pinned" : L"Setup", hot(BarItem::Settings),
           pressed(BarItem::Settings), scene.pinned, alpha, s);
}

void Overlay::drawCaptionBand(const OverlayScene& scene) {
    const float alpha = std::clamp(scene.captionAlpha, 0.0F, 1.0F);
    const auto& band = scene.caption.band;
    fillScrim(band, band.bottom(), band.y, alpha);
}

void Overlay::drawCaption(const OverlayScene& scene) {
    const float alpha = std::clamp(scene.captionAlpha, 0.0F, 1.0F);
    if (alpha <= 0.0F || !scene.caption.row.visible()) return;
    const auto& caption = scene.caption;
    text(scene.captionTitle, OverlayTextStyle::Small, caption.title, withAlpha(kText, alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    struct Button { CaptionItem item; wchar_t code; const wchar_t* fallback; };
    const std::array<Button, kCaptionItemCount> buttons{{
        {CaptionItem::Minimize, kGlyphChromeMinimize, L"_"},
        {CaptionItem::Maximize, scene.maximized ? kGlyphChromeRestore : kGlyphChromeMaximize,
         scene.maximized ? L"[]" : L"[ ]"},
        {CaptionItem::Close, kGlyphChromeClose, L"X"},
    }};
    for (const auto& button : buttons) {
        const auto& box = caption[button.item];
        if (!box.visible()) continue;
        const int index = static_cast<int>(button.item);
        const bool hot = scene.hotCaptionItem == index;
        const bool pressed = scene.pressedCaptionItem == index;
        // Square, edge to edge, and Close turns red: the buttons every
        // Windows title bar has.
        if (button.item == CaptionItem::Close && (hot || pressed)) {
            fillRound(box, 0.0F, withAlpha(rgba(196, 43, 28, pressed ? 0.82F : 1.0F), alpha));
        } else if (pressed) {
            fillRound(box, 0.0F, withAlpha(rgba(255, 255, 255, 0.20F), alpha));
        } else if (hot) {
            fillRound(box, 0.0F, withAlpha(rgba(255, 255, 255, 0.12F), alpha));
        }
        if (formats_.captionIcon) {
            text(std::wstring(1, button.code), OverlayTextStyle::CaptionIcon, box, withAlpha(kText, alpha));
        } else {
            text(button.fallback, OverlayTextStyle::Small, box, withAlpha(kText, alpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
    }
}

void Overlay::drawNotice(const OverlayScene& scene) {
    const float alpha = std::clamp(scene.noticeAlpha, 0.0F, 1.0F);
    if (alpha <= 0.0F || scene.notice.empty()) return;
    const float s = scene.scale;
    const float height = 36.0F * s;
    const float width = std::min(scene.width - 32.0F * s,
                                 measureText(scene.notice, OverlayTextStyle::BodyBold, s) + 32.0F * s);
    if (width <= 0.0F) return;
    // Below the caption while it is up, so the two never overlap.
    const float top = std::max(20.0F * s, scene.caption.row.bottom() * scene.captionAlpha + 8.0F * s);
    const OverlayRect box{(scene.width - width) * 0.5F, top, width, height};
    fillRound(box, height * 0.5F, withAlpha(kPillFill, alpha));
    strokeRound(box, height * 0.5F, withAlpha(kPillEdge, alpha), std::max(1.0F, s));
    text(scene.notice, OverlayTextStyle::BodyBold, box, withAlpha(kText, alpha),
         DWRITE_TEXT_ALIGNMENT_CENTER, true);
}

void Overlay::drawTooltip(const OverlayScene& scene) {
    if (scene.tooltip.empty() || !scene.tooltipAnchor.visible()) return;
    const float s = scene.scale;
    const float height = 24.0F * s;
    const float width = measureText(scene.tooltip, OverlayTextStyle::Small, s) + 16.0F * s;
    OverlayRect box{scene.tooltipAnchor.x + (scene.tooltipAnchor.width - width) * 0.5F,
                    scene.tooltipAnchor.y - height - 6.0F * s, width, height};
    box.x = std::clamp(box.x, 4.0F * s, std::max(4.0F * s, scene.width - width - 4.0F * s));
    if (box.y < 4.0F * s) box.y = scene.tooltipAnchor.bottom() + 6.0F * s;
    fillRound(box, 6.0F * s, rgba(10, 12, 16, 0.9F));
    strokeRound(box, 6.0F * s, kPillEdge, std::max(1.0F, s));
    text(scene.tooltip, OverlayTextStyle::Small, box, kText);
}

void Overlay::toggleSwitch(const OverlayRect& box, bool on, bool hot, float alpha, float scale) {
    if (!box.visible()) return;
    const float radius = box.height * 0.5F;
    D2D1_COLOR_F track = on ? rgba(63, 132, 232, 0.9F) : rgba(255, 255, 255, 0.22F);
    if (hot) track.a = std::min(1.0F, track.a + 0.1F);
    fillRound(box, radius, withAlpha(track, alpha));
    const float knob = box.height - 6.0F * scale;
    const float x = on ? box.right() - 3.0F * scale - knob : box.x + 3.0F * scale;
    brush_->SetColor(withAlpha(kText, alpha));
    context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + knob * 0.5F, box.y + box.height * 0.5F),
                                        knob * 0.5F, knob * 0.5F), brush_.Get());
}

void Overlay::drawPanel(const OverlayScene& scene) {
    const auto& panel = scene.panel;
    const float alpha = std::clamp(panel.alpha, 0.0F, 1.0F);
    if (alpha <= 0.0F || !panel.layout.sheet.visible()) return;
    const float s = scene.scale;
    const auto& layout = panel.layout;
    fillRound(layout.sheet, 0.0F, withAlpha(rgba(12, 14, 18, 0.94F), alpha));
    for (const auto& row : panel.rows) {
        if (row.kind != PanelRowKind::MediaDetail) continue;
        drawMediaBackdrop(row.mediaDetail, layout.content, alpha, panel.thumbnails);
        break;
    }
    fillRound({layout.sheet.x, layout.sheet.y, std::max(1.0F, s), layout.sheet.height}, 0.0F,
              withAlpha(rgba(255, 255, 255, 0.10F), alpha));
    const float pad = 20.0F * s;
    const float titleRight = (layout.sheetSwitch.visible() ? layout.sheetSwitch.x : layout.close.x) - 8.0F * s;
    text(panel.title, OverlayTextStyle::Title,
         {layout.header.x + pad, layout.header.y,
          std::max(0.0F, titleRight - layout.header.x - pad), layout.header.height},
         withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    button(layout.close, kGlyphClose, L"X", panel.hotKind == PanelHitKind::Close,
           panel.pressedKind == PanelHitKind::Close, false, alpha, s);
    if (layout.sheetSwitch.visible() && !panel.switchCaption.empty()) {
        // The switch to the sheet's other face, drawn as a chosen segment so
        // it reads as a button beside the plain close glyph.
        const bool hot = panel.hotKind == PanelHitKind::Switch;
        const bool pressed = panel.pressedKind == PanelHitKind::Switch;
        fillRound(layout.sheetSwitch, 8.0F * s, withAlpha(pressed ? rgba(255, 255, 255, 0.24F)
            : rgba(63, 132, 232, hot ? 0.48F : 0.32F), alpha));
        strokeRound(layout.sheetSwitch, 8.0F * s, withAlpha(rgba(63, 132, 232, 0.9F), alpha), std::max(1.0F, s));
        text(panel.switchCaption, OverlayTextStyle::Small, layout.sheetSwitch,
             withAlpha(rgba(190, 216, 255, 1.0F), alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
    fillRound({layout.header.x, layout.header.bottom() - std::max(1.0F, s), layout.header.width,
               std::max(1.0F, s)}, 0.0F, withAlpha(rgba(255, 255, 255, 0.08F), alpha));
    if (layout.tabStrip.visible()) {
        // The tabs are drawn as a Choice row's segments: the chosen one in
        // the accent, the strip closed by a rule the content scrolls under.
        for (std::size_t i = 0; i < layout.tabs.size() && i < panel.tabs.size(); ++i) {
            const auto& box = layout.tabs[i];
            const bool selected = panel.tab == static_cast<int>(i);
            const bool hot = panel.hotKind == PanelHitKind::Tab && panel.hotPart == static_cast<int>(i);
            const bool pressed = panel.pressedKind == PanelHitKind::Tab && panel.pressedPart == static_cast<int>(i);
            D2D1_COLOR_F fill = selected ? rgba(63, 132, 232, 0.35F) : rgba(255, 255, 255, 0.06F);
            if (pressed) fill = rgba(255, 255, 255, 0.24F);
            else if (hot) fill.a += 0.08F;
            fillRound(box, 8.0F * s, withAlpha(fill, alpha));
            if (selected) strokeRound(box, 8.0F * s, withAlpha(rgba(63, 132, 232, 0.9F), alpha), std::max(1.0F, s));
            text(panel.tabs[i], OverlayTextStyle::Small, box,
                 withAlpha(selected ? rgba(190, 216, 255, 1.0F) : kText, alpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
        fillRound({layout.tabStrip.x, layout.tabStrip.bottom() - std::max(1.0F, s), layout.tabStrip.width,
                   std::max(1.0F, s)}, 0.0F, withAlpha(rgba(255, 255, 255, 0.08F), alpha));
    }

    context_->PushAxisAlignedClip(toRect(layout.content), D2D1_ANTIALIAS_MODE_ALIASED);
    const D2D1_COLOR_F dim = withAlpha(kTextDim, alpha);
    for (std::size_t i = 0; i < panel.rows.size() && i < layout.rows.size(); ++i) {
        const auto& row = panel.rows[i];
        const auto& geometry = layout.rows[i];
        if (geometry.row.bottom() < layout.content.y || geometry.row.y > layout.content.bottom()) continue;
        const float rowAlpha = row.enabled ? alpha : alpha * 0.4F;
        const D2D1_COLOR_F textColor = withAlpha(kText, rowAlpha);
        const bool hotRow = panel.hotRow == static_cast<int>(i);
        const bool pressedRow = panel.pressedRow == static_cast<int>(i);
        const OverlayRect labelLine{geometry.row.x, geometry.row.y, geometry.row.width, 24.0F * s};
        const auto drawAdd = [&](const OverlayRect& box, int part) {
            if (!box.visible()) return;
            const bool hot = row.enabled && hotRow && panel.hotKind == PanelHitKind::Add && panel.hotPart == part;
            const bool pressed = row.enabled && pressedRow && panel.pressedKind == PanelHitKind::Add && panel.pressedPart == part;
            fillRound(box, 7.0F * s, withAlpha(pressed ? rgba(43, 102, 195, 0.98F)
                : hot ? rgba(63, 132, 232, 0.96F) : rgba(12, 20, 32, 0.88F), rowAlpha));
            strokeRound(box, 7.0F * s, withAlpha(rgba(172, 205, 252, hot ? 0.9F : 0.45F), rowAlpha), std::max(1.0F, s));
            text(box.width >= 48.0F * s ? L"+ Add" : L"+", OverlayTextStyle::Small, box,
                 withAlpha(kText, rowAlpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
        };
        switch (row.kind) {
        case PanelRowKind::MediaDetail:
            drawMediaDetail(row, geometry,
                row.enabled && hotRow && panel.hotKind == PanelHitKind::Button ? panel.hotPart : -1,
                row.enabled && pressedRow && panel.pressedKind == PanelHitKind::Button ? panel.pressedPart : -1,
                rowAlpha, s, panel.thumbnails);
            break;
        case PanelRowKind::Header:
            if (i > 0) {
                fillRound({geometry.row.x, geometry.row.y + 4.0F * s, geometry.row.width, std::max(1.0F, s)},
                          0.0F, withAlpha(rgba(255, 255, 255, 0.08F), alpha));
            }
            text(row.label, OverlayTextStyle::BodyBold,
                 {geometry.row.x, geometry.row.y + 12.0F * s, geometry.row.width, geometry.row.height - 12.0F * s},
                 withAlpha(rgba(150, 190, 245, 1.0F), alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            break;
        case PanelRowKind::Note:
            if (row.wrapNote) {
                mediaText(row.label, OverlayTextStyle::Small, panelWrappedNoteTextBox(geometry.row, s),
                          kPanelWrappedNoteLineHeight * s, dim);
            } else {
                text(row.label, OverlayTextStyle::Small,
                     {geometry.row.x, geometry.row.y + 2.0F * s, geometry.row.width, geometry.row.height - 2.0F * s},
                     dim, DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
            }
            break;
        case PanelRowKind::Toggle:
            text(row.label, OverlayTextStyle::Body,
                 {geometry.row.x, geometry.row.y, geometry.row.width - geometry.control.width - 12.0F * s,
                  geometry.row.height},
                 textColor, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            toggleSwitch(geometry.control, row.on, hotRow && row.enabled, rowAlpha, s);
            break;
        case PanelRowKind::Slider: {
            text(row.label, OverlayTextStyle::Body, labelLine, textColor, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            text(row.value, OverlayTextStyle::Small, labelLine, withAlpha(kTextDim, rowAlpha),
                 DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            const bool emphasised = row.enabled && (hotRow || panel.dragRow == static_cast<int>(i));
            rail(geometry.control, 8.0F * s, (emphasised ? 6.0F : 4.0F) * s, row.slider01, 0.0F,
                 true, 7.0F * s, rowAlpha);
            if (geometry.trailing.visible()) {
                button(geometry.trailing, row.trailingOn ? kGlyphMute : kGlyphVolume,
                       row.trailingOn ? L"M" : L"S",
                       hotRow && panel.hotKind == PanelHitKind::Trailing,
                       pressedRow && panel.pressedKind == PanelHitKind::Trailing,
                       row.trailingOn, rowAlpha, s);
            }
            break;
        }
        case PanelRowKind::Choice:
            text(row.label, OverlayTextStyle::Small, labelLine, withAlpha(kTextDim, rowAlpha),
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const auto& box = geometry.parts[part];
                const bool selected = row.selected == static_cast<int>(part);
                const bool hot = hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Segment;
                const bool pressed = pressedRow && panel.pressedPart == static_cast<int>(part);
                D2D1_COLOR_F fill = selected ? rgba(63, 132, 232, 0.35F) : rgba(255, 255, 255, 0.08F);
                if (pressed) fill = rgba(255, 255, 255, 0.24F);
                else if (hot) fill.a += 0.08F;
                fillRound(box, 8.0F * s, withAlpha(fill, rowAlpha));
                if (selected) {
                    strokeRound(box, 8.0F * s, withAlpha(rgba(63, 132, 232, 0.9F), rowAlpha), std::max(1.0F, s));
                }
                text(row.options[part], OverlayTextStyle::Small, box,
                     withAlpha(selected ? rgba(190, 216, 255, 1.0F) : kText, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            break;
        case PanelRowKind::Item: {
            if (row.enabled && (hotRow || pressedRow)) {
                fillRound(geometry.row, 8.0F * s,
                          withAlpha(rgba(255, 255, 255, pressedRow ? 0.16F : 0.08F), alpha));
            }
            if (row.current) {
                // The item being played: a bar at the left and its name in the accent.
                fillRound(geometry.row, 8.0F * s, withAlpha(rgba(63, 132, 232, 0.14F), alpha));
                fillRound({geometry.row.x, geometry.row.y + 8.0F * s, 3.0F * s, geometry.row.height - 16.0F * s},
                          1.5F * s, withAlpha(rgba(63, 132, 232, 0.95F), alpha));
            }
            const OverlayRect add = geometry.addParts.empty() ? OverlayRect{} : geometry.addParts[0];
            const float textRight = add.visible() ? add.x - 8.0F * s : geometry.row.right();
            const float available = std::max(0.0F, textRight - geometry.row.x);
            const float valueWidth = row.value.empty() ? 0.0F : std::min(
                available * 0.4F, measureText(row.value, OverlayTextStyle::Small, s) + 8.0F * s);
            // The tag sits just before the value, so a column of them runs
            // down the list beside the lengths.
            const float tagWidth = row.tag.empty() ? 0.0F : std::min(
                available * 0.2F, measureText(row.tag, OverlayTextStyle::Small, s) + 14.0F * s);
            const float tagSpace = tagWidth > 0.0F ? tagWidth + 6.0F * s : 0.0F;
            const OverlayRect labelBox{geometry.row.x + 8.0F * s, geometry.row.y,
                                       std::max(0.0F, available - valueWidth - tagSpace - 20.0F * s),
                                       geometry.row.height};
            text(row.label, OverlayTextStyle::Body, labelBox,
                 row.current ? withAlpha(rgba(190, 216, 255, 1.0F), rowAlpha) : textColor,
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (valueWidth > 0.0F) {
                const OverlayRect valueBox{textRight - valueWidth - 8.0F * s, geometry.row.y,
                                           valueWidth, geometry.row.height};
                text(row.value, OverlayTextStyle::Small, valueBox,
                     withAlpha(row.on ? rgba(150, 190, 245, 1.0F) : kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            }
            if (tagWidth > 0.0F) {
                const float height = 20.0F * s;
                const OverlayRect tagBox{textRight - valueWidth - 8.0F * s - tagSpace,
                                         geometry.row.y + (geometry.row.height - height) * 0.5F, tagWidth, height};
                fillRound(tagBox, height * 0.5F, withAlpha(rgba(63, 132, 232, 0.32F), rowAlpha));
                text(row.tag, OverlayTextStyle::Small, tagBox, withAlpha(rgba(190, 216, 255, 1.0F), rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            drawAdd(add, 0);
            break;
        }
        case PanelRowKind::Tiles:
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const bool hot = hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Tile;
                const bool pressed = pressedRow && panel.pressedPart == static_cast<int>(part) &&
                                     panel.pressedKind == PanelHitKind::Tile;
                drawTile(row, part, geometry.parts[part], hot && row.enabled, pressed, rowAlpha, s,
                         panel.thumbnails);
                if (part < geometry.addParts.size()) drawAdd(geometry.addParts[part], static_cast<int>(part));
            }
            break;
        case PanelRowKind::Buttons:
            if (!row.label.empty()) {
                text(row.label, OverlayTextStyle::Small, labelLine, withAlpha(kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
                text(row.value, OverlayTextStyle::Small, labelLine, textColor,
                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            }
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const auto& box = geometry.parts[part];
                const bool hot = hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Button;
                const bool pressed = pressedRow && panel.pressedPart == static_cast<int>(part);
                D2D1_COLOR_F fill = rgba(255, 255, 255, 0.10F);
                if (pressed) fill = rgba(255, 255, 255, 0.24F);
                else if (hot) fill = rgba(255, 255, 255, 0.18F);
                fillRound(box, 8.0F * s, withAlpha(fill, rowAlpha));
                text(row.options[part], OverlayTextStyle::Body, box, textColor,
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            break;
        }
    }
    context_->PopAxisAlignedClip();
    trimBitmaps();
    // The scrollbar: a thin thumb that widens under the pointer or a drag.
    const OverlayRect thumb = panelScrollThumb(layout, s);
    if (thumb.visible()) {
        const bool active = panel.scrollbarHot || panel.scrollbarDragging;
        const float width = (active ? 6.0F : 3.0F) * s;
        fillRound({layout.sheet.right() - 3.0F * s - width, thumb.y, width, thumb.height}, width * 0.5F,
                  withAlpha(rgba(255, 255, 255, active ? 0.55F : 0.25F), alpha));
    }
}

void Overlay::drawEmptyHint(const OverlayScene& scene) {
    if (!scene.showEmptyHint) return;
    const float s = scene.scale;
    const float centerY = scene.height * 0.5F;
    text(L"Drop videos here", OverlayTextStyle::Title,
         {0.0F, centerY - 40.0F * s, scene.width, 36.0F * s}, kTextDim);
    text(L"O  open      right-click  menu      F5  settings      F6  Emby", OverlayTextStyle::Body,
         {0.0F, centerY + 2.0F * s, scene.width, 24.0F * s}, rgba(120, 126, 138, 1.0F));
}

// ---------------------------------------------------------------------------
// Browser tiles

namespace {

constexpr int kDecodesPerFrame = 6;
constexpr std::size_t kBitmapsKept = 160;
constexpr std::size_t kBitmapsLimit = 220;

}  // namespace

ID2D1Bitmap* Overlay::bitmapFor(const std::string& key, const ThumbnailCache& cache) {
    if (key.empty() || !context_) return nullptr;
    if (const auto found = bitmaps_.find(key); found != bitmaps_.end()) {
        found->second.used = frame_;
        return found->second.bitmap.Get();
    }
    if (!wic_ || decodesThisFrame_ >= kDecodesPerFrame) return nullptr;
    const auto bytes = cache.find(key);
    if (!bytes || bytes->empty()) return nullptr;  // not fetched yet, or nothing to show
    ++decodesThisFrame_;
    CachedBitmap entry;
    entry.used = frame_;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    // The bytes outlive the stream: the decode finishes inside this call and
    // CreateBitmapFromWicBitmap copies the pixels.
    if (SUCCEEDED(wic_->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes->data())), static_cast<DWORD>(bytes->size()))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(wic_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                        nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
        if (FAILED(context_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &entry.bitmap))) {
            entry.bitmap.Reset();
        }
    }
    // A failure is remembered too, so the same bytes are not tried every frame.
    ID2D1Bitmap* result = entry.bitmap.Get();
    bitmaps_[key] = std::move(entry);
    return result;
}

void Overlay::trimBitmaps() {
    if (bitmaps_.size() <= kBitmapsLimit) return;
    std::vector<std::pair<std::uint64_t, std::string>> byAge;
    byAge.reserve(bitmaps_.size());
    for (const auto& [key, entry] : bitmaps_) byAge.emplace_back(entry.used, key);
    std::sort(byAge.begin(), byAge.end());
    for (std::size_t i = 0; i < byAge.size() && bitmaps_.size() > kBitmapsKept; ++i) {
        bitmaps_.erase(byAge[i].second);
    }
}

void Overlay::drawMediaBackdrop(const PanelMediaDetail& detail, const OverlayRect& box,
                                float alpha, const ThumbnailCache* cache) {
    if (!box.visible() || !cache) return;
    ID2D1Bitmap* bitmap = bitmapFor(detail.backdropKey, *cache);
    if (!bitmap) return;
    const D2D1_SIZE_F size = bitmap->GetSize();
    if (size.width <= 0.0F || size.height <= 0.0F) return;
    // Cover the panel, independent of the detail row's scroll position.
    const float fit = std::max(box.width / size.width, box.height / size.height);
    const float sourceWidth = box.width / fit, sourceHeight = box.height / fit;
    const D2D1_RECT_F source = D2D1::RectF((size.width - sourceWidth) * 0.5F,
        (size.height - sourceHeight) * 0.5F, (size.width + sourceWidth) * 0.5F,
        (size.height + sourceHeight) * 0.5F);
    context_->PushAxisAlignedClip(toRect(box), D2D1_ANTIALIAS_MODE_ALIASED);
    context_->DrawBitmap(bitmap, toRect(box), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
    // A uniform layer keeps bright artwork legible at the top; the lower
    // gradient settles synopsis and season/episode controls onto darkness.
    fillRound(box, 0.0F, withAlpha(rgba(8, 10, 15, 0.56F), alpha));
    if (!mediaScrim_) {
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0F, rgba(8, 10, 15, 0.10F)}, {0.55F, rgba(8, 10, 15, 0.35F)},
            {1.0F, rgba(8, 10, 15, 0.90F)}};
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(context_->CreateGradientStopCollection(stops, 3, &collection))) {
            context_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)),
                collection.Get(), &mediaScrim_);
        }
    }
    if (mediaScrim_) {
        mediaScrim_->SetStartPoint(D2D1::Point2F(box.x, box.y));
        mediaScrim_->SetEndPoint(D2D1::Point2F(box.x, box.bottom()));
        mediaScrim_->SetOpacity(alpha);
        context_->FillRectangle(toRect(box), mediaScrim_.Get());
    }
    context_->PopAxisAlignedClip();
}

void Overlay::drawMediaDetail(const PanelRow& row, const PanelRowGeometry& rowGeometry,
                              int hotPart, int pressedPart, float alpha, float s,
                              const ThumbnailCache* cache) {
    const auto& detail = row.mediaDetail;
    const auto& geometry = rowGeometry.mediaDetail;
    const auto contain = [&](ID2D1Bitmap* bitmap, const OverlayRect& box, bool centered) {
        if (!bitmap || !box.visible()) return false;
        const D2D1_SIZE_F size = bitmap->GetSize();
        if (size.width <= 0.0F || size.height <= 0.0F) return false;
        const float fit = std::min(box.width / size.width, box.height / size.height);
        const float width = size.width * fit, height = size.height * fit;
        const OverlayRect destination{box.x + (centered ? (box.width - width) * 0.5F : 0.0F),
                                      box.y + (box.height - height) * 0.5F, width, height};
        context_->DrawBitmap(bitmap, toRect(destination), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return true;
    };
    fillRound(geometry.poster, 10.0F * s, withAlpha(rgba(22, 27, 36, 0.90F), alpha));
    ID2D1Bitmap* poster = cache ? bitmapFor(detail.posterKey, *cache) : nullptr;
    if (!contain(poster, geometry.poster, true) && !detail.title.empty()) {
        std::wstring initial(1, detail.title.front());
        if (initial[0] >= 0xD800 && initial[0] <= 0xDBFF && detail.title.size() > 1) initial += detail.title[1];
        text(initial, OverlayTextStyle::MediaTitle, geometry.poster, withAlpha(kTextDim, alpha * 0.45F));
    }
    strokeRound(geometry.poster, 10.0F * s, withAlpha(rgba(255, 255, 255, 0.12F), alpha), std::max(1.0F, s));

    ID2D1Bitmap* logo = cache ? bitmapFor(detail.logoKey, *cache) : nullptr;
    if (!contain(logo, geometry.title, false)) {
        mediaText(detail.title, geometry.stacked ? OverlayTextStyle::MediaTitleCompact : OverlayTextStyle::MediaTitle,
                  geometry.title, (geometry.stacked ? 34.0F : 42.0F) * s, withAlpha(kText, alpha));
    }
    mediaText(detail.metadata, OverlayTextStyle::Body, geometry.metadata, 22.0F * s, withAlpha(kText, alpha));
    mediaText(detail.mediaInfo, OverlayTextStyle::Small, geometry.mediaInfo, 20.0F * s, withAlpha(kTextDim, alpha));
    if (geometry.progress.visible()) {
        fillRound(geometry.progress, 5.0F * s, withAlpha(rgba(255, 255, 255, 0.20F), alpha));
        const float progress = std::isfinite(detail.progress) ? std::clamp(detail.progress, 0.0F, 1.0F) : 0.0F;
        fillRound({geometry.progress.x, geometry.progress.y, geometry.progress.width * progress,
                   geometry.progress.height}, 5.0F * s, withAlpha(kAccent, alpha));
        mediaText(detail.progressText, OverlayTextStyle::Small, geometry.progressText, 20.0F * s,
                  withAlpha(kTextDim, alpha));
    }
    mediaText(detail.overview, OverlayTextStyle::Body, geometry.overview, 22.0F * s, withAlpha(kText, alpha));
    for (std::size_t part = 0; part < geometry.actions.size() && part < row.options.size(); ++part) {
        const auto& box = geometry.actions[part];
        if (!box.visible() || row.options[part].empty()) continue;
        const bool hot = hotPart == static_cast<int>(part), pressed = pressedPart == static_cast<int>(part);
        D2D1_COLOR_F fill = part == 0 ? kAccent : rgba(255, 255, 255, part == 2 ? 0.04F : 0.12F);
        if (pressed) fill = part == 0 ? rgba(43, 102, 195, 1.0F) : rgba(255, 255, 255, 0.24F);
        else if (hot) fill = part == 0 ? rgba(85, 150, 244, 1.0F) : rgba(255, 255, 255, 0.18F);
        fillRound(box, 9.0F * s, withAlpha(fill, alpha));
        if (part != 0 && part != 2) {
            strokeRound(box, 9.0F * s, withAlpha(rgba(255, 255, 255, 0.14F), alpha), std::max(1.0F, s));
        }
        text(row.options[part], part == 0 ? OverlayTextStyle::BodyBold : OverlayTextStyle::Body, box,
             withAlpha(part == 2 ? rgba(172, 205, 252, 1.0F) : kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
}

void Overlay::drawTile(const PanelRow& row, std::size_t part, const OverlayRect& box, bool hot, bool pressed,
                       float alpha, float s, const ThumbnailCache* cache) {
    const float pictureHeight = box.width / std::max(0.1F, row.tileAspect);
    const OverlayRect picture{box.x, box.y, box.width, pictureHeight};
    const float radius = 8.0F * s;
    fillRound(picture, radius, withAlpha(rgba(255, 255, 255, pressed ? 0.20F : hot ? 0.14F : 0.07F), alpha));
    const std::string& key = part < row.tileKeys.size() ? row.tileKeys[part] : std::string();
    ID2D1Bitmap* bitmap = cache ? bitmapFor(key, *cache) : nullptr;
    if (bitmap) {
        // The whole picture inside the frame, letterboxed: a portrait phone
        // clip keeps its head, a poster keeps its title.
        const D2D1_SIZE_F size = bitmap->GetSize();
        const float fit = std::min(picture.width / std::max(1.0F, size.width),
                                   picture.height / std::max(1.0F, size.height));
        const float width = size.width * fit;
        const float height = size.height * fit;
        const OverlayRect dest{picture.x + (picture.width - width) * 0.5F,
                               picture.y + (picture.height - height) * 0.5F, width, height};
        context_->PushAxisAlignedClip(toRect(picture), D2D1_ANTIALIAS_MODE_ALIASED);
        context_->DrawBitmap(bitmap, toRect(dest), alpha * (pressed ? 0.75F : 1.0F),
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        context_->PopAxisAlignedClip();
    } else if (!row.options[part].empty()) {
        // No picture (yet): the title's first character, large and dim.
        std::wstring initial(1, row.options[part][0]);
        if (initial[0] >= 0xD800 && initial[0] <= 0xDBFF && row.options[part].size() > 1) {
            initial += row.options[part][1];
        }
        text(initial, OverlayTextStyle::Title, picture, withAlpha(kTextDim, alpha * 0.6F));
    }
    const bool current = row.selected == static_cast<int>(part);
    if (hot || pressed || current) {
        strokeRound(picture, radius, withAlpha(rgba(63, 132, 232, current ? 1.0F : 0.9F), alpha),
                    std::max(1.0F, (current ? 3.0F : 2.0F) * s));
    }
    // How far it was watched, along the picture's bottom edge.
    const float progress = part < row.tileProgress.size() ? std::clamp(row.tileProgress[part], 0.0F, 1.0F) : 0.0F;
    if (progress > 0.0F) {
        const OverlayRect track{picture.x + 6.0F * s, picture.bottom() - 7.0F * s, picture.width - 12.0F * s, 3.0F * s};
        fillRound(track, 1.5F * s, withAlpha(rgba(0, 0, 0, 0.55F), alpha));
        fillRound({track.x, track.y, track.width * progress, track.height}, 1.5F * s,
                  withAlpha(rgba(63, 132, 232, 1.0F), alpha));
    }
    // A caption in a pill at a corner of the picture: at the top, or at the
    // bottom above the watched bar.
    const auto pill = [&](const std::wstring& caption, bool left, bool top, D2D1_COLOR_F fill) {
        if (caption.empty()) return;
        const float height = 20.0F * s;
        const float width = std::min(picture.width * 0.5F - 8.0F * s,
                                     measureText(caption, OverlayTextStyle::Small, s) + 14.0F * s);
        if (width <= 0.0F) return;
        const float y = top ? picture.y + 6.0F * s
                            : picture.bottom() - (progress > 0.0F ? 13.0F : 6.0F) * s - height;
        const OverlayRect pillBox{left ? picture.x + 6.0F * s : picture.right() - width - 6.0F * s,
                                  y, width, height};
        fillRound(pillBox, height * 0.5F, withAlpha(fill, alpha));
        text(caption, OverlayTextStyle::Small, pillBox, withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
    };
    // Watched state at the left, length at the right: both, side by side;
    // a tag in the accent below them.
    const D2D1_COLOR_F shade = rgba(0, 0, 0, 0.65F);
    if (part < row.tileMarks.size()) pill(row.tileMarks[part], true, true, shade);
    if (part < row.tileBadges.size()) pill(row.tileBadges[part], false, true, shade);
    if (part < row.tileTags.size()) pill(row.tileTags[part], true, false, rgba(63, 132, 232, 0.92F));
    const OverlayRect labelBox{box.x, picture.bottom() + 4.0F * s, box.width,
                               std::max(0.0F, box.height - pictureHeight - 4.0F * s)};
    text(row.options[part], OverlayTextStyle::Small, labelBox,
         withAlpha(current ? rgba(190, 216, 255, 1.0F) : kText, alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
}

}  // namespace quaddeck
