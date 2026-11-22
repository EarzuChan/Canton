// Canton 的 2D 批渲染着色器：一个实例一张四边形，形状、图片、覆盖率掩码与阴影共用一对着色器。
// 约定见 d3d11/Renderer.cpp 的 QuadInstance。

cbuffer Frame : register(b0) {
    float2 viewport;
    float2 framePadding;
    // 设备坐标到裁剪形状局部坐标的仿射
    float4 clipRow0;
    float4 clipRow1;
    // 裁剪形状：中心与半尺寸
    float4 clipShape;
    float4 clipRadii;
    // x：0 无、1 圆角矩形、2 椭圆；y：是否采样裁剪掩码
    float4 clipFlags;
};

Texture2D imageTexture : register(t0);
Texture2D coverageTexture : register(t1);
Texture2D gradientTexture : register(t2);
Texture2D clipMaskTexture : register(t3);
SamplerState linearClamp : register(s0);

struct VsInput {
    float4 rect : INSTANCE0;
    float4 xform0 : INSTANCE1;
    float4 xform1 : INSTANCE2;
    float4 shape : INSTANCE3;
    float4 radii : INSTANCE4;
    float4 params : INSTANCE5;
    float4 color : INSTANCE6;
    float4 brush : INSTANCE7;
    float4 brushGeometry : INSTANCE8;
    float4 brushRow0 : INSTANCE9;
    float4 brushRow1 : INSTANCE10;
    float4 uv : INSTANCE11;
    float4 shape2 : INSTANCE12;
    float4 radii2 : INSTANCE13;
    uint vertex : SV_VertexID;
};

struct PsInput {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float2 brushPoint : TEXCOORD2;
    nointerpolation float4 shape : TEXCOORD3;
    nointerpolation float4 radii : TEXCOORD4;
    nointerpolation float4 params : TEXCOORD5;
    nointerpolation float4 color : TEXCOORD6;
    nointerpolation float4 brush : TEXCOORD7;
    nointerpolation float4 brushGeometry : TEXCOORD8;
    nointerpolation float4 shape2 : TEXCOORD9;
    nointerpolation float4 radii2 : TEXCOORD10;
    nointerpolation float2 kind : TEXCOORD11;
    // 屏幕坐标到局部坐标的雅可比，把局部距离换算成像素
    nointerpolation float4 jacobian : TEXCOORD12;
};

PsInput vsMain(VsInput input) {
    PsInput output;
    float2 corner = float2(input.vertex & 1, input.vertex >> 1);
    float2 local = input.rect.xy + corner * input.rect.zw;
    float2 device = float2(input.xform0.x * local.x + input.xform0.z * local.y + input.xform1.x, input.xform0.y * local.x + input.xform0.w * local.y + input.xform1.y);
    output.position = float4(device.x / viewport.x * 2.0 - 1.0, 1.0 - device.y / viewport.y * 2.0, 0.0, 1.0);
    output.local = local;
    output.uv = lerp(input.uv.xy, input.uv.zw, corner);
    output.brushPoint = float2(dot(input.brushRow0.xy, local) + input.brushRow0.z, dot(input.brushRow1.xy, local) + input.brushRow1.z);
    output.shape = input.shape;
    output.radii = input.radii;
    output.params = input.params;
    output.color = input.color;
    output.brush = input.brush;
    output.brushGeometry = input.brushGeometry;
    output.shape2 = input.shape2;
    output.radii2 = input.radii2;
    output.kind = input.xform1.zw;
    float determinant = input.xform0.x * input.xform0.w - input.xform0.y * input.xform0.z;
    output.jacobian = float4(input.xform0.w, -input.xform0.z, -input.xform0.y, input.xform0.x) / (abs(determinant) > 1e-12 ? determinant : 1.0);
    return output;
}

// 以下两个函数返回 (距离, 局部空间里的单位梯度)。梯度解析求出，不用屏幕导数：
// 屏幕导数在恰好落在边上的像素处不稳定，硬件与 WARP 的结果会不一样

