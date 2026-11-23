import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { registerHooks } from 'node:module'
import { resolve } from 'node:path'
import test from 'node:test'
import { fileURLToPath } from 'node:url'
import { compileCantonCta } from '../../packages/vite-plugin/src/ctaTransform.ts'
import { compileHlslToDxbc } from '../../packages/vite-plugin/src/hlslCompiler.ts'
import { transformHlslModule } from '../../packages/vite-plugin/src/hlslTransform.ts'
import * as runtime from '../../packages/framework/src/index.ts'
import * as ui from '../../packages/framework/src/ui.ts'
import * as internal from '../../packages/framework/src/internal.ts'
import { createLayoutCoordinates } from '../../packages/framework/src/layout/coordinates.ts'
import { renderEffectSnapshot } from '../../packages/framework/src/draw/renderEffect.ts'
import { DrawInstance } from '../../packages/framework/src/draw/draw.ts'
import { createFakeNative } from '../helpers/fakeNative.ts'

// Node 单测环境下将静态 .hlsl 导入映射为 createRuntimeEffect（生产/开发走 Vite 插件双轨编译）
registerHooks({
    load(url, context, nextLoad) {
        if (url.endsWith('.hlsl')) {
            const filePath = fileURLToPath(url)
            const source = readFileSync(filePath, 'utf8')
            return {
                format: 'module',
                shortCircuit: true,
                source: `import { createRuntimeEffect } from '@canton/framework/ui'\nexport default createRuntimeEffect(${JSON.stringify(source)}, { name: ${JSON.stringify(filePath)} })\n`,
            }
        }
        return nextLoad(url, context)
    },
})

const prevNative = globalThis.__CANTON_NATIVE__
globalThis.__CANTON_NATIVE__ = createFakeNative()
const kyantGlass = await import('../../packages/kyant-glass/src/index.ts')
globalThis.__CANTON_NATIVE__ = prevNative
const { InverseLayerScope, createColorControlsColorFilter } = kyantGlass

const sampleCtaPath = resolve(process.cwd(), 'tests/fixtures/shader-effects/SampleUnitCheck.cta')

function compileCtaSnippet(scriptBody: string): string {
    const source = `<template>
    <Box :modifier="mod" />
</template>
<script>
import { M, Color, BlendMode, TileMode, ColorFilter, RenderEffect, createRuntimeEffect, createRuntimeShader, shaderBrush, rounded } from '@canton/framework/ui'
${scriptBody}
</script>`
    return compileCantonCta(source, sampleCtaPath).code
}

test('CTA 单位契约：RenderEffect.blur / offset 严格要求 px，ColorFilter / RuntimeShader.setColor 要求 Color', () => {
    const ok = compileCtaSnippet(`
const eff = createRuntimeEffect('float4 main() : SV_Target { return float4(1,0,0,1); }')
const sh = createRuntimeShader(eff).setColor('tint', Color(0xff112233))
const re = RenderEffect.chain(RenderEffect.blur(8.px, 6.px, TileMode.Clamp), RenderEffect.offset(4.px, (-2).px), RenderEffect.colorFilter(ColorFilter.tint(Color(0xff00ff00), BlendMode.SrcIn)))
const mod = M.background(shaderBrush(sh), rounded(8.dp)).graphicsLayer({ renderEffect: re, blendMode: BlendMode.Multiply, colorFilter: ColorFilter.lighting(Color(0xffffffff), Color(0x00101010)) })
`)
    assert.match(ok, /RenderEffect\.blur/)

    // RenderEffect.blur 传入 dp 必须报错
    assert.throws(() => {
        compileCtaSnippet(`
const re = RenderEffect.blur(8.dp, 8.dp)
const mod = M.graphicsLayer({ renderEffect: re })
`)
    }, /PX/)

    // RenderEffect.offset 传入裸数字必须报错
    assert.throws(() => {
        compileCtaSnippet(`
const re = RenderEffect.offset(4, 2)
const mod = M.graphicsLayer({ renderEffect: re })
`)
    }, /PX/)

    // ColorFilter.tint 传入裸数字必须报错
    assert.throws(() => {
        compileCtaSnippet(`
const cf = ColorFilter.tint(0xffff0000)
const mod = M.graphicsLayer({ colorFilter: cf })
`)
    }, /COLOR/)

    // RuntimeShader.setColor 传入裸数字必须报错
    assert.throws(() => {
        compileCtaSnippet(`
const eff = createRuntimeEffect('float4 main() : SV_Target { return float4(1,0,0,1); }')
const sh = createRuntimeShader(eff).setColor('tint', 0xff00ff00)
const mod = M.background(shaderBrush(sh))
`)
    }, /COLOR/)
})

