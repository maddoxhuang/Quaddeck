#include "D3DRenderer.hpp"
#include "ColorConversion.hpp"
#include "Diagnostics.hpp"
#include "ShaderCompat.hpp"
#include "SoftwareVideoFrame.hpp"
#include "VideoDevices.hpp"

#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

// Windows 10 1803; older SDK headers leave it out, and older systems refuse
// the flag, which the renderer then does without.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace quaddeck {

namespace {
// QUARTER_TURNS, 1 to 3, makes the quad for a picture stored on its side or
// on its head: the pane's coordinate is taken within the upright picture and
// then turned into the texture, which holds the picture as it is stored. A
// quarter turn clockwise shows the texture's left edge along the top.
constexpr char kVertexShader[] = R"(
#ifndef QUARTER_TURNS
#define QUARTER_TURNS 0
#endif
cbuffer UvTransform : register(b0) { float4 uvTransform; };
struct VSOut {
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
};
VSOut main(uint id : SV_VertexID) {
    const float2 positions[4] = {
        float2(-1.0,  1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0, -1.0)
    };
    const float2 coords[4] = {
        float2(0.0, 0.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 1.0)
    };
    VSOut output;
    output.position = float4(positions[id], 0.0, 1.0);
#if QUARTER_TURNS == 1
    float2 upright = coords[id] * uvTransform.xy + uvTransform.zw;
    output.uv0 = float2(upright.y, 1.0 - upright.x);
#elif QUARTER_TURNS == 2
    float2 upright = coords[id] * uvTransform.xy + uvTransform.zw;
    output.uv0 = 1.0 - upright;
#elif QUARTER_TURNS == 3
    float2 upright = coords[id] * uvTransform.xy + uvTransform.zw;
    output.uv0 = float2(1.0 - upright.y, upright.x);
#else
    output.uv0 = coords[id] * uvTransform.xy + uvTransform.zw;
#endif
    // PotPlayer/MPC shaders found in the wild do not consistently use the
    // same TEXCOORD index. Supply the transformed video coordinate on the
    // common legacy indices so an absent semantic cannot collapse sampling
    // to the top-left pixel and turn a pane into one solid colour.
    output.uv1 = output.uv0;
    output.uv2 = output.uv0;
    output.uv3 = output.uv0;
    return output;
})";

constexpr char kPixelShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return videoTexture.Sample(videoSampler, uv);
})";

// ST 2084 (PQ): codes to absolute linear light in nits, and back. Put
// before the source of every shader that works on a PQ picture
// (withPqFunctions).
constexpr char kPqFunctions[] = R"(
float3 pqToNits(float3 code) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(code), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-6), 1.0 / m1);
}
float3 nitsToPq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
)";

std::string withPqFunctions(const char* shader) {
    return std::string(kPqFunctions) + shader;
}

// Software fallback remains SDR while the selected effect runs. This final
// presentation pass maps its G22 / BT.709 RGB to absolute PQ / BT.2020. Merely
// writing the original 8-bit codes into a PQ swap chain makes SDR white 10,000
// nits; fallback must be correct even when the driver refuses the video path.
constexpr char kSdrToPqShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
cbuffer SdrOutputParams : register(b0) { float4 outputParams; };
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    float3 linear709 = pow(saturate(color.rgb), 2.2);
    float3 linear2020 = float3(
        dot(linear709, float3(0.6274040, 0.3292820, 0.0433136)),
        dot(linear709, float3(0.0690970, 0.9195400, 0.0113612)),
        dot(linear709, float3(0.0163916, 0.0880132, 0.8955950)));
    return float4(nitsToPq(linear2020 * outputParams.x), color.a);
})";

// An HDR10 picture on an SDR output. The input is PQ-coded BT.2020 R'G'B',
// the video processor's or a software frame's; the output is the G22 / BT.709
// picture every SDR pass after it expects. The curve is BT.2390's EETF taken
// from the stream's peak (toneParams.x) down to SDR white (toneParams.y, 203
// nits): the picture below the knee keeps its brightness and the highlights
// above it are rolled off into what is left, rather than cut. It is applied
// to the largest of the three channels and the others follow in proportion,
// so no channel clips and the hue stays. A colour BT.709 cannot show is
// drawn toward its own luminance until it can.
// An HLG picture comes the same way (toneParams.z): its codes are taken back
// to scene light and through BT.2100's system gamma for a 1000-nit display
// (1.2, on the luminance, so the hue stays), which gives nits as PQ's do.
// On the PQ output (toneParams.w) that is all that is done to it: the nits
// are coded as PQ and shown as an HDR10 picture is.
// A full-range HDR surface on a card whose processor takes no full-range
// BT.2020 input comes here undone with the BT.709 matrix, the one full-range
// pair every processor has (toneOptions.x): the 709 matrix is put back and
// BT.2020's applied, which is exact wherever the 709 picture did not leave
// 0 to 1. What that card's 709 conversion was measured to add to each channel
// (toneOptions.yzw) is taken off first. A PQ picture so mended for the PQ
// output goes out as it is.
constexpr char kToneMapShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
cbuffer ToneMapParams : register(b0) { float4 toneParams; float4 toneOptions; };

float3 bt709DecodeToBt2020(float3 rgb) {
    float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    return float3(y + 1.4746 * cr, y - 0.16455312684366 * cb - 0.57135312684366 * cr, y + 1.8814 * cb);
}

float nitsToPq(float nits) {
    return nitsToPq(float3(nits, nits, nits)).x;
}

float toneCurve(float nits) {
    float sourcePeak = nitsToPq(toneParams.x);
    float e = saturate(nitsToPq(nits) / sourcePeak);
    float white = min(nitsToPq(toneParams.y) / sourcePeak, 1.0);
    float knee = 1.5 * white - 0.5;
    if (e > knee) {
        float t = (e - knee) / max(1.0 - knee, 1e-6);
        float t2 = t * t;
        float t3 = t2 * t;
        e = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) +
            (3.0 * t2 - 2.0 * t3) * white;
    }
    return pqToNits(float3(e * sourcePeak, 0.0, 0.0)).x;
}

float3 hlgToNits(float3 code) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    code = saturate(code);
    float3 scene = code <= 0.5 ? code * code / 3.0 : (exp((code - c) / a) + b) / 12.0;
    float luminance = dot(scene, float3(0.2627, 0.6780, 0.0593));
    return 1000.0 * pow(max(luminance, 1e-9), 0.2) * scene;
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float3 code = videoTexture.Sample(videoSampler, uv).rgb;
    if (toneOptions.x > 0.5) code = saturate(bt709DecodeToBt2020(code - toneOptions.yzw));
    if (toneParams.w > 0.5 && toneParams.z < 0.5) return float4(code, 1.0);
    float3 rgb = toneParams.z > 0.5 ? hlgToNits(code) : pqToNits(code);
    if (toneParams.w > 0.5) return float4(nitsToPq(rgb), 1.0);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    if (peak > 1e-4) rgb *= toneCurve(peak) / peak;
    float3 linear709 = float3(
        dot(rgb, float3( 1.6604910, -0.5876411, -0.0728499)),
        dot(rgb, float3(-0.1245505,  1.1328999, -0.0083494)),
        dot(rgb, float3(-0.0181508, -0.1005789,  1.1187297))) / toneParams.y;
    float luminance = saturate(dot(linear709, float3(0.2126, 0.7152, 0.0722)));
    float low = min(linear709.r, min(linear709.g, linear709.b));
    float high = max(linear709.r, max(linear709.g, linear709.b));
    float keep = 1.0;
    if (low < 0.0) keep = min(keep, luminance / (luminance - low));
    if (high > 1.0) keep = min(keep, (1.0 - luminance) / (high - luminance));
    linear709 = luminance + (linear709 - luminance) * keep;
    return float4(pow(saturate(linear709), 1.0 / 2.2), 1.0);
})";

constexpr char kSharpenShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    uint width, height; videoTexture.GetDimensions(width, height);
    float2 pixel = 1.0 / float2(width, height);
    float4 center = videoTexture.Sample(videoSampler, uv) * 5.0;
    center -= videoTexture.Sample(videoSampler, uv + float2(pixel.x, 0));
    center -= videoTexture.Sample(videoSampler, uv - float2(pixel.x, 0));
    center -= videoTexture.Sample(videoSampler, uv + float2(0, pixel.y));
    center -= videoTexture.Sample(videoSampler, uv - float2(0, pixel.y));
    return float4(saturate(center.rgb), 1.0);
})";

// Grayscale and Invert are written for an SDR picture. One that is PQ BT.2020
// (b2's colorSpace.x above a half: an HDR picture on the PQ output, or RTX
// Video HDR's) is taken to nits first, so that grey is the picture's
// luminance by BT.2020's weights rather than a weighting of its codes, and
// the inverse of black is the display's SDR white (colorSpace.y, in nits),
// what it is for an SDR picture beside it, rather than PQ's 10,000 nits.
// Light above SDR white inverts to black.
constexpr char kPresetColorSpace[] = R"(
cbuffer PictureColorSpace : register(b2) { float4 colorSpace; };
)";

constexpr char kGrayscaleShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    if (colorSpace.x > 0.5) {
        float nits = dot(pqToNits(color.rgb), float3(0.2627, 0.6780, 0.0593));
        return float4(nitsToPq(float3(nits, nits, nits)), color.a);
    }
    float gray = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    return float4(gray, gray, gray, color.a);
})";

constexpr char kInvertShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    if (colorSpace.x > 0.5) {
        float white = max(colorSpace.y, 1.0);
        float3 code = pow(saturate(pqToNits(color.rgb) / white), 1.0 / 2.2);
        return float4(nitsToPq(white * pow(1.0 - code, 2.2)), color.a);
    }
    return float4(1.0 - color.rgb, color.a);
})";

// Native D3D11 form of Smart Vibrance Plus. The SDR branch is a rewrite of
// aston89's Smart_Vibrance_Plus.fx, GPL-3.0
// (https://github.com/aston89/Smart-vibrance-for-reshade), which is why
// QuadDeck as a whole is GPL-3.0. The reference ReShade effect is a
// single stateless pass; its four user uniforms live in b1 so the existing b0
// PotPlayer compatibility contract remains unchanged. b2 describes this pane's
// converted texture: SDR, RTX-produced PQ, or PQ to leave untouched.
constexpr char kSmartVibrancePlusShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
cbuffer SmartVibranceParams : register(b1) {
    float intensity;
    float saturationPivot;
    float grayPivot;
    float graySharpness;
};
cbuffer VibranceColorSpace : register(b2) { float4 colorSpace; };

float sigmoid(float x) {
    return 1.0 / (1.0 + exp(-x));
}

float3 smartVibrance(float3 color) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float3 chroma = color - luminance;
    float chromaEnergy = dot(chroma, chroma);
    float chromaMagnitude = sqrt(chromaEnergy);
    float normalizedSaturation = saturate(chromaMagnitude / saturationPivot);
    float rolloff = 1.0 - normalizedSaturation;
    float grayProtection = sigmoid((grayPivot - chromaEnergy) * graySharpness);
    float response = lerp(rolloff, 1.0, grayProtection);
    float gain = (intensity - 1.0) * response;
    return saturate(luminance + chroma + chroma * gain);
}

// A PQ picture's vibrance is worked in absolute linear RGB in nits
// (pqToNits); the SDR preset above stays unchanged.
float3 hdrVibrance(float3 code) {
    // Avoid even PQ round-trip error at the neutral setting.
    if (intensity == 1.0) return code;
    float3 rgb = pqToNits(code);
    float y = dot(rgb, float3(0.2627, 0.6780, 0.0593));
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    float low = min(rgb.r, min(rgb.g, rgb.b));
    if (y <= 0.01 || y >= 1000.0 || peak - low <= peak * 1e-5) return code;

    float3 chroma = rgb - y;
    // Relative colourfulness, independent of exposure. Gray controls suppress
    // near-neutral noise here; their SDR reference semantics remain above.
    float3 relativeChroma = chroma / max(peak, 0.01);
    float energy = dot(relativeChroma, relativeChroma);
    float grayMask = smoothstep(0.0, grayPivot, energy) *
                     sigmoid((energy - grayPivot) * graySharpness);
    float saturation = (peak - low) / peak;
    float rolloff = 1.0 - smoothstep(saturationPivot * 0.75, 1.0, saturation);
    // Conservative artistic defaults, not display calibration or tone mapping.
    float exposure = smoothstep(0.01, 1.0, y) * (1.0 - smoothstep(203.0, 1000.0, y));
    // Reduce enhancement in the warm red-yellow region, including many skin
    // tones. This is a hue heuristic, not a face/skin detector.
    float warm = smoothstep(0.0, 0.25, (rgb.r - rgb.g) / peak) *
                 smoothstep(0.0, 0.25, (rgb.g - rgb.b) / peak);
    float extra = (intensity - 1.0) * grayMask * rolloff * exposure * (1.0 - 0.5 * warm);
    if (abs(extra) < 1e-7) return code;

    // Compress the requested gain smoothly toward the legal BT.2020/PQ RGB
    // boundary along the constant-Y gray axis. Per-channel clipping would
    // change Y and hue. This does not model a particular display's gamut.
    float3 room = float3(10000.0 - y, 10000.0 - y, 10000.0 - y);
    room = float3(chroma.r < 0.0 ? y : room.r,
                  chroma.g < 0.0 ? y : room.g,
                  chroma.b < 0.0 ? y : room.b);
    float3 limits = room / max(abs(chroma), 1e-6);
    float headroom = max(0.0, min(limits.r, min(limits.g, limits.b)) - 1.0);
    if (extra > 0.0) extra *= headroom / max(headroom + extra, 1e-6);
    return nitsToPq(y + chroma * (1.0 + extra));
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    if (colorSpace.x > 1.5) return color;
    return float4(colorSpace.x > 0.5 ? hdrVibrance(color.rgb) : smartVibrance(color.rgb), color.a);
}
)";

// Blends the interface layer over the video in the 10-bit PQ output mode.
// The layer is premultiplied sRGB as Direct2D left it; each pixel is
// un-premultiplied, linearised, scaled to the display's SDR white level and
// PQ-encoded, then premultiplied again for the ONE / INV_SRC_ALPHA blend.
// Blending in PQ rather than linear light is a small error on a translucent
// pill, and what every SDR-over-HDR composition on Windows accepts.
constexpr char kOverlayCompositeShader[] = R"(
Texture2D overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);
cbuffer OverlayParams : register(b0) { float4 overlayParams; };

float3 srgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 linearToPq(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 layer = overlayTexture.Sample(overlaySampler, uv);
    if (layer.a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    // A 10-bit sRGB back buffer takes the layer as it is.
    if (overlayParams.y > 0.5) return layer;
    float3 straight = saturate(layer.rgb / layer.a);
    float3 pq = linearToPq(srgbToLinear(straight) * overlayParams.x);
    return float4(pq * layer.a, layer.a);
})";

// The output pass: a pane's picture enlarged to its place by the chosen
// scaler, then PotPlayer's colour controls and the output levels. The
// kernels read their taps through the point sampler at texel centres; the
// clamp addressing repeats the edge. Catmull-Rom and Lanczos ring, so the
// result is held to 0-1. VideoOutput.hpp's videoColorAdjusted() does the
// colour sums the same way, for the probe.
constexpr char kOutputShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);
cbuffer OutputParams : register(b0) {
    float4 sourceSize;     // texels across and down, and their reciprocals
    float4 outputMode;     // scaler (0 bilinear, 1 bicubic, 2 Lanczos, 3 nearest), colour, limited range
    float4 colorControls;  // brightness, contrast, saturation, gamma exponent
    float4 hueRotation;    // cos, sin
};

