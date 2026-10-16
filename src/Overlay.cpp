#include "Overlay.hpp"

#include "LocalPlaylist.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cwchar>
#include <functional>
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
constexpr wchar_t kGlyphFullscreen = L'\xE740';
constexpr wchar_t kGlyphWindowed = L'\xE73F';
constexpr wchar_t kGlyphRepeat = L'\xE8EE';
constexpr wchar_t kGlyphClose = L'\xE711';
constexpr wchar_t kGlyphPrevious = L'\xE892';
constexpr wchar_t kGlyphNext = L'\xE893';
constexpr wchar_t kGlyphSubtitles = L'\xED1E';
constexpr wchar_t kGlyphLayout = L'\xE8A9';   // ViewAll
constexpr wchar_t kGlyphChromeMinimize = L'\xE921';
constexpr wchar_t kGlyphChromeMaximize = L'\xE922';
constexpr wchar_t kGlyphChromeRestore = L'\xE923';
constexpr wchar_t kGlyphChromeClose = L'\xE8BB';
constexpr wchar_t kGlyphHeartFill = L'\xEB52';
constexpr wchar_t kGlyphHeart = L'\xEB51';
constexpr wchar_t kGlyphChevronDown = L'\xE70D';
constexpr wchar_t kGlyphSearch = L'\xE721';

D2D1_COLOR_F rgba(int r, int g, int b, float a) {
    return D2D1::ColorF(r / 255.0F, g / 255.0F, b / 255.0F, a);
}
const D2D1_COLOR_F kAccent = rgba(63, 132, 232, 1.0F);
const D2D1_COLOR_F kText = rgba(240, 243, 248, 1.0F);
const D2D1_COLOR_F kTextDim = rgba(190, 196, 208, 1.0F);
const D2D1_COLOR_F kPillFill = rgba(10, 12, 16, 0.74F);
const D2D1_COLOR_F kPillEdge = rgba(255, 255, 255, 0.10F);
// An Emby favorite's heart, as the server's own clients colour it.
const D2D1_COLOR_F kFavorite = rgba(236, 72, 96, 1.0F);
// A note saying something went wrong, as a failed sign-in's.
const D2D1_COLOR_F kAlert = rgba(255, 150, 130, 1.0F);

// The text caches: a layout or width unused this many frames (about two
// seconds) goes -- a clock's string changes every second -- and beyond
// this many the least recently used go first.
constexpr std::uint64_t kTextCacheFrames = 120;
constexpr std::size_t kTextCacheKept = 512;

std::size_t mixHash(std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

// Equal floats hash alike: 0 and -0 alike.
std::size_t floatHash(float value) {
    return value == 0.0F ? 0 : static_cast<std::size_t>(std::bit_cast<std::uint32_t>(value));
}

template <typename Map>
void dropLeastRecentlyUsed(Map& map, std::size_t kept) {
    if (map.size() <= kept) return;
    std::vector<std::pair<std::uint64_t, typename Map::iterator>> byAge;
    byAge.reserve(map.size());
    for (auto entry = map.begin(); entry != map.end(); ++entry) byAge.emplace_back(entry->second.used, entry);
    std::sort(byAge.begin(), byAge.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    for (std::size_t i = 0; i < byAge.size() && map.size() > kept; ++i) map.erase(byAge[i].second);
}

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

void Overlay::releaseDeviceResources() {
    subtitleSurfaces_ = {};
    bitmaps_.clear();
    scrim_.Reset();
    mediaScrim_.Reset();
    brush_.Reset();
    context_.Reset();
    device_.Reset();
}

void Overlay::release() {
    clearTextCaches();
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
    // Every cached layout and width was made at the old size.
    clearTextCaches();
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

std::size_t Overlay::TextLayoutKeyHash::operator()(const TextLayoutKey& key) const noexcept {
    std::size_t hash = std::hash<std::wstring>{}(key.text);
    hash = mixHash(hash, static_cast<std::size_t>(key.style));
    hash = mixHash(hash, floatHash(key.width));
    hash = mixHash(hash, floatHash(key.height));
    hash = mixHash(hash, static_cast<std::size_t>(key.align));
    hash = mixHash(hash, (key.wrap ? 1U : 0U) | (key.media ? 2U : 0U));
    return mixHash(hash, floatHash(key.lineHeight));
}

std::size_t Overlay::TextWidthKeyHash::operator()(const TextWidthKey& key) const noexcept {
    return mixHash(std::hash<std::wstring>{}(key.text), static_cast<std::size_t>(key.style));
}

void Overlay::clearTextCaches() {
    textLayouts_.clear();
    textWidths_.clear();
}

void Overlay::trimTextCaches() {
    const auto stale = [this](const auto& entry) { return frame_ - entry.second.used > kTextCacheFrames; };
    std::erase_if(textLayouts_, stale);
    std::erase_if(textWidths_, stale);
    dropLeastRecentlyUsed(textLayouts_, kTextCacheKept);
    dropLeastRecentlyUsed(textWidths_, kTextCacheKept);
}

float Overlay::measureText(const std::wstring& value, OverlayTextStyle style, float scale) {
    if (!ensureFormats(scale) || value.empty()) return 0.0F;
    TextWidthKey key{value, style};
    if (const auto found = textWidths_.find(key); found != textWidths_.end()) {
        found->second.used = frame_;
        return found->second.width;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                         format(style), 4096.0F, 256.0F, &layout))) {
        return 0.0F;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0F;
    const float width = std::ceil(metrics.widthIncludingTrailingWhitespace);
    textWidths_.emplace(std::move(key), CachedTextWidth{width, frame_});
    return width;
}

IDWriteTextLayout* Overlay::textLayout(const std::wstring& value, OverlayTextStyle style, float width, float height,
                                       DWRITE_TEXT_ALIGNMENT align, bool wrap, bool media, float lineHeight) {
    if (value.empty() || !dwrite_ || !format(style) || !(width > 0.0F) || !(height > 0.0F) ||
        !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(lineHeight)) return nullptr;
    TextLayoutKey key{value, style, width, height, align, wrap, media, lineHeight};
    if (const auto found = textLayouts_.find(key); found != textLayouts_.end()) {
        found->second.used = frame_;
        return found->second.layout.Get();
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                                         format(style), width, height, &layout))) return nullptr;
    if (media) {
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        // Match measurement and painting, including explicit paragraphs and
        // emergency wrapping of long unbroken titles at narrow dock widths.
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, lineHeight * 0.8F);
    } else {
        layout->SetTextAlignment(align);
        layout->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                           : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    IDWriteTextLayout* result = layout.Get();
    textLayouts_.emplace(std::move(key), CachedTextLayout{std::move(layout), frame_});
    return result;
}