// radii：左上、右上、右下、左下；y 向下
float3 sdRoundBox(float2 p, float2 halfSize, float4 radii) {
    float2 side = p.x > 0.0 ? radii.yz : radii.xw;
    float radius = p.y > 0.0 ? side.y : side.x;
    float2 q = abs(p) - halfSize + radius;
    float2 direction = float2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
    if (max(q.x, q.y) < 0.0) return float3(max(q.x, q.y) - radius, q.x > q.y ? float2(direction.x, 0.0) : float2(0.0, direction.y));
    float2 outside = max(q, 0.0);
    float size = length(outside);
    return float3(size - radius, (size > 1e-6 ? outside / size : float2(0.70710678, 0.70710678)) * direction);
}

float3 sdEllipse(float2 p, float2 halfSize) {
    float2 ab = max(halfSize, 1e-4);
    float k0 = length(p / ab);
    if (k0 < 1e-4) return float3(-min(ab.x, ab.y), 1.0, 0.0);
    float2 gradient = p / (ab * ab);
    float k1 = max(length(gradient), 1e-8);
    return float3(k0 * (k0 - 1.0) / k1, gradient / k1);
}

// 距离换成覆盖率：沿梯度方向一个像素对应多长的局部距离，由雅可比给出
float coverageOf(float3 field, float4 jacobian) {
    float2 screen = float2(jacobian.x * field.y + jacobian.z * field.z, jacobian.y * field.y + jacobian.w * field.z);
    return saturate(0.5 - field.x / max(length(screen), 1e-6));
}

float erfApprox(float x) {
    float a = abs(x);
    float t = 1.0 + a * (0.278393 + a * (0.230389 + a * (0.000972 + a * 0.078108)));
    t *= t;
    t *= t;
    return sign(x) * (1.0 - 1.0 / t);
}

// 形状在高度 y 处的水平范围（相对中心）
float2 spanAt(float y, float2 halfSize, float4 radii, float ellipse) {
    if (ellipse > 0.5) {
        float ratio = saturate(1.0 - (y / max(halfSize.y, 1e-4)) * (y / max(halfSize.y, 1e-4)));
        float w = halfSize.x * sqrt(ratio);
        return float2(-w, w);
    }
    float leftRadius = y < 0.0 ? radii.x : radii.w;
    float rightRadius = y < 0.0 ? radii.y : radii.z;
    float fromEdge = halfSize.y - abs(y);
    float leftInset = max(leftRadius - fromEdge, 0.0);
    float rightInset = max(rightRadius - fromEdge, 0.0);
    float left = -halfSize.x + leftRadius - sqrt(max(leftRadius * leftRadius - leftInset * leftInset, 0.0));
    float right = halfSize.x - rightRadius + sqrt(max(rightRadius * rightRadius - rightInset * rightInset, 0.0));
    return float2(left, right);
}

// 形状与高斯核的卷积：水平方向解析积分，垂直方向数值积分
float blurredShape(float2 p, float2 halfSize, float4 radii, float ellipse, float sigma) {
    float low = max(-halfSize.y, p.y - 3.0 * sigma);
    float high = min(halfSize.y, p.y + 3.0 * sigma);
    if (high <= low) return 0.0;
    const int samples = 16;
    float step = (high - low) / samples;
    float inverse = 1.0 / (sigma * 1.41421356);
    float sum = 0.0;
    for (int i = 0; i < samples; ++i) {
        float y = low + (i + 0.5) * step;
        float2 span = spanAt(y, halfSize, radii, ellipse);
        float offset = (p.y - y) / sigma;
        sum += exp(-0.5 * offset * offset) * 0.5 * (erfApprox((span.y - p.x) * inverse) - erfApprox((span.x - p.x) * inverse));
    }
    return saturate(sum * step / (sigma * 2.50662827));
}

float4 brushColor(PsInput input) {
    float kind = input.brush.x;
    if (kind < 0.5) return input.color;
    float2 p = input.brushPoint;
    float4 g = input.brushGeometry;
    float t;
    if (kind < 1.5) {
        float2 axis = g.zw - g.xy;
        t = dot(p - g.xy, axis) / max(dot(axis, axis), 1e-12);
    } else if (kind < 2.5) {
        t = length(p - g.xy) / max(g.z, 1e-6);
    } else {
        float turn = (atan2(p.y - g.y, p.x - g.x) - g.z) / 6.28318531;
        t = turn - floor(turn);
    }
    float2 at = float2((saturate(t) * 255.0 + 0.5) / 256.0, input.brush.y);
    return gradientTexture.SampleLevel(linearClamp, at, 0) * input.color.a;
}