test('Vite .hlsl 双轨转换：Live 自接受 HMR 与 Dist 预编译 DXBC 及精确行号诊断', () => {
    const validHlsl = `#include <canton.hlsl>
cbuffer Params : register(b0) { float time; };
float4 main(CantonInput input) : SV_Target {
    return float4(input.uv, sin(time) * 0.5 + 0.5, 1.0);
}
`
    const emittedAssets = new Set<string>()
    const serveOut = transformHlslModule(validHlsl, resolve(process.cwd(), 'test-demo.hlsl'), true, emittedAssets)
    assert.match(serveOut, /registerHlslRuntimeEffect/)
    assert.match(serveOut, /import\.meta\.hot\.accept/)
    assert.match(serveOut, /updateHlslRuntimeEffect/)

    let emittedFile = ''
    let emittedBytes: Uint8Array | undefined
    const buildOut = transformHlslModule(validHlsl, resolve(process.cwd(), 'test-demo.hlsl'), false, emittedAssets, {
        emitFile(file) {
            emittedFile = file.fileName
            emittedBytes = file.source
            return file.fileName
        },
    })
    assert.match(emittedFile, /^assets\/test-demo-[0-9a-f]+\.dxbc$/)
    assert.ok(emittedBytes && emittedBytes.length > 64)
    assert.equal(Buffer.from(emittedBytes.subarray(0, 4)).toString(), 'DXBC')
    assert.match(buildOut, /loadHlslRuntimeEffectAsset/)

    // 语法错误必须包含文件名与准确行号
    const brokenHlsl = `#include <canton.hlsl>
float4 main(CantonInput input) : SV_Target {
    float4 x = unknownIdent + 1.0;
    return x;
}
`
    assert.throws(() => compileHlslToDxbc(brokenHlsl, { sourceName: 'broken-line-check.hlsl' }), /broken-line-check\.hlsl.*\(3,/)
})

test('RuntimeEffect 热更报错保留旧版着色器且不抛异常中断宿主', () => {
    let shouldFail = false
    let updateCount = 0
    const fake = createFakeNative({
        updateRuntimeEffect: () => {
            if (shouldFail) return { ok: false, error: 'HLSL compile error at line 4' }
            updateCount++
            return { ok: true }
        },
    })
    const prev = globalThis.__CANTON_NATIVE__
    globalThis.__CANTON_NATIVE__ = fake
    try {
        const effect = internal.registerHlslRuntimeEffect('hot-test.hlsl', 'float4 main() : SV_Target { return float4(1,0,0,1); }')
        const shader = ui.createRuntimeShader(effect).setFloat('t', 1.5).setColor('c', 0xff336699)
        const revBefore = effect.revision

        shouldFail = true
        const okFail = internal.updateHlslRuntimeEffect('hot-test.hlsl', 'broken hlsl')
        assert.equal(okFail, false)
        assert.equal(updateCount, 0)
        assert.equal(effect.revision, revBefore)

        shouldFail = false
        const okRecover = internal.updateHlslRuntimeEffect('hot-test.hlsl', 'float4 main() : SV_Target { return float4(0,1,0,1); }')
        assert.equal(okRecover, true)
        assert.equal(updateCount, 1)
        assert.ok(effect.revision > revBefore)
        shader.dispose()
    } finally {
        globalThis.__CANTON_NATIVE__ = prev
    }
})

test('ColorMatrix [0..255] 偏置与 RenderEffect 链及 drawWithCache 自动销毁 RenderLayer / RuntimeShader', () => {
    const mat = new ui.ColorMatrix()
    mat.setToSaturation(0)
    assert.ok(Math.abs(mat.values[0]! - 0.213) < 1e-4)
    assert.equal(mat.values[18], 1)

    const filter = createColorControlsColorFilter(0.1, 1.2, 1.5)
    assert.equal(filter.type, 'colorMatrix')
    assert.ok(Math.abs(filter.matrix[4]! - (0.5 - 1.2 * 0.5 + 0.1) * 255) < 1e-3)

    const chain = ui.RenderEffect.chain(ui.RenderEffect.blur(8, 8, ui.TileMode.Clamp), ui.RenderEffect.offset(2, 3), ui.RenderEffect.colorFilter(filter))
    assert.equal(renderEffectSnapshot(chain)?.steps.length, 3)

    let releasedLayers = 0
    const fake = createFakeNative({
        releaseRenderLayer: () => {
            releasedLayers++
        },
    })
    const prev = globalThis.__CANTON_NATIVE__
    globalThis.__CANTON_NATIVE__ = fake
    try {
        const eff = ui.createRuntimeEffect('float4 main() : SV_Target { return float4(1,1,1,1); }')
        let capturedLayer: ui.RenderLayer | undefined
        let capturedShader: ui.RuntimeShader | undefined
        let customDisposed = 0

        const seenWidths: number[] = []
        const scope = runtime.effectScope()
        scope.run(() => {
            const mod = ui.M.drawWithCache(({ size, density, onDispose }) => {
                seenWidths.push(size.width)
                assert.equal(density.dpToPx(10), 10)
                capturedLayer = ui.createRenderLayer()
                capturedShader = ui.createRuntimeShader(eff)
                onDispose(() => {
                    customDisposed++
                })
                return {
                    onDrawBehind(draw) {
                        assert.ok(capturedLayer)
                        draw.recordLayer(capturedLayer, () => {
                            draw.drawRect({ color: 0xffff0000 })
                        })
                        draw.drawLayer(capturedLayer)
                    },
                }
            })
            const el = mod.elements[0]!
            const inst = new DrawInstance(el.type, el.value as never)
            inst.prepare(120, 80)
            assert.equal(capturedLayer?.isDisposed, false)
            assert.equal(capturedShader?.isDisposed, false)
            // 尺寸变化重建缓存时自动释放旧 RenderLayer 与 RuntimeShader
            inst.prepare(200, 100)
            assert.equal(customDisposed, 1)
            assert.ok(releasedLayers >= 1)
            inst.stop()
        })
        scope.stop()
        assert.deepEqual(seenWidths, [120, 200])
        assert.ok(customDisposed >= 2)
    } finally {
        globalThis.__CANTON_NATIVE__ = prev
    }
})

test('LayoutCoordinates 旋转/缩放祖先跨节点坐标换算、同窗口校验与离树报错', () => {
    const parentToken = { attached: true }
    const parent = createLayoutCoordinates(
        {
            windowId: 1,
            width: 400,
            height: 300,
            a: 0,
            b: 2,
            c: -2,
            d: 0,
            tx: 100,
            ty: 50,
            parentX: 0,
            parentY: 0,
            clipX: 0,
            clipY: 0,
            clipWidth: 400,
            clipHeight: 300,
        },
        parentToken,
    )
    const child = createLayoutCoordinates(
        {
            windowId: 1,
            width: 60,
            height: 40,
            a: 0,
            b: 2,
            c: -2,
            d: 0,
            tx: 80,
            ty: 90,
            parentX: 20,
            parentY: 10,
            clipX: 0,
            clipY: 0,
            clipWidth: 400,
            clipHeight: 300,
        },
        { attached: true },
    )
    // parent.localToWindow(20, 10) = (0*20 - 2*10 + 100, 2*20 + 0*10 + 50) = (80, 90) == child origin!
    const winPt = parent.localToWindow({ x: 20, y: 10 })
    assert.ok(Math.abs(winPt.x - 80) < 1e-5 && Math.abs(winPt.y - 90) < 1e-5)
    const back = parent.windowToLocal(winPt)
    assert.ok(Math.abs(back.x - 20) < 1e-5 && Math.abs(back.y - 10) < 1e-5)

    const rel = parent.localPositionOf(child)
    assert.ok(Math.abs(rel.x - 20) < 1e-5 && Math.abs(rel.y - 10) < 1e-5)

    // 跨窗口报错
    const otherWin = createLayoutCoordinates(
        { windowId: 2, width: 100, height: 100, a: 1, b: 0, c: 0, d: 1, tx: 0, ty: 0, parentX: 0, parentY: 0, clipX: 0, clipY: 0, clipWidth: 100, clipHeight: 100 },
        { attached: true },
    )
    assert.throws(() => parent.localPositionOf(otherWin), /跨窗口/)

    // 离树报错
    parentToken.attached = false
    assert.equal(parent.isAttached, false)
    assert.throws(() => parent.positionInWindow(), /卸载/)
})

test('kyant-glass InverseLayerScope 逆向抵消旋转与非等比缩放矩阵精确性', () => {
    const scope = new InverseLayerScope()
    scope.evaluate(
        { width: 200, height: 100 },
        { dpToPx: (v) => v, pxToDp: (v) => v, spToPx: (v) => v, pxToSp: (v) => v },
        (layer) => {
            layer.rotationZ = 37
            layer.scaleX = 1.6
            layer.scaleY = 0.75
        },
    )
    const inv = scope.computeInverseMatrix()
    assert.ok(inv)
    const rad = (37 * Math.PI) / 180
    const a = Math.cos(rad) * 1.6
    const b = Math.sin(rad) * 1.6
    const c = -Math.sin(rad) * 0.75
    const d = Math.cos(rad) * 0.75
    // M * inv == I
    assert.ok(Math.abs(a * inv.a + c * inv.b - 1) < 1e-5)
    assert.ok(Math.abs(a * inv.c + c * inv.d) < 1e-5)
    assert.ok(Math.abs(b * inv.a + d * inv.b) < 1e-5)
    assert.ok(Math.abs(b * inv.c + d * inv.d - 1) < 1e-5)
})
