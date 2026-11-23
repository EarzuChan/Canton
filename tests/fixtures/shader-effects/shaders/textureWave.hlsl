#include <canton.hlsl>

Texture2D sourceTex : register(t0);

cbuffer Params : register(b0) {
    float time;
    float amplitude;
    float frequency;
    float _pad;
};

float4 main(CantonInput input) : SV_Target {
    float2 uv = input.uv;
    float dx = sin(uv.y * frequency + time * 2.0) * amplitude;
    float dy = cos(uv.x * frequency - time * 1.6) * amplitude;
    float4 texel = sourceTex.SampleLevel(cantonLinearWrap, uv + float2(dx, dy), 0);
    float vignette = 1.0 - 0.25 * dot(uv - 0.5, uv - 0.5);
    return float4(saturate(texel.rgb * vignette), texel.a);
}
