// Canton M5 内置后处理、混合与用户着色器固定顶点管线（构建期由 fxc 编译嵌入）

// ── 1. 用户像素着色器的固定 2D 顶点着色器 ─────────────────────────────────────
cbuffer UserVs : register(b13) {
    float4 userRect;      // 局部四边形 (x, y, w, h)
    float4 userXform0;    // 仿射 (a, b, c, d)
    float4 userXform1;    // (tx, ty, viewportW, viewportH)
    float4 userFragRect;  // (fragOriginX, fragOriginY, invResX, invResY)
};

struct CantonInput {
    float4 position : SV_Position;
    float2 fragCoord : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

CantonInput userVsMain(uint vertex : SV_VertexID) {
    CantonInput output;
    float2 corner = float2(vertex & 1, vertex >> 1);
    float2 local = userRect.xy + corner * userRect.zw;
    float2 device = float2(
        userXform0.x * local.x + userXform0.z * local.y + userXform1.x,
        userXform0.y * local.x + userXform0.w * local.y + userXform1.y
    );
    output.position = float4(device.x / userXform1.z * 2.0 - 1.0, 1.0 - device.y / userXform1.w * 2.0, 0.0, 1.0);
    output.fragCoord = local - userFragRect.xy;
    output.uv = output.fragCoord * userFragRect.zw;
    return output;
}

// ── 2. 后处理与合成通用顶点着色器 ────────────────────────────────────────────
cbuffer PostVs : register(b0) {
    float4 postRect;       // 局部四边形 (x, y, w, h)
    float4 postXform0;     // 仿射 (a, b, c, d)
    float4 postXform1;     // (tx, ty, viewportW, viewportH)
    float4 postUvRect;     // 源纹理 UV (u0, v0, u1, v1)
    float4 postDstUvRect;  // 目标拷出背景纹理 UV (u0, v0, u1, v1)
};

struct PostPsInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float2 dstOrigin : TEXCOORD1;
    float2 local : TEXCOORD2;
    nointerpolation float4 jacobian : TEXCOORD3;
};

PostPsInput postVsMain(uint vertex : SV_VertexID) {
    PostPsInput output;
    float2 corner = float2(vertex & 1, vertex >> 1);
    float2 local = postRect.xy + corner * postRect.zw;
    float2 device = float2(
        postXform0.x * local.x + postXform0.z * local.y + postXform1.x,
        postXform0.y * local.x + postXform0.w * local.y + postXform1.y
    );
    output.position = float4(device.x / postXform1.z * 2.0 - 1.0, 1.0 - device.y / postXform1.w * 2.0, 0.0, 1.0);
    output.uv = lerp(postUvRect.xy, postUvRect.zw, corner);
    output.dstOrigin = postDstUvRect.xy;
    output.local = local;
    float det = postXform0.x * postXform0.w - postXform0.y * postXform0.z;
    output.jacobian = float4(postXform0.w, -postXform0.z, -postXform0.y, postXform0.x) / (abs(det) > 1e-12 ? det : 1.0);
    return output;
}

// ── 共享资源与常量缓冲 ────────────────────────────────────────────────────────
Texture2D srcTexture : register(t0);
Texture2D coverageMaskTexture : register(t1);
Texture2D clipMaskTexture : register(t2);
Texture2D dstTexture : register(t3);
SamplerState linearClamp : register(s0);

cbuffer BlurParams : register(b1) {
    float4 blurSizeAndInv;  // (contentW, contentH, invTexW, invTexH)
    float4 blurDirSigma;    // (dirX, dirY, sigma, halfTaps)
    float4 blurTileShift;   // (tileMode, srcOffsetX, srcOffsetY, unused)
};

cbuffer CompositeParams : register(b1) {
    float4 compClipRow0;
    float4 compClipRow1;
    float4 compClipShape;
    float4 compClipRadii;
    float4 compClipFlags;    // (clipKind, useClipMask, useCoverageMask, useAdvancedBlend)
    float4 compBlendAlpha;   // (alpha, blendMode, clearPunchOut, useBoxAA)
    float4 compBoxShape;     // (centerX, centerY, halfW, halfH)
    float4 compFilterMeta;   // (filterKind, tintBlendMode, unused, unused)
    float4 compFilterColor;  // premul tint / lighting mul
    float4 compFilterAdd;    // lighting add / unused
    float4 compMatrixRow0;
    float4 compMatrixRow1;
    float4 compMatrixRow2;
    float4 compMatrixRow3;
    float4 compMatrixBias;   // 已除以 255.0 的偏置
};

// ── 解析裁剪与盒抗锯齿（与 Quad.hlsl 同源） ──────────────────────────────────
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

