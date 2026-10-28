#pragma once

// How the picture reaches the screen, PotPlayer's video settings as far as
// one D3D11 swap chain for every pane allows: the renderer and the graphics
// card it runs on, the display fullscreen goes to, the output's depth,
// deinterlacing, the scaler that enlarges the picture, and the colour
// controls. Pure: the choices, the clamps and the names need no device;
// D3DRenderer acts on them, VideoDevices lists the
// cards and displays, and App (AppVideo.cpp) keeps them in QCONFIG.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace quaddeck {

// The renderers there are. Each is the same D3D11 compositor; they differ
// in the device and in how a frame is handed to Windows. The flip model is
// what vsync without stalling the decoders rests on (D3DRenderer), and what
// 10-bit and HDR output need; bitblt hands frames over the old way, for
// capture tools and drivers the flip model troubles. WARP is Windows' own
// D3D11 on the CPU: no GPU at all, so no hardware decoding.
enum class VideoRenderer : int { Direct3D11, Direct3D11Bitblt, Warp };
inline constexpr int kVideoRendererCount = 3;

inline const wchar_t* videoRendererLabel(VideoRenderer renderer) {
    switch (renderer) {
    case VideoRenderer::Direct3D11: return L"Direct3D 11";
    case VideoRenderer::Direct3D11Bitblt: return L"Direct3D 11, bitblt";
    case VideoRenderer::Warp: return L"Direct3D 11 WARP (CPU)";
    }
    return L"";
}

// Interlaced frames, as their decoder marks them, are deinterlaced by the
// D3D11 video processor, software-decoded ones included; Off shows the two
// fields woven, as before there was a choice.
enum class VideoDeinterlace : int { Off, Automatic };
inline constexpr int kVideoDeinterlaceCount = 2;

inline const wchar_t* videoDeinterlaceLabel(VideoDeinterlace mode) {
    return mode == VideoDeinterlace::Off ? L"Off" : L"Automatic";
}

// How a picture is enlarged to its place. Bilinear is the sampler's own and
// costs nothing; the others are a pass of their own (D3DRenderer's post
// pass). Shrinking stays the video processor's or the decoder's: a picture
// is never converted much larger than its place.
enum class VideoScaling : int { Bilinear, Bicubic, Lanczos, Nearest };
inline constexpr int kVideoScalingCount = 4;

inline const wchar_t* videoScalingLabel(VideoScaling scaling) {
    switch (scaling) {
    case VideoScaling::Bilinear: return L"Bilinear";
    case VideoScaling::Bicubic: return L"Bicubic";
    case VideoScaling::Lanczos: return L"Lanczos";
    case VideoScaling::Nearest: return L"Nearest neighbour";
    }
    return L"";
}

// PotPlayer's colour controls, and a gamma. Each is a whole number around
// 0: brightness, contrast, saturation and gamma from -100 to 100, the hue
// in degrees either way.
enum class VideoColorControl : int { Brightness, Contrast, Saturation, Hue, Gamma };
inline constexpr int kVideoColorControlCount = 5;
inline constexpr int kVideoColorRange = 100;
inline constexpr int kVideoHueRange = 180;

inline const wchar_t* videoColorControlLabel(VideoColorControl control) {
    switch (control) {
    case VideoColorControl::Brightness: return L"Brightness";
    case VideoColorControl::Contrast: return L"Contrast";
    case VideoColorControl::Saturation: return L"Saturation";
    case VideoColorControl::Hue: return L"Hue";
    case VideoColorControl::Gamma: return L"Gamma";
    }
    return L"";
}

inline int videoColorControlRange(VideoColorControl control) {
    return control == VideoColorControl::Hue ? kVideoHueRange : kVideoColorRange;
}

struct VideoColorControls {
    int brightness{};
    int contrast{};
    int saturation{};
    int hue{};
    int gamma{};
    bool operator==(const VideoColorControls&) const = default;
};

inline int videoColorValue(const VideoColorControls& color, VideoColorControl control) {
    switch (control) {
    case VideoColorControl::Brightness: return color.brightness;
    case VideoColorControl::Contrast: return color.contrast;
    case VideoColorControl::Saturation: return color.saturation;
    case VideoColorControl::Hue: return color.hue;
    case VideoColorControl::Gamma: return color.gamma;
    }
    return 0;
}

