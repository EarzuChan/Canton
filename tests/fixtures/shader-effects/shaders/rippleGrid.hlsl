#include <canton.hlsl>

cbuffer Params : register(b0) {
    float time;
    float gridScale;
    float2 _pad;
    float4 baseColor;
    float4 accentColor;
};

float4 main(CantonInput input) : SV_Target {
    float2 uv = input.uv;
    float2 centered = (uv - float2(0.5, 0.5)) * float2(cantonResolution.x / max(cantonResolution.y, 1.0), 1.0);
    float dist = length(centered);
    float ripple = 0.5 + 0.5 * sin(dist * 26.0 - time * 3.2);

    float2 grid = abs(frac(uv * max(gridScale, 2.0) - 0.5) - 0.5) / max(gridScale, 2.0);
    float lineMask = 1.0 - smoothstep(0.004, 0.014, min(grid.x, grid.y));
    float mixFactor = saturate(lineMask * 0.75 + ripple * 0.45);
    float4 col = lerp(baseColor, accentColor, mixFactor);
    return float4(saturate(col.rgb), 1.0);
}