float coverageOf(float3 field, float4 jacobian) {
    float2 screen = float2(jacobian.x * field.y + jacobian.z * field.z, jacobian.y * field.y + jacobian.w * field.z);
    return saturate(0.5 - field.x / max(length(screen), 1e-6));
}

float boxCoverage(float2 p, float2 halfSize, float4 jacobian) {
    if (halfSize.x <= 0.0 || halfSize.y <= 0.0) return 0.0;
    float2 q = abs(p) - halfSize;
    float2 width = max(float2(length(jacobian.xy), length(jacobian.zw)), 1e-6);
    float2 cov = saturate(0.5 - q / width);
    return cov.x * cov.y;
}

float compositeClipCoverage(float2 pixel) {
    float cov = 1.0;
    if (compClipFlags.x > 0.5) {
        float2 q = float2(dot(compClipRow0.xy, pixel) + compClipRow0.z, dot(compClipRow1.xy, pixel) + compClipRow1.z) - compClipShape.xy;
        float3 field = compClipFlags.x < 1.5 ? sdRoundBox(q, compClipShape.zw, compClipRadii) : sdEllipse(q, compClipShape.zw);
        cov *= coverageOf(field, float4(compClipRow0.x, compClipRow0.y, compClipRow1.x, compClipRow1.y));
    }
    if (compClipFlags.y > 0.5) cov *= clipMaskTexture.Load(int3(pixel, 0)).r;
    return cov;
}

// ── W3C / Skia 精确 29 种 BlendMode（预乘 RGBA） ─────────────────────────────
float lum(float3 c) {
    return dot(c, float3(0.3, 0.59, 0.11));
}