// Sets one control, held to its range.
inline void setVideoColorValue(VideoColorControls& color, VideoColorControl control, int value) {
    const int range = videoColorControlRange(control);
    value = std::clamp(value, -range, range);
    switch (control) {
    case VideoColorControl::Brightness: color.brightness = value; break;
    case VideoColorControl::Contrast: color.contrast = value; break;
    case VideoColorControl::Saturation: color.saturation = value; break;
    case VideoColorControl::Hue: color.hue = value; break;
    case VideoColorControl::Gamma: color.gamma = value; break;
    }
}

// The gamma a control's value stands for: 0.5 to 2, 1 at 0, each end a
// doubling. Mid-tones come up as it rises.
inline double videoGamma(int value) {
    return std::pow(2.0, std::clamp(value, -kVideoColorRange, kVideoColorRange) / 100.0);
}

// A control's value as the settings window writes it beside its slider.
inline std::wstring videoColorValueText(VideoColorControl control, int value) {
    if (control == VideoColorControl::Gamma) {
        const int hundredths = static_cast<int>(std::lround(videoGamma(value) * 100.0));
        std::wstring fraction = std::to_wstring(hundredths % 100);
        if (fraction.size() < 2) fraction.insert(0, 1, L'0');
        return std::to_wstring(hundredths / 100) + L"." + fraction;
    }
    std::wstring text = value > 0 ? L"+" + std::to_wstring(value) : std::to_wstring(value);
    if (control == VideoColorControl::Hue) text += L"\x00B0";
    return text;
}

struct VideoOutputSettings {
    VideoRenderer renderer{VideoRenderer::Direct3D11};
    // The graphics card by the name Windows gives it, and which of the cards
    // of that name, counted from 0 in Windows' order; empty takes the one
    // driving the main display, as D3D11 does by itself.
    std::wstring adapter;
    int adapterIndex{};
    // The display fullscreen goes to, by Windows' device name for it
    // (\\.\DISPLAY2), and what it was called, for while it is not
    // connected; empty is the one the window is on.
    std::wstring display;
    std::wstring displayName;
    // A 10-bit back buffer for SDR, as the HDR output already is.
    bool tenBit{};
    VideoDeinterlace deinterlace{VideoDeinterlace::Automatic};
    VideoScaling scaling{VideoScaling::Bilinear};
    // 16-235 for a TV that expects video levels; 0-255 otherwise.
    bool limitedRange{};
    VideoColorControls color;
    bool operator==(const VideoOutputSettings&) const = default;
};

inline VideoOutputSettings clampVideoOutputSettings(VideoOutputSettings settings) {
    const auto inRange = [](int value, int count) { return value >= 0 && value < count; };
    if (!inRange(static_cast<int>(settings.renderer), kVideoRendererCount)) settings.renderer = VideoRenderer::Direct3D11;
    if (!inRange(static_cast<int>(settings.deinterlace), kVideoDeinterlaceCount)) {
        settings.deinterlace = VideoDeinterlace::Automatic;
    }
    if (!inRange(static_cast<int>(settings.scaling), kVideoScalingCount)) settings.scaling = VideoScaling::Bilinear;
    if (settings.adapter.empty()) settings.adapterIndex = 0;
    settings.adapterIndex = std::clamp(settings.adapterIndex, 0, 15);
    if (settings.display.empty()) settings.displayName.clear();
    for (int control = 0; control < kVideoColorControlCount; ++control) {
        const auto which = static_cast<VideoColorControl>(control);
        setVideoColorValue(settings.color, which, videoColorValue(settings.color, which));
    }
    return settings;
}

// What the device is built as. A change of either is a new device, and the
// videos are opened again on it; the rest of the settings take effect on
// the next frame.
struct VideoRendererChoice {
    VideoRenderer renderer{VideoRenderer::Direct3D11};
    std::wstring adapter;
    int adapterIndex{};
    bool operator==(const VideoRendererChoice&) const = default;
};

inline VideoRendererChoice videoRendererChoice(const VideoOutputSettings& settings) {
    VideoRendererChoice choice{settings.renderer, settings.adapter, settings.adapterIndex};
    // WARP is no card; a card chosen beside it waits for another renderer.
    if (choice.renderer == VideoRenderer::Warp) {
        choice.adapter.clear();
        choice.adapterIndex = 0;
    }
    return choice;
}

// Neither the colour controls nor the levels change anything.
inline bool videoColorNeutral(const VideoOutputSettings& settings) {
    return settings.color == VideoColorControls{} && !settings.limitedRange;
}