float cubicWeight(float x) {
    x = abs(x);
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

float lanczosWeight(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    float a = 3.14159265 * x;
    return 3.0 * sin(a) * sin(a / 3.0) / (a * a);
}

float4 tap(float2 texel) {
    return videoTexture.SampleLevel(pointSampler, (texel + 0.5) * sourceSize.zw, 0);
}

float4 bicubic(float2 uv) {
    float2 position = uv * sourceSize.xy - 0.5;
    float2 base = floor(position);
    float2 f = position - base;
    float wx[4];
    float wy[4];
    [unroll] for (int i = 0; i < 4; ++i) {
        wx[i] = cubicWeight(float(i - 1) - f.x);
        wy[i] = cubicWeight(float(i - 1) - f.y);
    }
    float4 sum = 0.0;
    float total = 0.0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            float w = wx[x] * wy[y];
            sum += tap(base + float2(x - 1, y - 1)) * w;
            total += w;
        }
    }
    return sum / total;
}

float4 lanczos(float2 uv) {
    float2 position = uv * sourceSize.xy - 0.5;
    float2 base = floor(position);
    float2 f = position - base;
    float wx[6];
    float wy[6];
    [unroll] for (int i = 0; i < 6; ++i) {
        wx[i] = lanczosWeight(float(i - 2) - f.x);
        wy[i] = lanczosWeight(float(i - 2) - f.y);
    }
    float4 sum = 0.0;
    float total = 0.0;
    [unroll] for (int y = 0; y < 6; ++y) {
        [unroll] for (int x = 0; x < 6; ++x) {
            float w = wx[x] * wy[y];
            sum += tap(base + float2(x - 2, y - 2)) * w;
            total += w;
        }
    }
    return sum / total;
}

float3 adjustColor(float3 rgb) {
    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    float cb = (rgb.b - luma) / 1.8556;
    float cr = (rgb.r - luma) / 1.5748;
    float y = (luma - 0.5) * colorControls.y + 0.5 + colorControls.x;
    float blue = (cb * hueRotation.x - cr * hueRotation.y) * colorControls.z;
    float red = (cb * hueRotation.y + cr * hueRotation.x) * colorControls.z;
    float3 result = float3(y + 1.5748 * red, y - 0.187324 * blue - 0.468124 * red, y + 1.8556 * blue);
    return pow(saturate(result), colorControls.w);
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color;
    if (outputMode.x > 2.5) color = videoTexture.SampleLevel(pointSampler, uv, 0);
    else if (outputMode.x > 1.5) color = lanczos(uv);
    else if (outputMode.x > 0.5) color = bicubic(uv);
    else color = videoTexture.SampleLevel(linearSampler, uv, 0);
    color.rgb = saturate(color.rgb);
    if (outputMode.y > 0.5) color.rgb = adjustColor(color.rgb);
    if (outputMode.z > 0.5) color.rgb = color.rgb * (219.0 / 255.0) + 16.0 / 255.0;
    return float4(color.rgb, 1.0);
})";

// NVIDIA's RTX Video features are private D3D11 video-processor stream
// extensions. The GUIDs and the version/method words are the ones mpv
// (vf_d3d11vpp.c) and MPC Video Renderer (D3D11VP.cpp) ship, checked
// against each other; NVIDIA publishes no header. Super Resolution and
// RTX Video HDR ("TrueHDR") are two different interfaces.
constexpr GUID kNvidiaSuperResolutionInterface = {
    0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
constexpr GUID kNvidiaTrueHdrInterface = {
    0xfdd62bb4, 0x620b, 0x4fd7, {0x9a, 0xb3, 0x1e, 0x59, 0xd0, 0xd5, 0x44, 0xb3}};
struct NvidiaSuperResolutionExtension {
    UINT version{1};
    UINT method{2};
    UINT enable{};
};
struct NvidiaTrueHdrExtension {
    UINT version{4};
    UINT method{3};
    UINT enable : 1;
    UINT reserved : 31;
};
constexpr UINT kNvidiaVendorId = 0x10DE;

std::string narrow(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t c : text) result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return result;
}

// A compiled shader's inputs or outputs, as D3D11 links them.
bool signatureValues(ID3DBlob* blob, bool inputs, std::vector<ShaderSignatureValue>& values) {
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC description{};
    if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&reflection))) ||
        FAILED(reflection->GetDesc(&description))) return false;
    const UINT count = inputs ? description.InputParameters : description.OutputParameters;
    for (UINT i = 0; i < count; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (FAILED(inputs ? reflection->GetInputParameterDesc(i, &parameter)
                          : reflection->GetOutputParameterDesc(i, &parameter))) return false;
        ShaderSignatureValue value;
        value.semantic = parameter.SemanticName;
        value.index = parameter.SemanticIndex;
        value.registerIndex = parameter.Register;
        value.mask = parameter.Mask;
        // An output's ReadWriteMask is the components it never writes.
        value.read = inputs ? parameter.ReadWriteMask : 0;
        value.position = parameter.SystemValueType == D3D_NAME_POSITION;
        value.distance = parameter.SystemValueType == D3D_NAME_CLIP_DISTANCE ||
                         parameter.SystemValueType == D3D_NAME_CULL_DISTANCE;
        value.system = parameter.SystemValueType != D3D_NAME_UNDEFINED && !value.position && !value.distance;
        value.floating = parameter.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32;
        values.push_back(std::move(value));
    }
    return true;
}

// What the quad's vertex shader writes, which every pixel shader is drawn
// after: SV_POSITION in register 0, then TEXCOORD0-3, each the same video
// coordinate.
const std::vector<ShaderSignatureValue>& quadOutputs() {
    static const std::vector<ShaderSignatureValue> outputs = [] {
        std::vector<ShaderSignatureValue> values;
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        if (SUCCEEDED(D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "QuadDeckVS", nullptr, nullptr,
                                 "main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, nullptr)) &&
            !signatureValues(blob.Get(), false, values)) {
            values.clear();
        }
        return values;
    }();
    return outputs;
}

}

namespace {
// How many frames apart the display is looked at, about two seconds.
constexpr unsigned kDisplayStatusFrames = 120;
// How often render() tries again for a back buffer a failure left missing,
// or for the size a resize could not get, and how many failed tries apart
// such a try is logged (the first failure always is).
constexpr auto kBackBufferRetryInterval = std::chrono::seconds(1);
constexpr unsigned kBackBufferRetriesPerLog = 60;

bool lostDeviceResult(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// A line in the log about how the device was made: what happened, its
// HRESULT, and what is done next.
void describeDevice(const char* what, HRESULT hr, const char* next = nullptr) {
    std::ostringstream out;
    out << "Renderer: " << what << " (0x" << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<unsigned long>(hr) << ")";
    if (next) out << "; " << next;
    appendDiagnostic(out.str());
}
}  // namespace

D3DRenderer::~D3DRenderer() { releaseDeviceResources(); }

void D3DRenderer::releaseDeviceResources() {
    // The swap chain hands out a handle the caller owns.
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    if (frameFenceEvent_) {
        CloseHandle(frameFenceEvent_);
        frameFenceEvent_ = nullptr;
    }
    if (frameQueryTimer_) {
        CloseHandle(frameQueryTimer_);
        frameQueryTimer_ = nullptr;
    }
    if (paceTimer_) {
        CloseHandle(paceTimer_);
        paceTimer_ = nullptr;
    }
    dropSlots();
    renderTarget_.Reset();
    outputShader_.Reset();
    outputParamsBuffer_.Reset();
    pointSampler_.Reset();
    effectShaderActive_ = false;
    overlayLayerView_.Reset();
    overlayLayer_.Reset();
    overlayCompositeShader_.Reset();
    overlayBlend_.Reset();
    overlayParamsBuffer_.Reset();
    sampler_.Reset();
    pixelShader_.Reset();
    copyShader_.Reset();
    sdrToPqShader_.Reset();
    sdrToPqParams_.Reset();
    toneMapShader_.Reset();
    toneMapParams_.Reset();
    loggedToneMap_ = -1;
    pqRefusalLogged_ = false;
    fullRangeHdrLogged_ = false;
    full709_ = {};
    hdrMetadata_ = {};
    hdrPicture_ = HdrPicture::None;
    smartVibranceBuffer_.Reset();
    vibranceColorBuffer_.Reset();
    smartVibranceSelected_ = false;
    pqAwarePreset_ = false;
    pixelParamsBuffer_.Reset();
    uvTransformBuffer_.Reset();
    vertexShader_.Reset();
    for (auto& shader : turnedVertexShaders_) shader.Reset();
    swapChain_.Reset();
    videoContext1_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    // A flip swap chain is destroyed when the context is next flushed, not
    // when it is let go, and until then the window refuses a new one
    // (E_ACCESSDENIED). Anything else holding the device -- the overlay's
    // Direct2D device, a frame still on screen -- would keep it unflushed
    // through the rebuild, and every retry would fail the same way.
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    frameFence_.Reset();
    fenceContext_.Reset();
    frameFenceValue_ = 0;
    frameFenceUnavailable_ = false;
    frameQuery_.Reset();
    context_.Reset();
    device_.Reset();
    adapter_.Reset();
    displayFactory_.Reset();
    adapterLuid_ = {};
    swapChainFlags_ = 0;
    hdrOutput_ = false;
    tenBitOutput_ = false;
    enhancementStatus_.hdrOutput = false;
    enhancementStatus_.superResolutionApplied = false;
    enhancementStatus_.rtxHdrApplied = false;
    loggedSuperResolution_ = -1;
    loggedRtxHdr_ = -1;
    superResolutionTenBitLogged_ = false;
    pendingWidth_ = 0;
    pendingHeight_ = 0;
    nextBackBufferRetry_ = {};
    backBufferRetries_ = 0;
}

bool D3DRenderer::recover(HWND window) {
    std::scoped_lock lock(deviceMutex_);
    releaseDeviceResources();
    // Keep the latch raised throughout reconstruction. If any stage fails,
    // App must see deviceLost() again and make its next retry rather than
    // treating an empty renderer as healthy forever.
    deviceLost_ = true;
    deviceRebuildInProgress_ = true;
    error_.clear();
    // smartVibranceSettings_ survives the rebuild and seeds the new constant
    // buffer; the caller restores the shader selection, which it owns.
    bool recovered = initialize(window);
    // initialize() goes on past an output-mode switch that failed; a device
    // that switch found removed is not a recovered one.
    if (recovered && device_ && lostDeviceResult(device_->GetDeviceRemovedReason())) recovered = false;
    deviceRebuildInProgress_ = false;
    deviceLost_ = !recovered;
    return recovered;
}

void D3DRenderer::noteFailure(const char* operation, HRESULT hr) {
    setError(operation, hr);
    // Any other failure is a lost device only when the device says it is
    // gone. A rebuild tears every decoder down, so a failure the device
    // survived (out of memory, an invalid call) is retried where it
    // happened instead.
    HRESULT reason = hr;
    if (device_) reason = device_->GetDeviceRemovedReason();
    if (!lostDeviceResult(hr) && !lostDeviceResult(reason)) return;
    deviceLost_ = true;
    std::ostringstream out;
    out << "GPU DEVICE LOST during " << operation << ": removed reason 0x" << std::hex
        << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(reason);
    appendDiagnostic(out.str());
}

bool D3DRenderer::initialize(HWND window) {
    window_ = window;
    RECT client{};
    GetClientRect(window, &client);
    width_ = std::max(1L, client.right - client.left);
    height_ = std::max(1L, client.bottom - client.top);
    if (!(createDevice(window) && createPipeline() && createBackBuffer())) return false;
    // The swap chain is born 8-bit; a PQ request is a mode switch on top of
    // a working renderer, and its failure leaves the 8-bit chain in place.
    applyOutputMode();
    return true;
}

HRESULT D3DRenderer::createDeviceOn(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver) {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT refused = S_OK;
    for (;;) {
        device_.Reset();
        context_.Reset();
        D3D_FEATURE_LEVEL actual{};
        const HRESULT hr = D3D11CreateDevice(
            adapter, driver, nullptr, flags,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &device_, &actual, &context_);
        if (SUCCEEDED(hr)) {
            if (FAILED(refused)) describeDevice("the device refused video support and was made without it", refused);
            return hr;
        }
        if (flags & D3D11_CREATE_DEVICE_DEBUG) {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        } else if ((flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) && retryWithoutVideoSupport(hr, driver)) {
            flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            refused = hr;
        } else {
            return hr;
        }
    }
}

bool D3DRenderer::createD3DDevice() {
    builtChoice_ = videoRendererChoice(videoOutput_);
    builtRenderer_ = builtChoice_.renderer;
    adapterFallback_ = false;
    adapterMissing_ = false;
    HRESULT hr = E_FAIL;
    bool created = false;
    if (builtChoice_.renderer == VideoRenderer::Warp) {
        hr = createDeviceOn(nullptr, D3D_DRIVER_TYPE_WARP);
        created = SUCCEEDED(hr);
        if (!created) {
            describeDevice("WARP could not be created; rendering on the graphics card", hr);
            builtRenderer_ = VideoRenderer::Direct3D11;
        }
    } else if (!builtChoice_.adapter.empty()) {
        // Without an adapter D3D11 takes the one driving the main display.
        const auto chosen = findVideoAdapter(builtChoice_.adapter, builtChoice_.adapterIndex);
        if (chosen) {
            hr = createDeviceOn(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN);
            created = SUCCEEDED(hr);
            if (!created) describeDevice("the chosen card could not be used; rendering on the default one", hr);
        } else {
            adapterMissing_ = true;
            appendDiagnostic("Renderer: the chosen card " + narrow(builtChoice_.adapter) +
                             " is not connected; rendering on the default one");
        }
        adapterFallback_ = !created;
    }
    if (!created) {
        hr = createDeviceOn(nullptr, D3D_DRIVER_TYPE_HARDWARE);
    }
    // No card makes a device at all. WARP still shows the videos, decoded
    // in software, where the window would not open; a rebuild after a
    // device on a card (a lost device, a driver being installed) waits for
    // the card instead, as below. The log says WARP is tried, and createDevice
    // then that it renders; should WARP fail as well, its own failure is
    // logged and reported beside the card's.
    HRESULT warp = S_OK;
    if (FAILED(hr) && builtChoice_.renderer != VideoRenderer::Warp &&
        deviceAdapterAcceptable(true, deviceOnCard_, builtChoice_.renderer)) {
        describeDevice("no graphics card could make a device", hr, "trying WARP");
        warp = createDeviceOn(nullptr, D3D_DRIVER_TYPE_WARP);
        if (SUCCEEDED(warp)) {
            hr = warp;
            builtRenderer_ = VideoRenderer::Warp;
        } else {
            describeDevice("WARP could not be created either", warp);
        }
    }
    if (FAILED(hr)) {
        setError("D3D11CreateDevice", hr);
        if (FAILED(warp)) {
            std::ostringstream out;
            out << "; WARP failed: 0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<unsigned long>(warp);
            error_ += out.str();
        }
        return false;
    }
    // During a driver update D3D11 takes the Basic Render Driver for the
    // card, and the device is made there without video support. A rebuild
    // after a device on the card is refused it, and App tries again until
    // the card is back, instead of the videos staying on the CPU, decoded
    // in software, for the rest of the session.
    DXGI_ADAPTER_DESC1 adapter{};
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> made;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> made1;
    const bool software = SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&made)) &&
                          SUCCEEDED(made.As(&made1)) && SUCCEEDED(made1->GetDesc1(&adapter)) &&
                          (adapter.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (!deviceAdapterAcceptable(software, deviceOnCard_, builtChoice_.renderer)) {
        context_.Reset();
        device_.Reset();
        error_ = "The rebuild found only " + narrow(adapter.Description) +
                 ", a software adapter; waiting for the graphics card";
        appendDiagnostic("Renderer: " + error_);
        return false;
    }
    deviceOnCard_ = !software;
    return true;
}