// 距离落在 range 之内的环：外沿与内沿各自求覆盖率
float ringCoverage(float3 field, float2 range, float4 jacobian) {
    float outer = coverageOf(float3(field.x - range.y, field.yz), jacobian);
    float inner = coverageOf(float3(range.x - field.x, -field.yz), jacobian);
    return saturate(outer + inner - 1.0);
}

// 直角矩形：两个方向各自求覆盖率再相乘，轴对齐时角上也是精确的面积
float boxCoverage(float2 p, float2 halfSize, float4 jacobian) {
    if (halfSize.x <= 0.0 || halfSize.y <= 0.0) return 0.0;
    float2 q = abs(p) - halfSize;
    float2 width = max(float2(length(jacobian.xy), length(jacobian.zw)), 1e-6);
    float2 coverage = saturate(0.5 - q / width);
    return coverage.x * coverage.y;
}

float clipCoverage(float2 pixel) {
    float coverage = 1.0;
    if (clipFlags.x > 0.5) {
        float2 q = float2(dot(clipRow0.xy, pixel) + clipRow0.z, dot(clipRow1.xy, pixel) + clipRow1.z) - clipShape.xy;
        float3 field = clipFlags.x < 1.5 ? sdRoundBox(q, clipShape.zw, clipRadii) : sdEllipse(q, clipShape.zw);
        coverage *= coverageOf(field, float4(clipRow0.x, clipRow0.y, clipRow1.x, clipRow1.y));
    }
    if (clipFlags.y > 0.5) coverage *= clipMaskTexture.Load(int3(pixel, 0)).r;
    return coverage;
}

float4 psMain(PsInput input) : SV_Target {
    int kind = (int)(input.kind.x + 0.5);
    int flags = (int)(input.kind.y + 0.5);
    float2 p = input.local - input.shape.xy;
    float4 color;
    float coverage;
    if (kind == 0) {
        color = brushColor(input);
        if (dot(input.radii, float4(1, 1, 1, 1)) <= 0.0 && input.params.y <= 0.0) {
            // 直角：填充是整个矩形，向内的描边是矩形减去收缩后的矩形
            coverage = boxCoverage(p, input.shape.zw, input.jacobian);
            if (input.params.x > -1.0e8) coverage = saturate(coverage - boxCoverage(p, input.shape.zw + input.params.x, input.jacobian));
        } else
            coverage = ringCoverage(sdRoundBox(p, input.shape.zw, input.radii), input.params.xy, input.jacobian);
    } else if (kind == 1) {
        color = brushColor(input);
        coverage = ringCoverage(sdEllipse(p, input.shape.zw), input.params.xy, input.jacobian);
    } else if (kind == 2) {
        // 图片：flags 第 0 位是着色，第 1 位是平铺；radii2 是源区域的 uv 范围
        float2 uv = input.uv;
        float width, height;
        imageTexture.GetDimensions(width, height);
        float2 inset = 0.5 / float2(width, height);
        if (flags & 2) uv = lerp(input.radii2.xy, input.radii2.zw, frac((input.local - input.shape2.xy) / max(input.shape2.zw, 1e-4)));
        uv = clamp(uv, input.radii2.xy + inset, input.radii2.zw - inset);
        float4 texel = imageTexture.SampleLevel(linearClamp, uv, 0);
        color = (flags & 1) ? input.color * texel.a : texel * input.color.a;
        coverage = boxCoverage(p, input.shape.zw, input.jacobian);
    } else if (kind == 3) {
        color = brushColor(input);
        coverage = coverageTexture.SampleLevel(linearClamp, input.uv, 0).r;
    } else if (kind == 4) {
        color = input.color;
        coverage = blurredShape(p, input.shape.zw, input.radii, input.params.w, input.params.z);
    } else {
        // 内阴影：阴影色铺满，挖去洞的模糊结果，再裁到外形
        color = input.color;
        float hole = blurredShape(input.local - input.shape2.xy, input.shape2.zw, input.radii2, input.params.w, input.params.z);
        float3 outer = input.params.w > 0.5 ? sdEllipse(p, input.shape.zw) : sdRoundBox(p, input.shape.zw, input.radii);
        coverage = (1.0 - hole) * coverageOf(outer, input.jacobian);
    }
    return color * (coverage * clipCoverage(input.position.xy));
}