// What the post pass's shader takes for the colour controls: an offset of
// the luma, its gain around mid-grey, the chroma's gain, the hue's rotation
// and the exponent the gamma comes to.
struct VideoColorParams {
    float brightness{};
    float contrast{1.0F};
    float saturation{1.0F};
    float hueCos{1.0F};
    float hueSin{};
    float exponent{1.0F};
    bool limitedRange{};
};

inline VideoColorParams videoColorParams(const VideoOutputSettings& settings) {
    const auto color = clampVideoOutputSettings(settings).color;
    VideoColorParams params;
    // Up to half the range either way: at +100 black is mid-grey.
    params.brightness = static_cast<float>(color.brightness) / 200.0F;
    params.contrast = 1.0F + static_cast<float>(color.contrast) / 100.0F;
    params.saturation = 1.0F + static_cast<float>(color.saturation) / 100.0F;
    const double radians = color.hue * 3.14159265358979323846 / 180.0;
    params.hueCos = static_cast<float>(std::cos(radians));
    params.hueSin = static_cast<float>(std::sin(radians));
    params.exponent = static_cast<float>(1.0 / videoGamma(color.gamma));
    params.limitedRange = settings.limitedRange;
    return params;
}

// What the post pass does to one SDR pixel, each channel 0 to 1: the
// controls on its BT.709 luma and chroma, the gamma, then the levels. The
// shader does the same sums.
inline std::array<float, 3> videoColorAdjusted(const VideoColorParams& params, std::array<float, 3> rgb) {
    const float luma = 0.2126F * rgb[0] + 0.7152F * rgb[1] + 0.0722F * rgb[2];
    const float cb = (rgb[2] - luma) / 1.8556F;
    const float cr = (rgb[0] - luma) / 1.5748F;
    const float y = (luma - 0.5F) * params.contrast + 0.5F + params.brightness;
    const float blue = (cb * params.hueCos - cr * params.hueSin) * params.saturation;
    const float red = (cb * params.hueSin + cr * params.hueCos) * params.saturation;
    std::array<float, 3> result{y + 1.5748F * red, y - 0.187324F * blue - 0.468124F * red, y + 1.8556F * blue};
    for (auto& channel : result) {
        channel = std::pow(std::clamp(channel, 0.0F, 1.0F), params.exponent);
        if (params.limitedRange) channel = channel * (219.0F / 255.0F) + 16.0F / 255.0F;
    }
    return result;
}

// A graphics card as Windows lists it (DXGI's adapters, the software one
// left out: that is the WARP renderer).
struct VideoAdapterInfo {
    std::wstring name;
    std::uint32_t vendorId{};
    std::uint64_t memoryBytes{};
    bool operator==(const VideoAdapterInfo&) const = default;
};

// Which of the cards named as list[index] is, counted from 0.
inline int videoAdapterOrdinal(const std::vector<VideoAdapterInfo>& list, std::size_t index) {
    if (index >= list.size()) return 0;
    return static_cast<int>(std::count_if(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(index),
                                          [&](const VideoAdapterInfo& other) { return other.name == list[index].name; }));
}

// The place in the list of the card chosen by name and ordinal, -1 when it
// is not there.
inline int videoAdapterIndex(const std::vector<VideoAdapterInfo>& list, const std::wstring& name, int ordinal) {
    if (name.empty()) return -1;
    int seen = 0;
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index].name != name) continue;
        if (seen++ == ordinal) return static_cast<int>(index);
    }
    return -1;
}

// A card's name, numbered when another card has the same one.
inline std::wstring videoAdapterLabel(const std::vector<VideoAdapterInfo>& list, std::size_t index) {
    if (index >= list.size()) return {};
    const auto& name = list[index].name;
    const auto same = std::count_if(list.begin(), list.end(), [&](const VideoAdapterInfo& other) {
        return other.name == name;
    });
    if (same < 2) return name;
    return name + L" (" + std::to_wstring(videoAdapterOrdinal(list, index) + 1) + L")";
}

// A display as Windows lists it: its device name, the monitor's own name
// and where it is on the desktop.
struct VideoDisplayInfo {
    std::wstring device;
    std::wstring name;
    bool primary{};
    int left{};
    int top{};
    int right{};
    int bottom{};
    bool operator==(const VideoDisplayInfo&) const = default;
};

// Windows' device names are matched as it matches them, whatever the case.
inline int videoDisplayIndex(const std::vector<VideoDisplayInfo>& list, const std::wstring& device) {
    if (device.empty()) return -1;
    const auto same = [](const std::wstring& a, const std::wstring& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
            return std::towupper(x) == std::towupper(y);
        });
    };
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (same(list[index].device, device)) return static_cast<int>(index);
    }
    return -1;
}