bool D3DRenderer::createDevice(HWND window) {
    if (!createD3DDevice()) return false;
    HRESULT hr = S_OK;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) { setError("Query IDXGIDevice", hr); return false; }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) { setError("GetAdapter", hr); return false; }
    adapter_ = adapter;
    {
        // D3D11CreateDevice with a null adapter takes the one driving the
        // primary display. On a machine with an integrated GPU as well, that
        // decides whether NVIDIA's extensions can exist at all, so say which.
        DXGI_ADAPTER_DESC adapterDescription{};
        if (SUCCEEDED(adapter->GetDesc(&adapterDescription))) {
            adapterLuid_ = adapterDescription.AdapterLuid;
            enhancementStatus_.adapterName = adapterDescription.Description;
            enhancementStatus_.nvidiaAdapter = adapterDescription.VendorId == kNvidiaVendorId;
            std::ostringstream out;
            out << "GPU adapter: " << narrow(enhancementStatus_.adapterName) << " (vendor 0x"
                << std::hex << adapterDescription.VendorId << ", device 0x"
                << adapterDescription.DeviceId << ")";
            appendDiagnostic(out.str());
        }
    }
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { setError("Query IDXGIFactory2", hr); return false; }
    displayFactory_ = factory;
    if (!createSwapChain(window, factory.Get())) return false;
    appendDiagnostic(std::string("Renderer: ") + narrow(videoRendererLabel(builtRenderer_)) +
                     (flipModel_ ? ", flip model" : ", bitblt model"));

    // WARP and the Basic Render Driver come without the video processor.
    // The renderer works on without it; hardware decoding cannot, and the
    // decoders find that out for themselves and decode in software.
    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        videoDevice_.Reset();
        videoContext_.Reset();
        appendDiagnostic("Renderer: no D3D11 video processor on this device; "
                         "hardware decoding and deinterlacing are off");
    } else if (FAILED(context_.As(&videoContext1_))) {
        videoContext1_.Reset();
    }
    probeVideoEnhancements();
    probeProcessorRotation();
    refreshDisplayStatus();
    return true;
}

bool D3DRenderer::createSwapChain(HWND window, IDXGIFactory2* factory) {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width_;
    description.Height = height_;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    HRESULT hr = E_FAIL;
    flipModel_ = builtRenderer_ != VideoRenderer::Direct3D11Bitblt;
    if (!flipModel_) {
        // The bitblt model: one buffer, copied into DWM's picture of the
        // window as Windows did before the flip model, which capture tools
        // that read windows the old way can see. DWM's composition is the
        // vsync, so Present does not wait for the blank inside the lock.
        description.BufferCount = 1;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapChainFlags_ = 0;
        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), window, &description, nullptr, nullptr, &swapChain_);
        if (FAILED(hr)) {
            std::ostringstream out;
            out << "Renderer: the bitblt swap chain was refused (0x" << std::hex << std::setw(8)
                << std::setfill('0') << static_cast<unsigned long>(hr) << "); using the flip model";
            appendDiagnostic(out.str());
            flipModel_ = true;
            builtRenderer_ = VideoRenderer::Direct3D11;
        }
    }
    if (flipModel_) {
        // A frame-latency waitable swap chain is what makes vsync affordable
        // here. Present must stay inside the same critical section as the
        // D3D11VA decoders, so a Present that blocked for the vertical blank
        // would stall every decoder for most of a refresh interval. Waiting
        // on the swap chain's own event instead happens outside that lock,
        // and by the time the renderer presents, the queue has room and
        // Present returns promptly.
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swapChainFlags_ = description.Flags;
        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), window, &description, nullptr, nullptr, &swapChain_);
        if (FAILED(hr)) {
            // Waitable swap chains need Windows 8.1 or newer. Fall back to a
            // plain flip chain; presentation then syncs inside the lock as
            // before.
            description.Flags = 0;
            swapChainFlags_ = 0;
            hr = factory->CreateSwapChainForHwnd(
                device_.Get(), window, &description, nullptr, nullptr, &swapChain_);
        }
    }
    if (FAILED(hr)) {
        setError("CreateSwapChainForHwnd", hr);
        return false;
    }
    // The app translates Alt+Enter itself; DXGI's own handling would fight it.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    if (swapChainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        Microsoft::WRL::ComPtr<IDXGISwapChain2> waitableSwapChain;
        if (SUCCEEDED(swapChain_.As(&waitableSwapChain))) {
            waitableSwapChain->SetMaximumFrameLatency(1);
            frameLatencyWaitable_ = waitableSwapChain->GetFrameLatencyWaitableObject();
        }
    }
    if (!flipModel_) {
        // Windows 10 1803 and later; before it the timer resolves to the
        // system tick, which still beats the main loop's own timeout.
        paceTimer_ = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!paceTimer_) paceTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        armPaceTimer();
    }
    return true;
}

void D3DRenderer::armPaceTimer() {
    if (!paceTimer_) return;
    // The first refresh after now, from DWM's own clock of its blanks; a
    // sixtieth of a second when DWM does not say.
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    LONGLONG due = 166'667;
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0 &&
        frequency.QuadPart > 0) {
        const auto period = static_cast<LONGLONG>(timing.qpcRefreshPeriod);
        // A frame the timer has just started is on its blank; the one wanted
        // is the next, so less than an eighth of a period ahead is too soon.
        const LONGLONG earliest = now.QuadPart + period / 8;
        LONGLONG next = static_cast<LONGLONG>(timing.qpcVBlank);
        if (next < earliest) next += ((earliest - next) / period + 1) * period;
        due = (next - now.QuadPart) * 10'000'000 / frequency.QuadPart;
    }
    LARGE_INTEGER relative{};
    relative.QuadPart = -std::max<LONGLONG>(due, 1);
    SetWaitableTimer(paceTimer_, &relative, 0, nullptr, nullptr, FALSE);
}

void D3DRenderer::probeVideoEnhancements() {
    enhancementStatus_.superResolutionSupported = false;
    enhancementStatus_.rtxHdrSupported = false;
    if (!enhancementStatus_.nvidiaAdapter || !videoDevice_ || !videoContext_) return;
    // A throwaway 1080p processor is enough to ask the driver; the answer is
    // per device, not per size.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = 1920;
    content.InputHeight = 1080;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = 1920;
    content.OutputHeight = 1080;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator)) ||
        FAILED(videoDevice_->CreateVideoProcessor(enumerator.Get(), 0, &processor))) {
        appendDiagnostic("RTX Video probe: no video processor could be created");
        return;
    }
    NvidiaSuperResolutionExtension superResolution{};
    superResolution.enable = 1;
    HRESULT hr = videoContext_->VideoProcessorSetStreamExtension(
        processor.Get(), 0, &kNvidiaSuperResolutionInterface,
        sizeof(superResolution), &superResolution);
    enhancementStatus_.superResolutionSupported = SUCCEEDED(hr);
    // The RTX Video HDR interface answers a capability query as well as
    // taking the enable; mpv asks first and so do we.
    UINT trueHdrSupported = 0;
    const HRESULT queried = videoContext_->VideoProcessorGetStreamExtension(
        processor.Get(), 0, &kNvidiaTrueHdrInterface, sizeof(trueHdrSupported), &trueHdrSupported);
    enhancementStatus_.rtxHdrSupported = SUCCEEDED(queried) && trueHdrSupported != 0;
    std::ostringstream out;
    out << "RTX Video probe: Super Resolution "
        << (enhancementStatus_.superResolutionSupported ? "accepted" : "declined") << " (0x"
        << std::hex << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(hr)
        << "), RTX Video HDR " << (enhancementStatus_.rtxHdrSupported ? "supported" : "unsupported")
        << " (0x" << std::setw(8) << static_cast<unsigned long>(queried) << ", "
        << std::dec << trueHdrSupported << ")";
    appendDiagnostic(out.str());
}

void D3DRenderer::probeProcessorRotation() {
    processorRotates_ = false;
    if (!videoDevice_ || !videoContext_) return;
    // As for the extensions, a throwaway processor's answer is the device's:
    // a phone's 1080p stood upright.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = 1920;
    content.InputHeight = 1080;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = 1080;
    content.OutputHeight = 1920;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator)) ||
        FAILED(enumerator->GetVideoProcessorCaps(&caps))) {
        return;
    }
    processorRotates_ = (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0;
    appendDiagnostic(std::string("Video processor: ") +
                     (processorRotates_ ? "turns a picture itself" : "does not turn a picture; the pane's draw does"));
}

void D3DRenderer::refreshDisplayFactory(bool force) {
    if (!displayFactory_ || (!force && displayFactory_->IsCurrent())) return;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    // Without a new one the old one stays, and is asked again next time.
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> found;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
    for (UINT index = 0; factory->EnumAdapters1(index, candidate.ReleaseAndGetAddressOf()) == S_OK; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(candidate->GetDesc1(&description)) &&
            description.AdapterLuid.LowPart == adapterLuid_.LowPart &&
            description.AdapterLuid.HighPart == adapterLuid_.HighPart) {
            found = candidate;
            break;
        }
    }
    displayFactory_ = factory;
    adapter_ = found;
    appendDiagnostic(found ? "Display: outputs listed again after a display change"
                           : "Display: the device's adapter is no longer listed");
}

void D3DRenderer::noteDisplayChange() {
    std::scoped_lock lock(deviceMutex_);
    framesSinceDisplayStatus_ = kDisplayStatusFrames;
}

void D3DRenderer::refreshDisplayStatus() {
    framesSinceDisplayStatus_ = 0;
    refreshDisplayFactory();
    bool displayHdr = false;
    float sdrWhiteNits = 200.0F;
    const HMONITOR monitor = window_ ? MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST) : nullptr;
    std::wstring outputName;
    const auto findOn = [&](IDXGIAdapter* adapter) {
        for (UINT index = 0; adapter; ++index) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(index, &output) != S_OK) break;
            Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 description{};
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&description)) ||
                description.Monitor != monitor) continue;
            displayHdr = description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            outputName = description.DeviceName;
            return true;
        }
        return false;
    };
    // The display is an output of the card it is plugged into, which need
    // not be the one the device is on: a laptop's panel hangs off its
    // integrated card while a discrete one renders, and the device may be on
    // an adapter with no outputs at all. The device's own card first, then
    // every other.
    bool otherAdapter = false;
    if (monitor && !findOn(adapter_.Get()) && displayFactory_) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        for (UINT index = 0; displayFactory_->EnumAdapters1(index, candidate.ReleaseAndGetAddressOf()) == S_OK;
             ++index) {
            if (candidate.Get() != adapter_.Get() && findOn(candidate.Get())) {
                otherAdapter = true;
                break;
            }
        }
    }
    // Windows' SDR white level for that output, the brightness it gives SDR
    // windows on an HDR display; the interface layer is drawn to match it.
    if (!outputName.empty()) {
        for (const auto& display : activeDisplayPaths()) {
            if (display.device != outputName) continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = display.path.targetInfo.adapterId;
            white.header.id = display.path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) {
                // Reported in thousandths of the 80-nit reference.
                sdrWhiteNits = std::clamp(white.SDRWhiteLevel / 1000.0F * 80.0F, 80.0F, 1000.0F);
            }
            break;
        }
    }
    if (displayHdr != enhancementStatus_.displayHdr ||
        std::abs(sdrWhiteNits - enhancementStatus_.sdrWhiteNits) > 0.5F) {
        std::ostringstream out;
        out << "Display: " << narrow(outputName) << (displayHdr ? " HDR on" : " HDR off")
            << ", SDR white " << sdrWhiteNits << " nits";
        if (otherAdapter) out << " (an output of another card than the one rendering)";
        appendDiagnostic(out.str());
    }
    enhancementStatus_.displayHdr = displayHdr;
    enhancementStatus_.sdrWhiteNits = sdrWhiteNits;
}

bool D3DRenderer::rtxHdrReady() const {
    return enhancements_.rtxHdr && enhancementStatus_.nvidiaAdapter && enhancementStatus_.rtxHdrSupported;
}

bool D3DRenderer::wantHdrOutput() const {
    // HDR presentation is the flip model's, and its processors name their
    // colour spaces by DXGI type. An HDR video (HDR10 or HLG) is enough, on
    // any card;
    // RTX Video HDR asks for it for SDR video as well.
    return hdrOutputWanted(enhancementStatus_.displayHdr, videoContext1_ != nullptr && flipModel_,
                           rtxHdrReady(), hdrVideoShown_);
}

bool D3DRenderer::noteHdrVideo(bool shownNow) {
    const auto now = std::chrono::steady_clock::now();
    const bool held = hdrVideoHeld(shownNow, hdrVideoShown_,
                                   std::chrono::duration<double>(now - hdrVideoLastShown_).count());
    if (shownNow) hdrVideoLastShown_ = now;
    const bool changed = held != hdrVideoShown_;
    hdrVideoShown_ = held;
    return changed;
}

void D3DRenderer::applyHdrMetadata(const HdrStaticMetadata& metadata) {
    if (metadata == hdrMetadata_ || !swapChain_) return;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain4;
    if (FAILED(swapChain_.As(&swapChain4))) return;
    // Windows may or may not pass this on to the display (it composes the
    // desktop itself unless the window is alone on an overlay plane); where
    // it does, the display's own tone mapping starts from the film's levels
    // rather than from a guess.
    HRESULT hr;
    if (metadata.present) {
        DXGI_HDR_METADATA_HDR10 hdr10{};
        for (int axis = 0; axis < 2; ++axis) {
            hdr10.RedPrimary[axis] = metadata.red[axis];
            hdr10.GreenPrimary[axis] = metadata.green[axis];
            hdr10.BluePrimary[axis] = metadata.blue[axis];
            hdr10.WhitePoint[axis] = metadata.white[axis];
        }
        hdr10.MaxMasteringLuminance = metadata.maxMasteringNits;
        hdr10.MinMasteringLuminance = metadata.minMasteringTenThousandths;
        hdr10.MaxContentLightLevel = metadata.maxContentNits;
        hdr10.MaxFrameAverageLightLevel = metadata.maxFrameAverageNits;
        hr = swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr10), &hdr10);
    } else {
        hr = swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
    }
    // Not asked again for the same picture whether or not it was taken.
    hdrMetadata_ = metadata;
    std::ostringstream out;
    if (metadata.present) {
        out << "HDR10 metadata: mastering " << metadata.maxMasteringNits << " nits, MaxCLL "
            << metadata.maxContentNits << ", MaxFALL " << metadata.maxFrameAverageNits;
    } else {
        out << "HDR10 metadata: none";
    }
    if (FAILED(hr)) out << " (refused, 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
    appendDiagnostic(out.str());
}

bool D3DRenderer::wantTenBitOutput() const {
    return videoOutput_.tenBit && flipModel_;
}

bool D3DRenderer::applyOutputMode() {
    const bool wanted = wantHdrOutput();
    // PQ is 10-bit already; the 10-bit SDR choice waits behind it.
    const bool tenBit = !wanted && wantTenBitOutput();
    const bool current = wanted == hdrOutput_ && tenBit == tenBitOutput_;
    // recover() holds the latch up while it builds; its new chain switches
    // here as a first one does.
    if (!wanted) pqRefusalLogged_ = false;
    if (current || !swapChain_ || (deviceLost_ && !deviceRebuildInProgress_)) return current;
    // A refusal is said once: with an HDR10 video on screen this is asked
    // again every couple of seconds.
    const auto refused = [&](const char* why) {
        if (!pqRefusalLogged_) appendDiagnostic(why);
        pqRefusalLogged_ = true;
        return false;
    };
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain_.As(&swapChain3))) {
        // 10-bit sRGB is the chain's colour space without being told; PQ
        // has to be told.
        if (wanted) return refused("HDR output: swap chain has no colour-space control; staying SDR");
    }
    const DXGI_FORMAT format = wanted || tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_COLOR_SPACE_TYPE colorSpace = wanted
        ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT support = 0;
    if (wanted && (FAILED(swapChain3->CheckColorSpaceSupport(colorSpace, &support)) ||
                   !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))) {
        return refused("HDR output: the swap chain cannot present PQ here; staying SDR");
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    HRESULT hr = swapChain_->ResizeBuffers(0, width_, height_, format, swapChainFlags_);
    if (FAILED(hr)) {
        noteFailure("ResizeBuffers(format)", hr);
        // Try to get the previous chain back rather than leave no back
        // buffer; render() tries again for a target still missing.
        swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
        if (!createBackBuffer()) nextBackBufferRetry_ = std::chrono::steady_clock::now() + kBackBufferRetryInterval;
        return false;
    }
    if (swapChain3) {
        hr = swapChain3->SetColorSpace1(colorSpace);
        if (FAILED(hr)) setError("SetColorSpace1", hr);
    }
    hdrOutput_ = wanted;
    tenBitOutput_ = tenBit;
    enhancementStatus_.hdrOutput = wanted;
    pqRefusalLogged_ = false;
    loggedToneMap_ = -1;
    // The chain's new buffers have been told nothing.
    hdrMetadata_ = {};
    // Every slot's output texture and processor were built for the old format.
    dropSlots();
    // The chain has switched either way; render() tries again for a target
    // that could not be made on its new buffers.
    const bool targetMade = createBackBuffer();
    if (!targetMade) nextBackBufferRetry_ = std::chrono::steady_clock::now() + kBackBufferRetryInterval;
    appendDiagnostic(wanted ? (rtxHdrReady() ? "Output: 10-bit PQ (RTX Video HDR)"
                                             : "Output: 10-bit PQ (HDR video)")
                            : tenBit ? "Output: 10-bit sRGB" : "Output: 8-bit sRGB");
    return targetMade;
}

