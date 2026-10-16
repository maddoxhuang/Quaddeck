// QuadDeck external pixel shader contract:
// entry point: main, target: ps_4_0, input TEXCOORD0, texture t0, sampler s0.
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    color.r *= 1.08;
    color.b *= 0.92;
    return float4(saturate(color.rgb), color.a);
}
