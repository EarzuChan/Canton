import { BlendMode, M, RenderEffect, TileMode, createRenderLayer, createRuntimeShader, shaderBrush, type DrawScope, type Modifier, type RuntimeShader, type Shape } from '@canton/framework/ui'
import { resolveShapeCornerRadiiPx } from './internal/shapeCorners.ts'
import highlightDefaultEffect from './shaders/highlightDefault.hlsl'
import highlightAmbientEffect from './shaders/highlightAmbient.hlsl'

export type HighlightStyle =
    | Readonly<{
        kind: 'plain'
        color: number
        blendMode: BlendMode
    }>
    | Readonly<{
        kind: 'default'
        color: number
        blendMode: BlendMode
        angle: number
        falloff: number
    }>
    | Readonly<{
        kind: 'ambient'
        intensity: number
        color: number
        blendMode: BlendMode
    }>

export const HighlightStyle = Object.freeze({
    plain(options?: { color?: number; blendMode?: BlendMode }): HighlightStyle {
        return Object.freeze({
            kind: 'plain',
            color: options?.color ?? 0x61ffffff, // White 0.38
            blendMode: options?.blendMode ?? BlendMode.Plus,
        })
    },
    default(options?: { color?: number; blendMode?: BlendMode; angle?: number; falloff?: number }): HighlightStyle {
        return Object.freeze({
            kind: 'default',
            color: options?.color ?? 0x80ffffff, // White 0.50
            blendMode: options?.blendMode ?? BlendMode.Plus,
            angle: options?.angle ?? 45,
            falloff: options?.falloff ?? 1,
        })
    },
    ambient(options?: { intensity?: number }): HighlightStyle {
        const intensity = Math.max(0, Math.min(1, options?.intensity ?? 0.38))
        const a = Math.round(intensity * 255)
        return Object.freeze({
            kind: 'ambient',
            intensity,
            color: ((a & 0xff) << 24) | 0x00ffffff,
            blendMode: BlendMode.SrcOver,
        })
    },
    Default: Object.freeze({
        kind: 'default' as const,
        color: 0x80ffffff,
        blendMode: BlendMode.Plus,
        angle: 45,
        falloff: 1,
    }),
    Ambient: Object.freeze({
        kind: 'ambient' as const,
        intensity: 0.38,
        color: 0x61ffffff,
        blendMode: BlendMode.SrcOver,
    }),
    Plain: Object.freeze({
        kind: 'plain' as const,
        color: 0x61ffffff,
        blendMode: BlendMode.Plus,
    }),
})

export interface HighlightOptions {
    widthDp?: number
    blurRadiusDp?: number
    alpha?: number
    style?: HighlightStyle
}

export interface Highlight {
    readonly widthDp: number
    readonly blurRadiusDp: number
    readonly alpha: number
    readonly style: HighlightStyle
}

export function createHighlight(options: HighlightOptions = {}): Highlight {
    const widthDp = options.widthDp ?? 0.5
    return Object.freeze({
        widthDp,
        blurRadiusDp: options.blurRadiusDp ?? widthDp * 0.5,
        alpha: options.alpha ?? 1,
        style: options.style ?? HighlightStyle.Default,
    })
}

export const Highlight = Object.freeze({
    create: createHighlight,
    Default: createHighlight(),
    Ambient: createHighlight({ style: HighlightStyle.Ambient }),
    Plain: createHighlight({ style: HighlightStyle.Plain }),
})

function withOpaqueAlpha(color: number): number {
    return (0xff000000 | (color & 0x00ffffff)) >>> 0
}

function alphaChannel(color: number): number {
    return ((color >>> 24) & 0xff) / 255
}

export function highlightModifier(shape: () => Shape, highlight: () => Highlight | null | undefined): Modifier {
    return M.drawWithCache(({ onDispose }) => {
        const layer = createRenderLayer()
        let defaultShader: RuntimeShader | null = null
        let ambientShader: RuntimeShader | null = null
        onDispose(() => {
            layer.dispose()
            defaultShader?.dispose()
            ambientShader?.dispose()
            defaultShader = null
            ambientShader = null
        })

        return {
            onDrawWithContent(scope: DrawScope, drawContent: () => void) {
                drawContent()

                const cfg = highlight()
                if (!cfg || cfg.widthDp <= 0 || cfg.alpha <= 0) return

                const currentShape = shape()
                const { size, density } = scope
                if (size.width <= 0 || size.height <= 0) return

                const widthPx = Math.min(density.dpToPx(cfg.widthDp), Math.min(size.width, size.height) * 0.5)
                const strokeWidth = Math.ceil(widthPx) * 2
                const blurPx = Math.max(0, density.dpToPx(cfg.blurRadiusDp))
                const safeW = Math.ceil(size.width) + 2
                const safeH = Math.ceil(size.height) + 2

                const style = cfg.style
                let brush = undefined
                let paintColor: number | undefined = style.color
                let styleAlpha = 1

                if (style.kind === 'default') {
                    defaultShader ??= createRuntimeShader(highlightDefaultEffect)
                    const corners = resolveShapeCornerRadiiPx(currentShape, size, density)
                    defaultShader.setFloat('size', size.width, size.height).setFloat('cornerRadii', corners[0], corners[1], corners[2], corners[3]).setColor('color', withOpaqueAlpha(style.color)).setFloat('angle', (style.angle * Math.PI) / 180).setFloat('falloff', style.falloff)
                    brush = shaderBrush(defaultShader)
                    paintColor = undefined
                    styleAlpha = alphaChannel(style.color)
                } else if (style.kind === 'ambient') {
                    ambientShader ??= createRuntimeShader(highlightAmbientEffect)
                    const corners = resolveShapeCornerRadiiPx(currentShape, size, density)
                    ambientShader.setFloat('size', size.width, size.height).setFloat('cornerRadii', corners[0], corners[1], corners[2], corners[3]).setFloat('angle', (45 * Math.PI) / 180).setFloat('falloff', 1)
                    brush = shaderBrush(ambientShader)
                    paintColor = undefined
                    styleAlpha = style.intensity
                }

                layer.alpha = Math.max(0, Math.min(1, cfg.alpha * styleAlpha))
                layer.blendMode = style.blendMode
                layer.renderEffect = blurPx > 0 ? RenderEffect.blur(blurPx, blurPx, TileMode.Decal) : null

                scope.recordLayer(layer, { width: safeW, height: safeH }, () => {
                    scope.withTransform({ translationX: 1, translationY: 1 }, () => {
                        scope.clipOutline(currentShape, { x: 0, y: 0, width: size.width, height: size.height }, () => {
                            scope.drawOutline(currentShape, {
                                x: 0,
                                y: 0,
                                width: size.width,
                                height: size.height,
                                color: paintColor,
                                brush,
                                strokeWidth,
                            })
                        })
                    })
                })

                scope.withTransform({ translationX: -1, translationY: -1 }, () => {
                    scope.drawLayer(layer)
                })
            },
        }
    })
}
