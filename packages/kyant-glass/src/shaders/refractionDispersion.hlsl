#include <canton.hlsl>

Texture2D content : register(t0);

cbuffer Params : register(b0) {
    float2 size;
    float2 offset;
    float4 cornerRadii;
    float refractionHeight;
    float refractionAmount;
    float depthEffect;
    float chromaticAberration;
};

float radiusAt(float2 centeredCoord, float4 radii) {
    if (centeredCoord.x >= 0.0) {
        return centeredCoord.y <= 0.0 ? radii.y : radii.z;
    }
    return centeredCoord.y <= 0.0 ? radii.x : radii.w;
}

float sdRoundedRect(float2 coord, float2 halfSize, float radius) {
    float2 cornerCoord = abs(coord) - (halfSize - float2(radius, radius));
    float outside = length(max(cornerCoord, 0.0)) - radius;
    float inside = min(max(cornerCoord.x, cornerCoord.y), 0.0);
    return outside + inside;
}

float2 safeNormalize(float2 v) {
    float lenSq = dot(v, v);
    return lenSq > 1e-8 ? v * rsqrt(lenSq) : float2(0.0, 0.0);
}

float2 gradSdRoundedRect(float2 coord, float2 halfSize, float radius) {
    float2 cornerCoord = abs(coord) - (halfSize - float2(radius, radius));
    float2 sgn = float2(coord.x < 0.0 ? -1.0 : 1.0, coord.y < 0.0 ? -1.0 : 1.0);
    if (cornerCoord.x >= 0.0 || cornerCoord.y >= 0.0) {
        return sgn * safeNormalize(max(cornerCoord, 0.0));
    }
    float gradX = step(cornerCoord.y, cornerCoord.x);
    return sgn * float2(gradX, 1.0 - gradX);
}

float circleMap(float x) {
    float clamped = clamp(x, 0.0, 1.0);
    return 1.0 - sqrt(max(0.0, 1.0 - clamped * clamped));
}

float4 sampleContent(float2 coord) {
    return content.SampleLevel(cantonLinearClamp, coord * cantonInvResolution, 0);
}

float4 main(CantonInput input) : SV_Target {
    float2 coord = input.fragCoord;
    float2 halfSize = max(size * 0.5, float2(1e-4, 1e-4));
    float2 centeredCoord = (coord + offset) - halfSize;
    float radius = radiusAt(centeredCoord, cornerRadii);

    float sd = sdRoundedRect(centeredCoord, halfSize, radius);
    if (-sd >= refractionHeight || refractionHeight <= 1e-4) {
        return sampleContent(coord);
    }
    sd = min(sd, 0.0);

    float d = circleMap(1.0 - (-sd) / refractionHeight) * refractionAmount;
    float gradRadius = min(radius * 1.5, min(halfSize.x, halfSize.y));
    float2 grad = safeNormalize(gradSdRoundedRect(centeredCoord, halfSize, gradRadius) + depthEffect * safeNormalize(centeredCoord));

    float2 refractedCoord = coord + d * grad;
    float dispersionIntensity = chromaticAberration * ((centeredCoord.x * centeredCoord.y) / max(halfSize.x * halfSize.y, 1e-4));
    float2 dispersedCoord = d * grad * dispersionIntensity;

    float4 color = float4(0.0, 0.0, 0.0, 0.0);

    float4 red = sampleContent(refractedCoord + dispersedCoord);
    color.r += red.r / 3.5;
    color.a += red.a / 7.0;

    float4 orange = sampleContent(refractedCoord + dispersedCoord * (2.0 / 3.0));
    color.r += orange.r / 3.5;
    color.g += orange.g / 7.0;
    color.a += orange.a / 7.0;

    float4 yellow = sampleContent(refractedCoord + dispersedCoord * (1.0 / 3.0));
    color.r += yellow.r / 3.5;
    color.g += yellow.g / 3.5;
    color.a += yellow.a / 7.0;

    float4 green = sampleContent(refractedCoord);
    color.g += green.g / 3.5;
    color.a += green.a / 7.0;

    float4 cyan = sampleContent(refractedCoord - dispersedCoord * (1.0 / 3.0));
    color.g += cyan.g / 3.5;
    color.b += cyan.b / 3.0;
    color.a += cyan.a / 7.0;

    float4 blue = sampleContent(refractedCoord - dispersedCoord * (2.0 / 3.0));
    color.b += blue.b / 3.0;
    color.a += blue.a / 7.0;

    float4 purple = sampleContent(refractedCoord - dispersedCoord);
    color.r += purple.r / 7.0;
    color.b += purple.b / 3.0;
    color.a += purple.a / 7.0;

    return color;
}