void D3DRenderer::dropSlots() {
    slots_ = {};
    softwareUploads_ = {};
    softwarePq_ = {};
    postTextures_ = {};
    loggedSoftwareRoute_.fill(-1);
}

void D3DRenderer::setVideoEnhancements(const VideoEnhancementSettings& settings) {
    std::scoped_lock lock(deviceMutex_);
    if (settings == enhancements_) return;
    enhancements_ = settings;
    // Rebuild every processor with the new extension state on the next frame.
    dropSlots();
    if (!deviceLost_ && swapChain_) {
        refreshDisplayStatus();
        applyOutputMode();
    }
}

VideoEnhancementStatus D3DRenderer::enhancementStatus() const {
    std::scoped_lock lock(deviceMutex_);
    return enhancementStatus_;
}

bool D3DRenderer::setVideoOutput(const VideoOutputSettings& settings) {
    std::scoped_lock lock(deviceMutex_);
    const VideoOutputSettings wanted = clampVideoOutputSettings(settings);
    const VideoOutputSettings previous = videoOutput_;
    videoOutput_ = wanted;
    // Not built yet: initialize() builds it as they say. Lost with no device
    // (a rebuild failed), a new renderer or card is worth trying at once.
    if (!device_) return deviceLost_ && videoRendererChoice(wanted) != builtChoice_;
    if (videoRendererChoice(wanted) != builtChoice_) return true;
    // A processor is built to deinterlace or not.
    if (wanted.deinterlace != previous.deinterlace) dropSlots();
    if (wanted.tenBit != previous.tenBit && !deviceLost_ && swapChain_) applyOutputMode();
    return false;
}

void D3DRenderer::requestRebuild() {
    std::scoped_lock lock(deviceMutex_);
    if (!device_) return;
    deviceLost_ = true;
}

VideoRendererStatus D3DRenderer::rendererStatus() const {
    std::scoped_lock lock(deviceMutex_);
    VideoRendererStatus status;
    status.built = device_ != nullptr && !deviceLost_;
    status.renderer = builtRenderer_;
    status.adapterName = enhancementStatus_.adapterName;
    status.adapterFallback = adapterFallback_;
    status.adapterMissing = adapterMissing_;
    status.videoProcessor = videoDevice_ != nullptr;
    status.flipModel = flipModel_;
    status.tenBitOutput = tenBitOutput_;
    status.hdrOutput = hdrOutput_;
    status.hdrPicture = hdrPicture_;
    return status;
}

void D3DRenderer::applyStreamExtensions(std::size_t slot, bool hdrSource) {
    auto& target = slots_[slot];
    target.rtxHdrApplied = false;
    target.superResolutionApplied = false;
    if (!enhancementStatus_.nvidiaAdapter) return;
    // Super Resolution upscales; asked for on a stream shown at or below
    // its own size the driver still runs its network and, at 4K60, that
    // alone can take most of a frame. So it is set only when the processor
    // magnifies, and a source shown at its own size is left alone.
    // A picture the processor turns onto its side comes out the other way
    // round.
    const bool sideways = turnedSideways(target.turned);
    const bool magnifies = target.outputWidth > (sideways ? target.inputHeight : target.inputWidth) ||
                           target.outputHeight > (sideways ? target.inputWidth : target.inputHeight);
    if (enhancements_.superResolution && enhancementStatus_.superResolutionSupported && magnifies) {
        NvidiaSuperResolutionExtension extension{};
        extension.enable = 1;
        const HRESULT hr = videoContext_->VideoProcessorSetStreamExtension(
            target.processor.Get(), 0, &kNvidiaSuperResolutionInterface, sizeof(extension), &extension);
        enhancementStatus_.superResolutionApplied = SUCCEEDED(hr);
        target.superResolutionApplied = SUCCEEDED(hr);
        const int outcome = SUCCEEDED(hr) ? 1 : 0;
        if (outcome != loggedSuperResolution_) {
            loggedSuperResolution_ = outcome;
            std::ostringstream out;
            out << "RTX Super Resolution " << (outcome ? "enabled" : "refused") << " on "
                << target.inputWidth << "x" << target.inputHeight << " -> "
                << target.outputWidth << "x" << target.outputHeight;
            if (!outcome) out << " (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            appendDiagnostic(out.str());
        }
    } else if (enhancements_.superResolution && enhancementStatus_.superResolutionSupported) {
        enhancementStatus_.superResolutionApplied = false;
        if (loggedSuperResolution_ != 2) {
            loggedSuperResolution_ = 2;
            std::ostringstream out;
            out << "RTX Super Resolution idle: " << target.inputWidth << "x" << target.inputHeight
                << " shown at " << target.outputWidth << "x" << target.outputHeight << ", no upscaling";
            appendDiagnostic(out.str());
        }
    } else {
        enhancementStatus_.superResolutionApplied = false;
        loggedSuperResolution_ = -1;
    }
    // RTX Video HDR only makes sense with a PQ output to write into, and
    // only for an SDR stream; an HDR10 stream is passed through as it is and
    // an HLG one is made PQ by the renderer's own pass.
    if (hdrOutput_ && enhancements_.rtxHdr && !hdrSource) {
        NvidiaTrueHdrExtension extension{};
        extension.enable = 1;
        extension.reserved = 0;
        const HRESULT hr = videoContext_->VideoProcessorSetStreamExtension(
            target.processor.Get(), 0, &kNvidiaTrueHdrInterface, sizeof(extension), &extension);
        enhancementStatus_.rtxHdrApplied = SUCCEEDED(hr);
        target.rtxHdrApplied = SUCCEEDED(hr);
        const int outcome = SUCCEEDED(hr) ? 1 : 0;
        if (outcome != loggedRtxHdr_) {
            loggedRtxHdr_ = outcome;
            std::ostringstream out;
            out << "RTX Video HDR " << (outcome ? "enabled" : "refused");
            if (!outcome) out << " (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            appendDiagnostic(out.str());
        }
    } else if (hdrOutput_ && hdrSource) {
        enhancementStatus_.rtxHdrApplied = false;
        if (loggedRtxHdr_ != 2) {
            loggedRtxHdr_ = 2;
            appendDiagnostic("HDR source shown as PQ; RTX Video HDR not applied to it");
        }
    } else {
        enhancementStatus_.rtxHdrApplied = false;
        loggedRtxHdr_ = -1;
    }
}

bool D3DRenderer::createPipeline() {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        kVertexShader, sizeof(kVertexShader) - 1, "QuadDeckVS", nullptr, nullptr,
        "main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexBlob, &errors);
    if (FAILED(hr)) {
        setError("D3DCompile(vertex)", hr);
        return false;
    }
    hr = device_->CreateVertexShader(
        vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) {
        setError("CreateVertexShader", hr);
        return false;
    }
    // The same quad for a picture a quarter, a half and three quarters of a
    // turn from upright as it is stored.
    for (std::size_t turns = 1; turns <= turnedVertexShaders_.size(); ++turns) {
        const std::string count = std::to_string(turns);
        const D3D_SHADER_MACRO macros[] = {{"QUARTER_TURNS", count.c_str()}, {nullptr, nullptr}};
        Microsoft::WRL::ComPtr<ID3DBlob> turnedBlob;
        hr = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "QuadDeckVS", macros, nullptr,
                        "main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &turnedBlob, nullptr);
        if (FAILED(hr)) {
            setError("D3DCompile(turned vertex)", hr);
            return false;
        }
        hr = device_->CreateVertexShader(turnedBlob->GetBufferPointer(), turnedBlob->GetBufferSize(), nullptr,
                                         &turnedVertexShaders_[turns - 1]);
        if (FAILED(hr)) {
            setError("CreateVertexShader(turned)", hr);
            return false;
        }
    }
    D3D11_BUFFER_DESC transformDescription{};
    transformDescription.ByteWidth = sizeof(float) * 4;
    transformDescription.Usage = D3D11_USAGE_DEFAULT;
    transformDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&transformDescription, nullptr, &uvTransformBuffer_);
    if (FAILED(hr)) {
        setError("Create UV transform buffer", hr);
        return false;
    }
    D3D11_BUFFER_DESC pixelParamsDescription{};
    pixelParamsDescription.ByteWidth = sizeof(float) * 12;
    pixelParamsDescription.Usage = D3D11_USAGE_DEFAULT;
    pixelParamsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&pixelParamsDescription, nullptr, &pixelParamsBuffer_);
    if (FAILED(hr)) {
        setError("Create PotPlayer shader parameter buffer", hr);
        return false;
    }
    D3D11_BUFFER_DESC vibranceDescription{};
    vibranceDescription.ByteWidth = sizeof(float) * 4;
    vibranceDescription.Usage = D3D11_USAGE_DEFAULT;
    vibranceDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const auto vibrance = clampSmartVibranceSettings(smartVibranceSettings_);
    const float vibranceValues[] = {
        vibrance.intensity, vibrance.saturationPivot,
        vibrance.grayPivot, vibrance.graySharpness};
    D3D11_SUBRESOURCE_DATA vibranceData{};
    vibranceData.pSysMem = vibranceValues;
    hr = device_->CreateBuffer(
        &vibranceDescription, &vibranceData, &smartVibranceBuffer_);
    if (FAILED(hr)) {
        setError("Create Smart Vibrance parameter buffer", hr);
        return false;
    }
    if (!compilePixelShader(kPixelShader, sizeof(kPixelShader) - 1, "QuadDeckPS")) return false;
    copyShader_ = pixelShader_;
    const std::string sdrToPq = withPqFunctions(kSdrToPqShader);
    if (!compilePixelShader(sdrToPq.data(), sdrToPq.size(), "SdrToPq")) return false;
    sdrToPqShader_ = pixelShader_;
    const std::string toneMap = withPqFunctions(kToneMapShader);
    if (!compilePixelShader(toneMap.data(), toneMap.size(), "ToneMap")) return false;
    toneMapShader_ = pixelShader_;
    pixelShader_ = copyShader_;
    hr = device_->CreateBuffer(&vibranceDescription, nullptr, &sdrToPqParams_);
    if (FAILED(hr)) { setError("Create SDR presentation parameter buffer", hr); return false; }
    D3D11_BUFFER_DESC toneMapDescription = vibranceDescription;
    toneMapDescription.ByteWidth = sizeof(float) * 8;
    hr = device_->CreateBuffer(&toneMapDescription, nullptr, &toneMapParams_);
    if (FAILED(hr)) { setError("Create tone-map parameter buffer", hr); return false; }
    hr = device_->CreateBuffer(&vibranceDescription, nullptr, &vibranceColorBuffer_);
    if (FAILED(hr)) { setError("Create Vibrance colour-space buffer", hr); return false; }
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        Microsoft::WRL::ComPtr<ID3DBlob> compositeErrors;
        hr = D3DCompile(kOverlayCompositeShader, sizeof(kOverlayCompositeShader) - 1,
                        "OverlayComposite", nullptr, nullptr, "main", "ps_4_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &compositeErrors);
        if (FAILED(hr)) {
            if (compositeErrors) {
                error_.assign(static_cast<const char*>(compositeErrors->GetBufferPointer()),
                              compositeErrors->GetBufferSize());
            } else {
                setError("D3DCompile(overlay composite)", hr);
            }
            return false;
        }
        hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                        nullptr, &overlayCompositeShader_);
        if (FAILED(hr)) { setError("Create overlay composite shader", hr); return false; }
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device_->CreateBlendState(&blend, &overlayBlend_);
        if (FAILED(hr)) { setError("Create overlay blend state", hr); return false; }
        D3D11_BUFFER_DESC paramsDescription{};
        paramsDescription.ByteWidth = sizeof(float) * 4;
        paramsDescription.Usage = D3D11_USAGE_DEFAULT;
        paramsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device_->CreateBuffer(&paramsDescription, nullptr, &overlayParamsBuffer_);
        if (FAILED(hr)) { setError("Create overlay parameter buffer", hr); return false; }
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler, &sampler_);
    if (FAILED(hr)) {
        setError("CreateSamplerState", hr);
        return false;
    }
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = device_->CreateSamplerState(&sampler, &pointSampler_);
    if (FAILED(hr)) { setError("Create point sampler", hr); return false; }
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        Microsoft::WRL::ComPtr<ID3DBlob> outputErrors;
        hr = D3DCompile(kOutputShader, sizeof(kOutputShader) - 1, "OutputPass", nullptr, nullptr,
                        "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &outputErrors);
        if (FAILED(hr)) {
            if (outputErrors) {
                error_.assign(static_cast<const char*>(outputErrors->GetBufferPointer()),
                              outputErrors->GetBufferSize());
            } else {
                setError("D3DCompile(output pass)", hr);
            }
            return false;
        }
        hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &outputShader_);
        if (FAILED(hr)) { setError("Create output pass shader", hr); return false; }
        D3D11_BUFFER_DESC paramsDescription{};
        paramsDescription.ByteWidth = sizeof(float) * 16;
        paramsDescription.Usage = D3D11_USAGE_DEFAULT;
        paramsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device_->CreateBuffer(&paramsDescription, nullptr, &outputParamsBuffer_);
        if (FAILED(hr)) { setError("Create output pass parameter buffer", hr); return false; }
    }
    // The pipeline starts on the plain copy.
    effectShaderActive_ = false;
    return true;
}

bool D3DRenderer::compilePixelShader(const char* source, std::size_t length, const char* name) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, length, name, nullptr, nullptr, "main", "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) error_.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        else setError("D3DCompile(pixel)", hr);
        return false;
    }
    return installPixelShader(blob.Get(), "CreatePixelShader");
}

