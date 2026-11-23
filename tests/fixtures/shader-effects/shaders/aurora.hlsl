#include <canton.hlsl>

cbuffer Params : register(b0) {
    float time;
    float intensity;
    float2 _pad;
    float4 colorA;
    float4 colorB;
    float4 colorC;
};

float4 main(CantonInput input) : SV_Target {
    float2 uv = input.uv;
    float wave1 = 0.5 + 0.5 * sin(uv.x * 6.28318 + time * 1.4 + sin(uv.y * 4.5 - time * 0.9));
    float wave2 = 0.5 + 0.5 * cos(uv.y * 5.2 - time * 1.1 + uv.x * 3.1);
    float4 mix1 = lerp(colorA, colorB, smoothstep(0.15, 0.85, wave1));
    float4 col = lerp(mix1, colorC, smoothstep(0.25, 0.9, wave2) * clamp(intensity, 0.0, 1.0));
    float glow = 0.88 + 0.12 * sin((uv.x + uv.y) * 9.0 + time * 2.0);
    return float4(saturate(col.rgb * glow), 1.0);
}
