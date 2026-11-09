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
constexpr wchar_t kGlyphAllVideos = L'\xE80A';  // a grid, unlike the arrangement's four squares
constexpr wchar_t kGlyphVideoSets = L'\xE8FD';  // BulletedList
constexpr wchar_t kGlyphLink = L'\xE71B';
constexpr wchar_t kGlyphChromeMinimize = L'\xE921';
constexpr wchar_t kGlyphChromeMaximize = L'\xE922';
constexpr wchar_t kGlyphChromeRestore = L'\xE923';
constexpr wchar_t kGlyphChromeClose = L'\xE8BB';
constexpr wchar_t kGlyphHeartFill = L'\xEB52';
constexpr wchar_t kGlyphHeart = L'\xEB51';
constexpr wchar_t kGlyphChevronDown = L'\xE70D';
constexpr wchar_t kGlyphChevronUp = L'\xE70E';
constexpr wchar_t kGlyphSearch = L'\xE721';
constexpr wchar_t kGlyphCheckMark = L'\xE73E';
constexpr wchar_t kGlyphChevronRight = L'\xE76C';

D2D1_COLOR_F rgba(int r, int g, int b, float a) {
    return D2D1::ColorF(r / 255.0F, g / 255.0F, b / 255.0F, a);
}
// The palette: every surface -- the bar and its capsules, the strips, the
// menus and the sync panel, the sheet, the settings window, the notices --
// takes its colours from here and its controls' faces from the few that
// draw them (quietButton and the rest), so they are one app's.
const D2D1_COLOR_F kAccent = rgba(63, 132, 232, 1.0F);
// Words: the main ones; the second -- labels over controls, values, notes;
// the quietest -- keys in caps, chevrons, hints; and the accent's own for a
// heading or what was changed from its default.
const D2D1_COLOR_F kText = rgba(240, 243, 248, 1.0F);
const D2D1_COLOR_F kTextDim = rgba(190, 196, 208, 1.0F);
const D2D1_COLOR_F kTextMuted = rgba(150, 158, 172, 1.0F);
const D2D1_COLOR_F kAccentText = rgba(150, 190, 245, 1.0F);
// Grounds: the sheet's, which the settings window is painted in too, and a
// raised card's, a shade lighter: a menu, the sync panel, a drop-down's list,
// and the cards the settings window's rows stand on.
const D2D1_COLOR_F kSheetGround = rgba(12, 14, 18, 1.0F);
const D2D1_COLOR_F kCardGround = rgba(31, 35, 44, 1.0F);
// Hairlines: a rule between parts, and a card's edge.
const D2D1_COLOR_F kRule = rgba(255, 255, 255, 0.08F);
const D2D1_COLOR_F kCardEdge = rgba(255, 255, 255, 0.13F);
// White over a ground, for a control's face: a button at rest, under the
// pointer and pressed; a line or a segment lit and pressed; a track.
constexpr float kFaceRest = 0.08F;
constexpr float kFaceHot = 0.14F;
constexpr float kFacePressed = 0.20F;
constexpr float kLineHot = 0.08F;
constexpr float kLinePressed = 0.16F;
constexpr float kTrack = 0.06F;
// A rail's track, upright or not, and a list's thumb at rest.
constexpr float kRailTrack = 0.28F;
// What is greyed, whatever it is.
constexpr float kDisabledAlpha = 0.4F;
// Windows' own red for a Close button under the pointer.
const D2D1_COLOR_F kCloseRed = rgba(196, 43, 28, 1.0F);
const D2D1_COLOR_F kPillFill = rgba(10, 12, 16, 0.74F);
const D2D1_COLOR_F kPillEdge = rgba(255, 255, 255, 0.10F);
// A capsule of controls standing on the picture with no scrim: the bar's
// "All", a Linked video's two ends.
const D2D1_COLOR_F kCapsuleFill = rgba(14, 16, 20, 0.86F);
const D2D1_COLOR_F kCapsuleEdge = rgba(255, 255, 255, 0.16F);
// An Emby favorite's heart, as the server's own clients colour it.
const D2D1_COLOR_F kFavorite = rgba(236, 72, 96, 1.0F);
// A note saying something went wrong, as a failed sign-in's.
const D2D1_COLOR_F kAlert = rgba(255, 150, 130, 1.0F);

// Each video's own colour, by its place on the screen: V1 the accent, so
// one video looks as it always has, then orange, green, purple and pink,
// all deep enough for white text. Whatever is about one video is lit in it:
// its pane's pill, rails, lit chips and edges, its menu, its rows in the
// sheet and the browser's marks of what it plays. -1 is no video: the
// accent.
D2D1_COLOR_F paneColor(int position, float alpha = 1.0F) {
    switch (position) {
    case 1: return rgba(226, 118, 28, alpha);
    case 2: return rgba(46, 160, 80, alpha);
    case 3: return rgba(150, 96, 232, alpha);
    case 4: return rgba(214, 64, 152, alpha);
    default: return rgba(63, 132, 232, alpha);
    }
}

// `color` taken `amount` of the way to `toward`, keeping its alpha.
D2D1_COLOR_F blend(D2D1_COLOR_F color, D2D1_COLOR_F toward, float amount) {
    const float keep = 1.0F - amount;
    return {color.r * keep + toward.r * amount, color.g * keep + toward.g * amount,
            color.b * keep + toward.b * amount, color.a};
}

// The name of the item being played, in a pale shade of its video's colour.
D2D1_COLOR_F paneTextColor(int position) {
    return blend(paneColor(position), rgba(255, 255, 255, 1.0F), 0.62F);
}

// A glyph or words lit in a video's colour, on a ground of it: a paler
// shade, V1's (and no video's) the accent's own.
D2D1_COLOR_F paneInkColor(int position) {
    return position >= 1 ? blend(paneColor(position), rgba(255, 255, 255, 1.0F), 0.7F) : rgba(190, 216, 255, 1.0F);
}

// A control's corners: eight DIPs, but no more than a quarter of its
// height, so a key's small cap has five.
float controlRadius(const OverlayRect& box, float scale) {
    return std::min(8.0F * scale, box.height * 0.25F);
}

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