bool D3DRenderer::installPixelShader(ID3DBlob* blob, const char* what) {
    std::scoped_lock lock(deviceMutex_);
    if (!device_ || (deviceLost_ && !deviceRebuildInProgress_)) {
        error_ = "Cannot create pixel shader while graphics device is unavailable";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    const HRESULT hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
    if (FAILED(hr)) { setError(what, hr); return false; }
    pixelShader_ = std::move(shader);
    smartVibranceSelected_ = false;
    pqAwarePreset_ = false;
    effectShaderActive_ = true;
    error_.clear();
    return true;
}

bool D3DRenderer::setShaderPreset(ShaderPreset preset) {
    std::scoped_lock lock(deviceMutex_);
    switch (preset) {
    case ShaderPreset::Sharpen: return compilePixelShader(kSharpenShader, sizeof(kSharpenShader) - 1, "Sharpen");
    case ShaderPreset::Grayscale:
    case ShaderPreset::Invert: {
        const std::string source = withPqFunctions(kPresetColorSpace) +
            (preset == ShaderPreset::Grayscale ? kGrayscaleShader : kInvertShader);
        if (!compilePixelShader(source.data(), source.size(),
                                preset == ShaderPreset::Grayscale ? "Grayscale" : "Invert")) return false;
        pqAwarePreset_ = true;
        return true;
    }
    case ShaderPreset::SmartVibrancePlus: {
        const std::string source = withPqFunctions(kSmartVibrancePlusShader);
        if (!compilePixelShader(source.data(), source.size(), "SmartVibrancePlus")) return false;
        smartVibranceSelected_ = true;
        return true;
    }
    default:
        if (!compilePixelShader(kPixelShader, sizeof(kPixelShader) - 1, "Normal")) return false;
        // The plain copy: the output pass reads the converted picture itself.
        effectShaderActive_ = false;
        return true;
    }
}

void D3DRenderer::setSmartVibranceSettings(const SmartVibranceSettings& settings) {
    std::scoped_lock lock(deviceMutex_);
    smartVibranceSettings_ = clampSmartVibranceSettings(settings);
    if (!context_ || !smartVibranceBuffer_ || deviceLost_) return;
    const float values[] = {
        smartVibranceSettings_.intensity,
        smartVibranceSettings_.saturationPivot,
        smartVibranceSettings_.grayPivot,
        smartVibranceSettings_.graySharpness};
    context_->UpdateSubresource(smartVibranceBuffer_.Get(), 0, nullptr, values, 0, 0);
}

bool D3DRenderer::loadPixelShader(const std::wstring& path) {
    return compilePixelShaderFile(path);
}

bool D3DRenderer::compilePixelShaderFile(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        error_ = "Could not open pixel shader file";
        return false;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    const std::string text = bytes.str();
    // D3D11 links the vertex and pixel shaders by register. A main() that
    // reads an input the quad's vertex shader does not feed -- a TEXCOORD0
    // without the position before it packs into the position's register --
    // drew every pane one colour; it is refused as a file that does not
    // compile is, saying what main() needs. A legacy main() is given the
    // position first. One that does not compile so, or that the vertex
    // shader does not feed so, is compiled again without it, as it was
    // adapted before, and taken when the vertex shader feeds that: a
    // position the adapter did not see (a typedef'd or nested struct, a
    // macro) then stays first. One that neither feeds is compiled once more
    // with the position before a struct parameter the adapter took to begin
    // with one, by a definition of the struct that is not the one compiled
    // (in an #if 0). The refusal is the first one's when that compiled, and
    // otherwise the second one's: the file's compile error as it was
    // adapted before, or what its main() reads where, rather than an error
    // about the position inserted, code the author never wrote, or columns
    // it moved.
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    std::vector<std::string> tried;
    std::string refusal;
    bool adaptedCompiled = false;
    for (const MainPosition position : {MainPosition::Given, MainPosition::AsWritten, MainPosition::BeforeStructs}) {
        const std::string source = adaptPotPlayerShader(text, position);
        if (std::find(tried.begin(), tried.end(), source) != tried.end()) continue;
        tried.push_back(source);
        Microsoft::WRL::ComPtr<ID3DBlob> compiled;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), "PotPlayer-compatible pixel shader",
            nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &compiled, &errors);
        std::string why;
        std::vector<ShaderSignatureValue> inputs;
        if (FAILED(hr)) {
            if (errors) {
                why.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            } else {
                setError("Compile pixel shader file", hr);
                why = error_;
            }
        } else if (!signatureValues(compiled.Get(), true, inputs)) {
            why = "Could not read the pixel shader's inputs";
        } else {
            why = unfedShaderInput(quadOutputs(), inputs);
        }
        if (why.empty()) {
            blob = std::move(compiled);
            break;
        }
        if (position == MainPosition::Given) {
            adaptedCompiled = SUCCEEDED(hr);
            refusal = std::move(why);
        } else if (position == MainPosition::AsWritten && !adaptedCompiled) {
            refusal = std::move(why);
        }
    }
    if (!blob) {
        error_ = std::move(refusal);
        return false;
    }
    return installPixelShader(blob.Get(), "Create external pixel shader");
}

void D3DRenderer::bindVibranceColorSpace(std::size_t slot) {
    ID3D11Buffer* buffer = nullptr;
    if (smartVibranceSelected_ || pqAwarePreset_) {
        const auto& target = slots_[slot];
        // Under HDR output a 10-bit slot is the PQ one: 10-bit SDR output
        // is never on with it.
        const bool pq = hdrOutput_ && (target.hdrSource ||
            (target.hardware && target.outputFormat == DXGI_FORMAT_R10G10B10A2_UNORM));
        const float mode = !pq ? 0.0F :
            (target.rtxHdrApplied && !target.hdrSource ? 1.0F : 2.0F);
        // Grayscale and Invert read the display's SDR white beside it.
        const float values[] = {mode, std::clamp(enhancementStatus_.sdrWhiteNits, 80.0F, 1000.0F), 0.0F, 0.0F};
        context_->UpdateSubresource(vibranceColorBuffer_.Get(), 0, nullptr, values, 0, 0);
        buffer = vibranceColorBuffer_.Get();
    }
    // b0/b1 retain their existing contracts; custom shaders get no new b2.
    context_->PSSetConstantBuffers(2, 1, &buffer);
}

bool D3DRenderer::createBackBuffer() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        noteFailure("GetBuffer", hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
    if (FAILED(hr)) {
        noteFailure("CreateRenderTargetView", hr);
        return false;
    }
    // The interface layer follows the back buffer's size; it is rebuilt on
    // demand by the first PQ frame that needs it.
    overlayLayerView_.Reset();
    overlayLayer_.Reset();
    return true;
}

bool D3DRenderer::compositeOverlayLayer() {
    if (!overlayLayer_) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width_;
        description.Height = height_;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device_->CreateTexture2D(&description, nullptr, &overlayLayer_);
        if (FAILED(hr)) { setError("Create interface layer", hr); return false; }
        hr = device_->CreateShaderResourceView(overlayLayer_.Get(), nullptr, &overlayLayerView_);
        if (FAILED(hr)) { setError("Create interface layer view", hr); overlayLayer_.Reset(); return false; }
    }
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(overlayLayer_.As(&surface))) return false;
    overlay_(surface.Get(), true);
    // Direct2D leaves its own state behind; bind everything this pass needs.
    context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0F;
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, uvTransformBuffer_.GetAddressOf());
    const float identity[] = {1.0F, 1.0F, 0.0F, 0.0F};
    context_->UpdateSubresource(uvTransformBuffer_.Get(), 0, nullptr, identity, 0, 0);
    const float params[] = {enhancementStatus_.sdrWhiteNits, hdrOutput_ ? 0.0F : 1.0F, 0.0F, 0.0F};
    context_->UpdateSubresource(overlayParamsBuffer_.Get(), 0, nullptr, params, 0, 0);
    context_->PSSetShader(overlayCompositeShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, overlayParamsBuffer_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ID3D11ShaderResourceView* view = overlayLayerView_.Get();
    context_->PSSetShaderResources(0, 1, &view);
    context_->OMSetBlendState(overlayBlend_.Get(), nullptr, 0xFFFFFFFF);
    context_->Draw(4, 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    return true;
}

void D3DRenderer::resize(unsigned width, unsigned height) {
    if (!swapChain_ || width == 0 || height == 0) return;
    std::scoped_lock deviceLock(deviceMutex_);
    if (width == width_ && height == height_ && renderTarget_) {
        // Back to the size the buffers already have: nothing waits any more.
        pendingWidth_ = 0;
        pendingHeight_ = 0;
        return;
    }
    if (!resizeBackBuffer(width, height)) return;
    // A resize often means the window moved; the output it sits on may differ.
    refreshDisplayStatus();
    applyOutputMode();
}

bool D3DRenderer::resizeBackBuffer(unsigned width, unsigned height) {
    const auto retryLater = [this] {
        nextBackBufferRetry_ = std::chrono::steady_clock::now() + kBackBufferRetryInterval;
    };
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (FAILED(hr)) {
        noteFailure("ResizeBuffers", hr);
        const bool resized = width != width_ || height != height_;
        pendingWidth_ = resized ? width : 0;
        pendingHeight_ = resized ? height : 0;
        retryLater();
        // A refused size can leave the chain with its old buffers (a buffer
        // still referenced) or with none (a size it calls invalid), so it is
        // asked for the old size again, as applyOutputMode does, before the
        // target is made; width_ and height_ still describe that size.
        if (!deviceLost_) {
            swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
            createBackBuffer();
        }
        return false;
    }
    width_ = width;
    height_ = height;
    pendingWidth_ = 0;
    pendingHeight_ = 0;
    if (!createBackBuffer()) {
        retryLater();
        return false;
    }
    backBufferRetries_ = 0;
    return true;
}

void D3DRenderer::retryBackBuffer() {
    if ((renderTarget_ && !pendingWidth_) || !swapChain_ || deviceLost_) return;
    if (std::chrono::steady_clock::now() < nextBackBufferRetry_) return;
    const unsigned width = pendingWidth_ ? pendingWidth_ : width_;
    const unsigned height = pendingWidth_ ? pendingHeight_ : height_;
    const unsigned tries = ++backBufferRetries_;
    quietErrors_ = tries % kBackBufferRetriesPerLog != 0;
    const bool restored = resizeBackBuffer(width, height);
    quietErrors_ = false;
    if (!restored) return;
    appendDiagnostic("Back buffer: " + std::to_string(width) + "x" + std::to_string(height) +
                     " made after " + std::to_string(tries) + (tries == 1 ? " retry" : " retries"));
    refreshDisplayStatus();
    applyOutputMode();
}

bool D3DRenderer::createSlotResources(
    std::size_t slot, const HardwareVideoFrame& frame, const SurfaceSize& requested) {
    auto& target = slots_[slot];
    if (!videoDevice_ || !videoContext_) {
        error_ = "No D3D11 video processor on this device";
        return false;
    }
    D3D11_TEXTURE2D_DESC inputDescription{};
    frame.texture->GetDesc(&inputDescription);
    const int outputWidth = requested.width;
    const int outputHeight = requested.height;
    const auto color = videoColorDescription(frame.ownerFrame, frame.width, frame.height);
    const bool hdrSource = hdrOutput_ && color.hdr();
    // An HDR picture on an SDR output: the processor makes BT.2020 RGB of it,
    // its PQ or HLG codes kept, and the tone-map pass brings that down to
    // SDR. An HLG one on the PQ output goes the same way to be made PQ.
    const bool toneMapAsked = !hdrOutput_ && color.hdr() && videoContext1_ && toneMapShader_;
    const bool hlgToPq = hdrOutput_ && color.hlg && toneMapShader_;
    const bool fullRangeHdr = color.hdr() && color.fullRange;
    const bool deinterlace = videoOutput_.deinterlace != VideoDeinterlace::Off;
    const bool interlacedFrame = deinterlace && frame.ownerFrame &&
                                 (frame.ownerFrame->flags & AV_FRAME_FLAG_INTERLACED);
    // The processor stands a picture stored on its side or on its head
    // upright where it can: `requested` is then the upright picture's size.
    const int turns = processorTurns(frame);
    if (target.texture && target.inputWidth == frame.width && target.inputHeight == frame.height &&
        target.turned == turns &&
        target.inputFormat == inputDescription.Format && target.outputWidth == outputWidth &&
        target.outputHeight == outputHeight && target.hdrOutputMode == hdrOutput_ &&
        target.tenBitOutputMode == tenBitOutput_ && target.hdrSource == hdrSource &&
        target.toneMapAsked == toneMapAsked && target.hlg == color.hlg &&
        target.fullRangeHdr == fullRangeHdr &&
        (target.interlaced || !interlacedFrame)) {
        return true;
    }

    // A stream that has shown an interlaced frame keeps a deinterlacing
    // processor through a new size or output mode as well.
    const bool interlaced = interlacedFrame || (deinterlace && target.interlaced);
    const bool newlyInterlaced = interlaced && !target.interlaced;
    target = SlotTexture{};
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = interlaced ? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
                                          : D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = static_cast<UINT>(frame.width);
    content.InputHeight = static_cast<UINT>(frame.height);
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(outputWidth);
    content.OutputHeight = static_cast<UINT>(outputHeight);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &target.enumerator);
    if (FAILED(hr)) { setError("CreateVideoProcessorEnumerator", hr); return false; }
    UINT inputSupport{};
    hr = target.enumerator->CheckVideoProcessorFormat(inputDescription.Format, &inputSupport);
    if (FAILED(hr) || !(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        setError("D3D11 Video Processor input format unsupported", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    // The processor the driver lists first for deinterlacing that needs no
    // past or future frames: each frame is processed on its own here. When
    // none says so, its first, which does what it can with one frame.
    UINT rateConversion = 0;
    if (interlaced) {
        constexpr UINT deinterlacing = D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND |
            D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB |
            D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE |
            D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION;
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        if (SUCCEEDED(target.enumerator->GetVideoProcessorCaps(&caps))) {
            for (UINT index = 0; index < caps.RateConversionCapsCount; ++index) {
                D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rate{};
                if (FAILED(target.enumerator->GetVideoProcessorRateConversionCaps(index, &rate))) continue;
                if ((rate.ProcessorCaps & deinterlacing) && rate.PastFrames == 0 && rate.FutureFrames == 0) {
                    rateConversion = index;
                    break;
                }
            }
        }
    }
    hr = videoDevice_->CreateVideoProcessor(target.enumerator.Get(), rateConversion, &target.processor);
    if (FAILED(hr)) { setError("CreateVideoProcessor", hr); return false; }
    if (interlaced) {
        // Two pictures out for each frame in, one of each field, asked for
        // one at a time (convertSurface): the first for the first half of
        // the frame's time and the second for the rest. Until 2026-11-24
        // the rate was the half one, a single picture for the frame (its
        // first field, from the two drivers tried), and a 50i recording
        // moved 25 times a second.
        videoContext_->VideoProcessorSetStreamOutputRate(
            target.processor.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
        if (newlyInterlaced) {
            std::ostringstream out;
            out << "Deinterlacing pane " << (slot + 1) << ": " << frame.width << "x" << frame.height
                << ", processor " << rateConversion;
            appendDiagnostic(out.str());
        }
    }
    // D3D11's rotations are quarter turns clockwise, as quarterTurns counts
    // them. What the driver then draws into the output, such as RTX Video's
    // status indicator, is upright with the picture.
    if (turns != 0) {
        constexpr D3D11_VIDEO_PROCESSOR_ROTATION rotations[] = {
            D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY, D3D11_VIDEO_PROCESSOR_ROTATION_90,
            D3D11_VIDEO_PROCESSOR_ROTATION_180, D3D11_VIDEO_PROCESSOR_ROTATION_270};
        videoContext_->VideoProcessorSetStreamRotation(target.processor.Get(), 0, TRUE, rotations[turns]);
    }
    target.turned = turns;
    target.interlaced = interlaced;
    target.inputWidth = frame.width;
    target.inputHeight = frame.height;
    target.outputWidth = outputWidth;
    target.outputHeight = outputHeight;
    applyStreamExtensions(slot, hdrSource);
    // Some drivers accept the SDR/PQ colour-space pair but merely copy SDR
    // codes when TrueHDR is disabled/refused. Use a known SDR processor output
    // plus our explicit presentation transform in that case, for both decode
    // paths. Native PQ is still passed through without another SDR transform.
    const bool pqOutput = hdrOutput_ && (hdrSource || target.rtxHdrApplied);
    const auto outputSupported = [&](DXGI_FORMAT format, bool report) {
        UINT support{};
        const HRESULT checked = target.enumerator->CheckVideoProcessorFormat(format, &support);
        if (FAILED(checked) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            if (report) {
                setError("D3D11 Video Processor output format unsupported", FAILED(checked) ? checked : E_FAIL);
            }
            return false;
        }
        return true;
    };
    // The 10-bit SDR output keeps a 10-bit stream's depth to the back buffer
    // where the processor can write it; 8-bit where it cannot, which is no
    // error: the picture is whole either way.
    // A card whose processor does not turn PQ into PQ RGB leaves the HDR10
    // picture to the 709 approximation below, as before there was a pass.
    const bool toneMap = toneMapAsked && processorConvertsPq(target.enumerator.Get(), inputDescription.Format);
    if (toneMapAsked) {
        const int outcome = (toneMap ? 1 : 0) + (color.hlg ? 2 : 0);
        if (outcome != loggedToneMap_) {
            loggedToneMap_ = outcome;
            appendDiagnostic(std::string(color.hlg ? "HLG" : "HDR10") +
                (toneMap ? " on an SDR output: tone-mapped to SDR"
                         : " on an SDR output: this card's video processor does not convert PQ; "
                           "shown through the BT.709 approximation"));
        }
    }
    // A full-range HDR surface is undone as full range by the processor's
    // BT.2020 pair where it has one, else by its BT.709 pair with the matrix
    // mended in the pass after it.
    bool rematrix = false;
    if (pqOutput || toneMap) {
        target.chromaTopLeftInput = processorConverts(target.enumerator.Get(), inputDescription.Format,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (fullRangeHdr) {
            target.fullRangeHdrInput = processorConverts(target.enumerator.Get(), inputDescription.Format,
                DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020);
            rematrix = !target.fullRangeHdrInput && toneMapShader_ &&
                processorConverts(target.enumerator.Get(), inputDescription.Format,
                    DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) &&
                measureFullRange709(target, inputDescription.Format);
        }
    }
    // The pass after the processor: to SDR, an HLG picture to PQ, or that
    // matrix.
    const bool transferPass = toneMap || hlgToPq || rematrix;
    // With Super Resolution running, NVIDIA's driver writes a 10-bit RGB
    // output as PQ, the SDR picture at the display's SDR white, whatever
    // colour space the processor is told; the SDR chain then shows those
    // codes as a flat grey picture. Its 8-bit output stays SDR. Measured on
    // an RTX 4060 Ti with Windows HDR on (2026-11-26), legacy and DXGI colour
    // spaces alike.
    const bool tenBitSdr = tenBitOutput_ && !target.superResolutionApplied;
    if (tenBitOutput_ && target.superResolutionApplied && !superResolutionTenBitLogged_) {
        superResolutionTenBitLogged_ = true;
        appendDiagnostic("RTX Super Resolution on the 10-bit SDR output: its pane's picture is 8-bit, "
                         "the driver writes 10-bit as PQ");
    }
    const bool canFallBack = !pqOutput && !toneMap && tenBitSdr;
    DXGI_FORMAT outputFormat = pqOutput || toneMap || tenBitSdr ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!outputSupported(outputFormat, !canFallBack)) {
        if (!canFallBack) return false;
        outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!outputSupported(outputFormat, true)) return false;
    }

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputDescription.Width = static_cast<UINT>(outputWidth);
    outputDescription.Height = static_cast<UINT>(outputHeight);
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = outputFormat;
    outputDescription.SampleDesc.Count = 1;
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // The processor writes the picture every later pass reads, or, to be
    // tone-mapped or made PQ, the picture that pass reads.
    auto& processed = transferPass ? target.source : target.texture;
    auto& processedView = transferPass ? target.sourceView : target.view;
    hr = device_->CreateTexture2D(&outputDescription, nullptr, &processed);
    if (FAILED(hr)) { setError("Create zero-copy output texture", hr); return false; }
    hr = device_->CreateShaderResourceView(processed.Get(), nullptr, &processedView);
    if (FAILED(hr)) { setError("Create output shader view", hr); return false; }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDescription{};
    outputViewDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDescription.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(
        processed.Get(), target.enumerator.Get(), &outputViewDescription, &target.outputView);
    if (FAILED(hr)) { setError("CreateVideoProcessorOutputView", hr); return false; }
    if (transferPass && !createToneMapTarget(slot, outputWidth, outputHeight)) {
        // Without its picture the slot is not one: the next frame builds it again.
        target.texture.Reset();
        return false;
    }
    target.inputFormat = inputDescription.Format;
    target.outputFormat = outputFormat;
    target.pqOutput = pqOutput;
    target.hdrSource = hdrSource;
    target.hdrOutputMode = hdrOutput_;
    target.tenBitOutputMode = tenBitOutput_;
    target.toneMapAsked = toneMapAsked;
    target.toneMapped = toneMap;
    target.hlg = color.hlg;
    target.hlgToPq = hlgToPq;
    target.fullRangeHdr = fullRangeHdr;
    target.rematrix = rematrix;
    return true;
}

bool D3DRenderer::processorConvertsPq(ID3D11VideoProcessorEnumerator* enumerator, DXGI_FORMAT input) {
    return processorConverts(enumerator, input, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
                             DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
}

bool D3DRenderer::processorConverts(ID3D11VideoProcessorEnumerator* enumerator, DXGI_FORMAT input,
                                    DXGI_COLOR_SPACE_TYPE inputSpace, DXGI_COLOR_SPACE_TYPE outputSpace) {
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator1> enumerator1;
    if (!enumerator || FAILED(enumerator->QueryInterface(IID_PPV_ARGS(&enumerator1)))) return false;
    BOOL supported = FALSE;
    return SUCCEEDED(enumerator1->CheckVideoProcessorFormatConversion(
               input, inputSpace, DXGI_FORMAT_R10G10B10A2_UNORM, outputSpace, &supported)) &&
           supported;
}

bool D3DRenderer::measureFullRange709(SlotTexture& target, DXGI_FORMAT format) {
    if (!videoContext1_ || (format != DXGI_FORMAT_NV12 && format != DXGI_FORMAT_P010)) return false;
    const bool deep = format == DXGI_FORMAT_P010;
    auto& known = full709_[deep ? 1 : 0];
    if (known.measured) {
        target.rematrixOffset = known.offset;
        return known.usable;
    }
    known.measured = true;
    // A ramp of neutral greys, one a column: each rounds differently into
    // the 10-bit result, so their mean stands well inside one step of it.
    constexpr int columns = 64, rows = 16;
    const int sample = deep ? 2 : 1;
    const auto grey = [&](int column) { return deep ? 384 + column * 4 : 96 + column; };
    std::vector<std::uint8_t> planes(static_cast<std::size_t>(columns) * sample * rows * 3 / 2);
    const auto put = [&](int row, int index, int value) {
        auto* at = planes.data() + (static_cast<std::size_t>(row) * columns + index) * sample;
        if (deep) {
            const auto word = static_cast<std::uint16_t>(value << 6);
            std::memcpy(at, &word, sizeof(word));
        } else {
            *at = static_cast<std::uint8_t>(value);
        }
    };
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) put(row, column, grey(column));
    for (int row = rows; row < rows * 3 / 2; ++row)
        for (int index = 0; index < columns; ++index) put(row, index, deep ? 512 : 128);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = columns;
    description.Height = rows;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = format;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = planes.data();
    data.SysMemPitch = static_cast<UINT>(columns * sample);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input, output, staging;
    if (FAILED(device_->CreateTexture2D(&description, &data, &input))) return false;
    description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &output))) return false;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &staging))) return false;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
    inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(
            input.Get(), target.enumerator.Get(), &inputDescription, &inputView))) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDescription{};
    outputDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(
            output.Get(), target.enumerator.Get(), &outputDescription, &outputView))) return false;
    // Everything set here is set again for each frame (convertSurface).
    ID3D11VideoProcessor* processor = target.processor.Get();
    videoContext1_->VideoProcessorSetStreamColorSpace1(processor, 0, DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
    videoContext1_->VideoProcessorSetOutputColorSpace1(processor, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    const RECT whole{0, 0, columns, rows};
    videoContext_->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &whole);
    videoContext_->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &whole);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    if (FAILED(videoContext_->VideoProcessorBlt(processor, outputView.Get(), 0, 1, &stream))) return false;
    context_->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::array<double, 3> sum{};
    const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
                      static_cast<std::size_t>(rows / 2) * mapped.RowPitch;
    for (int column = 0; column < columns; ++column) {
        std::uint32_t packed{};
        std::memcpy(&packed, row + column * 4, sizeof(packed));
        const double expected = deep ? grey(column) / 1023.0 : grey(column) / 255.0;
        for (int channel = 0; channel < 3; ++channel) {
            sum[channel] += ((packed >> (10 * channel)) & 1023U) / 1023.0 - expected;
        }
    }
    context_->Unmap(staging.Get(), 0);
    known.usable = true;
    for (int channel = 0; channel < 3; ++channel) {
        known.offset[channel] = static_cast<float>(sum[channel] / columns);
        // More than this is not a rounding of constants but another
        // conversion altogether, which an offset does not mend.
        if (std::abs(known.offset[channel]) > 0.02F) known.usable = false;
    }
    target.rematrixOffset = known.offset;
    return known.usable;
}