// "Display 2: DELL U2720Q", the number as Windows' settings show it, taken
// from the device name's end; the size beside it tells two of one model
// apart, and the main display says so.
inline std::wstring videoDisplayLabel(const VideoDisplayInfo& display) {
    std::size_t digits = display.device.size();
    while (digits > 0 && std::iswdigit(display.device[digits - 1])) --digits;
    std::wstring label = digits < display.device.size()
        ? L"Display " + display.device.substr(digits) : display.device;
    if (!display.name.empty()) label += L": " + display.name;
    const int width = display.right - display.left;
    const int height = display.bottom - display.top;
    if (width > 0 && height > 0) label += L"  " + std::to_wstring(width) + L"\x00D7" + std::to_wstring(height);
    if (display.primary) label += L"  (main)";
    return label;
}

// What the renderer actually built, for the line under its choices.
struct VideoRendererStatus {
    bool built{};
    VideoRenderer renderer{VideoRenderer::Direct3D11};
    std::wstring adapterName;
    // The card chosen was not used, so the one D3D11 picks is: it was not
    // connected (missing), or it would not make a device.
    bool adapterFallback{};
    bool adapterMissing{};
    // The D3D11 video processor and hardware decoding are there.
    bool videoProcessor{};
    bool flipModel{};
    bool tenBitOutput{};
    bool hdrOutput{};
};

// When the renderer is built on WARP though WARP was not chosen, which no
// card making a device leaves it to: which cards could not be used, and that
// the CPU renders. Empty otherwise. It begins the Video tab's note, and App
// shows it as a notice when a build ends so, since every pane is then drawn
// on the CPU and decoded in software.
inline std::wstring videoRendererWarpFallback(const VideoOutputSettings& wanted, const VideoRendererStatus& status) {
    if (!status.built || status.renderer != VideoRenderer::Warp || wanted.renderer == VideoRenderer::Warp) return {};
    const bool missing = status.adapterFallback && status.adapterMissing;
    return std::wstring(missing ? L"The chosen card is not connected, and the default one could not be used"
                        : status.adapterFallback ? L"Neither the chosen card nor the default one could be used"
                                                 : L"No graphics card could be used") +
           L"; rendering on the CPU (WARP)";
}

// One line: what renders, on what, and how deep; or the one thing that is
// not as chosen.
inline std::wstring videoRendererNote(const VideoOutputSettings& wanted, const VideoRendererStatus& status) {
    if (!status.built) return L"The renderer is being built again.";
    std::wstring note;
    if (const std::wstring fallback = videoRendererWarpFallback(wanted, status); !fallback.empty()) {
        // WARP was not chosen: no card made a device. The renderer stays on
        // the CPU until it is built again, and every rebuild tries the card
        // first: one for another renderer or card, one for a chosen card
        // connected again (App::reclaimVideoAdapter), and a restart.
        const bool missing = status.adapterFallback && status.adapterMissing;
        note = fallback + L"; videos decode in software. " +
               (missing ? L"Connect the card, choose another renderer or card, or restart QuadDeck to try again. "
                        : L"Choose another renderer or card, or restart QuadDeck, to try again. ");
    } else if (status.adapterFallback) {
        note = (status.adapterMissing ? L"The chosen card is not connected; rendering on "
                                      : L"The chosen card could not be used; rendering on ") +
               status.adapterName + L". ";
    } else if (status.renderer == VideoRenderer::Warp) {
        note = L"Rendering on the CPU (WARP); videos decode in software. ";
    } else {
        note = L"Rendering with " + std::wstring(videoRendererLabel(status.renderer)) + L" on " +
               (status.adapterName.empty() ? std::wstring(L"the default card") : status.adapterName) + L". ";
    }
    if (status.hdrOutput) {
        note += L"Output: 10-bit PQ for RTX Video HDR.";
    } else if (status.tenBitOutput) {
        note += L"Output: 10-bit.";
    } else if (wanted.tenBit && !status.flipModel) {
        note += L"Output: 8-bit; 10-bit needs the flip model, not bitblt.";
    } else {
        note += L"Output: 8-bit.";
    }
    if (!status.videoProcessor && status.renderer != VideoRenderer::Warp) {
        note += L" No video processor: hardware decoding and deinterlacing are off.";
    }
    return note;
}

}  // namespace quaddeck