// A drop-down's chosen option stands out in the heavier face.
OverlayTextStyle panelMenuLabelStyle(const PanelMenuItem& item) {
    return item.kind == PanelMenuItemKind::Option && item.checked ? OverlayTextStyle::BodyBold
                                                                  : OverlayTextStyle::Body;
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

void Overlay::measureCaptions(PanelRow& row, float scale) {
    if (row.kind != PanelRowKind::Choice && row.kind != PanelRowKind::Buttons) return;
    row.captionWidths.clear();
    row.labelWidth = row.valueWidth = -1.0F;
    if (!std::isfinite(scale) || scale <= 0.0F || !ensureFormats(scale)) return;
    const auto width = [&](const std::wstring& text, OverlayTextStyle style) {
        return measureText(text, style, scale) / scale;
    };
    // A drop-down's options as its list draws the chosen one, bold: its box
    // is then as wide as the list whichever is chosen (PanelMetrics).
    const OverlayTextStyle captions = row.dropDown ? OverlayTextStyle::BodyBold : OverlayTextStyle::Body;
    row.captionWidths.reserve(row.options.size());
    for (const auto& caption : row.options) row.captionWidths.push_back(width(caption, captions));
    row.labelWidth = width(row.label, OverlayTextStyle::Body);
    row.valueWidth = width(row.value, OverlayTextStyle::Small);
}

void Overlay::measurePanelMenu(const std::vector<PanelMenuItem>& items, float scale,
                               std::vector<float>& labelWidths, std::vector<float>& shortcutWidths) {
    labelWidths.assign(items.size(), 0.0F);
    shortcutWidths.assign(items.size(), 0.0F);
    if (!std::isfinite(scale) || scale <= 0.0F || !ensureFormats(scale)) return;
    for (std::size_t i = 0; i < items.size(); ++i) {
        labelWidths[i] = measureText(items[i].label, panelMenuLabelStyle(items[i]), scale);
        shortcutWidths[i] = measureText(items[i].shortcut, OverlayTextStyle::Small, scale);
    }
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

void Overlay::buttonGround(const OverlayRect& box, bool hot, bool pressed, bool toggled, float alpha, float scale,
                           int video) {
    // On, it is chosen, as a segment is; off, it has no face until the
    // pointer comes, and lights then as a button with one does.
    const float radius = controlRadius(box, scale);
    if (toggled) chosenFace(box, hot, alpha, scale, video);
    if (pressed) fillRound(box, radius, withAlpha(rgba(255, 255, 255, kFacePressed), alpha));
    else if (hot && !toggled) fillRound(box, radius, withAlpha(rgba(255, 255, 255, kFaceHot), alpha));
}

void Overlay::button(const OverlayRect& box, wchar_t code, const wchar_t* fallback,
                     bool hot, bool pressed, bool toggled, float alpha, float scale, bool dimmed, int video) {
    if (!box.visible()) return;
    buttonGround(box, hot, pressed, toggled, alpha, scale, video);
    const D2D1_COLOR_F ink = toggled ? paneInkColor(video) : dimmed ? withAlpha(kText, kDisabledAlpha) : kText;
    glyph(code, fallback, box, withAlpha(ink, alpha));
}

void Overlay::labelButton(const OverlayRect& box, const wchar_t* label, bool hot, bool pressed, bool toggled,
                          float alpha, float scale, int video) {
    if (!box.visible()) return;
    buttonGround(box, hot, pressed, toggled, alpha, scale, video);
    text(label, OverlayTextStyle::BodyBold, box,
         withAlpha(toggled ? paneInkColor(video) : paneTextColor(video), alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
}

void Overlay::quietButton(const OverlayRect& box, bool hot, bool pressed, float alpha, float scale, bool edged) {
    if (!box.visible()) return;
    const float radius = controlRadius(box, scale);
    fillRound(box, radius, withAlpha(rgba(255, 255, 255, pressed ? kFacePressed : hot ? kFaceHot : kFaceRest), alpha));
    if (edged) {
        strokeRound(box, radius, withAlpha(rgba(255, 255, 255, hot ? 0.22F : 0.14F), alpha), std::max(1.0F, scale));
    }
}

// Lighter under the pointer and darker pressed, as a video's pill is.
void Overlay::primaryButton(const OverlayRect& box, bool hot, bool pressed, float alpha, float scale, int video) {
    if (!box.visible()) return;
    const D2D1_COLOR_F own = paneColor(video);
    fillRound(box, controlRadius(box, scale),
              withAlpha(pressed ? blend(own, rgba(0, 0, 0, 1.0F), 0.25F)
                        : hot   ? blend(own, rgba(255, 255, 255, 1.0F), 0.18F) : own, alpha));
}

void Overlay::dangerButton(const OverlayRect& box, bool hot, bool pressed, float alpha, float scale) {
    if (!box.visible()) return;
    fillRound(box, controlRadius(box, scale),
              withAlpha(pressed ? blend(kCloseRed, rgba(0, 0, 0, 1.0F), 0.25F)
                        : hot   ? blend(kCloseRed, rgba(255, 255, 255, 1.0F), 0.14F) : kCloseRed, alpha));
}

void Overlay::chosenFace(const OverlayRect& box, bool hot, float alpha, float scale, int video) {
    if (!box.visible()) return;
    const float radius = controlRadius(box, scale);
    fillRound(box, radius, withAlpha(paneColor(video, hot ? 0.50F : 0.38F), alpha));
    strokeRound(box, radius, withAlpha(paneColor(video, 0.95F), alpha), std::max(1.0F, scale));
}

void Overlay::litFace(const OverlayRect& box, bool hot, bool pressed, float alpha, float scale) {
    if (!box.visible() || !(hot || pressed)) return;
    fillRound(box, controlRadius(box, scale),
              withAlpha(rgba(255, 255, 255, pressed ? kLinePressed : kLineHot), alpha));
}

void Overlay::segmentTrack(const OverlayRect& box, float alpha, float scale) {
    fillRound(box, controlRadius(box, scale), withAlpha(rgba(255, 255, 255, kTrack), alpha));
}

void Overlay::fieldBox(const OverlayRect& box, bool hot, bool active, float alpha, float scale, int video) {
    if (!box.visible()) return;
    const float radius = controlRadius(box, scale);
    const float hairline = std::max(1.0F, scale);
    fillRound(box, radius, withAlpha(rgba(255, 255, 255, active || hot ? 0.10F : 0.06F), alpha));
    strokeRound(box, radius,
                withAlpha(active ? paneColor(video, 0.95F) : rgba(255, 255, 255, hot ? 0.22F : 0.12F), alpha),
                active ? 2.0F * hairline : hairline);
}

void Overlay::keyCap(const OverlayRect& box, const std::wstring& key, float alpha, float scale) {
    if (!box.visible()) return;
    const float radius = controlRadius(box, scale);
    fillRound(box, radius, withAlpha(rgba(255, 255, 255, 0.06F), alpha));
    strokeRound(box, radius, withAlpha(kRule, alpha), std::max(1.0F, scale));
    text(key, OverlayTextStyle::Small, box, withAlpha(kTextMuted, alpha));
}

void Overlay::flatCard(const OverlayRect& box, float radius, float alpha, float scale) {
    if (!box.visible()) return;
    fillRound(box, radius, withAlpha(kCardGround, alpha));
    strokeRound(box, radius, withAlpha(kCardEdge, alpha), std::max(1.0F, scale));
}

void Overlay::currentMark(const OverlayRect& box, int video, float alpha, float scale) {
    if (!box.visible()) return;
    const float inset = controlRadius(box, scale);
    fillRound(box, inset, withAlpha(paneColor(video, 0.14F), alpha));
    fillRound({box.x, box.y + inset, 3.0F * scale, std::max(0.0F, box.height - 2.0F * inset)}, 1.5F * scale,
              withAlpha(paneColor(video, 0.95F), alpha));
}

void Overlay::captionButton(const OverlayRect& box, wchar_t code, const wchar_t* fallback, bool close, bool hot,
                            bool pressed, float alpha) {
    if (!box.visible()) return;
    if (close && (hot || pressed)) {
        fillRound(box, 0.0F, withAlpha(kCloseRed, alpha * (pressed ? 0.82F : 1.0F)));
    } else if (pressed || hot) {
        fillRound(box, 0.0F, withAlpha(rgba(255, 255, 255, pressed ? kFacePressed : kFaceHot), alpha));
    }
    if (formats_.captionIcon) {
        text(std::wstring(1, code), OverlayTextStyle::CaptionIcon, box, withAlpha(kText, alpha));
    } else {
        text(fallback, OverlayTextStyle::Small, box, withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
}

void Overlay::scrollThumb(float right, float y, float height, bool active, float alpha, float scale) {
    const float width = (active ? 6.0F : 3.0F) * scale;
    fillRound({right - width, y, width, height}, width * 0.5F,
              withAlpha(rgba(255, 255, 255, active ? 0.55F : kRailTrack), alpha));
}

void Overlay::rail(const OverlayRect& band, float inset, float thickness, float position01,
                   float buffered01, bool showThumb, float thumbRadius, float alpha, int video) {
    if (!band.visible()) return;
    const float left = band.x + inset;
    const float right = band.right() - inset;
    if (right <= left) return;
    const float centerY = band.y + band.height * 0.5F;
    const float half = thickness * 0.5F;
    const OverlayRect track{left, centerY - half, right - left, thickness};
    fillRound(track, half, withAlpha(rgba(255, 255, 255, kRailTrack), alpha));
    const float playhead = left + (right - left) * std::clamp(position01, 0.0F, 1.0F);
    if (buffered01 > position01) {
        const float bufferedX = left + (right - left) * std::clamp(buffered01, 0.0F, 1.0F);
        fillRound({playhead, centerY - half, bufferedX - playhead, thickness}, half,
                  withAlpha(rgba(255, 255, 255, kRailTrack), alpha));
    }
    fillRound({left, centerY - half, playhead - left, thickness}, half, withAlpha(paneColor(video), alpha));
    if (showThumb && thumbRadius > 0.0F) {
        brush_->SetColor(withAlpha(kText, alpha));
        context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(playhead, centerY), thumbRadius, thumbRadius),
                              brush_.Get());
    }
}

void Overlay::verticalRail(const OverlayRect& band, float inset, float thickness, float position01,
                           float thumbRadius, float alpha, int video) {
    if (!band.visible()) return;
    const float top = band.y + inset;
    const float bottom = band.bottom() - inset;
    if (bottom <= top) return;
    const float centerX = band.x + band.width * 0.5F;
    const float half = thickness * 0.5F;
    fillRound({centerX - half, top, thickness, bottom - top}, half, withAlpha(rgba(255, 255, 255, kRailTrack), alpha));
    const float level = bottom - (bottom - top) * std::clamp(position01, 0.0F, 1.0F);
    fillRound({centerX - half, level, thickness, bottom - level}, half, withAlpha(paneColor(video), alpha));
    if (thumbRadius > 0.0F) {
        brush_->SetColor(withAlpha(kText, alpha));
        context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, level), thumbRadius, thumbRadius),
                              brush_.Get());
    }
}

bool Overlay::beginFrame(IDXGISurface* surface, bool transparentLayer) {
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          transparentLayer ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        96.0F, 96.0F);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
    if (FAILED(context_->CreateBitmapFromDxgiSurface(surface, &properties, &target))) return false;
    context_->SetTarget(target.Get());
    ++frame_;
    decodesThisFrame_ = 0;
    context_->BeginDraw();
    return true;
}

void Overlay::draw(IDXGISurface* surface, const OverlayScene& scene, bool transparentLayer) {
    if (!context_ || !surface || !ensureFormats(scene.scale) || !beginFrame(surface, transparentLayer)) return;
    // Text is already grayscale-antialiased (initialize), which is what a
    // transparent target needs; ClearType would leave alpha wrong.
    if (transparentLayer) context_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
    drawPanes(scene);
    drawBar(scene);
    drawCaptionBand(scene);
    drawEmptyHint(scene);
    drawPanel(scene);
    drawCaption(scene);
    drawSyncPanel(scene);
    drawPanelMenu(scene);
    drawNotice(scene);
    drawTooltip(scene);
    endFrame();
}

void Overlay::drawSheetWindow(IDXGISurface* surface, const OverlayScene& scene) {
    if (!context_ || !surface || !ensureFormats(scene.scale) || !beginFrame(surface, false)) return;
    // The colour the sheet is painted in over the picture, here over nothing.
    context_->Clear(kSheetGround);
    drawPanel(scene);
    drawTooltip(scene);
    endFrame();
}

void Overlay::drawMenuWindow(IDXGISurface* surface, const OverlayScene& scene, bool border) {
    if (!context_ || !surface || !ensureFormats(scene.scale) || !beginFrame(surface, false)) return;
    // The colour a raised card is filled with (raisedCard).
    context_->Clear(kCardGround);
    const auto& menu = scene.panelMenu;
    drawPanelMenuCard(menu.items, menu.layout, menu.highlighted, menu.pressed, -1, 1.0F, 0.0F, scene.scale,
                      menu.position, false);
    if (border) {
        strokeRound({0.0F, 0.0F, scene.width, scene.height}, 0.0F, kCardEdge, std::max(1.0F, scene.scale));
    }
    endFrame();
}

void Overlay::measureDeleteDialog(const DeleteDialogContent& content, float width, float scale,
                                  DeleteDialogMeasures& measures) {
    measures = {};
    if (!std::isfinite(width) || !std::isfinite(scale) || width <= 0.0F || scale <= 0.0F || !ensureFormats(scale)) {
        return;
    }
    const DeleteDialogMetrics metrics{};
    // How tall `value` wraps at `wrapWidth`, `line` a line, as mediaText
    // draws it; 0 where it cannot be laid out.
    const auto wrapped = [&](const std::wstring& value, OverlayTextStyle style, float wrapWidth, float line) {
        IDWriteTextLayout* layout = mediaTextLayout(value, style, wrapWidth, 1000000.0F, line * scale);
        DWRITE_TEXT_METRICS measured{};
        if (!layout || FAILED(layout->GetMetrics(&measured)) || !std::isfinite(measured.height)) return 0.0F;
        return std::ceil(measured.height);
    };
    const float inner = deleteDialogInnerWidth(width, scale, metrics);
    measures.titleWidth = measureText(content.title, OverlayTextStyle::BodyBold, scale);
    measures.titleHeight = wrapped(content.title, OverlayTextStyle::BodyBold, inner, metrics.titleLine);
    measures.typeWidth = measureText(content.type, OverlayTextStyle::Small, scale);
    const float fileWidth = deleteDialogFileWidth(width, scale, metrics);
    measures.fileHeights.reserve(content.files.size());
    for (const auto& file : content.files) {
        measures.fileHeights.push_back(wrapped(file, OverlayTextStyle::Small, fileWidth, metrics.smallLine));
    }
    measures.warningHeight = wrapped(content.warning, OverlayTextStyle::Small, inner, metrics.smallLine);
    measures.cancelWidth = measureText(content.cancel, OverlayTextStyle::Body, scale);
    measures.confirmWidth = measureText(content.confirm, OverlayTextStyle::Body, scale);
}

