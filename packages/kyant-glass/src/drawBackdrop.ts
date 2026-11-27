import { inject, shallowRef } from '@canton/framework'
import { CompositingStrategy, GraphicsEnvironmentKey, M, createRenderLayer, type DrawCallback, type DrawScope, type GraphicsEnvironment, type LayoutCoordinates, type Modifier, type Shape } from '@canton/framework/ui'
import type { Backdrop, LayerBackdrop } from './backdrop.ts'
import { BackdropEffectScope } from './effects.ts'
import { Highlight, highlightModifier } from './highlight.ts'
import { InverseLayerScope, type BackdropLayerBlock } from './internal/inverseLayerScope.ts'
import { Shadow, innerShadowModifier, shadowModifier, type InnerShadow } from './shadow.ts'

export interface DrawPlainBackdropOptions {
    readonly backdrop: Backdrop
    readonly shape: () => Shape
    readonly effects: (scope: BackdropEffectScope) => void
    readonly layerBlock?: BackdropLayerBlock
    readonly exportedBackdrop?: LayerBackdrop | null
    readonly onDrawBehind?: DrawCallback
    readonly onDrawBackdrop?: (scope: DrawScope, drawBackdrop: () => void) => void
    readonly onDrawSurface?: DrawCallback
    readonly onDrawFront?: DrawCallback
    /** 可选显式覆写：为 true 时强制满血效果，为 false 时强制降级；省略时自动跟随 GraphicsEnvironmentKey.recommendedTier */
    readonly forceFullEffects?: boolean
}

export interface DrawBackdropOptions extends DrawPlainBackdropOptions {
    readonly highlight?: (() => Highlight | null | undefined) | null
    readonly shadow?: (() => Shadow | null | undefined) | null
    readonly innerShadow?: (() => InnerShadow | null | undefined) | null
}

const defaultHighlightGetter = () => Highlight.Default
const defaultShadowGetter = () => Shadow.Default
const defaultOnDrawBackdrop = (_scope: DrawScope, draw: () => void) => {
    draw()
}

function evaluateLayerBlock(layerBlock: BackdropLayerBlock) {
    const evalScope = new InverseLayerScope()
    evalScope.evaluate(
        { width: 0, height: 0 },
        { dpToPx: (d) => d, pxToDp: (p) => p, spToPx: (s) => s, pxToSp: (p) => p },
        layerBlock,
    )
    return evalScope.toGraphicsLayerOptions()
}

function resolveGraphicsEnvironment(): GraphicsEnvironment | undefined {
    try {
        return inject(GraphicsEnvironmentKey, undefined)
    } catch {
        return undefined
    }
}

function isReducedTier(options: DrawPlainBackdropOptions, env: GraphicsEnvironment | undefined): boolean {
    if (options.forceFullEffects === true) return false
    if (options.forceFullEffects === false) return true
    return env?.recommendedTier === 'reduced'
}