float sat(float3 c) {
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

float3 clipColor(float3 c) {
    float l = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return saturate(c);
}

float3 setLum(float3 c, float l) {
    return clipColor(c + (l - lum(c)));
}

float3 setSat(float3 c, float s) {
    float cmin = min(c.r, min(c.g, c.b));
    float cmax = max(c.r, max(c.g, c.b));
    float span = cmax - cmin;
    if (span <= 1e-6) return float3(0.0, 0.0, 0.0);
    return (c - cmin) * (s / span);
}

float softLightChannel(float cs, float cd) {
    if (cs <= 0.5) {
        return cd - (1.0 - 2.0 * cs) * cd * (1.0 - cd);
    }
    float d = cd <= 0.25 ? ((16.0 * cd - 12.0) * cd + 4.0) * cd : sqrt(max(cd, 0.0));
    return cd + (2.0 * cs - 1.0) * (d - cd);
}

float colorDodgeChannel(float cs, float cd) {
    if (cd <= 0.0) return 0.0;
    if (cs >= 1.0) return 1.0;
    return min(1.0, cd / max(1.0 - cs, 1e-6));
}

float colorBurnChannel(float cs, float cd) {
    if (cd >= 1.0) return 1.0;
    if (cs <= 0.0) return 0.0;
    return 1.0 - min(1.0, (1.0 - cd) / max(cs, 1e-6));
}

float hardLightChannel(float cs, float cd) {
    return cs <= 0.5 ? 2.0 * cs * cd : 1.0 - 2.0 * (1.0 - cs) * (1.0 - cd);
}

float4 blend29(float4 src, float4 dst, int mode) {
    if (mode == 0) return float4(0.0, 0.0, 0.0, 0.0);                                           // Clear
    if (mode == 1) return src;                                                                   // Src
    if (mode == 2) return dst;                                                                   // Dst
    if (mode == 3) return src + dst * (1.0 - src.a);                                             // SrcOver
    if (mode == 4) return dst + src * (1.0 - dst.a);                                             // DstOver
    if (mode == 5) return src * dst.a;                                                           // SrcIn
    if (mode == 6) return dst * src.a;                                                           // DstIn
    if (mode == 7) return src * (1.0 - dst.a);                                                   // SrcOut
    if (mode == 8) return dst * (1.0 - src.a);                                                   // DstOut
    if (mode == 9) return src * dst.a + dst * (1.0 - src.a);                                     // SrcAtop
    if (mode == 10) return dst * src.a + src * (1.0 - dst.a);                                    // DstAtop
    if (mode == 11) return src * (1.0 - dst.a) + dst * (1.0 - src.a);                            // Xor
    if (mode == 12) return saturate(src + dst);                                                  // Plus
    if (mode == 13) return src * dst;                                                            // Modulate
    if (mode == 14) return src + dst - src * dst;                                                // Screen

    float3 cs = src.a > 1e-6 ? saturate(src.rgb / src.a) : float3(0.0, 0.0, 0.0);
    float3 cd = dst.a > 1e-6 ? saturate(dst.rgb / dst.a) : float3(0.0, 0.0, 0.0);
    float3 blended = cs;

    if (mode == 15) {  // Overlay
        blended = float3(hardLightChannel(cd.r, cs.r), hardLightChannel(cd.g, cs.g), hardLightChannel(cd.b, cs.b));
    } else if (mode == 16) {  // Darken
        blended = min(cs, cd);
    } else if (mode == 17) {  // Lighten
        blended = max(cs, cd);
    } else if (mode == 18) {  // ColorDodge
        blended = float3(colorDodgeChannel(cs.r, cd.r), colorDodgeChannel(cs.g, cd.g), colorDodgeChannel(cs.b, cd.b));
    } else if (mode == 19) {  // ColorBurn
        blended = float3(colorBurnChannel(cs.r, cd.r), colorBurnChannel(cs.g, cd.g), colorBurnChannel(cs.b, cd.b));
    } else if (mode == 20) {  // HardLight
        blended = float3(hardLightChannel(cs.r, cd.r), hardLightChannel(cs.g, cd.g), hardLightChannel(cs.b, cd.b));
    } else if (mode == 21) {  // SoftLight
        blended = float3(softLightChannel(cs.r, cd.r), softLightChannel(cs.g, cd.g), softLightChannel(cs.b, cd.b));
    } else if (mode == 22) {  // Difference
        blended = abs(cd - cs);
    } else if (mode == 23) {  // Exclusion
        blended = cd + cs - 2.0 * cd * cs;
    } else if (mode == 24) {  // Multiply
        blended = cs * cd;
    } else if (mode == 25) {  // Hue
        blended = setLum(setSat(cs, sat(cd)), lum(cd));
    } else if (mode == 26) {  // Saturation
        blended = setLum(setSat(cd, sat(cs)), lum(cd));
    } else if (mode == 27) {  // Color
        blended = setLum(cs, lum(cd));
    } else if (mode == 28) {  // Luminosity
        blended = setLum(cd, lum(cs));
    }

    float outA = saturate(src.a + dst.a - src.a * dst.a);
    float3 outRgb = (1.0 - dst.a) * src.rgb + (1.0 - src.a) * dst.rgb + src.a * dst.a * blended;
    return float4(saturate(outRgb), outA);
}

// ── ColorFilter（Tint / ColorMatrix / Lighting） ──────────────────────────────
float4 applyColorFilter(float4 c) {
    int kind = (int)(compFilterMeta.x + 0.5);
    if (kind == 0) return c;
    if (kind == 1) {
        int mode = (int)(compFilterMeta.y + 0.5);
        return blend29(compFilterColor, c, mode);
    }
    if (kind == 2) {
        float4 straight = c.a > 1e-6 ? float4(c.rgb / c.a, c.a) : float4(0.0, 0.0, 0.0, 0.0);
        float4 res;
        res.r = dot(compMatrixRow0, straight) + compMatrixBias.r;
        res.g = dot(compMatrixRow1, straight) + compMatrixBias.g;
        res.b = dot(compMatrixRow2, straight) + compMatrixBias.b;
        res.a = dot(compMatrixRow3, straight) + compMatrixBias.a;
        res = saturate(res);
        return float4(res.rgb * res.a, res.a);
    }
    if (kind == 3) {
        float3 straight = c.a > 1e-6 ? (c.rgb / c.a) : float3(0.0, 0.0, 0.0);
        float3 res = saturate(straight * compFilterColor.rgb + compFilterAdd.rgb);
        return float4(res * c.a, c.a);
    }
    return c;
}

// ── 3. 两 Pass 可分离高斯模糊（支持分桶 RT 上的四种 TileMode） ────────────────
float4 sampleBlurTile(float2 pixelCoord, float2 contentSize, float2 invTexSize, int tileMode) {
    if (tileMode == 0) {  // Clamp
        float2 p = clamp(pixelCoord, float2(0.5, 0.5), contentSize - float2(0.5, 0.5));
        return srcTexture.SampleLevel(linearClamp, p * invTexSize, 0);
    }
    if (tileMode == 2) {  // Mirror
        float2 cycle = max(contentSize * 2.0, float2(2.0, 2.0));
        float2 t = abs(pixelCoord);
        float2 m = t - floor(t / cycle) * cycle;
        float2 p = float2(m.x > contentSize.x ? cycle.x - m.x : m.x, m.y > contentSize.y ? cycle.y - m.y : m.y);
        p = clamp(p, float2(0.5, 0.5), contentSize - float2(0.5, 0.5));
        return srcTexture.SampleLevel(linearClamp, p * invTexSize, 0);
    }
    if (tileMode == 3) {  // Decal
        float2 fade = saturate(pixelCoord + float2(0.5, 0.5)) * saturate(contentSize + float2(0.5, 0.5) - pixelCoord);
        float f = fade.x * fade.y;
        if (f <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
        float2 p = clamp(pixelCoord, float2(0.5, 0.5), contentSize - float2(0.5, 0.5));
        return srcTexture.SampleLevel(linearClamp, p * invTexSize, 0) * f;
    }
    // Repeated (tileMode == 1)：在分桶 RT 上手写精确双线性回绕，消除缝隙与越界采样
    float2 q = pixelCoord - float2(0.5, 0.5);
    float2 base = floor(q);
    float2 fracPart = q - base;
    int2 sizeI = max(int2(contentSize + 0.5), int2(1, 1));
    int2 p0 = int2(((int)base.x % sizeI.x + sizeI.x) % sizeI.x, ((int)base.y % sizeI.y + sizeI.y) % sizeI.y);
    int2 p1 = int2((p0.x + 1) % sizeI.x, (p0.y + 1) % sizeI.y);
    float4 c00 = srcTexture.Load(int3(p0.x, p0.y, 0));
    float4 c10 = srcTexture.Load(int3(p1.x, p0.y, 0));
    float4 c01 = srcTexture.Load(int3(p0.x, p1.y, 0));
    float4 c11 = srcTexture.Load(int3(p1.x, p1.y, 0));
    return lerp(lerp(c00, c10, fracPart.x), lerp(c01, c11, fracPart.x), fracPart.y);
}

float4 blurPsMain(PostPsInput input) : SV_Target {
    float2 contentSize = blurSizeAndInv.xy;
    float2 invTexSize = blurSizeAndInv.zw;
    float2 dir = blurDirSigma.xy;
    float sigma = max(blurDirSigma.z, 1e-4);
    int halfTaps = clamp((int)(blurDirSigma.w + 0.5), 0, 256);
    int tileMode = (int)(blurTileShift.x + 0.5);
    float2 center = input.position.xy + blurTileShift.yz;

    float4 acc = sampleBlurTile(center, contentSize, invTexSize, tileMode);
    if (halfTaps <= 0) return acc;

    float invTwoSigmaSq = 0.5 / (sigma * sigma);
    float wSum = 1.0;

    [loop]
    for (int i = 1; i <= halfTaps; i += 2) {
        float f1 = (float)i;
        float w1 = exp(-f1 * f1 * invTwoSigmaSq);
        float w = w1;
        float offset = f1;
        if (i + 1 <= halfTaps) {
            float f2 = (float)(i + 1);
            float w2 = exp(-f2 * f2 * invTwoSigmaSq);
            w = w1 + w2;
            offset = (f1 * w1 + f2 * w2) / max(w, 1e-8);
        }
        float2 delta = dir * offset;
        acc += (sampleBlurTile(center + delta, contentSize, invTexSize, tileMode) +
                sampleBlurTile(center - delta, contentSize, invTexSize, tileMode)) * w;
        wSum += 2.0 * w;
    }
    return acc / wSum;
}

// ── 4. 独立 ColorFilter 后处理 Pass ──────────────────────────────────────────
float4 colorFilterPsMain(PostPsInput input) : SV_Target {
    float4 c = srcTexture.SampleLevel(linearClamp, input.uv, 0);
    return applyColorFilter(c);
}

// ── 5. 离屏层 / shaderBrush / 高级 BlendMode 合成 Pass ───────────────────────
float4 compositePsMain(PostPsInput input) : SV_Target {
    float4 src = srcTexture.SampleLevel(linearClamp, input.uv, 0);
    src = applyColorFilter(src);
    src *= compBlendAlpha.x;

    float cov = compositeClipCoverage(input.position.xy);
    if (compClipFlags.z > 0.5) {
        int2 covCoord = int2(input.position.xy) - int2(compFilterMeta.zw);
        cov *= coverageMaskTexture.Load(int3(covCoord, 0)).a;
    }
    if (compBlendAlpha.w > 0.5) {
        cov *= boxCoverage(input.local - compBoxShape.xy, compBoxShape.zw, input.jacobian);
    }

    // Clear 走硬件 Src=ZERO, Dst=INV_SRC_ALPHA 时，输出 alpha = cov 即可平滑抗锯齿挖空
    if (compBlendAlpha.z > 0.5) {
        return float4(0.0, 0.0, 0.0, cov);
    }

    if (compClipFlags.w > 0.5) {
        int2 dstCoord = int2(input.position.xy) - int2(input.dstOrigin + 0.5);
        float4 dst = dstTexture.Load(int3(dstCoord, 0));
        int mode = (int)(compBlendAlpha.y + 0.5);
        float4 blended = blend29(src, dst, mode);
        return lerp(dst, blended, cov);
    }

    return src * cov;
}