void Overlay::drawDeleteDialog(IDXGISurface* surface, const OverlayDeleteDialogScene& scene) {
    if (!context_ || !surface || !scene.content || !scene.layout || !ensureFormats(scene.scale) ||
        !beginFrame(surface, false)) {
        return;
    }
    const DeleteDialogContent& content = *scene.content;
    const DeleteDialogLayout& layout = *scene.layout;
    const DeleteDialogMetrics metrics{};
    const float s = scene.scale;
    // The settings window's ground and caption strip: its question small at
    // the left, the square close turning red.
    context_->Clear(kSheetGround);
    text(content.caption, OverlayTextStyle::Small, layout.captionTitle, kText, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    captionButton(layout.close, kGlyphChromeClose, L"X", true, scene.hot == DeleteDialogHit::Close,
                  scene.pressed == DeleteDialogHit::Close, 1.0F);
    // The title in a list's bold, its type dim after it; where it plays
    // under them.
    if (layout.titleWrapped) mediaText(content.title, OverlayTextStyle::BodyBold, layout.title, metrics.titleLine * s, kText);
    else text(content.title, OverlayTextStyle::BodyBold, layout.title, kText, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    text(content.type, OverlayTextStyle::Small, layout.type, kTextMuted, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    text(content.playing, OverlayTextStyle::Small, layout.playing, kTextDim, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    // The files on a raised card's ground, scrolled inside its padding.
    if (layout.list.visible()) {
        fillRound(layout.list, metrics.listRadius * s, kCardGround);
        if (layout.listView.visible()) {
            context_->PushAxisAlignedClip(toRect(layout.listView), D2D1_ANTIALIAS_MODE_ALIASED);
            for (std::size_t i = 0; i < layout.files.size() && i < content.files.size(); ++i) {
                const OverlayRect& box = layout.files[i];
                if (box.bottom() <= layout.listView.y || box.y >= layout.listView.bottom()) continue;
                mediaText(content.files[i], OverlayTextStyle::Small, box, metrics.smallLine * s, kTextDim);
            }
            context_->PopAxisAlignedClip();
        }
        if (const OverlayRect thumb = deleteDialogThumb(layout, s, metrics); thumb.visible()) {
            scrollThumb(layout.scrollbar.right(), thumb.y, thumb.height,
                        scene.thumbDragging || scene.hot == DeleteDialogHit::Scrollbar, 1.0F, s);
        }
    }
    mediaText(content.warning, OverlayTextStyle::Small, layout.warning, metrics.smallLine * s, kAlert);
    // Cancel a plain button; Delete in a close button's red.
    quietButton(layout.cancel, scene.hot == DeleteDialogHit::Cancel, scene.pressed == DeleteDialogHit::Cancel, 1.0F, s);
    text(content.cancel, OverlayTextStyle::Body, layout.cancel, kText);
    dangerButton(layout.confirm, scene.hot == DeleteDialogHit::Confirm, scene.pressed == DeleteDialogHit::Confirm,
                 1.0F, s);
    text(content.confirm, OverlayTextStyle::Body, layout.confirm, rgba(255, 255, 255, 1.0F));
    endFrame();
}

void Overlay::endFrame() {
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
            strokeRound(cell, 0.0F, paneColor(pane.position, 0.95F), std::max(2.0F, 3.0F * s));
        } else if (const float edge = paneMarkAlpha(pane.audioOn, scene.activePaneCount, pane.chromeAlpha);
                   edge > 0.0F) {
            // Which pane is being heard, without a label: a fine edge in the
            // video's colour that fades with the pane's chrome.
            strokeRound(cell, 0.0F, withAlpha(paneColor(pane.position, 0.55F), edge), std::max(1.0F, 2.0F * s));
        }
        if (const float outline = paneMarkAlpha(pane.targeted, scene.activePaneCount, pane.chromeAlpha);
            outline > 0.0F) {
            // The playback destination is independent of the audio edge.
            // Its inner outline, in the same colour but solid and inside
            // it, fades with the pane's chrome.
            const float inset = 4.0F * s;
            strokeRound({cell.x + inset, cell.y + inset, cell.width - 2.0F * inset,
                         cell.height - 2.0F * inset}, 0.0F, withAlpha(paneColor(pane.position, 0.95F), outline),
                        std::max(1.0F, 2.0F * s));
        }

        // Subtitles belong to the picture, not the chrome: they stay while
        // the strip has faded. libass's picture, when it drew them;
        // otherwise the plain lines, the bottom ones at their own height
        // plus what the viewer raised them by, or clear of the transport bar
        // and the pane's strip while they are up, whichever is higher.
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

        // The tag fades with the pane's chrome; the rest of the strip with
        // its own reveal, on the pane under the pointer.
        const float alpha = std::clamp(pane.chromeAlpha, 0.0F, 1.0F);
        const float stripAlpha = std::clamp(pane.timelineAlpha, 0.0F, 1.0F);
        if (std::max(alpha, stripAlpha) <= 0.0F) continue;
        const auto& chrome = pane.chrome;
        // With no scrim -- the row alone, a Linked video's on the bar's rail,
        // a short pane's or the only video's -- the row is one capsule as
        // the bar's "All" is (`PaneChromeLayout::capsule`): the tag, pause
        // and time, then the buttons, which need no discs of their own.
        const bool floating = !chrome.band.visible();
        if (stripAlpha > 0.0F) {
            // A scrim as the bar's: clear at its top, darkest at the edge.
            if (!floating) fillScrim(chrome.band, chrome.band.y, chrome.band.bottom(), stripAlpha);
            else if (chrome.capsule.visible()) {
                const float radius = chrome.capsule.height * 0.5F;
                fillRound(chrome.capsule, radius, withAlpha(kCapsuleFill, stripAlpha));
                strokeRound(chrome.capsule, radius, withAlpha(kCapsuleEdge, stripAlpha), std::max(1.0F, s));
            }
            if (chrome.timeline.visible() && pane.seekable) {
                const bool emphasised = pane.railHot || pane.railDragging;
                rail(chrome.timeline, 12.0F * s, (emphasised ? 6.0F : 4.0F) * s,
                     pane.position01, pane.buffered01, emphasised, 7.0F * s, stripAlpha, pane.position);
            } else if (chrome.timeline.visible()) {
                rail(chrome.timeline, 12.0F * s, 4.0F * s, 0.0F, 0.0F, false, 0.0F, stripAlpha * 0.5F);
            }
            if (chrome.time.visible()) {
                text(pane.timeText, OverlayTextStyle::Small, chrome.time, withAlpha(kText, stripAlpha),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
            }
            struct Chip { wchar_t code; const wchar_t* fallback; bool toggled; bool dimmed; };
            const std::array<Chip, kPaneChipCount> chips{{
                {pane.audioOn ? kGlyphVolume : kGlyphMute, L"Aud", pane.audioOn, false},
                {pane.paused ? kGlyphPlay : kGlyphPause, pane.paused ? L"Go" : L"||", pane.paused, false},
                {pane.solo ? kGlyphWindowed : kGlyphFullscreen, L"Solo", pane.solo, false},
                {kGlyphRepeat, L"Rpt", pane.repeat, false},
                {kGlyphClose, L"X", false, false},
                // A switch, as the bar's: lit while its lines show, greyed
                // while they do not.
                {kGlyphSubtitles, L"CC", pane.subtitlesOn, !pane.subtitlesOn},
            }};
            for (std::size_t chip = 0; chip < chips.size(); ++chip) {
                const auto& box = chrome.chips[chip];
                if (!box.visible()) continue;
                if (!floating) fillRound(box, box.height * 0.5F, withAlpha(kPillFill, stripAlpha));
                const bool idle = chip == static_cast<std::size_t>(PaneChip::Repeat) && pane.repeatIdle;
                button(box, chips[chip].code, chips[chip].fallback,
                       pane.hotChip == static_cast<int>(chip),
                       pane.pressedChip == static_cast<int>(chip), chips[chip].toggled,
                       idle ? stripAlpha * 0.45F : stripAlpha, s, chips[chip].dimmed, pane.position);
            }
            if (chrome.volume.visible()) {
                const bool emphasised = pane.volumeHot || pane.volumeDragging;
                if (chrome.volumeUpright) {
                    // Standing on the speaker, as the bar's popup: a pill
                    // with the rail standing in it.
                    const OverlayRect pill = uprightVolumePill(chrome);
                    fillRound(pill, pill.width * 0.5F, withAlpha(kPillFill, stripAlpha));
                    verticalRail(pill, 12.0F * s, (emphasised ? 5.0F : 4.0F) * s, pane.volume01, 6.0F * s,
                                 stripAlpha, pane.position);
                } else {
                    rail(chrome.volume, 12.0F * s, (emphasised ? 5.0F : 4.0F) * s, pane.volume01, 0.0F,
                         true, 6.0F * s, stripAlpha, pane.position);
                }
            }
        }
        if (chrome.pill.visible()) {
            // In the video's own colour, lighter under the pointer, darker pressed.
            const float tagAlpha = std::max(alpha, stripAlpha);
            const float radius = chrome.pill.height * 0.5F;
            const D2D1_COLOR_F own = paneColor(pane.position, 0.9F);
            fillRound(chrome.pill, radius,
                      withAlpha(pane.pillPressed ? blend(own, rgba(0, 0, 0, 1.0F), 0.25F)
                                : pane.pillHot ? blend(own, rgba(255, 255, 255, 1.0F), 0.18F) : own, tagAlpha));
            strokeRound(chrome.pill, radius,
                        withAlpha(pane.targeted ? rgba(230, 237, 248, 0.72F) : kPillEdge, tagAlpha),
                        std::max(1.0F, s));
            text(pane.label, OverlayTextStyle::BodyBold, chrome.pill, withAlpha(kText, tagAlpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
        // Over the rail at the pointer: where a seek lands and what it reaches.
        if (stripAlpha > 0.0F && chrome.timeline.visible() && pane.seekable && !pane.railText.empty()) {
            const float width = measureText(pane.railText, OverlayTextStyle::Small, s) + 16.0F * s;
            const float height = 22.0F * s;
            OverlayRect bubble{pane.railX - width * 0.5F, chrome.timeline.y - height, width, height};
            bubble.x = std::clamp(bubble.x, cell.x, std::max(cell.x, cell.right() - width));
            fillRound(bubble, height * 0.5F, withAlpha(kPillFill, stripAlpha));
            strokeRound(bubble, height * 0.5F, withAlpha(paneColor(pane.position, 0.8F), stripAlpha),
                        std::max(1.0F, s));
            text(pane.railText, OverlayTextStyle::Small, bubble, withAlpha(kText, stripAlpha));
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
    // The capsule is a pill of its own, edged, apart from any pane's strip;
    // its "All" a lighter pill in it.
    if (bar.capsule && bar.row.visible()) {
        const float radius = bar.row.height * 0.5F;
        fillRound(bar.row, radius, withAlpha(kCapsuleFill, alpha));
        strokeRound(bar.row, radius, withAlpha(kCapsuleEdge, alpha), std::max(1.0F, s));
        for (const OverlayRect* line : {&bar.divider, &bar.videosDivider}) {
            if (line->visible()) fillRound(*line, 0.0F, withAlpha(rgba(255, 255, 255, 0.25F), alpha));
        }
    }
    if (scene.activePaneCount > 1 && bar.scopeLabel.visible() && !scene.scopeText.empty()) {
        fillRound(bar.scopeLabel, bar.scopeLabel.height * 0.5F,
                  withAlpha(bar.capsule ? rgba(255, 255, 255, 0.14F) : kPillFill, alpha));
        text(scene.scopeText, bar.capsule ? OverlayTextStyle::BodyBold : OverlayTextStyle::Small, bar.scopeLabel,
             withAlpha(bar.capsule ? kText : kTextDim, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
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
             7.0F * s, scene.seekable ? alpha : alpha * 0.45F, scene.seekPosition);
        // Where each video sharing it ends: a tick across it in its colour.
        const auto& seek = bar[BarItem::Seek];
        const float left = seek.x + 8.0F * s;
        const float right = seek.right() - 8.0F * s;
        const float middle = seek.y + seek.height * 0.5F;
        for (std::size_t position = 0; position < scene.seekEnds.size(); ++position) {
            const float end = scene.seekEnds[position];
            if (end < 0.0F || end > 1.0F || right <= left) continue;
            const float x = left + (right - left) * end;
            fillRound({x - 1.0F * s, middle - 7.0F * s, 2.0F * s, 14.0F * s}, 1.0F * s,
                      withAlpha(paneColor(static_cast<int>(position)), alpha));
        }
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
    // The videos' buttons: All videos lit while every video shows, a
    // video's lit in its colour while it shows alone.
    if (bar[BarItem::AllVideos].visible()) {
        button(bar[BarItem::AllVideos], kGlyphAllVideos, L"All", hot(BarItem::AllVideos),
               pressed(BarItem::AllVideos), scene.soloPosition < 0, alpha, s);
    }
    constexpr std::array<const wchar_t*, kMaxPanes> kVideoNames{L"V1", L"V2", L"V3", L"V4", L"V5"};
    for (std::size_t position = 0; position < kMaxPanes; ++position) {
        const BarItem item = soloBarItem(position);
        if (!bar[item].visible()) continue;
        const int place = static_cast<int>(position);
        labelButton(bar[item], kVideoNames[position], hot(item), pressed(item), scene.soloPosition == place,
                    alpha, s, place);
    }
    // Lit while the videos share one timeline; lit under the pointer while
    // its panel is open.
    if (bar[BarItem::Sync].visible()) {
        button(bar[BarItem::Sync], kGlyphLink, L"Sync", hot(BarItem::Sync) || scene.syncPanel.layout.card.visible(),
               pressed(BarItem::Sync), scene.syncLinked, alpha, s);
    }
    if (bar[BarItem::Layout].visible()) {
        button(bar[BarItem::Layout], kGlyphLayout, L"Grid", hot(BarItem::Layout),
               pressed(BarItem::Layout), false, alpha, s);
    }
    if (bar[BarItem::Sets].visible()) {
        button(bar[BarItem::Sets], kGlyphVideoSets, L"Sets", hot(BarItem::Sets), pressed(BarItem::Sets),
               scene.videoSetPlaying, alpha, s);
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
        // Square, edge to edge, and Close turns red: the buttons every
        // Windows title bar has, and the settings window's.
        captionButton(box, button.code, button.fallback, button.item == CaptionItem::Close,
                      scene.hotCaptionItem == index, scene.pressedCaptionItem == index, alpha);
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

// Raised cards a shade lighter than the sheet, softly shadowed: options
// with the chosen one ticked in the accent, or in the colour of the video
// the menu is about, switches, segments cut from a track, a submenu's
// chevron, keys in dim caps at the lines' ends. A submenu's card lies over
// the edge of the one it opened from, whose line stays lit.
void Overlay::drawPanelMenu(const OverlayScene& scene) {
    const auto& menu = scene.panelMenu;
    if (menu.items.empty()) return;
    drawPanelMenuCard(menu.items, menu.layout, menu.highlighted, menu.pressed,
                      menu.submenus.empty() ? -1 : menu.submenus.front().from, menu.alpha, menu.offset, scene.scale,
                      menu.position);
    const std::vector<PanelMenuItem>* items = &menu.items;
    for (std::size_t depth = 0; depth < menu.submenus.size(); ++depth) {
        const auto& submenu = menu.submenus[depth];
        if (submenu.from < 0 || static_cast<std::size_t>(submenu.from) >= items->size()) break;
        items = &(*items)[static_cast<std::size_t>(submenu.from)].children;
        drawPanelMenuCard(*items, submenu.layout, submenu.highlighted, submenu.pressed,
                          depth + 1 < menu.submenus.size() ? menu.submenus[depth + 1].from : -1, submenu.alpha,
                          0.0F, scene.scale, menu.position);
    }
}

void Overlay::drawPanelMenuCard(const std::vector<PanelMenuItem>& items, const PanelMenuLayout& layout,
                                int highlighted, int pressed, int opened, float cardAlpha, float offset, float s,
                                int video, bool raised) {
    const float alpha = std::clamp(cardAlpha, 0.0F, 1.0F);
    if (alpha <= 0.0F || items.empty() || !layout.card.visible()) return;
    const float hairline = std::max(1.0F, s);
    const auto placed = [&](OverlayRect box) {
        box.y += offset;
        return box;
    };
    const OverlayRect card = placed(layout.card);
    if (raised) raisedCard(card, alpha, s);
    // Cut to a short window, its lines scroll inside the card, a thin thumb
    // at its right edge saying how far.
    const OverlayRect view = placed(layout.view);
    if (!view.visible()) return;
    context_->PushAxisAlignedClip(toRect(view), D2D1_ANTIALIAS_MODE_ALIASED);
    if (layout.maxScroll > 0.0F) {
        const float thumbHeight = std::max(24.0F * s, view.height * view.height / (view.height + layout.maxScroll));
        const float thumbY = view.y + (view.height - thumbHeight) * (layout.scroll / layout.maxScroll);
        scrollThumb(card.right() - 2.0F * s, thumbY, thumbHeight, false, alpha, s);
    }
    for (const auto& track : layout.tracks) segmentTrack(placed(track), alpha, s);
    const D2D1_COLOR_F chosen = paneInkColor(video);
    const D2D1_COLOR_F tick = blend(paneColor(video), rgba(255, 255, 255, 1.0F), 0.35F);
    const PanelMenuMetrics metrics{};
    for (std::size_t i = 0; i < items.size() && i < layout.items.size(); ++i) {
        const auto& item = items[i];
        const OverlayRect box = placed(layout.items[i]);
        if (!box.visible()) continue;
        const float itemAlpha = item.enabled ? alpha : alpha * kDisabledAlpha;
        // A submenu's line stays lit while its card is open, unless the
        // pointer lights another line of this card: one line lit at a time.
        const bool lit = item.enabled && (highlighted == static_cast<int>(i) ||
                                          (highlighted < 0 && opened == static_cast<int>(i)));
        const bool down = lit && pressed == static_cast<int>(i);
        switch (item.kind) {
        case PanelMenuItemKind::Separator:
            fillRound({box.x + 6.0F * s, std::round(box.y + box.height * 0.5F), box.width - 12.0F * s, hairline},
                      0.0F, withAlpha(kRule, alpha));
            break;
        case PanelMenuItemKind::Segment: {
            if (item.checked) chosenFace(box, lit, itemAlpha, s, video);
            else litFace(box, lit, down, itemAlpha, s);
            text(item.label, OverlayTextStyle::Body, box, withAlpha(item.checked ? chosen : kText, itemAlpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
            break;
        }
        case PanelMenuItemKind::Option:
        case PanelMenuItemKind::Check:
        case PanelMenuItemKind::Submenu: {
            litFace(box, lit, down, alpha, s);
            const bool option = item.kind == PanelMenuItemKind::Option;
            const float labelX = box.x + (item.kind == PanelMenuItemKind::Check ? layout.checkLabelInset
                                                                              : layout.optionLabelInset);
            float labelRight = box.right() - 12.0F * s;
            if (option && item.checked) {
                glyph(kGlyphCheckMark, L"\x2713", {box.x, box.y, layout.optionLabelInset, box.height},
                      withAlpha(tick, itemAlpha));
            }
            if (item.kind == PanelMenuItemKind::Check) {
                const OverlayRect knob{box.right() - (metrics.inset + metrics.switchWidth) * s,
                                       box.y + (box.height - metrics.switchHeight * s) * 0.5F,
                                       metrics.switchWidth * s, metrics.switchHeight * s};
                toggleSwitch(knob, item.checked, lit, itemAlpha, s, video);
                labelRight = knob.x - 8.0F * s;
            }
            if (item.kind == PanelMenuItemKind::Submenu) {
                const OverlayRect chevron{box.right() - 28.0F * s, box.y, 20.0F * s, box.height};
                const D2D1_COLOR_F color = withAlpha(lit ? kText : kTextMuted, itemAlpha);
                if (formats_.captionIcon) {
                    text(std::wstring(1, kGlyphChevronRight), OverlayTextStyle::CaptionIcon, chevron, color);
                } else {
                    text(L"\x203A", OverlayTextStyle::Small, chevron, color, DWRITE_TEXT_ALIGNMENT_CENTER, true);
                }
                labelRight = chevron.x - 4.0F * s;
            }
            if (!item.shortcut.empty()) {
                // Its key ends the line, or stands before its switch; a
                // one-character key in a square cap, not a sliver.
                const float keyWidth = std::max(20.0F * s, measureText(item.shortcut, OverlayTextStyle::Small, s) +
                                                               12.0F * s);
                const float keyRight = item.kind == PanelMenuItemKind::Check ? labelRight : box.right() - 6.0F * s;
                const OverlayRect key{keyRight - keyWidth, box.y + (box.height - 20.0F * s) * 0.5F, keyWidth, 20.0F * s};
                keyCap(key, item.shortcut, itemAlpha, s);
                labelRight = key.x - 8.0F * s;
            }
            text(item.label, panelMenuLabelStyle(item), {labelX, box.y, std::max(0.0F, labelRight - labelX), box.height},
                 withAlpha(option && item.checked ? chosen : kText, itemAlpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            break;
        }
        }
    }
    context_->PopAxisAlignedClip();
}

void Overlay::raisedCard(const OverlayRect& card, float alpha, float s) {
    const float radius = 10.0F * s;
    for (int ring = 3; ring >= 1; --ring) {
        const float spread = static_cast<float>(ring) * 3.0F * s;
        fillRound({card.x - spread, card.y - spread + 5.0F * s, card.width + 2.0F * spread,
                   card.height + 2.0F * spread}, radius + spread, rgba(0, 0, 0, 0.09F * alpha));
    }
    fillRound(card, radius, withAlpha(kCardGround, alpha));
    strokeRound(card, radius, withAlpha(kCardEdge, alpha), std::max(1.0F, s));
}

// The sync panel on a menu's card: the mode's segments as a drop-down's,
// each video's tag in its colour, its offset lit in it while it has one,
// its steps as small keys, its repeat as its strip's; the reset with its key
// in a cap.
void Overlay::drawSyncPanel(const OverlayScene& scene) {
    const auto& panel = scene.syncPanel;
    const float alpha = std::clamp(panel.alpha, 0.0F, 1.0F);
    if (alpha <= 0.0F || !panel.layout.card.visible()) return;
    const float s = scene.scale;
    const auto placed = [&](OverlayRect box) {
        box.y += panel.offset;
        return box;
    };
    const auto& layout = panel.layout;
    const auto is = [&](const SyncPanelHit& hit, SyncPanelHitKind kind, int row, int part) {
        return hit.kind == kind && hit.row == row && hit.part == part;
    };
    raisedCard(placed(layout.card), alpha, s);
    text(L"Seek bars", OverlayTextStyle::Body, placed(layout.title), withAlpha(kText, alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    segmentTrack(placed(layout.track), alpha, s);
    constexpr std::array<const wchar_t*, 2> modeNames{L"Linked", L"Independent"};
    for (std::size_t mode = 0; mode < layout.modes.size(); ++mode) {
        const OverlayRect box = placed(layout.modes[mode]);
        const int index = static_cast<int>(mode);
        const bool chosen = panel.mode == index;
        const bool hot = is(panel.hot, SyncPanelHitKind::Mode, -1, index);
        const bool down = is(panel.pressed, SyncPanelHitKind::Mode, -1, index);
        if (chosen) chosenFace(box, hot, alpha, s);
        else litFace(box, hot, down, alpha, s);
        text(modeNames[mode], OverlayTextStyle::Body, box, withAlpha(chosen ? paneInkColor(-1) : kText, alpha),
             DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
    text(panel.hint, OverlayTextStyle::Small, placed(layout.hint), withAlpha(kTextDim, alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    fillRound(placed(layout.rule), 0.0F, withAlpha(kRule, alpha));

    constexpr std::array<const wchar_t*, kSyncSteps> stepNames{L"\x2212" L"1 s", L"\x2212" L"0.1 s", L"+0.1 s",
                                                              L"+1 s"};
    for (std::size_t index = 0; index < layout.rowCount && index < panel.rows.size(); ++index) {
        const auto& row = layout.rows[index];
        const auto& video = panel.rows[index];
        const int rowIndex = static_cast<int>(index);
        const OverlayRect tag = placed(row.tag);
        fillRound(tag, tag.height * 0.5F, withAlpha(paneColor(video.position, 0.9F), alpha));
        strokeRound(tag, tag.height * 0.5F, withAlpha(kPillEdge, alpha), std::max(1.0F, s));
        text(video.label, OverlayTextStyle::BodyBold, tag, withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
        text(video.offset, OverlayTextStyle::Body, placed(row.value),
             withAlpha(video.offsetSet ? paneInkColor(video.position) : kText, alpha),
             DWRITE_TEXT_ALIGNMENT_LEADING, true);
        for (std::size_t step = 0; step < kSyncSteps; ++step) {
            const OverlayRect box = placed(row.steps[step]);
            const int part = static_cast<int>(step);
            const bool down = is(panel.pressed, SyncPanelHitKind::Step, rowIndex, part) &&
                              is(panel.hot, SyncPanelHitKind::Step, rowIndex, part);
            const bool hot = is(panel.hot, SyncPanelHitKind::Step, rowIndex, part);
            quietButton(box, hot, down, alpha, s);
            text(stepNames[step], OverlayTextStyle::Small, box, withAlpha(kText, alpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
        const OverlayRect repeat = placed(row.repeat);
        fillRound(repeat, repeat.height * 0.5F, withAlpha(kPillFill, alpha));
        button(repeat, kGlyphRepeat, L"Rpt", is(panel.hot, SyncPanelHitKind::Repeat, rowIndex, -1),
               is(panel.pressed, SyncPanelHitKind::Repeat, rowIndex, -1), video.repeat,
               video.repeatInForce ? alpha : alpha * kDisabledAlpha, s, false, video.position);
    }
    fillRound(placed(layout.footRule), 0.0F, withAlpha(kRule, alpha));

    // The reset, greyed while no video has an offset to take away.
    const OverlayRect reset = placed(layout.reset);
    const float resetAlpha = panel.resetEnabled ? alpha : alpha * kDisabledAlpha;
    const bool resetHot = panel.resetEnabled && is(panel.hot, SyncPanelHitKind::Reset, -1, -1);
    const bool resetDown = resetHot && is(panel.pressed, SyncPanelHitKind::Reset, -1, -1);
    quietButton(reset, resetHot, resetDown, resetAlpha, s);
    float labelRight = reset.right() - 12.0F * s;
    if (!panel.resetKey.empty()) {
        const float keyWidth = std::max(20.0F * s, measureText(panel.resetKey, OverlayTextStyle::Small, s) + 12.0F * s);
        const OverlayRect key{reset.right() - 8.0F * s - keyWidth, reset.y + (reset.height - 20.0F * s) * 0.5F,
                              keyWidth, 20.0F * s};
        keyCap(key, panel.resetKey, resetAlpha, s);
        labelRight = key.x - 8.0F * s;
    }
    text(L"Reset all offsets", OverlayTextStyle::Small,
         {reset.x + 12.0F * s, reset.y, std::max(0.0F, labelRight - reset.x - 12.0F * s), reset.height},
         withAlpha(kText, resetAlpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    text(panel.footHint, OverlayTextStyle::Small, placed(layout.footHint), withAlpha(kTextDim, alpha),
         DWRITE_TEXT_ALIGNMENT_TRAILING, true);
}

void Overlay::toggleSwitch(const OverlayRect& box, bool on, bool hot, float alpha, float scale, int video) {
    if (!box.visible()) return;
    const float radius = box.height * 0.5F;
    D2D1_COLOR_F track = on ? paneColor(video, 0.9F) : rgba(255, 255, 255, 0.22F);
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
             withAlpha(field.focused ? kAccentText : kTextDim, alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }
    // Typed into, it is ringed in the accent, as an open drop-down is.
    fieldBox(box, hot, field.focused, alpha, s);
    if (field.magnifier) {
        glyph(kGlyphSearch, L"\x2315", {box.x + 12.0F * s, box.y, 22.0F * s, box.height},
              withAlpha(field.focused ? kAccentText : kTextDim, alpha));
    }

    const OverlayRect textBox = panelTextFieldTextBox(row, geometry, s);
    const std::wstring shown = panelTextFieldShown(field);
    if (textBox.visible()) {
        context_->PushAxisAlignedClip(toRect(textBox), D2D1_ANTIALIAS_MODE_ALIASED);
        if (shown.empty()) {
            text(field.placeholder, OverlayTextStyle::Body, textBox, withAlpha(kTextMuted, alpha),
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
            fillRound(clear, clear.height * 0.5F,
                      withAlpha(rgba(255, 255, 255, clearPressed ? kFacePressed : kFaceHot), alpha));
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
    fillRound(layout.sheet, 0.0F, withAlpha(kSheetGround, 0.94F * alpha));
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
    // Its edge against the picture; a window's sheet, the whole of it, has
    // none to stand against.
    if (layout.sheet.x > 0.5F) {
        fillRound({layout.sheet.x, layout.sheet.y, std::max(1.0F, s), layout.sheet.height}, 0.0F,
                  withAlpha(rgba(255, 255, 255, 0.10F), alpha));
    }
    const float pad = 20.0F * s;
    const bool tabColumn = layout.tabColumn.visible();
    const bool windowCaption = layout.caption.visible();
    if (tabColumn) {
        // The tabs down a column at the left, as a window's navigation: the
        // title over them, the chosen one marked as the item being played
        // in a list is, each with its icon where the icon face is there.
        fillRound(layout.tabColumn, 0.0F, withAlpha(rgba(255, 255, 255, 0.035F), alpha));
        fillRound({layout.tabColumn.right() - std::max(1.0F, s), layout.tabColumn.y, std::max(1.0F, s),
                   layout.tabColumn.height}, 0.0F, withAlpha(kRule, alpha));
        if (!windowCaption) {
            text(panel.title, OverlayTextStyle::Title,
                 {layout.tabTitle.x + pad, layout.tabTitle.y, std::max(0.0F, layout.tabTitle.width - pad - 8.0F * s),
                  layout.tabTitle.height},
                 withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
        const bool glyphs = formats_.icon && panel.tabGlyphs.size() >= layout.tabs.size();
        for (std::size_t i = 0; i < layout.tabs.size() && i < panel.tabs.size(); ++i) {
            const auto& box = layout.tabs[i];
            const bool selected = panel.tab == static_cast<int>(i);
            const bool hot = panel.hotKind == PanelHitKind::Tab && panel.hotPart == static_cast<int>(i);
            const bool pressed = panel.pressedKind == PanelHitKind::Tab && panel.pressedPart == static_cast<int>(i);
            if (selected) currentMark(box, -1, alpha, s);
            litFace(box, hot, pressed, alpha, s);
            const D2D1_COLOR_F ink = withAlpha(selected || hot ? kText : kTextDim, alpha);
            float labelX = box.x + 16.0F * s;
            if (glyphs) {
                text(std::wstring(1, panel.tabGlyphs[i]), OverlayTextStyle::Icon,
                     {box.x + 12.0F * s, box.y, 20.0F * s, box.height}, ink);
                labelX = box.x + 44.0F * s;
            }
            text(panel.tabs[i], OverlayTextStyle::Body,
                 {labelX, box.y, std::max(0.0F, box.right() - 8.0F * s - labelX), box.height}, ink,
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
    }
    const bool switches = !layout.sheetSwitches.empty() && layout.sheetSwitches.front().visible();
    const float titleRight = (switches ? layout.sheetSwitches.front().x : layout.close.x) - 8.0F * s;
    const bool tabHeading = tabColumn && panel.tab >= 0 && static_cast<std::size_t>(panel.tab) < panel.tabs.size();
    text(tabHeading ? panel.tabs[static_cast<std::size_t>(panel.tab)] : panel.title, OverlayTextStyle::Title,
         {layout.header.x + pad, layout.header.y,
          std::max(0.0F, titleRight - layout.header.x - pad), layout.header.height},
         withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    if (windowCaption) {
        // A window's own caption, as the player's: its title small at the
        // left, its close square at the right end, red under the pointer.
        text(panel.title, OverlayTextStyle::Small, layout.tabTitle, withAlpha(kText, alpha),
             DWRITE_TEXT_ALIGNMENT_LEADING, true);
        captionButton(layout.close, kGlyphChromeClose, L"X", true, panel.hotKind == PanelHitKind::Close,
                      panel.pressedKind == PanelHitKind::Close, alpha);
    } else {
        button(layout.close, kGlyphClose, L"X", panel.hotKind == PanelHitKind::Close,
               panel.pressedKind == PanelHitKind::Close, false, alpha, s);
    }
    for (std::size_t i = 0; i < layout.sheetSwitches.size() && i < panel.switchCaptions.size(); ++i) {
        // The switches to the sheet's other faces, drawn as chosen segments
        // so they read as buttons beside the plain close glyph.
        const auto& box = layout.sheetSwitches[i];
        if (!box.visible() || panel.switchCaptions[i].empty()) continue;
        const bool hot = panel.hotKind == PanelHitKind::Switch && panel.hotPart == static_cast<int>(i);
        const bool pressed = panel.pressedKind == PanelHitKind::Switch && panel.pressedPart == static_cast<int>(i);
        chosenFace(box, hot, alpha, s);
        if (pressed) fillRound(box, controlRadius(box, s), withAlpha(rgba(255, 255, 255, kFacePressed), alpha));
        text(panel.switchCaptions[i], OverlayTextStyle::Small, box, withAlpha(paneInkColor(-1), alpha),
             DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
    fillRound({layout.header.x, layout.header.bottom() - std::max(1.0F, s), layout.header.width,
               std::max(1.0F, s)}, 0.0F, withAlpha(kRule, alpha));
    if (layout.pinned.visible()) {
        // The pinned rows are closed by the same rule as the header.
        fillRound({layout.pinned.x, layout.pinned.bottom() - std::max(1.0F, s), layout.pinned.width,
                   std::max(1.0F, s)}, 0.0F, withAlpha(kRule, alpha));
    }

    // The cards under the content's rows, as Windows' own settings: a
    // hairline between two rows on one, but above a note, which belongs to
    // the row over it.
    const bool onCards = !layout.cards.empty();
    if (onCards) {
        context_->PushAxisAlignedClip(toRect(layout.content), D2D1_ANTIALIAS_MODE_ALIASED);
        const float radius = PanelMetrics{}.cardRadius * s;
        for (const auto& card : layout.cards) {
            if (card.box.bottom() < layout.content.y || card.box.y > layout.content.bottom()) continue;
            flatCard(card.box, radius, alpha, s);
            for (std::size_t i = card.first + 1; i < card.end && i < rows.size() && i < layout.rows.size(); ++i) {
                if (rows[i].kind == PanelRowKind::Note) continue;
                fillRound({card.box.x, layout.rows[i].row.y, card.box.width, std::max(1.0F, s)}, 0.0F,
                          withAlpha(kRule, alpha));
            }
        }
        context_->PopAxisAlignedClip();
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
        const float rowAlpha = row.enabled ? alpha : alpha * kDisabledAlpha;
        const D2D1_COLOR_F textColor = withAlpha(kText, rowAlpha);
        // A row about one video names it in a pale shade of its colour.
        const D2D1_COLOR_F videoLabel = withAlpha(paneTextColor(row.video), rowAlpha);
        const bool hotRow = panel.hotRow == static_cast<int>(i);
        const bool pressedRow = panel.pressedRow == static_cast<int>(i);
        const OverlayRect labelLine{geometry.row.x, geometry.row.y, geometry.row.width, 24.0F * s};
        const auto drawAdd = [&](const OverlayRect& box, int part) {
            if (!box.visible()) return;
            const bool hot = row.enabled && hotRow && panel.hotKind == PanelHitKind::Add && panel.hotPart == part;
            const bool pressed = row.enabled && pressedRow && panel.pressedKind == PanelHitKind::Add && panel.pressedPart == part;
            // Dark over a picture until the pointer comes; then a form's own
            // button, which it is.
            if (hot || pressed) {
                primaryButton(box, hot, pressed, rowAlpha, s);
            } else {
                fillRound(box, controlRadius(box, s), withAlpha(rgba(12, 20, 32, 0.88F), rowAlpha));
                strokeRound(box, controlRadius(box, s), withAlpha(rgba(172, 205, 252, 0.45F), rowAlpha),
                            std::max(1.0F, s));
            }
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
        case PanelRowKind::Header: {
            // Over a card it is the card's title, which needs no rule.
            if (i > 0 && !onCards) {
                fillRound({geometry.row.x, geometry.row.y + 4.0F * s, geometry.row.width, std::max(1.0F, s)},
                          0.0F, withAlpha(kRule, alpha));
            }
            // A count at its right ("48 videos") keeps its room; the label
            // is cut before it.
            const float valueWidth = row.value.empty() ? 0.0F
                : std::min(geometry.row.width * 0.5F, measureText(row.value, OverlayTextStyle::Small, s) + 12.0F * s);
            const float top = (onCards ? 8.0F : 12.0F) * s;
            const OverlayRect band{geometry.row.x, geometry.row.y + top, geometry.row.width,
                                   geometry.row.height - top};
            text(row.label, OverlayTextStyle::BodyBold, {band.x, band.y, band.width - valueWidth, band.height},
                 withAlpha(kAccentText, alpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (!row.value.empty()) {
                text(row.value, OverlayTextStyle::Small, {band.right() - valueWidth, band.y, valueWidth, band.height},
                     dim, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            }
            break;
        }
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
                 row.video >= 0 ? videoLabel : textColor, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            toggleSwitch(geometry.control, row.on, hotRow && row.enabled, rowAlpha, s, row.video);
            break;
        case PanelRowKind::Slider: {
            text(row.label, OverlayTextStyle::Body, labelLine, row.video >= 0 ? videoLabel : textColor,
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
            text(row.value, OverlayTextStyle::Small, labelLine, withAlpha(kTextDim, rowAlpha),
                 DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            const bool emphasised = row.enabled && (hotRow || panel.dragRow == static_cast<int>(i));
            rail(geometry.control, 8.0F * s, (emphasised ? 6.0F : 4.0F) * s, row.slider01, 0.0F,
                 true, 7.0F * s, rowAlpha, row.video);
            if (geometry.trailing.visible()) {
                button(geometry.trailing, row.trailingOn ? kGlyphMute : kGlyphVolume,
                       row.trailingOn ? L"M" : L"S",
                       hotRow && panel.hotKind == PanelHitKind::Trailing,
                       pressedRow && panel.pressedKind == PanelHitKind::Trailing,
                       row.trailingOn, rowAlpha, s, false, row.video);
            }
            break;
        }
        case PanelRowKind::Equalizer:
            drawEqualizer(row, geometry, hotRow && panel.hotKind == PanelHitKind::Slider ? panel.hotPart : -1,
                          panel.dragRow == static_cast<int>(i) ? panel.dragPart : -1, rowAlpha, s);
            break;
        case PanelRowKind::Choice:
            if (row.dropDown) {
                // The label as a toggle's; the chosen option in a box with a
                // chevron, lit under the pointer and while its list is open.
                text(row.label, OverlayTextStyle::Body, geometry.label, row.video >= 0 ? videoLabel : textColor,
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
                const OverlayRect& box = geometry.control;
                const bool open = pressedRow && panel.pressedKind == PanelHitKind::DropDown;
                const bool hot = row.enabled && hotRow && panel.hotKind == PanelHitKind::DropDown;
                fieldBox(box, hot, open, rowAlpha, s, row.video);
                const PanelMetrics metrics{};
                const float boxPad = metrics.dropDownPad * s;
                const float chevron = metrics.dropDownChevron * s;
                const bool chosen = row.selected >= 0 && static_cast<std::size_t>(row.selected) < row.options.size();
                text(chosen ? row.options[static_cast<std::size_t>(row.selected)] : row.value, OverlayTextStyle::Body,
                     {box.x + boxPad, box.y, std::max(0.0F, box.width - boxPad - chevron), box.height},
                     withAlpha(chosen ? kText : kTextDim, rowAlpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                const OverlayRect mark{box.right() - chevron, box.y, chevron - 6.0F * s, box.height};
                const D2D1_COLOR_F ink = withAlpha(open || hot ? kText : kTextDim, rowAlpha);
                if (formats_.captionIcon) {
                    text(std::wstring(1, kGlyphChevronDown), OverlayTextStyle::CaptionIcon, mark, ink);
                } else {
                    text(L"\x25BE", OverlayTextStyle::Small, mark, ink, DWRITE_TEXT_ALIGNMENT_CENTER, true);
                }
                break;
            }
            if (geometry.compact) {
                // The label as a toggle's; the segments in one track, a line
                // of them or a grid, the chosen one as a menu's and the sync
                // panel's, in the colour of the video the row is about.
                text(row.label, OverlayTextStyle::Body, geometry.label, row.video >= 0 ? videoLabel : textColor,
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
                if (!geometry.parts.empty()) {
                    const float inset = PanelMetrics{}.compactInset * s;
                    float left = geometry.parts.front().x, top = geometry.parts.front().y;
                    float right = geometry.parts.front().right(), bottom = geometry.parts.front().bottom();
                    for (const auto& box : geometry.parts) {
                        left = std::min(left, box.x);
                        top = std::min(top, box.y);
                        right = std::max(right, box.right());
                        bottom = std::max(bottom, box.bottom());
                    }
                    segmentTrack({left - inset, top - inset, right - left + 2.0F * inset, bottom - top + 2.0F * inset},
                                 rowAlpha, s);
                }
            } else {
                text(row.label, OverlayTextStyle::Small, labelLine, withAlpha(kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
            }
            // Apart, the ones not chosen have a button's face; in a track,
            // the track's.
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const auto& box = geometry.parts[part];
                const bool selected = row.selected == static_cast<int>(part);
                const bool hot = row.enabled && hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Segment;
                const bool pressed = row.enabled && pressedRow && panel.pressedPart == static_cast<int>(part) &&
                                     panel.pressedKind == PanelHitKind::Segment;
                if (selected) chosenFace(box, hot, rowAlpha, s, row.video);
                else if (geometry.compact) litFace(box, hot, pressed, rowAlpha, s);
                else quietButton(box, hot, pressed, rowAlpha, s);
                text(row.options[part], OverlayTextStyle::Body, box,
                     withAlpha(selected ? paneInkColor(row.video) : kText, rowAlpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            break;
        case PanelRowKind::CompactItem: {
            fillRound(geometry.row, controlRadius(geometry.row, s), withAlpha(rgba(255, 255, 255, 0.03F), rowAlpha));
            litFace(geometry.row, row.enabled && hotRow && !pressedRow, row.enabled && pressedRow, rowAlpha, s);
            const auto& picture = geometry.control;
            fillRound(picture, 6.0F * s, withAlpha(rgba(15, 19, 26, 0.94F), rowAlpha));
            ID2D1Bitmap* bitmap = panel.thumbnails && !row.tileKeys.empty()
                ? bitmapFor(row.tileKeys.front(), *panel.thumbnails) : nullptr;
            bool drawn = false;
            if (bitmap && picture.visible()) {
                const auto size = bitmap->GetSize();
                if (size.width > 0.0F && size.height > 0.0F) {
                    const float aspect = panel.thumbnails->drawAspect(row.tileKeys.front(), size.width, size.height);
                    const float width = std::min(picture.width, picture.height * aspect), height = width / aspect;
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
            // One of the videos on the screen, its picture framed in its colour.
            if (row.video >= 0) {
                strokeRound(picture, 6.0F * s, withAlpha(paneColor(row.video), rowAlpha), std::max(1.0F, 2.0F * s));
            }
            const float textX = picture.right() + 12.0F * s;
            const float textWidth = std::max(0.0F, geometry.row.right() - textX - 8.0F * s);
            mediaText(row.label, OverlayTextStyle::Body,
                      {textX, geometry.row.y + 10.0F * s, textWidth, 40.0F * s}, 20.0F * s,
                      row.video >= 0 ? videoLabel : textColor);
            text(row.value, OverlayTextStyle::Small,
                 {textX, geometry.row.y + 54.0F * s, textWidth, 20.0F * s},
                 withAlpha(row.on ? kAccentText : kTextDim, rowAlpha),
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
            break;
        }
        case PanelRowKind::Item: {
            litFace(geometry.row, row.enabled && hotRow && !pressedRow, row.enabled && pressedRow, alpha, s);
            // The item being played: a bar at the left and its name in the
            // colour of the video playing it.
            if (row.current) currentMark(geometry.row, row.playingPosition, alpha, s);
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
                 row.current ? withAlpha(paneTextColor(row.playingPosition), rowAlpha) : textColor,
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (valueWidth > 0.0F) {
                const OverlayRect valueBox{textRight - valueWidth - 8.0F * s, geometry.row.y,
                                           valueWidth, geometry.row.height};
                text(row.value, OverlayTextStyle::Small, valueBox,
                     withAlpha(row.on ? kAccentText : kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            }
            if (tagWidth > 0.0F) {
                const float height = 20.0F * s;
                const OverlayRect tagBox{textRight - valueWidth - 8.0F * s - tagSpace,
                                         geometry.row.y + (geometry.row.height - height) * 0.5F, tagWidth, height};
                // On the item being played, in its video's colour.
                const int tagVideo = row.current ? row.playingPosition : -1;
                fillRound(tagBox, height * 0.5F, withAlpha(paneColor(tagVideo, 0.32F), rowAlpha));
                text(row.tag, OverlayTextStyle::Small, tagBox, withAlpha(paneInkColor(tagVideo), rowAlpha),
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
        case PanelRowKind::Shortcut: {
            // The action's name, pale blue once its keys are not the
            // defaults; its keys in boxes at the right, a free one a dim
            // "+", the one waiting for a key press in the accent, asking
            // for it.
            const float labelRight = geometry.parts.empty() ? geometry.row.right()
                                                            : geometry.parts.front().x - 12.0F * s;
            text(row.label, OverlayTextStyle::Body,
                 {geometry.row.x, geometry.row.y, std::max(0.0F, labelRight - geometry.row.x), geometry.row.height},
                 row.on ? withAlpha(kAccentText, rowAlpha) : textColor, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const auto& box = geometry.parts[part];
                const bool waiting = row.selected == static_cast<int>(part);
                const bool free = row.options[part].empty();
                const bool hot = row.enabled && hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Button;
                const bool pressed = row.enabled && pressedRow && panel.pressedPart == static_cast<int>(part) &&
                                     panel.pressedKind == PanelHitKind::Button;
                // A key is a button; the one waiting is chosen; a free one
                // is an outline for a key to go in.
                if (waiting) {
                    chosenFace(box, hot, rowAlpha, s);
                } else if (free) {
                    litFace(box, hot, pressed, rowAlpha, s);
                    strokeRound(box, controlRadius(box, s), withAlpha(rgba(255, 255, 255, hot ? 0.30F : 0.14F), rowAlpha),
                                std::max(1.0F, s));
                } else {
                    quietButton(box, hot, pressed, rowAlpha, s);
                }
                text(waiting ? std::wstring(L"Press a key") : free ? std::wstring(L"+") : row.options[part],
                     OverlayTextStyle::Small, box,
                     waiting ? withAlpha(paneInkColor(-1), rowAlpha) : free ? withAlpha(kTextDim, rowAlpha) : textColor,
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            break;
        }
        case PanelRowKind::Buttons:
            if (row.spinBox) {
                // The label as a toggle's; the value in a box as a drop-down's,
                // its arrows in a column at the box's right end, each lit under
                // the pointer and while pressed, and dimmed at its end.
                text(row.label, OverlayTextStyle::Body, geometry.label, row.video >= 0 ? videoLabel : textColor,
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
                const OverlayRect& box = geometry.control;
                const bool typing = row.field.focused;
                const bool hot = row.enabled && hotRow &&
                                 (panel.hotKind == PanelHitKind::Button || panel.hotKind == PanelHitKind::Field);
                const float line = std::max(1.0F, s);
                // Typed into, it is ringed in the accent, as a text box is.
                fieldBox(box, hot, typing, rowAlpha, s, row.video);
                const float boxPad = PanelMetrics{}.dropDownPad * s;
                const OverlayRect number{geometry.value.x + boxPad, geometry.value.y,
                                         std::max(0.0F, geometry.value.width - boxPad), geometry.value.height};
                if (typing) {
                    // The digits typed, what is chosen of them lit and the
                    // caret among them, then the value's unit, not typed.
                    const TextFieldState& edit = row.field.edit;
                    const auto widthOf = [&](std::size_t count) {
                        return count == 0 ? 0.0F
                            : measureText(edit.text.substr(0, std::min(count, edit.text.size())), OverlayTextStyle::Body, s);
                    };
                    const float middle = number.y + number.height * 0.5F;
                    const float digits = widthOf(edit.text.size());
                    context_->PushAxisAlignedClip(toRect(number), D2D1_ANTIALIAS_MODE_ALIASED);
                    if (textFieldHasSelection(edit)) {
                        const float from = widthOf(textFieldSelectionStart(edit));
                        const float to = widthOf(textFieldSelectionEnd(edit));
                        fillRound({number.x + from, middle - 10.0F * s, std::max(0.0F, to - from), 20.0F * s}, 3.0F * s,
                                  withAlpha(rgba(63, 132, 232, 0.55F), rowAlpha));
                    }
                    text(edit.text, OverlayTextStyle::Body, {number.x, number.y, digits + 8.0F * s, number.height},
                         textColor, DWRITE_TEXT_ALIGNMENT_LEADING);
                    const std::size_t unit = row.value.find_first_not_of(L"0123456789");
                    if (unit != std::wstring::npos) {
                        text(row.value.substr(unit), OverlayTextStyle::Body,
                             {number.x + digits, number.y, std::max(0.0F, number.width - digits), number.height},
                             withAlpha(kTextDim, rowAlpha), DWRITE_TEXT_ALIGNMENT_LEADING);
                    }
                    if (row.field.caretShown) {
                        fillRound({number.x + widthOf(edit.caret), middle - 9.0F * s, panelTextFieldCaretWidth(s),
                                   18.0F * s},
                                  0.0F, textColor);
                    }
                    context_->PopAxisAlignedClip();
                } else {
                    text(row.value, OverlayTextStyle::Body, number, textColor, DWRITE_TEXT_ALIGNMENT_LEADING, true);
                }
                if (geometry.parts.size() >= 2) {
                    fillRound({geometry.parts[0].x, box.y + line, line, std::max(0.0F, box.height - 2.0F * line)}, 0.0F,
                              withAlpha(rgba(255, 255, 255, 0.12F), rowAlpha));
                    for (std::size_t part = 0; part < 2; ++part) {
                        const auto& arrow = geometry.parts[part];
                        const bool live = row.enabled && (part == 0 ? row.slider01 < 1.0F : row.slider01 > 0.0F);
                        const bool lit = live && hot && panel.hotPart == static_cast<int>(part);
                        const bool pressed = live && pressedRow && panel.pressedKind == PanelHitKind::Button &&
                                             panel.pressedPart == static_cast<int>(part);
                        if (lit || pressed) {
                            // Inside the box's frame and its rounded corners.
                            fillRound({arrow.x + line, arrow.y + (part == 0 ? line : 0.0F),
                                       std::max(0.0F, arrow.width - 2.0F * line), std::max(0.0F, arrow.height - line)},
                                      4.0F * s, withAlpha(rgba(255, 255, 255, pressed ? kLinePressed : kLineHot), rowAlpha));
                        }
                        const D2D1_COLOR_F ink = !live ? withAlpha(kTextDim, rowAlpha * 0.35F)
                                                       : withAlpha(lit || pressed ? kText : kTextDim, rowAlpha);
                        if (formats_.captionIcon) {
                            text(std::wstring(1, part == 0 ? kGlyphChevronUp : kGlyphChevronDown),
                                 OverlayTextStyle::CaptionIcon, arrow, ink);
                        } else {
                            text(part == 0 ? L"\x25B4" : L"\x25BE", OverlayTextStyle::Small, arrow, ink,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
                        }
                    }
                }
                break;
            }
            if (geometry.compact) {
                // The label as a toggle's, the value beside the buttons or at
                // the label line's end, each button its caption's width.
                text(row.label, OverlayTextStyle::Body, geometry.label, row.video >= 0 ? videoLabel : textColor,
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
                text(row.value, OverlayTextStyle::Small, geometry.value, withAlpha(kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
            } else if (!row.label.empty()) {
                // A favorite (an episode on an information page) ends its
                // label line with a heart.
                const float heart = row.favorite ? std::min(labelLine.height, 20.0F * s) : 0.0F;
                const OverlayRect textLine{labelLine.x, labelLine.y,
                                           std::max(0.0F, labelLine.width - (heart > 0.0F ? heart + 4.0F * s : 0.0F)),
                                           labelLine.height};
                text(row.label, OverlayTextStyle::Small, textLine,
                     row.video >= 0 ? videoLabel : withAlpha(kTextDim, rowAlpha), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                text(row.value, OverlayTextStyle::Small, textLine, withAlpha(kTextDim, rowAlpha),
                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
                if (heart > 0.0F) {
                    glyph(kGlyphHeartFill, L"\x2665",
                          {labelLine.right() - heart, labelLine.y + (labelLine.height - heart) * 0.5F, heart, heart},
                          withAlpha(kFavorite, rowAlpha));
                }
            }
            // Compact or not, the same buttons: a form's own in the accent,
            // a filter that is on chosen, the rest with a button's face.
            for (std::size_t part = 0; part < geometry.parts.size() && part < row.options.size(); ++part) {
                const auto& box = geometry.parts[part];
                if (!box.visible()) continue;
                const bool hot = row.enabled && hotRow && panel.hotPart == static_cast<int>(part) &&
                                 panel.hotKind == PanelHitKind::Button;
                const bool pressed = row.enabled && pressedRow && panel.pressedPart == static_cast<int>(part) &&
                                     panel.pressedKind == PanelHitKind::Button;
                const bool on = !row.primary && part < row.buttonsOn.size() && row.buttonsOn[part];
                if (row.primary) primaryButton(box, hot, pressed, rowAlpha, s);
                else if (on) chosenFace(box, hot, rowAlpha, s);
                else quietButton(box, hot, pressed, rowAlpha, s);
                if (on && pressed) fillRound(box, controlRadius(box, s), withAlpha(rgba(255, 255, 255, kFacePressed), rowAlpha));
                text(row.options[part], OverlayTextStyle::Body, box, on ? withAlpha(paneInkColor(-1), rowAlpha) : textColor,
                     DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
            break;
        }
    }
    context_->PopAxisAlignedClip();
    // The scrollbar: a thin thumb that widens under the pointer or a drag.
    const OverlayRect thumb = panelScrollThumb(layout, s);
    if (thumb.visible()) {
        scrollThumb(layout.sheet.right() - 3.0F * s, thumb.y, thumb.height,
                    panel.scrollbarHot || panel.scrollbarDragging, alpha, s);
        // The videos being played, where the list shows them: a tick wider
        // than the thumb, over it, in each one's colour; those of one row
        // share the tick side by side.
        const auto& marks = panel.scrollMarks;
        const float tickWidth = 10.0F * s, tickHeight = 4.0F * s;
        const float tickLeft = layout.sheet.right() - 3.0F * s - tickWidth;
        for (std::size_t first = 0; first < marks.size();) {
            std::size_t end = first + 1;
            while (end < marks.size() && marks[end].y == marks[first].y) ++end;
            const float share = tickWidth / static_cast<float>(end - first);
            for (std::size_t i = first; i < end; ++i) {
                fillRound({tickLeft + share * static_cast<float>(i - first), marks[i].y - tickHeight * 0.5F, share,
                           tickHeight}, std::min(share, tickHeight) * 0.5F, withAlpha(paneColor(marks[i].position), alpha));
            }
            first = end;
        }
    }
}

void Overlay::drawEmptyHint(const OverlayScene& scene) {
    if (!scene.showEmptyHint) return;
    const float s = scene.scale;
    const EmptyHintLayout& hint = scene.emptyHint;
    text(L"Drop videos here", OverlayTextStyle::Title, hint.title, kTextDim);
    text(scene.emptyHintKeys, OverlayTextStyle::Body, hint.keys, kTextMuted);
    // In the accent, as a form's own button; the two alike.
    const float alpha = std::clamp(scene.emptyButtonsAlpha, 0.0F, 1.0F);
    if (alpha <= 0.0F) return;
    const auto button = [&](const OverlayRect& box, const wchar_t* label, bool hot, bool pressed) {
        if (!box.visible()) return;
        primaryButton(box, hot, pressed, alpha, s);
        text(label, OverlayTextStyle::BodyBold, box, withAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
    };
    button(hint.open, L"Open file", scene.emptyOpenHot, scene.emptyOpenPressed);
    button(hint.emby, L"Open Emby", scene.emptyEmbyHot, scene.emptyEmbyPressed);
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
    const auto contain = [&](ID2D1Bitmap* bitmap, const OverlayRect& box, bool centered,
                             const std::string& key = std::string()) {
        if (!bitmap || !box.visible()) return false;
        const D2D1_SIZE_F size = bitmap->GetSize();
        if (size.width <= 0.0F || size.height <= 0.0F) return false;
        // A video's own picture in the video's shape (pictureDrawAspect).
        const float aspect = cache ? cache->drawAspect(key, size.width, size.height) : size.width / size.height;
        const float width = std::min(box.width, box.height * aspect), height = width / aspect;
        const OverlayRect destination{box.x + (centered ? (box.width - width) * 0.5F : 0.0F),
                                      box.y + (box.height - height) * 0.5F, width, height};
        context_->DrawBitmap(bitmap, toRect(destination), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return true;
    };
    fillRound(geometry.poster, 10.0F * s, withAlpha(rgba(22, 27, 36, 0.90F), alpha));
    ID2D1Bitmap* poster = cache ? bitmapFor(detail.posterKey, *cache) : nullptr;
    if (!contain(poster, geometry.poster, true, detail.posterKey) && !detail.title.empty()) {
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
                   geometry.progress.height}, 5.0F * s, withAlpha(paneColor(detail.playingPosition), alpha));
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
        const float radius = controlRadius(box, s);
        if (primary) {
            // "Play → V2" in V2's colour, as V2's outline as the destination
            // is; a form's own button else.
            primaryButton(box, hot, pressed, alpha, s, detail.targetPosition >= 1 ? detail.targetPosition : -1);
        } else if (favorite) {
            fillRound(box, radius, withAlpha(withAlpha(kFavorite, pressed ? 0.48F : hot ? 0.36F : 0.26F), alpha));
            strokeRound(box, radius, withAlpha(withAlpha(kFavorite, 0.70F), alpha), std::max(1.0F, s));
        } else if (part == 2) {
            // The quiet one, its words a link's: no face until the pointer comes.
            litFace(box, hot, pressed, alpha, s);
        } else {
            // Over the backdrop, outlined.
            quietButton(box, hot, pressed, alpha, s, true);
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

// A graphic equalizer on a card of its own: each band an upright track
// filled from its middle -- no change -- to its knob, in the accent while
// the equalizer is on; its value over it and its caption under it. Faint
// lines cross each group of bands at its middle and its quarters, and a
// hairline stands in the gap that sets the leading bands apart.
void Overlay::drawEqualizer(const PanelRow& row, const PanelRowGeometry& geometry, int hotPart, int dragPart,
                            float alpha, float s) {
    const auto& parts = geometry.parts;
    if (parts.empty() || !geometry.control.visible()) return;
    fillRound(geometry.row, 10.0F * s, withAlpha(rgba(255, 255, 255, 0.035F), alpha));
    // As the sheet's sliders: the knob's travel stops this far from either
    // end of its band (kPanelRailInset).
    const float inset = 8.0F * s;
    const float hairline = std::max(1.0F, s);
    const auto levelY = [&](const OverlayRect& band, float level) {
        return band.bottom() - inset - (band.height - 2.0F * inset) * std::clamp(level, 0.0F, 1.0F);
    };
    const std::size_t apart = row.apartBands > 0 && static_cast<std::size_t>(row.apartBands) < parts.size()
        ? static_cast<std::size_t>(row.apartBands) : 0;
    const auto gridLines = [&](std::size_t first, std::size_t end) {
        if (first >= end) return;
        const float left = parts[first].x + 6.0F * s;
        const float right = parts[end - 1].right() - 6.0F * s;
        for (const float level : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
            const float y = std::round(levelY(parts[first], level));
            fillRound({left, y, std::max(0.0F, right - left), hairline}, 0.0F,
                      withAlpha(rgba(255, 255, 255, level == 0.5F ? 0.14F : 0.05F), alpha));
        }
    };
    gridLines(0, apart);
    gridLines(apart, parts.size());
    if (apart > 0) {
        const float x = std::round((parts[apart - 1].right() + parts[apart].x) * 0.5F);
        fillRound({x, geometry.control.y, hairline, geometry.control.height}, 0.0F,
                  withAlpha(rgba(255, 255, 255, 0.10F), alpha));
    }
    const PanelMetrics metrics{};
    const D2D1_COLOR_F fill = row.on ? paneColor(-1, 1.0F) : rgba(255, 255, 255, 0.34F);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const OverlayRect& band = parts[i];
        const float level = i < row.levels.size() ? row.levels[i] : 0.5F;
        const float centerX = band.x + band.width * 0.5F;
        const float middle = levelY(band, 0.5F);
        const float knob = levelY(band, level);
        const bool hot = static_cast<int>(i) == hotPart || static_cast<int>(i) == dragPart;
        const float thickness = (hot ? 6.0F : 4.0F) * s;
        fillRound({centerX - thickness * 0.5F, band.y + inset, thickness, band.height - 2.0F * inset},
                  thickness * 0.5F, withAlpha(rgba(255, 255, 255, kRailTrack), alpha));
        fillRound({centerX - thickness * 0.5F, std::min(middle, knob), thickness, std::abs(knob - middle)},
                  thickness * 0.5F, withAlpha(fill, alpha));
        const float radius = (hot ? 8.0F : 7.0F) * s;
        if (hot) {
            brush_->SetColor(withAlpha(paneColor(-1, 0.30F), alpha));
            context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, knob), radius + 5.0F * s, radius + 5.0F * s),
                                  brush_.Get());
        }
        brush_->SetColor(withAlpha(kText, alpha));
        context_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, knob), radius, radius), brush_.Get());
        const bool changed = std::abs(level - 0.5F) > 0.001F;
        if (i < row.levelTexts.size()) {
            text(row.levelTexts[i], OverlayTextStyle::Small,
                 {band.x, geometry.row.y + 4.0F * s, band.width, metrics.equalizerValueLine * s - 4.0F * s},
                 withAlpha(changed && row.on ? paneInkColor(-1) : kTextDim, alpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
        if (i < row.options.size()) {
            text(row.options[i], OverlayTextStyle::Small,
                 {band.x, band.bottom() + 2.0F * s, band.width, metrics.equalizerCaptionLine * s - 6.0F * s},
                 withAlpha(i < apart ? kText : kTextDim, alpha), DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
    }
}

void Overlay::drawTile(const PanelRow& row, std::size_t part, const OverlayRect& box, bool hot, bool pressed,
                       float alpha, float s, const ThumbnailCache* cache) {
    const float pictureHeight = box.width / std::max(0.1F, row.tileAspect);
    const OverlayRect picture{box.x, box.y, box.width, pictureHeight};
    const float radius = 8.0F * s;
    // Until its picture comes, a button's face.
    fillRound(picture, radius, withAlpha(rgba(255, 255, 255, pressed ? kFacePressed : hot ? kFaceHot : kFaceRest), alpha));
    const std::string& key = part < row.tileKeys.size() ? row.tileKeys[part] : std::string();
    ID2D1Bitmap* bitmap = cache ? bitmapFor(key, *cache) : nullptr;
    if (bitmap) {
        // The whole picture inside the frame, letterboxed: a portrait phone
        // clip keeps its head, a poster keeps its title. A video's own
        // picture is in the video's shape (pictureDrawAspect): one whose
        // pixels are not square came from the server stretched.
        const D2D1_SIZE_F size = bitmap->GetSize();
        const float aspect = cache->drawAspect(key, std::max(1.0F, size.width), std::max(1.0F, size.height));
        const float width = std::min(picture.width, picture.height * aspect);
        const float height = width / aspect;
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
    // The tile being played framed in the colour of the video playing it.
    const int playing = part < row.tilePlaying.size() ? row.tilePlaying[part]
                      : row.selected == static_cast<int>(part) ? 0 : -1;
    const bool current = playing >= 0;
    if (hot || pressed || current) {
        strokeRound(picture, radius, current ? withAlpha(paneColor(playing), alpha) : withAlpha(kAccent, 0.9F * alpha),
                    std::max(1.0F, (current ? 3.0F : 2.0F) * s));
    }
    // How far it was watched, along the picture's bottom edge: in the colour
    // of the video playing it, as its frame.
    const float progress = part < row.tileProgress.size() ? std::clamp(row.tileProgress[part], 0.0F, 1.0F) : 0.0F;
    if (progress > 0.0F) {
        const OverlayRect track{picture.x + 6.0F * s, picture.bottom() - 7.0F * s, picture.width - 12.0F * s, 3.0F * s};
        fillRound(track, 1.5F * s, withAlpha(rgba(0, 0, 0, 0.55F), alpha));
        fillRound({track.x, track.y, track.width * progress, track.height}, 1.5F * s,
                  withAlpha(paneColor(playing), alpha));
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
    // a tag in the accent below them, or in the colour of the video
    // playing it.
    const D2D1_COLOR_F shade = rgba(0, 0, 0, 0.65F);
    if (part < row.tileMarks.size()) pill(row.tileMarks[part], true, true, shade);
    if (part < row.tileBadges.size()) pill(row.tileBadges[part], false, true, shade);
    if (part < row.tileTags.size()) pill(row.tileTags[part], true, false, paneColor(playing, 0.92F));
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
         withAlpha(current ? paneTextColor(playing) : kText, alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
}

}  // namespace quaddeck