function buildDrawBackdropCore(options: DrawPlainBackdropOptions, env: GraphicsEnvironment | undefined): Modifier {
    const coordinatesRef = shallowRef<LayoutCoordinates | null>(null)
    const onDrawBackdrop = options.onDrawBackdrop ?? defaultOnDrawBackdrop

    return M.onGloballyPositioned((coords) => {
        if (coords.isAttached) {
            if (options.backdrop.isCoordinatesDependent) {
                coordinatesRef.value = coords
            } else if (coordinatesRef.value !== null) {
                coordinatesRef.value = null
            }
            if (options.exportedBackdrop) {
                options.exportedBackdrop.layerCoordinates.value = coords
            }
        } else {
            coordinatesRef.value = null
            if (options.exportedBackdrop) {
                options.exportedBackdrop.layerCoordinates.value = null
            }
        }
    }).drawWithCache(({ onDispose }) => {
        const backdropLayer = createRenderLayer()
        const effectScope = new BackdropEffectScope()
        onDispose(() => {
            backdropLayer.dispose()
            effectScope.dispose()
        })

        return {
            onDrawWithContent(scope: DrawScope, drawContent: () => void) {
                const currentShape = options.shape()
                const reduced = isReducedTier(options, env)

                if (reduced) {
                    // TODO: 液态玻璃 reduced 降级档的视觉回落样式待主人后续指导定稿
                    // 降级档（无驱 WARP / RDP / 系统关闭透明效果）：跳过离屏背景层录制、多 Pass 模糊与折射着色器，
                    // 暂回落为高对比度半透明表面与细高光描边，保证零重 Shader 开销且文字可读。
                    const drawReducedBody = (includeContent: boolean) => {
                        scope.clipOutline(currentShape, { x: 0, y: 0, width: scope.size.width, height: scope.size.height }, () => {
                            options.onDrawBehind?.(scope)
                            scope.drawOutline(currentShape, {
                                x: 0,
                                y: 0,
                                width: scope.size.width,
                                height: scope.size.height,
                                color: 0xcc1e2533,
                            })
                            options.onDrawSurface?.(scope)
                            if (includeContent) drawContent()
                            options.onDrawFront?.(scope)
                            scope.drawOutline(currentShape, {
                                x: 0,
                                y: 0,
                                width: scope.size.width,
                                height: scope.size.height,
                                color: 0x52ffffff,
                                strokeWidth: Math.max(1, scope.density.dpToPx(1)),
                            })
                        })
                    }
                    drawReducedBody(true)
                    if (options.exportedBackdrop) {
                        scope.recordLayer(options.exportedBackdrop.graphicsLayer, () => {
                            drawReducedBody(false)
                        })
                    }
                    return
                }

                effectScope.update(scope.size, scope.density, currentShape)
                effectScope.apply(options.effects)

                const pad = Math.max(0, Math.round(effectScope.padding))
                const coords = coordinatesRef.value

                backdropLayer.renderEffect = effectScope.renderEffect
                backdropLayer.topLeft = pad > 0 ? { x: -pad, y: -pad } : { x: 0, y: 0 }

                const recordW = Math.max(1, Math.ceil(scope.size.width) + pad * 2)
                const recordH = Math.max(1, Math.ceil(scope.size.height) + pad * 2)
                scope.recordLayer(backdropLayer, { width: recordW, height: recordH }, () => {
                    const drawBody = () => {
                        onDrawBackdrop(scope, () => {
                            options.backdrop.drawBackdrop({
                                scope,
                                density: scope.density,
                                coordinates: coords,
                                layerBlock: options.layerBlock,
                            })
                        })
                    }
                    if (pad > 0) {
                        scope.withTransform({ translationX: pad, translationY: pad }, drawBody)
                    } else {
                        drawBody()
                    }
                })

                const drawClippedBody = (includeContent: boolean) => {
                    scope.clipOutline(currentShape, { x: 0, y: 0, width: scope.size.width, height: scope.size.height }, () => {
                        options.onDrawBehind?.(scope)
                        scope.drawLayer(backdropLayer)
                        options.onDrawSurface?.(scope)
                        if (includeContent) drawContent()
                        options.onDrawFront?.(scope)
                    })
                }

                drawClippedBody(true)

                // exportedBackdrop：把本层玻璃的完整外观导出供上层玻璃级联采样
                if (options.exportedBackdrop) {
                    scope.recordLayer(options.exportedBackdrop.graphicsLayer, () => {
                        drawClippedBody(false)
                    })
                }
            },
        }
    })
}

export function drawPlainBackdrop(options: DrawPlainBackdropOptions): Modifier {
    const env = resolveGraphicsEnvironment()
    let modifier = M
    if (options.layerBlock) {
        const layerOpts = evaluateLayerBlock(options.layerBlock)
        modifier = modifier.graphicsLayer({
            ...layerOpts,
            compositingStrategy: layerOpts.compositingStrategy ?? CompositingStrategy.Offscreen,
        })
    }
    return modifier.then(buildDrawBackdropCore(options, env))
}

export function drawBackdrop(options: DrawBackdropOptions): Modifier {
    const env = resolveGraphicsEnvironment()
    const reduced = () => isReducedTier(options, env)
    const highlightGetter = options.highlight === undefined ? defaultHighlightGetter : options.highlight
    const shadowGetter = options.shadow === undefined ? defaultShadowGetter : options.shadow
    const innerShadowGetter = options.innerShadow ?? null

    let modifier = M
    if (options.layerBlock) {
        modifier = modifier.graphicsLayer(evaluateLayerBlock(options.layerBlock))
    }
    if (innerShadowGetter) {
        modifier = modifier.then(innerShadowModifier(options.shape, () => (reduced() ? null : innerShadowGetter())))
    }
    if (shadowGetter) {
        modifier = modifier.then(shadowModifier(options.shape, () => (reduced() ? null : shadowGetter())))
    }
    if (highlightGetter) {
        modifier = modifier.then(highlightModifier(options.shape, () => (reduced() ? null : highlightGetter())))
    }
    return modifier.then(buildDrawBackdropCore(options, env))
}
