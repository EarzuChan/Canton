#include <canton.hlsl>

cbuffer Params : register(b0) {
    float2 size;
    float angle;
    float falloff;
    float4 cornerRadii;
};

float radiusAt(float2 centeredCoord, float4 radii) {
    if (centeredCoord.x >= 0.0) {
        return centeredCoord.y <= 0.0 ? radii.y : radii.z;
    }
    return centeredCoord.y <= 0.0 ? radii.x : radii.w;
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

float4 main(CantonInput input) : SV_Target {
    float2 halfSize = max(size * 0.5, float2(1e-4, 1e-4));
    float2 centeredCoord = input.fragCoord - halfSize;
    float radius = radiusAt(centeredCoord, cornerRadii);

    float gradRadius = min(radius * 1.5, min(halfSize.x, halfSize.y));
    float2 grad = gradSdRoundedRect(centeredCoord, halfSize, gradRadius);
    float2 normal = float2(cos(angle), sin(angle));
    float d = dot(grad, normal);
    float intensity = pow(max(abs(d), 1e-6), max(falloff, 0.0));
    float t = step(0.0, d);
    return float4(t, t, t, 1.0) * intensity;
}