bool D3DRenderer::createToneMapTarget(std::size_t slot, int width, int height) {
    auto& target = slots_[slot];
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    // Deep enough that the roll-off's gradients do not band in a 10-bit output.
    description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&description, nullptr, &target.texture);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(target.texture.Get(), nullptr, &target.view);
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(target.texture.Get(), nullptr, &target.toneMapTarget);
    if (FAILED(hr)) {
        setError("Create tone-map texture", hr);
        target.toneMapTarget.Reset();
        target.view.Reset();
        target.texture.Reset();
        return false;
    }
    return true;
}

bool D3DRenderer::runToneMapPass(std::size_t slot, float peakNits) {
    auto& target = slots_[slot];
    if (!target.sourceView || !target.toneMapTarget || !toneMapShader_) {
        error_ = "No tone-map pass for this picture";
        return false;
    }
    beginFullscreenPass(target.outputWidth, target.outputHeight);
    // The pass writes PQ where the slot is not brought down to SDR: an HLG
    // picture made PQ, or a full-range one whose matrix is mended.
    const bool toPq = target.hlgToPq || (target.rematrix && !target.toneMapped);
    const float params[] = {std::clamp(peakNits, kHdrReferenceWhiteNits, 10000.0F), kHdrReferenceWhiteNits,
                            target.hlg ? 1.0F : 0.0F, toPq ? 1.0F : 0.0F,
                            target.rematrix ? 1.0F : 0.0F, target.rematrixOffset[0], target.rematrixOffset[1],
                            target.rematrixOffset[2]};
    context_->UpdateSubresource(toneMapParams_.Get(), 0, nullptr, params, 0, 0);
    context_->PSSetConstantBuffers(0, 1, toneMapParams_.GetAddressOf());
    context_->PSSetShader(toneMapShader_.Get(), nullptr, 0);
    context_->OMSetRenderTargets(1, target.toneMapTarget.GetAddressOf(), nullptr);
    context_->PSSetShaderResources(0, 1, target.sourceView.GetAddressOf());
    context_->Draw(4, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

bool D3DRenderer::convertSurface(
    std::size_t slot, const HardwareVideoFrame& frame, const SurfaceSize& requested) {
    auto& target = slots_[slot];
    if (!createSlotResources(slot, frame, requested)) return false;
    // Metadata belongs to each decoded frame, not the cached texture geometry.
    const auto color = videoColorDescription(frame.ownerFrame, frame.width, frame.height);
    if (target.pqOutput || target.toneMapped) {
        // The PQ output mode names the spaces by DXGI type: an HDR10 stream is
        // handed over as PQ BT.2020 and passed through, anything else as the
        // SDR space it is and left to RTX Video HDR to expand. A BT.2020 SDR
        // stream is the one case the 8-bit path approximates that this one
        // converts properly. A slot to be tone-mapped is an HDR10 stream
        // made PQ RGB the same way, on an SDR output.
        // An HLG stream is handed over as PQ too: with the same transfer
        // named on both sides the processor leaves the codes' curve alone
        // and only undoes the matrix, and the pass after it reads HLG. A
        // full-range HDR stream has no DXGI type of its own; it is named
        // gamma 2.2 on both sides for the same reason, where the processor
        // takes that. Where it does not, the full-range BT.709 pair undoes
        // the range and the pass after it mends the matrix (`rematrix`).
        // With neither it goes as limited range, too contrasty, which the
        // log says once.
        DXGI_COLOR_SPACE_TYPE inputSpace;
        DXGI_COLOR_SPACE_TYPE outputSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if (color.hdr() && color.fullRange && target.fullRangeHdrInput) {
            inputSpace = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020;
            outputSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020;
        } else if (color.hdr() && color.fullRange && target.rematrix) {
            inputSpace = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
            outputSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            if (!fullRangeHdrLogged_) {
                fullRangeHdrLogged_ = true;
                std::ostringstream out;
                out << "Full-range HDR video: undone with the video processor's full-range BT.709 pair, its "
                       "matrix mended afterwards; measured offsets " << target.rematrixOffset[0] << ' '
                    << target.rematrixOffset[1] << ' ' << target.rematrixOffset[2];
                appendDiagnostic(out.str());
            }
        } else if (color.hdr()) {
            inputSpace = color.chromaTopLeft && target.chromaTopLeftInput
                ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
            if (color.fullRange && !fullRangeHdrLogged_) {
                fullRangeHdrLogged_ = true;
                appendDiagnostic("Full-range HDR video: this card's video processor takes it only as limited "
                                 "range; black and white are a little off");
            }
        } else if (color.bt2020) {
            inputSpace = color.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        } else if (color.matrix == VideoColorMatrix::Bt709) {
            inputSpace = color.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        } else {
            inputSpace = color.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        }
        videoContext1_->VideoProcessorSetStreamColorSpace1(target.processor.Get(), 0, inputSpace);
        videoContext1_->VideoProcessorSetOutputColorSpace1(target.processor.Get(), outputSpace);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
        inputColor.YCbCr_Matrix = color.matrix == VideoColorMatrix::Bt709 ? 1 : 0;
        inputColor.RGB_Range = color.fullRange ? 0 : 1;
        inputColor.Nominal_Range = color.fullRange
            ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
            : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
        outputColor.RGB_Range = 0;
        outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        videoContext_->VideoProcessorSetStreamColorSpace(target.processor.Get(), 0, &inputColor);
        videoContext_->VideoProcessorSetOutputColorSpace(target.processor.Get(), &outputColor);
    }
    // Keep driver automatic enhancements independent of the selected shader
    // from silently changing the matrix/range conversion's output pixels.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(target.processor.Get(), 0, FALSE);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
    inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDescription.Texture2D.MipSlice = 0;
    inputDescription.Texture2D.ArraySlice = frame.arraySlice;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        frame.texture, target.enumerator.Get(), &inputDescription, &inputView);
    if (FAILED(hr)) { setError("CreateVideoProcessorInputView", hr); return false; }
    RECT source{0, 0, frame.width, frame.height};
    RECT destination{0, 0, target.outputWidth, target.outputHeight};
    // Each frame says for itself whether it is interlaced and which field
    // comes first; a progressive one among them passes through untouched.
    D3D11_VIDEO_FRAME_FORMAT frameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    if (target.interlaced && frame.ownerFrame && (frame.ownerFrame->flags & AV_FRAME_FLAG_INTERLACED)) {
        frameFormat = (frame.ownerFrame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)
            ? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
            : D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
    }
    videoContext_->VideoProcessorSetStreamFrameFormat(target.processor.Get(), 0, frameFormat);
    videoContext_->VideoProcessorSetStreamSourceRect(target.processor.Get(), 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(target.processor.Get(), 0, TRUE, &destination);
    // Which of an interlaced frame's two pictures: at the normal rate the
    // stream's output index says so, 0 the field that comes first and 1 the
    // other. A processor built to deinterlace also numbers its frames and
    // its pictures as they come, and is asked for each picture of an
    // interlaced frame twice: NVIDIA's (RTX 4060 Ti, driver 32.0.16.2135)
    // hands back at this rate the picture it was asked for the time before,
    // so one asking showed every field one late and the frame after a
    // paused seek not at all; asked again, it hands back the one asked for,
    // moving or still. Intel's (Arc, 32.0.101.9175) hands back the same
    // picture both times. A progressive frame is one late on neither.
    const bool interlacedFrame = frameFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    const bool secondField = interlacedFrame && secondFields_[slot];
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    if (target.interlaced) {
        if (frame.serial != target.serial) ++target.inputFrames;
        stream.InputFrameOrField = target.inputFrames;
        stream.OutputIndex = secondField ? 1U : 0U;
    }
    for (int ask = 0; ask < (interlacedFrame ? 2 : 1); ++ask) {
        hr = videoContext_->VideoProcessorBlt(
            target.processor.Get(), target.outputView.Get(),
            target.interlaced ? target.outputPictures++ : 0U, 1, &stream);
        if (FAILED(hr)) { noteFailure("VideoProcessorBlt", hr); return false; }
    }
    if ((target.toneMapped || target.hlgToPq || target.rematrix) &&
        !runToneMapPass(slot, hdrPicturePeakNits(frame.ownerFrame))) return false;
    target.secondField = secondField;
    target.serial = frame.serial;
    target.hardware = true;
    return true;
}

bool D3DRenderer::secondFieldWanted(std::size_t slot, const HardwareVideoFrame& frame) const {
    // As convertSurface names the frame's format: only in a slot built to
    // deinterlace, and only of a frame that says it is interlaced.
    return secondFields_[slot] && slots_[slot].interlaced && frame.ownerFrame &&
           (frame.ownerFrame->flags & AV_FRAME_FLAG_INTERLACED);
}

bool D3DRenderer::convertSoftwareSurface(
    std::size_t slot, const HardwareVideoFrame& frame, const SurfaceSize& requested) {
    if (!videoDevice_ || !videoContext_ || !frame.ownerFrame) return false;
    // An 8-bit SDR frame goes in as NV12; an HDR one deeper than that as
    // P010, which keeps ten of its bits where its BGRA copy keeps eight.
    const bool deep = softwareP010Eligible(*frame.ownerFrame);
    if (!deep && !softwareNv12Eligible(*frame.ownerFrame)) return false;
    const DXGI_FORMAT format = deep ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    auto& upload = softwareUploads_[slot];
    const auto& source = *frame.ownerFrame;
    if (upload.width != source.width || upload.height != source.height || upload.format != format) {
        const bool interlaced = upload.interlaced;
        upload = {};
        upload.width = source.width;
        upload.height = source.height;
        upload.format = format;
        upload.interlaced = interlaced;
    }
    // Retry after a size/output/settings/device change, not on every video
    // frame when a driver rejects a particular processing configuration.
    if (upload.failed && upload.failedOutputWidth == requested.width &&
        upload.failedOutputHeight == requested.height) return false;
    upload.failed = false;
    const auto fail = [&] {
        upload.failed = true;
        upload.failedOutputWidth = requested.width;
        upload.failedOutputHeight = requested.height;
        appendDiagnostic(std::string("Software ") + (deep ? "P010" : "NV12") + " video processor fallback: pane=" +
            std::to_string(slot + 1) + " input=" + std::to_string(source.width) + "x" +
            std::to_string(source.height) + " output=" + std::to_string(requested.width) + "x" +
            std::to_string(requested.height) + " reason=" + error_);
        return false;
    };
    if (!upload.texture) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(source.width);
        description.Height = static_cast<UINT>(source.height);
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = format;
        description.Usage = D3D11_USAGE_DEFAULT;
        // This is a processor input, independent of the codec's decoder caps.
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device_->CreateTexture2D(&description, nullptr, &upload.texture);
        if (FAILED(hr)) { setError(deep ? "Create software P010 input" : "Create software NV12 input", hr); return fail(); }
    }
    if (upload.serial != frame.serial) {
        int stride{};
        if (!(deep ? packSoftwareP010(source, upload.bytes, stride)
                   : packSoftwareNv12(source, upload.bytes, stride))) return false;
        context_->UpdateSubresource(upload.texture.Get(), 0, nullptr,
                                    upload.bytes.data(), static_cast<UINT>(stride), 0);
        ++stats_.softwareNv12Uploads;
        upload.serial = frame.serial;
    }
    HardwareVideoFrame uploaded;
    uploaded.ownerFrame = av_frame_clone(frame.ownerFrame);
    if (!uploaded.ownerFrame) { error_ = "Cannot retain software colour metadata"; return fail(); }
    uploaded.texture = upload.texture.Get();
    uploaded.width = source.width;
    uploaded.height = source.height;
    uploaded.quarterTurns = frame.quarterTurns;
    uploaded.serial = frame.serial;
    if (!convertSurface(slot, uploaded, requested)) return fail();
    // A processor that does not hand an HDR surface over as PQ would leave
    // it to the SDR approximation; its BGRA copy is tone-mapped instead.
    if (deep && !hdrOutput_ && !slots_[slot].toneMapped) {
        error_ = "the video processor does not convert PQ";
        return fail();
    }
    // Nor is a full-range one left to a processor that reads it as limited
    // range: swscale undoes the range of its BGRA copy.
    if (deep && slots_[slot].fullRangeHdr && !slots_[slot].fullRangeHdrInput && !slots_[slot].rematrix) {
        error_ = "the video processor does not take full-range HDR";
        return fail();
    }
    // Slot.hardware means a video-processor output; the public frame still
    // accurately says software decode. Metadata, range and colour stay intact.
    return true;
}

bool D3DRenderer::uploadSoftwareFrame(std::size_t slot, const HardwareVideoFrame& frame) {
    auto& target = slots_[slot];
    // swscale only changes matrix/range; it does not turn PQ/HLG into SDR.
    // An HDR frame's BGRA is its PQ or HLG BT.2020 codes: on an SDR output
    // the tone-map pass brings them down; on the PQ output PQ codes are
    // shown as they are and HLG ones are made PQ by the same pass.
    const auto color = videoColorDescription(frame.ownerFrame, frame.width, frame.height);
    const bool toneMap = color.hdr() && !hdrOutput_ && toneMapShader_;
    const bool hlgToPq = color.hlg && hdrOutput_ && toneMapShader_;
    const bool transferPass = toneMap || hlgToPq;
    if (target.texture && !target.hardware && target.serial == frame.serial &&
        target.toneMapped == toneMap && target.hlgToPq == hlgToPq && target.hlg == color.hlg &&
        target.outputWidth == frame.width && target.outputHeight == frame.height) return true;
    // UpdateSubresource reads stride x height bytes. A frame without that
    // many, or with no BGRA copy at all, must fail here rather than have
    // the runtime read past its end on the window thread.
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4 ||
        frame.bgra.size() < static_cast<std::size_t>(frame.stride) * static_cast<std::size_t>(frame.height)) {
        error_ = "Software frame has no BGRA pixels to upload";
        return false;
    }
    if (!target.texture || target.hardware || target.toneMapped != toneMap ||
        target.hlgToPq != hlgToPq || target.hlg != color.hlg ||
        target.outputWidth != frame.width || target.outputHeight != frame.height) {
        target = SlotTexture{};
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(frame.width);
        description.Height = static_cast<UINT>(frame.height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // The upload is the picture every later pass reads, or, to be
        // tone-mapped or made PQ, the picture that pass reads.
        auto& uploaded = transferPass ? target.source : target.texture;
        auto& uploadedView = transferPass ? target.sourceView : target.view;
        HRESULT hr = device_->CreateTexture2D(&description, nullptr, &uploaded);
        if (FAILED(hr)) { setError("Create software frame texture", hr); return false; }
        hr = device_->CreateShaderResourceView(uploaded.Get(), nullptr, &uploadedView);
        if (FAILED(hr)) { setError("Create software frame view", hr); return false; }
        if (transferPass && !createToneMapTarget(slot, frame.width, frame.height)) return false;
        target.outputWidth = frame.width;
        target.outputHeight = frame.height;
        target.outputFormat = transferPass ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        target.toneMapped = toneMap;
        target.hlgToPq = hlgToPq;
        target.hlg = color.hlg;
        target.hardware = false;
    }
    context_->UpdateSubresource((transferPass ? target.source : target.texture).Get(), 0, nullptr,
                                frame.bgra.data(), frame.stride, 0);
    ++stats_.softwareFrameUploads;
    // Keep the HDR-coded pictures that are shown as they are out of the SDR
    // transform and SDR Vibrance; a tone-mapped one is SDR from here on.
    target.hdrSource = !toneMap && color.hdr();
    if (transferPass && !runToneMapPass(slot, hdrPicturePeakNits(frame.ownerFrame))) return false;
    target.serial = frame.serial;
    return true;
}

bool D3DRenderer::prepareSoftwarePqTexture(std::size_t slot) {
    const auto& source = slots_[slot];
    auto& target = softwarePq_[slot];
    if (!source.view || source.outputFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        source.hdrSource || !hdrOutput_) return false;
    if (target.width != source.outputWidth || target.height != source.outputHeight) target = {};
    if (!target.pq) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(source.outputWidth);
        desc.Height = static_cast<UINT>(source.outputHeight);
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const auto create = [&](auto& texture, auto& view, auto& rtv) {
            HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
            if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
            if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(texture.Get(), nullptr, &rtv);
            if (FAILED(hr)) setError("Create software PQ presentation texture", hr);
            return SUCCEEDED(hr);
        };
        if (!create(target.effects, target.effectsView, target.effectsTarget) ||
            !create(target.pq, target.pqView, target.pqTarget)) { target = {}; return false; }
        target.width = source.outputWidth;
        target.height = source.outputHeight;
    }
    beginFullscreenPass(target.width, target.height);
    // Run every selected effect, including user shaders, in SDR before the
    // output transform. Per-frame execution preserves temporal shader inputs.
    drawEffectShader(slot, target.effectsTarget.Get(), target.width, target.height);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->OMSetRenderTargets(1, target.pqTarget.GetAddressOf(), nullptr);
    context_->PSSetShaderResources(0, 1, target.effectsView.GetAddressOf());
    const float white[] = {std::clamp(enhancementStatus_.sdrWhiteNits, 80.0F, 1000.0F), 0, 0, 0};
    context_->UpdateSubresource(sdrToPqParams_.Get(), 0, nullptr, white, 0, 0);
    context_->PSSetConstantBuffers(0, 1, sdrToPqParams_.GetAddressOf());
    context_->PSSetShader(sdrToPqShader_.Get(), nullptr, 0);
    context_->Draw(4, 0);
    context_->PSSetShaderResources(0, 1, &empty);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

bool D3DRenderer::runEffectsPass(std::size_t slot) {
    const auto& source = slots_[slot];
    auto& post = postTextures_[slot];
    if (!source.view) return false;
    if (post.width != source.outputWidth || post.height != source.outputHeight) post = {};
    if (!post.texture) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(source.outputWidth);
        desc.Height = static_cast<UINT>(source.outputHeight);
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        // Deep enough for a 10-bit or a PQ picture.
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &post.texture);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(post.texture.Get(), nullptr, &post.view);
        if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(post.texture.Get(), nullptr, &post.target);
        if (FAILED(hr)) {
            setError("Create output pass texture", hr);
            post = {};
            return false;
        }
        post.width = source.outputWidth;
        post.height = source.outputHeight;
    }
    beginFullscreenPass(post.width, post.height);
    drawEffectShader(slot, post.target.Get(), post.width, post.height);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void D3DRenderer::beginFullscreenPass(int width, int height) {
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, uvTransformBuffer_.GetAddressOf());
    const float identity[] = {1, 1, 0, 0};
    context_->UpdateSubresource(uvTransformBuffer_.Get(), 0, nullptr, identity, 0, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1;
    context_->RSSetViewports(1, &viewport);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
}