IDWriteTextLayout* Overlay::mediaTextLayout(const std::wstring& value, OverlayTextStyle style, float width,
                                            float height, float lineHeight) {
    return textLayout(value, style, width, height, DWRITE_TEXT_ALIGNMENT_LEADING, true, true, lineHeight);
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
        IDWriteTextLayout* layout = mediaTextLayout(*values[i], styles[i], geometry.title.width, 1000000.0F,
                                                    lineHeights[i] * scale);
        if (!layout) continue;
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
    IDWriteTextLayout* layout = mediaTextLayout(row.label, OverlayTextStyle::Small, rowWidth, 1000000.0F,
                                                kPanelWrappedNoteLineHeight * scale);
    if (!layout) return;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)) || !std::isfinite(metrics.height) || metrics.height <= 0.0F) return;
    row.measuredNoteHeight = std::ceil(metrics.height);
    row.measuredNoteWidth = rowWidth;
    row.measuredNoteScale = scale;
}

void Overlay::mediaText(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
                        float lineHeight, D2D1_COLOR_F color) {
    if (!box.visible()) return;
    IDWriteTextLayout* layout = mediaTextLayout(value, style, box.width, box.height, lineHeight);
    if (!layout) return;
    brush_->SetColor(color);
    context_->PushAxisAlignedClip(toRect(box), D2D1_ANTIALIAS_MODE_ALIASED);
    context_->DrawTextLayout(D2D1::Point2F(box.x, box.y), layout, brush_.Get());
    context_->PopAxisAlignedClip();
}

void Overlay::text(const std::wstring& value, OverlayTextStyle style, const OverlayRect& box,
                   D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align, bool clip, bool wrap) {
    if (value.empty() || !box.visible()) return;
    IDWriteTextLayout* layout = textLayout(value, style, box.width, box.height, align, wrap, false, 0.0F);
    if (!layout) return;
    brush_->SetColor(color);
    if (clip) context_->PushAxisAlignedClip(toRect(box), D2D1_ANTIALIAS_MODE_ALIASED);
    context_->DrawTextLayout(D2D1::Point2F(box.x, box.y), layout, brush_.Get(),
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
                     bool hot, bool pressed, bool toggled, float alpha, float scale, bool dimmed) {
    if (!box.visible()) return;
    const float radius = 8.0F * scale;
    if (toggled) {
        fillRound(box, radius, withAlpha(rgba(63, 132, 232, 0.32F), alpha));
        strokeRound(box, radius, withAlpha(rgba(63, 132, 232, 0.9F), alpha), std::max(1.0F, scale));
    }
    if (pressed) fillRound(box, radius, withAlpha(rgba(255, 255, 255, 0.22F), alpha));
    else if (hot) fillRound(box, radius, withAlpha(rgba(255, 255, 255, 0.14F), alpha));
    const D2D1_COLOR_F ink = toggled ? rgba(190, 216, 255, 1.0F)
                           : dimmed ? rgba(240, 243, 248, 0.38F) : kText;
    glyph(code, fallback, box, withAlpha(ink, alpha));
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
    trimTextCaches();
    trimBitmaps();
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
        } else if (const float edge = paneMarkAlpha(pane.audioOn, scene.activePaneCount, pane.chromeAlpha);
                   edge > 0.0F) {
            // Which pane is being heard, without a label: a fine accent edge
            // that fades with the pane's chrome.
            strokeRound(cell, 0.0F, withAlpha(rgba(63, 132, 232, 0.55F), edge), std::max(1.0F, 2.0F * s));
        }
        if (const float outline = paneMarkAlpha(pane.targeted, scene.activePaneCount, pane.chromeAlpha);
            outline > 0.0F) {
            // The playback destination is independent of the blue audio
            // edge. Its quiet inner outline fades with the pane's chrome.
            const float inset = 4.0F * s;
            strokeRound({cell.x + inset, cell.y + inset, cell.width - 2.0F * inset,
                         cell.height - 2.0F * inset}, 0.0F, withAlpha(rgba(230, 237, 248, 0.72F), outline),
                        std::max(1.0F, s));
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
        const float timelineAlpha = std::clamp(pane.timelineAlpha, 0.0F, 1.0F);
        if (std::max(alpha, timelineAlpha) <= 0.0F) continue;
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
            const bool idle = chip == static_cast<std::size_t>(PaneChip::Repeat) && pane.repeatIdle;
            button(box, chips[chip].code, chips[chip].fallback,
                   pane.hotChip == static_cast<int>(chip),
                   pane.pressedChip == static_cast<int>(chip), chips[chip].toggled,
                   idle ? alpha * 0.45F : alpha, s);
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
                 pane.position01, pane.buffered01, emphasised, 7.0F * s, timelineAlpha);
            if (emphasised && chrome.timeLabel.visible() && !pane.timeText.empty()) {
                const float labelWidth = std::min(
                    chrome.timeLabel.width,
                    measureText(pane.timeText, OverlayTextStyle::Small, s) + 16.0F * s);
                const OverlayRect label{chrome.timeLabel.x, chrome.timeLabel.y,
                                        labelWidth, chrome.timeLabel.height};
                fillRound(label, label.height * 0.5F, withAlpha(kPillFill, timelineAlpha));
                text(pane.timeText, OverlayTextStyle::Small, label, withAlpha(kText, timelineAlpha));
            }
        } else if (chrome.timeline.visible() && (pane.waiting || !pane.seekable)) {
            rail(chrome.timeline, 12.0F * s, 3.0F * s, 0.0F, 0.0F, false, 0.0F, timelineAlpha * 0.5F);
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
    if (scene.activePaneCount > 1 && bar.scopeLabel.visible() && !scene.scopeText.empty()) {
        fillRound(bar.scopeLabel, 9.0F * s, withAlpha(kPillFill, alpha));
        text(scene.scopeText, OverlayTextStyle::Small, bar.scopeLabel, withAlpha(kTextDim, alpha),
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
    // A switch: lit while the lines are shown, greyed while they are not.
    button(bar[BarItem::Subtitles], kGlyphSubtitles, L"CC", hot(BarItem::Subtitles),
           pressed(BarItem::Subtitles), scene.subtitlesOn, alpha, s, !scene.subtitlesOn);
    if (bar[BarItem::Layout].visible()) {
        button(bar[BarItem::Layout], kGlyphLayout, L"Grid", hot(BarItem::Layout),
               pressed(BarItem::Layout), false, alpha, s);
    }
    button(bar[BarItem::Fullscreen], scene.fullscreen ? kGlyphWindowed : kGlyphFullscreen,
           scene.fullscreen ? L"Exit" : L"Full", hot(BarItem::Fullscreen),
           pressed(BarItem::Fullscreen), scene.fullscreen, alpha, s);
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
    // Fullscreen shows Restore: the middle button leaves it.
    const bool restore = scene.maximized || scene.fullscreen;
    const std::array<Button, kCaptionItemCount> buttons{{
        {CaptionItem::Minimize, kGlyphChromeMinimize, L"_"},
        {CaptionItem::Maximize, restore ? kGlyphChromeRestore : kGlyphChromeMaximize,
         restore ? L"[]" : L"[ ]"},
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

void Overlay::drawTextField(const PanelRow& row, const PanelRowGeometry& geometry, bool hot, bool clearHot,
                            bool clearPressed, float alpha, float s) {
    const auto& field = row.field;
    const OverlayRect& box = geometry.control;
    if (!box.visible()) return;
    if (!row.label.empty()) {
        // A sign-in box's name above it, as a Choice row's.
        text(row.label, OverlayTextStyle::Small, {geometry.row.x, geometry.row.y, geometry.row.width, 24.0F * s},
             withAlpha(field.focused ? rgba(150, 190, 245, 1.0F) : kTextDim, alpha),
             DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }
    const float radius = 10.0F * s;
    // Typed into, it is outlined in the accent, as a chosen segment is.
    fillRound(box, radius, withAlpha(rgba(255, 255, 255, field.focused ? 0.10F : hot ? 0.10F : 0.07F), alpha));
    strokeRound(box, radius,
                withAlpha(field.focused ? rgba(63, 132, 232, 0.95F) : rgba(255, 255, 255, hot ? 0.26F : 0.14F), alpha),
                std::max(1.0F, s) * (field.focused ? 2.0F : 1.0F));
    if (field.magnifier) {
        glyph(kGlyphSearch, L"\x2315", {box.x + 12.0F * s, box.y, 22.0F * s, box.height},
              withAlpha(field.focused ? rgba(150, 190, 245, 1.0F) : kTextDim, alpha));
    }

    const OverlayRect textBox = panelTextFieldTextBox(row, geometry, s);
    const std::wstring shown = panelTextFieldShown(field);
    if (textBox.visible()) {
        context_->PushAxisAlignedClip(toRect(textBox), D2D1_ANTIALIAS_MODE_ALIASED);
        if (shown.empty()) {
            text(field.placeholder, OverlayTextStyle::Body, textBox, withAlpha(rgba(140, 147, 160, 1.0F), alpha),
                 DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        const float caretWidth = panelTextFieldCaretWidth(s);
        const std::size_t caret = textFieldShownCaret(field.edit, field.composition, field.compositionCaret);
        const auto widthOf = [&](std::size_t count) {
            return count == 0 ? 0.0F : measureText(shown.substr(0, std::min(count, shown.size())), OverlayTextStyle::Body, s);
        };
        const float textWidth = widthOf(shown.size());
        const float caretX = widthOf(caret);
        const float x = textBox.x - textFieldScroll(caretX, textWidth, textBox.width, caretWidth);
        const float middle = textBox.y + textBox.height * 0.5F;
        if (field.focused && field.composition.empty() && textFieldHasSelection(field.edit)) {
            const float from = widthOf(textFieldSelectionStart(field.edit));
            const float to = widthOf(textFieldSelectionEnd(field.edit));
            fillRound({x + from, middle - 11.0F * s, std::max(0.0F, to - from), 22.0F * s}, 3.0F * s,
                      withAlpha(rgba(63, 132, 232, 0.55F), alpha));
        }
        if (!shown.empty()) {
            text(shown, OverlayTextStyle::Body, {x, textBox.y, textWidth + caretWidth + 8.0F * s, textBox.height},
                 withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        if (!field.composition.empty()) {
            // The input method's unfinished text, underlined where it stands.
            const std::size_t start = std::min(field.edit.caret, field.edit.text.size());
            const float from = widthOf(start);
            const float to = widthOf(start + field.composition.size());
            fillRound({x + from, middle + 10.0F * s, std::max(0.0F, to - from), std::max(1.0F, s)}, 0.0F,
                      withAlpha(kText, alpha * 0.85F));
        }
        if (field.focused && field.caretShown) {
            fillRound({x + caretX, middle - 10.0F * s, caretWidth, 20.0F * s}, 0.0F, withAlpha(kText, alpha));
        }
        context_->PopAxisAlignedClip();
    }

    const OverlayRect& clear = geometry.trailing;
    if (clear.visible()) {
        if (clearPressed || clearHot) {
            fillRound(clear, clear.height * 0.5F, withAlpha(rgba(255, 255, 255, clearPressed ? 0.22F : 0.14F), alpha));
        }
        const D2D1_COLOR_F color = withAlpha(clearHot ? kText : kTextDim, alpha);
        // The window buttons' small cross where that face is installed.
        if (formats_.captionIcon) text(std::wstring(1, kGlyphClose), OverlayTextStyle::CaptionIcon, clear, color);
        else text(L"\x00D7", OverlayTextStyle::Body, clear, color);
    }
}

void Overlay::drawPanel(const OverlayScene& scene) {
    const auto& panel = scene.panel;
    const float alpha = std::clamp(panel.alpha, 0.0F, 1.0F);
    const auto& layout = panel.shownLayout();
    const auto& rows = panel.shownRows();
    if (alpha <= 0.0F || !layout.sheet.visible()) return;
    const float s = scene.scale;
    fillRound(layout.sheet, 0.0F, withAlpha(rgba(12, 14, 18, 0.94F), alpha));
    if (layout.mediaDetailRow >= 0 && static_cast<std::size_t>(layout.mediaDetailRow) < rows.size() &&
        rows[static_cast<std::size_t>(layout.mediaDetailRow)].kind == PanelRowKind::MediaDetail) {
        // Behind the pinned rows too, as when they scrolled with the page.
        const OverlayRect backdrop = layout.pinned.visible()
            ? OverlayRect{layout.content.x, layout.pinned.y, layout.content.width,
                          layout.content.bottom() - layout.pinned.y}
            : layout.content;
        drawMediaBackdrop(rows[static_cast<std::size_t>(layout.mediaDetailRow)].mediaDetail, backdrop, alpha,
                          panel.thumbnails);
    }
    fillRound({layout.sheet.x, layout.sheet.y, std::max(1.0F, s), layout.sheet.height}, 0.0F,
              withAlpha(rgba(255, 255, 255, 0.10F), alpha));
    const float pad = 20.0F * s;
    const bool switches = !layout.sheetSwitches.empty() && layout.sheetSwitches.front().visible();
    const float titleRight = (switches ? layout.sheetSwitches.front().x : layout.close.x) - 8.0F * s;
    text(panel.title, OverlayTextStyle::Title,
         {layout.header.x + pad, layout.header.y,
          std::max(0.0F, titleRight - layout.header.x - pad), layout.header.height},
         withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    button(layout.close, kGlyphClose, L"X", panel.hotKind == PanelHitKind::Close,
           panel.pressedKind == PanelHitKind::Close, false, alpha, s);
    for (std::size_t i = 0; i < layout.sheetSwitches.size() && i < panel.switchCaptions.size(); ++i) {
        // The switches to the sheet's other faces, drawn as chosen segments
        // so they read as buttons beside the plain close glyph.
        const auto& box = layout.sheetSwitches[i];
        if (!box.visible() || panel.switchCaptions[i].empty()) continue;
        const bool hot = panel.hotKind == PanelHitKind::Switch && panel.hotPart == static_cast<int>(i);
        const bool pressed = panel.pressedKind == PanelHitKind::Switch && panel.pressedPart == static_cast<int>(i);
        fillRound(box, 8.0F * s, withAlpha(pressed ? rgba(255, 255, 255, 0.24F)
            : rgba(63, 132, 232, hot ? 0.48F : 0.32F), alpha));
        strokeRound(box, 8.0F * s, withAlpha(rgba(63, 132, 232, 0.9F), alpha), std::max(1.0F, s));
        text(panel.switchCaptions[i], OverlayTextStyle::Small, box,
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

    if (layout.pinned.visible()) {
        // The pinned rows are closed by the same rule as the tab strip.
        fillRound({layout.pinned.x, layout.pinned.bottom() - std::max(1.0F, s), layout.pinned.width,
                   std::max(1.0F, s)}, 0.0F, withAlpha(rgba(255, 255, 255, 0.08F), alpha));
    }

    // The pinned rows are clipped to their band, the rest to the content.
    const std::size_t pinnedRows = layout.pinned.visible() ? layout.pinnedRows : 0;
    context_->PushAxisAlignedClip(toRect(pinnedRows > 0 ? layout.pinned : layout.content),
                                  D2D1_ANTIALIAS_MODE_ALIASED);
    const D2D1_COLOR_F dim = withAlpha(kTextDim, alpha);
    // The pinned rows, then the content's from the first reaching its top:
    // they follow each other down, so a long list's rows off the screen are
    // neither visited nor drawn.
    const std::size_t rowCount = std::min(rows.size(), layout.rows.size());
    const std::size_t leading = std::min(layout.pinnedRows, rowCount);
    const std::size_t firstShown = std::max(leading, panelFirstRowReaching(layout, layout.content.y));
    bool contentClip = pinnedRows == 0;
    for (std::size_t i = 0; i < rowCount; ++i) {
        if (i == leading && i < firstShown) {
            i = firstShown;
            if (i >= rowCount) break;
        }
        if (!contentClip && i >= pinnedRows) {
            context_->PopAxisAlignedClip();
            context_->PushAxisAlignedClip(toRect(layout.content), D2D1_ANTIALIAS_MODE_ALIASED);
            contentClip = true;
        }
        const auto& row = rows[i];
        const auto& geometry = layout.rows[i];
        const OverlayRect& clip = i < pinnedRows ? layout.pinned : layout.content;
        if (i >= leading && geometry.row.y > clip.bottom()) break;
        if (geometry.row.bottom() < clip.y || geometry.row.y > clip.bottom()) continue;
        // App fills every list row near the screen; one left bare would have
        // no text or pictures to index, so it is left blank.
        if (row.contentPending) continue;
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
        case PanelRowKind::TextField:
            drawTextField(row, geometry,
                hotRow && (panel.hotKind == PanelHitKind::Field || panel.hotKind == PanelHitKind::Trailing),
                hotRow && panel.hotKind == PanelHitKind::Trailing,
                pressedRow && panel.pressedKind == PanelHitKind::Trailing, rowAlpha, s);
            break;
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
        case PanelRowKind::Note: {
            const D2D1_COLOR_F color = row.alert ? withAlpha(kAlert, alpha) : dim;
            if (row.wrapNote) {
                mediaText(row.label, OverlayTextStyle::Small, panelWrappedNoteTextBox(geometry.row, s),
                          kPanelWrappedNoteLineHeight * s, color);
            } else {
                text(row.label, OverlayTextStyle::Small,
                     {geometry.row.x, geometry.row.y + 2.0F * s, geometry.row.width, geometry.row.height - 2.0F * s},
                     color, DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
            }
            break;
        }
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
        case PanelRowKind::CompactItem: {
            const bool active = row.enabled && (hotRow || pressedRow);
            fillRound(geometry.row, 8.0F * s,
                      withAlpha(active ? rgba(255, 255, 255, pressedRow ? 0.16F : 0.08F)
                                       : rgba(255, 255, 255, 0.03F), rowAlpha));
            const auto& picture = geometry.control;
            fillRound(picture, 6.0F * s, withAlpha(rgba(15, 19, 26, 0.94F), rowAlpha));
            ID2D1Bitmap* bitmap = panel.thumbnails && !row.tileKeys.empty()
                ? bitmapFor(row.tileKeys.front(), *panel.thumbnails) : nullptr;
            bool drawn = false;
            if (bitmap && picture.visible()) {
                const auto size = bitmap->GetSize();
                if (size.width > 0.0F && size.height > 0.0F) {
                    const float fit = std::min(picture.width / size.width, picture.height / size.height);
                    const float width = size.width * fit, height = size.height * fit;
                    const OverlayRect destination{picture.x + (picture.width - width) * 0.5F,
                        picture.y + (picture.height - height) * 0.5F, width, height};
                    context_->DrawBitmap(bitmap, toRect(destination), rowAlpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    drawn = true;
                }
            }
            if (!drawn) {
                text(row.label.substr(0, row.label.find(L' ')), OverlayTextStyle::BodyBold, picture,
                     withAlpha(kTextDim, rowAlpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            const float textX = picture.right() + 12.0F * s;
            const float textWidth = std::max(0.0F, geometry.row.right() - textX - 8.0F * s);
            mediaText(row.label, OverlayTextStyle::Body,
                      {textX, geometry.row.y + 10.0F * s, textWidth, 40.0F * s}, 20.0F * s, textColor);
            text(row.value, OverlayTextStyle::Small,
                 {textX, geometry.row.y + 54.0F * s, textWidth, 20.0F * s},
                 withAlpha(row.on ? rgba(150, 190, 245, 1.0F) : kTextDim, rowAlpha),
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
            break;
        }
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
            // A favorite's heart before the tag, in the same column.
            const float heart = row.favorite ? 20.0F * s : 0.0F;
            const float heartSpace = heart > 0.0F ? heart + 4.0F * s : 0.0F;
            const OverlayRect labelBox{geometry.row.x + 8.0F * s, geometry.row.y,
                                       std::max(0.0F, available - valueWidth - tagSpace - heartSpace - 20.0F * s),
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
            if (heart > 0.0F) {
                const OverlayRect heartBox{textRight - valueWidth - 8.0F * s - tagSpace - heartSpace,
                                           geometry.row.y + (geometry.row.height - heart) * 0.5F, heart, heart};
                glyph(kGlyphHeartFill, L"\x2665", heartBox, withAlpha(kFavorite, rowAlpha));
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
                // A favorite (an episode on an information page) ends its
                // label line with a heart.
                const float heart = row.favorite ? std::min(labelLine.height, 20.0F * s) : 0.0F;
                const OverlayRect textLine{labelLine.x, labelLine.y,
                                           std::max(0.0F, labelLine.width - (heart > 0.0F ? heart + 4.0F * s : 0.0F)),
                                           labelLine.height};
                text(row.label, OverlayTextStyle::Small, textLine, withAlpha(kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
                text(row.value, OverlayTextStyle::Small, textLine, textColor,
                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
                if (heart > 0.0F) {
                    glyph(kGlyphHeartFill, L"\x2665",
                          {labelLine.right() - heart, labelLine.y + (labelLine.height - heart) * 0.5F, heart, heart},
                          withAlpha(kFavorite, rowAlpha));
                }
            }
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const auto& box = geometry.parts[part];
                const bool hot = hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Button;
                const bool pressed = pressedRow && panel.pressedPart == static_cast<int>(part);
                // A form's own button, which its Enter presses, in the accent.
                D2D1_COLOR_F fill = row.primary ? rgba(63, 132, 232, 0.85F) : rgba(255, 255, 255, 0.10F);
                if (pressed) fill = row.primary ? rgba(43, 102, 195, 0.98F) : rgba(255, 255, 255, 0.24F);
                else if (hot) fill = row.primary ? rgba(63, 132, 232, 0.98F) : rgba(255, 255, 255, 0.18F);
                fillRound(box, 8.0F * s, withAlpha(fill, rowAlpha));
                text(row.options[part], OverlayTextStyle::Body, box, textColor,
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            break;
        }
    }
    context_->PopAxisAlignedClip();
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
    const EmptyHintLayout& hint = scene.emptyHint;
    text(L"Drop videos here", OverlayTextStyle::Title, hint.title, kTextDim);
    text(L"O  open      right-click  menu      F5  settings      F6  Emby", OverlayTextStyle::Body,
         hint.keys, rgba(120, 126, 138, 1.0F));
    // In the accent, as a form's own button.
    const float alpha = std::clamp(scene.emptyEmbyAlpha, 0.0F, 1.0F);
    if (!hint.emby.visible() || alpha <= 0.0F) return;
    const D2D1_COLOR_F fill = scene.emptyEmbyPressed ? rgba(43, 102, 195, 0.98F)
                            : scene.emptyEmbyHot ? rgba(63, 132, 232, 0.98F) : rgba(63, 132, 232, 0.85F);
    fillRound(hint.emby, 8.0F * s, withAlpha(fill, alpha));
    text(L"Open Emby", OverlayTextStyle::BodyBold, hint.emby, withAlpha(kText, alpha),
         DWRITE_TEXT_ALIGNMENT_CENTER, true);
}

// ---------------------------------------------------------------------------
// Browser tiles

namespace {

constexpr int kDecodesPerFrame = 6;
constexpr std::size_t kBitmapsKept = 160;
constexpr std::size_t kBitmapsLimit = 220;
// A picture past these is refused before a pixel of it is decoded: a
// server's original poster or 4K backdrop passes, a few kilobytes that
// say they are 16384 x 16384 do not.
constexpr UINT kPictureSideLimit = 8192;
constexpr std::uint64_t kPicturePixelLimit = 8192ULL * 4096ULL;

bool pictureSizeAllowed(UINT width, UINT height) {
    return width > 0 && height > 0 && width <= kPictureSideLimit && height <= kPictureSideLimit &&
           static_cast<std::uint64_t>(width) * height <= kPicturePixelLimit;
}

// The widest a picture is drawn, which its key names: what the browser
// asked the server for, or a local thumbnail's width. 0 for a key that
// names none.
UINT pictureWidthOf(const std::string& key) {
    if (const auto local = parseLocalThumbnailKey(key)) return static_cast<UINT>(local->width);
    if (const auto server = emby::parseImageKey(key)) return static_cast<UINT>(std::max(0, server->width));
    return 0;
}

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
    UINT width = 0;
    UINT height = 0;
    // The bytes outlive the stream: the decode finishes inside this call and
    // CreateBitmapFromWicBitmap copies the pixels. The size is read from the
    // header, before anything is decoded.
    if (SUCCEEDED(wic_->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes->data())), static_cast<DWORD>(bytes->size()))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) &&
        pictureSizeAllowed(width, height)) {
        // No wider than its key asked for: a server that sends its original
        // rather than the size asked for costs the memory one that does as
        // asked would. Never made larger.
        const UINT target = pictureWidthOf(key);
        UINT drawnWidth = width;
        UINT drawnHeight = height;
        if (target > 0 && width > target) {
            drawnWidth = target;
            drawnHeight = std::max<UINT>(1, static_cast<UINT>(std::lround(static_cast<double>(height) * target / width)));
        }
        const UINT largest = context_->GetMaximumBitmapSize();
        if (drawnWidth <= largest && drawnHeight <= largest &&
            SUCCEEDED(wic_->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
            // Scaled after the conversion, so edges blend premultiplied.
            Microsoft::WRL::ComPtr<IWICBitmapSource> source = converter;
            if (drawnWidth != width) {
                Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
                if (SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(converter.Get(), drawnWidth, drawnHeight,
                                                 WICBitmapInterpolationModeFant))) {
                    source = scaler;
                } else {
                    source.Reset();
                }
            }
            if (source && FAILED(context_->CreateBitmapFromWicBitmap(source.Get(), nullptr, &entry.bitmap))) {
                entry.bitmap.Reset();
            }
            if (entry.bitmap) {
                const D2D1_SIZE_U pixels = entry.bitmap->GetPixelSize();
                entry.bytes = static_cast<std::size_t>(pixels.width) * pixels.height * 4;
            }
        }
    }
    // A failure or a refusal is remembered too, so the same bytes are not
    // tried every frame.
    ID2D1Bitmap* result = entry.bitmap.Get();
    bitmaps_[key] = std::move(entry);
    return result;
}

void Overlay::trimBitmaps() {
    std::size_t bytes = 0;
    for (const auto& entry : bitmaps_) bytes += entry.second.bytes;
    const bool tooMany = bitmaps_.size() > kBitmapsLimit;
    if (!tooMany && bytes <= kBitmapBudget) return;
    std::vector<std::pair<std::uint64_t, decltype(bitmaps_)::iterator>> byAge;
    byAge.reserve(bitmaps_.size());
    for (auto entry = bitmaps_.begin(); entry != bitmaps_.end(); ++entry) byAge.emplace_back(entry->second.used, entry);
    std::sort(byAge.begin(), byAge.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [used, entry] : byAge) {
        // What this frame drew stays, past the budget or not: it would only
        // be decoded again next frame.
        if (used == frame_) break;
        if (!(tooMany && bitmaps_.size() > kBitmapsKept) && bytes <= kBitmapBudget) break;
        bytes -= entry->second.bytes;
        bitmaps_.erase(entry);
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
        // The favorite button while the item is one: the heart's colour.
        const bool favorite = part == 5 && row.on;
        const bool primary = part == 0 || (detail.compactActions && part == 1);
        D2D1_COLOR_F fill = primary ? kAccent : favorite ? withAlpha(kFavorite, 0.26F)
                                                           : rgba(255, 255, 255, part == 2 ? 0.04F : 0.12F);
        if (pressed) {
            fill = primary ? rgba(43, 102, 195, 1.0F)
                 : favorite  ? withAlpha(kFavorite, 0.48F) : rgba(255, 255, 255, 0.24F);
        } else if (hot) {
            fill = primary ? rgba(85, 150, 244, 1.0F)
                 : favorite  ? withAlpha(kFavorite, 0.36F) : rgba(255, 255, 255, 0.18F);
        }
        fillRound(box, 9.0F * s, withAlpha(fill, alpha));
        if (!primary && part != 2) {
            const D2D1_COLOR_F edge = favorite ? withAlpha(kFavorite, 0.70F) : rgba(255, 255, 255, 0.14F);
            strokeRound(box, 9.0F * s, withAlpha(edge, alpha), std::max(1.0F, s));
        }
        const auto color = withAlpha(part == 2 ? rgba(172, 205, 252, 1.0F)
            : favorite ? rgba(255, 196, 206, 1.0F) : kText, alpha);
        if (detail.compactActions && (part == 1 || part == 4)) {
            glyph(kGlyphChevronDown, L"\u25BE", box, color);
        } else if (detail.compactActions && part == 5) {
            glyph(row.on ? kGlyphHeartFill : kGlyphHeart, row.on ? L"\u2665" : L"\u2661", box, color);
        } else {
            text(row.options[part], part == 0 ? OverlayTextStyle::BodyBold : OverlayTextStyle::Body, box,
                 color, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
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
    // A favorite's heart in the right-hand column, under the badge: the
    // bottom-right corner is the Add button's, placed as settingsPanelLayout
    // places it, and on a tile too small for both the heart gives way.
    if (part < row.tileFavorites.size() && row.tileFavorites[part]) {
        const float size = 20.0F * s;
        const bool badge = part < row.tileBadges.size() && !row.tileBadges[part].empty();
        const OverlayRect heartBox{picture.right() - size - 6.0F * s, picture.y + (badge ? 30.0F : 6.0F) * s,
                                   size, size};
        float free = picture.bottom();
        if (part < row.tileAddAvailable.size() && row.tileAddAvailable[part] && box.width > 12.0F * s) {
            free -= std::min(28.0F * s, std::max(0.0F, pictureHeight - 12.0F * s)) + 12.0F * s;
        }
        if (heartBox.bottom() <= free) {
            fillRound(heartBox, size * 0.5F, withAlpha(shade, alpha));
            if (formats_.captionIcon) {
                text(std::wstring(1, kGlyphHeartFill), OverlayTextStyle::CaptionIcon, heartBox,
                     withAlpha(kFavorite, alpha));
            } else {
                text(L"\x2665", OverlayTextStyle::Small, heartBox, withAlpha(kFavorite, alpha),
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
        }
    }
    const OverlayRect labelBox{box.x, picture.bottom() + 4.0F * s, box.width,
                               std::max(0.0F, box.height - pictureHeight - 4.0F * s)};
    text(row.options[part], OverlayTextStyle::Small, labelBox,
         withAlpha(current ? rgba(190, 216, 255, 1.0F) : kText, alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
}

}  // namespace quaddeck