void D3DRenderer::drawEffectShader(std::size_t slot, ID3D11RenderTargetView* target, int width, int height) {
    const auto& source = slots_[slot];
    const float across = static_cast<float>(width);
    const float down = static_cast<float>(height);
    const float clock = static_cast<float>(std::fmod(std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count(), 3600.0));
    const float params[] = {across, down, static_cast<float>(source.serial), clock, 1 / across, 1 / down,
                            0, 0, 0, 0, 0, 0};
    context_->UpdateSubresource(pixelParamsBuffer_.Get(), 0, nullptr, params, 0, 0);
    context_->PSSetConstantBuffers(0, 1, pixelParamsBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(1, 1, smartVibranceBuffer_.GetAddressOf());
    bindVibranceColorSpace(slot);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->PSSetShaderResources(0, 1, source.view.GetAddressOf());
    context_->Draw(4, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
}

void D3DRenderer::drawOutputPass(ID3D11ShaderResourceView* view, int width, int height,
                                 VideoScaling scaling, bool color) {
    const auto params = videoColorParams(videoOutput_);
    const float across = static_cast<float>(std::max(1, width));
    const float down = static_cast<float>(std::max(1, height));
    const bool controls = color && videoOutput_.color != VideoColorControls{};
    const float values[] = {
        across, down, 1.0F / across, 1.0F / down,
        static_cast<float>(scaling), controls ? 1.0F : 0.0F, color && params.limitedRange ? 1.0F : 0.0F, 0.0F,
        params.brightness, params.contrast, params.saturation, params.exponent,
        params.hueCos, params.hueSin, 0.0F, 0.0F};
    context_->UpdateSubresource(outputParamsBuffer_.Get(), 0, nullptr, values, 0, 0);
    context_->PSSetShader(outputShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, outputParamsBuffer_.GetAddressOf());
    ID3D11SamplerState* samplers[] = {sampler_.Get(), pointSampler_.Get()};
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetShaderResources(0, 1, &view);
    context_->Draw(4, 0);
}

void D3DRenderer::render(
    const PaneArray<std::shared_ptr<const HardwareVideoFrame>>& frames,
    const PaneArray<PaneView>& views,
    const PaneArray<bool>& active, LayoutMode layout,
    int expandedPane, int soloPane, unsigned contentHeight,
    const PaneArray<float>& aspectRatios,
    AutoLayoutFocus autoFocus, int autoFocusPane, const PaneArray<bool>& secondFields) {
    // The bitblt chain's next frame is at the next refresh, whatever this
    // one does; without a timer set, the main loop goes on its timeout.
    armPaceTimer();
    // A missing render target is not a reason to stop here: a failure that
    // left it so is tried again below.
    if (!swapChain_ || width_ == 0 || height_ == 0 || deviceLost_) {
        return;
    }
    using PerfClock = std::chrono::steady_clock;
    const auto lockRequested = PerfClock::now();
    std::unique_lock deviceLock(deviceMutex_);
    const auto lockAcquired = PerfClock::now();
    secondFields_ = secondFields;
    retryBackBuffer();
    if (!renderTarget_ || deviceLost_) return;
    if (!frameFence_ && !frameFenceUnavailable_ && device_) {
        // Fences are D3D11.4: Windows 10 1703 with a WDDM 2.2 driver.
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        if (!frameFenceEvent_) frameFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (frameFenceEvent_ && SUCCEEDED(device_.As(&device5)) &&
            SUCCEEDED(context_.As(&fenceContext_)) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence_)))) {
            frameFenceValue_ = 0;
            appendDiagnostic("GPU frame wait: fence");
        } else {
            frameFence_.Reset();
            fenceContext_.Reset();
            frameFenceUnavailable_ = true;
        }
    }
    if (!frameFence_ && !frameQuery_ && device_) {
        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        if (FAILED(device_->CreateQuery(&query, &frameQuery_))) frameQuery_.Reset();
        // Windows 10 1803 and later; before it the poll sleeps a tick.
        if (!frameQueryTimer_) {
            frameQueryTimer_ = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }
        if (frameQuery_) {
            appendDiagnostic(frameQueryTimer_ ? "GPU frame wait: event query, high-resolution timer"
                                              : "GPU frame wait: event query, sleep");
        }
    }
    double convertMs = 0.0;
    const float videoHeight = static_cast<float>(std::min(height_, std::max(1U, contentHeight)));
    const auto cells = activeLayoutCells(
        static_cast<float>(width_), videoHeight, active, layout, expandedPane, soloPane,
        aspectRatios, autoFocus, autoFocusPane);
    // An HDR picture (HDR10 or HLG) about to be drawn asks for the PQ
    // output, and the last one gone gives it back. The swap chain is told
    // the metadata of the brightest HDR10 one; an HLG picture is made PQ
    // for a display of its nominal peak and has none to tell.
    bool hdrVideo = false;
    HdrStaticMetadata metadata;
    float metadataPeak = 0.0F;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        if (!frame || !frame->ownerFrame || cells[index].width <= 0 || cells[index].height <= 0) continue;
        const auto transfer = frame->ownerFrame->color_trc;
        if (transfer != AVCOL_TRC_SMPTE2084 && transfer != AVCOL_TRC_ARIB_STD_B67) continue;
        hdrVideo = true;
        if (transfer != AVCOL_TRC_SMPTE2084) continue;
        const auto said = hdrStaticMetadata(frame->ownerFrame);
        const float peak = hdrSourcePeakNits(frame->ownerFrame);
        if (said.present && peak > metadataPeak) {
            metadata = said;
            metadataPeak = peak;
        }
    }
    const bool hdrVideoChanged = noteHdrVideo(hdrVideo);
    // HDR can be switched in Windows, or the window carried to another
    // display, while playing; look every couple of seconds.
    if (++framesSinceDisplayStatus_ >= kDisplayStatusFrames) {
        refreshDisplayStatus();
        applyOutputMode();
        if (!renderTarget_) return;
    } else if (hdrVideoChanged) {
        applyOutputMode();
        if (!renderTarget_) return;
    }
    // While the output is only held for a picture that is not there this
    // frame (a seek, the next episode), what was said for it stands.
    if (hdrVideo || !hdrVideoShown_) applyHdrMetadata(hdrOutput_ ? metadata : HdrStaticMetadata{});
    HdrPicture hdrPicture = HdrPicture::None;
    // Only the hardware path goes through the video processor, so only it
    // can hand Super Resolution the upscale.
    const bool magnifyForSuperResolution = enhancements_.superResolution &&
                                           enhancementStatus_.superResolutionSupported;
    constexpr float background[] = {0.012F, 0.014F, 0.020F, 1.0F};
    context_->ClearRenderTargetView(renderTarget_.Get(), background);
    context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, uvTransformBuffer_.GetAddressOf());
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, pixelParamsBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(1, 1, smartVibranceBuffer_.GetAddressOf());

    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        if (!frame || cells[index].width <= 0 || cells[index].height <= 0) {
            continue;
        }
        const float paneZoom = std::clamp(views[index].zoom, 1.0F, 4.0F);
        // Only the video processor deinterlaces, so an interlaced software
        // frame goes to it as NV12, and the rest of its stream after it.
        if (!frame->hardware && frame->ownerFrame && (frame->ownerFrame->flags & AV_FRAME_FLAG_INTERLACED)) {
            softwareUploads_[index].interlaced = true;
        }
        const bool deinterlaceSoftware = videoOutput_.deinterlace != VideoDeinterlace::Off &&
                                         videoDevice_ && softwareUploads_[index].interlaced;
        // An HDR frame deeper than 8 bits always goes through the processor,
        // which keeps its depth; an 8-bit SDR one when something needs it.
        const bool softwareVideoProcessor = !frame->hardware && frame->ownerFrame && videoDevice_ &&
            (softwareP010Eligible(*frame->ownerFrame) ||
             ((hdrOutput_ || enhancements_.superResolution || deinterlaceSoftware) &&
              softwareNv12Eligible(*frame->ownerFrame)));
        const int sourceWidth = softwareVideoProcessor ? frame->ownerFrame->width : frame->width;
        const int sourceHeight = softwareVideoProcessor ? frame->ownerFrame->height : frame->height;
        // A picture stored on its side or on its head is stood upright by
        // the video processor where it goes through one that can: its
        // output is then the upright picture, and what the driver draws
        // into it, RTX Video's status indicator, stands upright too. Any
        // other picture stays as it is stored in every texture, sized for
        // the cell the other way round when it is on its side, and the
        // pane's own draw turns it.
        const int quarterTurns = frame->quarterTurns & 3;
        const bool throughProcessor = frame->hardware || softwareVideoProcessor;
        const int processorTurned = throughProcessor ? processorTurns(*frame) : 0;
        const bool storedSideways = turnedSideways(quarterTurns);
        const bool uprightOutput = storedSideways && turnedSideways(processorTurned);
        const auto requested = presentationSize(
            uprightOutput ? sourceHeight : sourceWidth, uprightOutput ? sourceWidth : sourceHeight,
            (storedSideways && !uprightOutput ? cells[index].height : cells[index].width) * paneZoom,
            (storedSideways && !uprightOutput ? cells[index].width : cells[index].height) * paneZoom,
            3840, 8, magnifyForSuperResolution && throughProcessor);
        // A frame's second field is another picture of the same serial.
        if (slots_[index].serial != frame->serial ||
            slots_[index].secondField != secondFieldWanted(index, *frame) ||
            ((frame->hardware || softwareVideoProcessor) && (slots_[index].outputWidth != requested.width ||
                                 slots_[index].outputHeight != requested.height))) {
            const auto convertStarted = PerfClock::now();
            const auto previousError = error_;
            bool converted = frame->hardware ? convertSurface(index, *frame, requested)
                : softwareVideoProcessor && convertSoftwareSurface(index, *frame, requested);
            if (!frame->hardware && !converted && !deviceLost_) {
                converted = uploadSoftwareFrame(index, *frame);
                // The rejected processor route has been logged. A successful
                // fallback is a usable frame, not a persistent renderer error.
                if (converted) error_ = previousError;
            }
            convertMs += std::chrono::duration<double, std::milli>(
                PerfClock::now() - convertStarted).count();
            if (!converted) continue;
            if (!frame->hardware) {
                const auto& made = slots_[index];
                const bool hdrFrame = made.hlg || made.toneMapped || made.hdrSource;
                const int route = made.hardware && hdrFrame ? (made.toneMapped ? 6 : 7)
                    : made.hardware ? (made.rtxHdrApplied ? 2 : 1)
                    : made.toneMapped ? 5
                    : (hdrOutput_ ? (made.hdrSource ? (made.hlgToPq ? 8 : 4) : 3) : 0);
                if (loggedSoftwareRoute_[index] != route) {
                    loggedSoftwareRoute_[index] = route;
                    const char* name = route == 2 ? "NV12 VP / TrueHDR request accepted" :
                        route == 1 ? (hdrOutput_ ? "NV12 VP SDR / explicit SDR-to-PQ (no RTX)"
                                                               : "NV12 VP / SDR") :
                        route == 3 ? "BGRA / SDR-to-PQ fallback (no RTX)" :
                        route == 4 ? "HDR10 BGRA / shown as PQ, 8-bit" :
                        route == 5 ? "HDR BGRA / tone-mapped to SDR, 8-bit" :
                        route == 6 ? "HDR P010 VP / tone-mapped to SDR" :
                        route == 7 ? "HDR P010 VP / shown as PQ" :
                        route == 8 ? "HLG BGRA / made PQ, 8-bit" : "BGRA / SDR";
                    appendDiagnostic("Software presentation: pane=" + std::to_string(index + 1) +
                        " input=" + std::to_string(sourceWidth) + "x" + std::to_string(sourceHeight) +
                        " output=" + std::to_string(slots_[index].outputWidth) + "x" +
                        std::to_string(slots_[index].outputHeight) + " route=" + name);
                }
            }
        }
        if (frame->ownerFrame && videoColorDescription(frame->ownerFrame, 0, 0).hdr()) {
            // The least faithful of the HDR pictures drawn is the one named.
            const HdrPicture shown = slots_[index].toneMapped ? HdrPicture::ToneMapped
                : hdrOutput_ && (slots_[index].hdrSource || slots_[index].pqOutput) ? HdrPicture::Hdr
                : HdrPicture::Approximated;
            hdrPicture = std::max(hdrPicture, shown);
        }
        const bool softwarePq = hdrOutput_ &&
            slots_[index].outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM && !slots_[index].hdrSource;
        if (softwarePq && !prepareSoftwarePqTexture(index)) continue;
        // The texture keeps the stored proportions; the picture is shaped by
        // its pixels' width as well, so a 720x576 DVD at 64:45 fills a 16:9
        // pane and the viewport and the crop stretch it there.
        // What the slot's processor has turned is no longer the draw's to
        // turn: a slot of a BGRA upload, the fallback from a processor too,
        // has turned nothing. Stood up from its side, the texture's pixels
        // are as tall as the stored ones were wide.
        const int drawTurns = (quarterTurns - slots_[index].turned) & 3;
        const bool sideways = turnedSideways(drawTurns);
        const float texturePixelAspect = turnedSideways(slots_[index].turned) && frame->pixelAspect > 0.0
            ? static_cast<float>(1.0 / frame->pixelAspect) : static_cast<float>(frame->pixelAspect);
        const PanePictureGeometry geometry = panePictureGeometry(
            cells[index], slots_[index].outputWidth, slots_[index].outputHeight,
            texturePixelAspect, views[index].mode, views[index].zoom, drawTurns);
        const RectF& fitted = geometry.viewport;
        const float scaleX = geometry.scaleX;
        const float scaleY = geometry.scaleY;
        const float transform[] = {scaleX, scaleY, geometry.offsetX, geometry.offsetY};
        const float width = static_cast<float>(std::max(1, slots_[index].outputWidth));
        const float height = static_cast<float>(std::max(1, slots_[index].outputHeight));
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = fitted.x + 1.0F;
        viewport.TopLeftY = fitted.y + 1.0F;
        viewport.Width = std::max(1.0F, fitted.width - 2.0F);
        viewport.Height = std::max(1.0F, fitted.height - 2.0F);
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;
        // The colour controls and the levels are for SDR; PQ is left as it
        // is. A scaler of the chosen kind enlarges, and shrinking (a
        // software frame shown smaller than it came) stays bilinear, which
        // the other kernels, never widened, would only make shimmer.
        const bool colorPass = !hdrOutput_ && !videoColorNeutral(videoOutput_);
        VideoScaling scaling = videoOutput_.scaling;
        // Across the viewport lie the texture's rows when it is on its side.
        if ((sideways ? height : width) * scaleX > viewport.Width * 1.01F ||
            (sideways ? width : height) * scaleY > viewport.Height * 1.01F) {
            scaling = VideoScaling::Bilinear;
        }
        bool outputPass = colorPass || scaling != VideoScaling::Bilinear;
        ID3D11ShaderResourceView* view = softwarePq ? softwarePq_[index].pqView.Get()
                                                  : slots_[index].view.Get();
        if (outputPass && !softwarePq && effectShaderActive_) {
            // The user's shader first, at the picture's own size.
            if (runEffectsPass(index)) view = postTextures_[index].view.Get();
            else outputPass = false;
        }
        // The fallback and the effects use offscreen passes; rebind the
        // presentation target and the pane's state for this pane and every
        // following pane.
        context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
        context_->VSSetShader(drawTurns != 0 ? turnedVertexShaders_[static_cast<std::size_t>(drawTurns) - 1].Get()
                                             : vertexShader_.Get(), nullptr, 0);
        context_->UpdateSubresource(uvTransformBuffer_.Get(), 0, nullptr, transform, 0, 0);
        context_->RSSetViewports(1, &viewport);
        if (outputPass) {
            drawOutputPass(view, slots_[index].outputWidth, slots_[index].outputHeight, scaling, colorPass);
        } else {
            context_->PSSetShader(softwarePq ? copyShader_.Get() : pixelShader_.Get(), nullptr, 0);
            context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
            context_->PSSetConstantBuffers(0, 1, pixelParamsBuffer_.GetAddressOf());
            context_->PSSetConstantBuffers(1, 1, smartVibranceBuffer_.GetAddressOf());
            const float clock = static_cast<float>(std::fmod(std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count(), 3600.0));
            const float params[] = {
                width, height, static_cast<float>(frame->serial), clock,
                1.0F / width, 1.0F / height, 0.0F, 0.0F,
                // Safe c2/pPrev initial state. True temporal feedback would require
                // a separate history texture or a player-specific analysis pass.
                0.0F, 0.0F, 0.0F, 0.0F};
            context_->UpdateSubresource(pixelParamsBuffer_.Get(), 0, nullptr, params, 0, 0);
            context_->PSSetShaderResources(0, 1, &view);
            bindVibranceColorSpace(index);
            context_->Draw(4, 0);
        }
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
    }
    hdrPicture_ = hdrPicture;
    if (overlay_ && (hdrOutput_ || tenBitOutput_)) {
        // Direct2D cannot draw sRGB colours straight into a PQ back buffer:
        // white would come out at 10,000 nits. It paints a layer instead and
        // a shader pass brings that in at the display's SDR white. Nor does
        // it draw into a 10-bit buffer at all; the layer is blended in as
        // it is.
        compositeOverlayLayer();
    } else if (overlay_) {
        // Direct2D draws through the same immediate context, so the painter
        // runs inside the lock too. In a flip-model chain buffer 0 is always
        // the buffer about to be presented.
        Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
        if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            overlay_(backBuffer.Get(), false);
            // D2D leaves its own pipeline state behind; the next frame
            // rebinds everything it needs, but the render target must be ours.
            context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
        }
    }
    const auto ms = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    // The renderer and every D3D11VA decoder share the immediate context, so
    // DXGI presentation stays in this critical section: Present overlapping
    // a decoder's context work can hang the whole device. But Present blocks
    // while the GPU is still on this frame -- RTX Video HDR on a 4K60 stream
    // can cost it most of the 16.7 ms -- and inside the lock that starved
    // the decoders to below the frame rate and made a seek's preroll crawl.
    // So the frame's GPU work is waited for first, outside the lock, and
    // Present then has nothing left to absorb but the blank, which the
    // caller has already waited for on the frame-latency object. The wait
    // has to end when the GPU does: one that oversleeps by a few
    // milliseconds misses the blank and the frame shows a refresh late.
    const auto drawFinished = PerfClock::now();
    const double heldBeforeWait = ms(lockAcquired, drawFinished);
    double gpuWaitMs = 0.0;
    if (frameFence_) {
        const UINT64 value = ++frameFenceValue_;
        HRESULT armed = fenceContext_->Signal(frameFence_.Get(), value);
        context_->Flush();
        // A frame whose wait timed out may still set the event later; that
        // must not end this frame's wait early.
        ResetEvent(frameFenceEvent_);
        if (SUCCEEDED(armed)) armed = frameFence_->SetEventOnCompletion(value, frameFenceEvent_);
        deviceLock.unlock();
        const auto waitStarted = PerfClock::now();
        // A failure is a device that is gone, which Present will report.
        if (SUCCEEDED(armed)) WaitForSingleObject(frameFenceEvent_, 250);
        gpuWaitMs = ms(waitStarted, PerfClock::now());
        deviceLock.lock();
    } else if (frameQuery_) {
        // Without fences, an event query polled with the lock taken only for
        // each poll.
        context_->End(frameQuery_.Get());
        context_->Flush();
        deviceLock.unlock();
        const auto waitStarted = PerfClock::now();
        for (;;) {
            HRESULT ready = S_OK;
            {
                std::scoped_lock poll(deviceMutex_);
                ready = context_->GetData(frameQuery_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            }
            // S_FALSE is "not yet"; anything else is done, or a device that
            // is gone and will say so at Present.
            if (ready != S_FALSE) break;
            if (ms(waitStarted, PerfClock::now()) > 250.0) break;
            LARGE_INTEGER due{};
            due.QuadPart = -5000;  // 0.5 ms, in 100 ns units
            if (!frameQueryTimer_ || !SetWaitableTimer(frameQueryTimer_, &due, 0, nullptr, nullptr, FALSE) ||
                WaitForSingleObject(frameQueryTimer_, 5) != WAIT_OBJECT_0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
        gpuWaitMs = ms(waitStarted, PerfClock::now());
        deviceLock.lock();
    }
    const auto presentStarted = PerfClock::now();
    // The bitblt chain copies into DWM's picture of the window, which DWM
    // shows at its next blank; waiting for one here would hold the lock.
    const HRESULT hr = swapChain_->Present(flipModel_ ? 1 : 0, 0);
    const auto presentFinished = PerfClock::now();
    if (FAILED(hr)) {
        noteFailure("Present", hr);
    }
    ++stats_.frames;
    stats_.lockWaitMs += ms(lockRequested, lockAcquired);
    stats_.convertMs += convertMs;
    stats_.presentMs += ms(presentStarted, presentFinished);
    stats_.gpuWaitMs += gpuWaitMs;
    stats_.lockHeldMs += heldBeforeWait + ms(presentStarted, presentFinished);
    // Whatever was inside the lock before the wait is the draw work itself.
    stats_.drawMs += heldBeforeWait - convertMs;
}

RenderStats D3DRenderer::takeStats() {
    std::scoped_lock lock(deviceMutex_);
    const RenderStats snapshot = stats_;
    stats_ = RenderStats{};
    return snapshot;
}

void D3DRenderer::setError(const char* operation, HRESULT hr) {
    std::ostringstream out;
    out << operation << " failed: 0x" << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<unsigned long>(hr);
    error_ = out.str();
    if (!quietErrors_) appendDiagnostic("GPU ERROR: " + error_);
}

}  // namespace quaddeck
