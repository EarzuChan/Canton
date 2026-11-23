import { BlendMode, CompositingStrategy, M, RenderEffect, TileMode, createRenderLayer, type DrawScope, type Modifier, type Shape } from '@canton/framework/ui'

export interface ShadowOffsetDp {
    readonly xDp: number
    readonly yDp: number
}

export interface ShadowOptions {
    radiusDp?: number
    offset?: Partial<ShadowOffsetDp>
    color?: number
    alpha?: number
    blendMode?: BlendMode
}

export interface Shadow {
    readonly radiusDp: number
    readonly offset: ShadowOffsetDp
    readonly color: number
    readonly alpha: number
    readonly blendMode: BlendMode
}

export function createShadow(options: ShadowOptions = {}): Shadow {
    const radiusDp = options.radiusDp ?? 24
    return Object.freeze({
        radiusDp,
        offset: Object.freeze({
            xDp: options.offset?.xDp ?? 0,
            yDp: options.offset?.yDp ?? radiusDp / 6,
        }),
        color: options.color ?? 0x1a000000, // Black 0.10
        alpha: options.alpha ?? 1,
        blendMode: options.blendMode ?? BlendMode.SrcOver,
    })
}

export const Shadow = Object.freeze({
    create: createShadow,
    Default: createShadow(),
})

export interface InnerShadowOptions {
    radiusDp?: number
    offset?: Partial<ShadowOffsetDp>
    color?: number
    alpha?: number
    blendMode?: BlendMode
}

export interface InnerShadow {
    readonly radiusDp: number
    readonly offset: ShadowOffsetDp
    readonly color: number
    readonly alpha: number
    readonly blendMode: BlendMode
}

export function createInnerShadow(options: InnerShadowOptions = {}): InnerShadow {
    const radiusDp = options.radiusDp ?? 24
    return Object.freeze({
        radiusDp,
        offset: Object.freeze({
            xDp: options.offset?.xDp ?? 0,
            yDp: options.offset?.yDp ?? radiusDp,
        }),
        color: options.color ?? 0x26000000, // Black 0.15
        alpha: options.alpha ?? 1,
        blendMode: options.blendMode ?? BlendMode.SrcOver,
    })
}

export const InnerShadow = Object.freeze({
    create: createInnerShadow,
    Default: createInnerShadow(),
})

function lerpNumber(a: number, b: number, t: number): number {
    return a + (b - a) * t
}

function lerpColor(a: number, b: number, t: number): number {
    const aa = (a >>> 24) & 0xff
    const ar = (a >>> 16) & 0xff
    const ag = (a >>> 8) & 0xff
    const ab = a & 0xff
    const ba = (b >>> 24) & 0xff
    const br = (b >>> 16) & 0xff
    const bg = (b >>> 8) & 0xff
    const bb = b & 0xff
    const oa = Math.round(lerpNumber(aa, ba, t)) & 0xff
    const or = Math.round(lerpNumber(ar, br, t)) & 0xff
    const og = Math.round(lerpNumber(ag, bg, t)) & 0xff
    const ob = Math.round(lerpNumber(ab, bb, t)) & 0xff
    return ((oa << 24) | (or << 16) | (og << 8) | ob) >>> 0
}

export function lerpInnerShadow(start: InnerShadow, stop: InnerShadow, fraction: number): InnerShadow {
    const t = Math.max(0, Math.min(1, fraction))
    return Object.freeze({
        radiusDp: lerpNumber(start.radiusDp, stop.radiusDp, t),
        offset: Object.freeze({
            xDp: lerpNumber(start.offset.xDp, stop.offset.xDp, t),
            yDp: lerpNumber(start.offset.yDp, stop.offset.yDp, t),
        }),
        color: lerpColor(start.color, stop.color, t),
        alpha: lerpNumber(start.alpha, stop.alpha, t),
        blendMode: t < 0.5 ? start.blendMode : stop.blendMode,
    })
}

export function shadowModifier(shape: () => Shape, shadow: () => Shadow | null | undefined): Modifier {
    return M.drawWithCache(({ onDispose }) => {
        const blurLayer = createRenderLayer()
        const shadowLayer = createRenderLayer()
        shadowLayer.compositingStrategy = CompositingStrategy.Offscreen
        onDispose(() => {
            blurLayer.dispose()
            shadowLayer.dispose()
        })

        return {
            onDrawWithContent(scope: DrawScope, drawContent: () => void) {
                const cfg = shadow()
                if (cfg && cfg.alpha > 0) {
                    const currentShape = shape()
                    const { size, density } = scope
                    if (size.width > 0 && size.height > 0) {
                        const radiusPx = Math.max(0, density.dpToPx(cfg.radiusDp))
                        const offsetX = density.dpToPx(cfg.offset.xDp)
                        const offsetY = density.dpToPx(cfg.offset.yDp)
                        const pad = Math.ceil(radiusPx * 2)
                        const padX = pad + Math.ceil(Math.abs(offsetX))
                        const padY = pad + Math.ceil(Math.abs(offsetY))
                        const totalW = Math.ceil(size.width) + padX * 2
                        const totalH = Math.ceil(size.height) + padY * 2

                        blurLayer.renderEffect = radiusPx > 0 ? RenderEffect.blur(radiusPx, radiusPx, TileMode.Decal) : null
                        scope.recordLayer(blurLayer, { width: totalW, height: totalH }, () => {
                            scope.withTransform({ translationX: padX + offsetX, translationY: padY + offsetY }, () => {
                                scope.drawOutline(currentShape, {
                                    x: 0,
                                    y: 0,
                                    width: size.width,
                                    height: size.height,
                                    color: cfg.color,
                                })
                            })
                        })

                        shadowLayer.alpha = cfg.alpha
                        shadowLayer.blendMode = cfg.blendMode
                        scope.recordLayer(shadowLayer, { width: totalW, height: totalH }, () => {
                            scope.drawLayer(blurLayer)
                            // 用 BlendMode.Clear 挖去玻璃本体，避免半透明玻璃内部被外阴影压暗
                            scope.withTransform({ translationX: padX, translationY: padY }, () => {
                                scope.drawOutline(currentShape, {
                                    x: 0,
                                    y: 0,
                                    width: size.width,
                                    height: size.height,
                                    color: 0xffffffff,
                                    blendMode: BlendMode.Clear,
                                })
                            })
                        })

                        scope.withTransform({ translationX: -padX, translationY: -padY }, () => {
                            scope.drawLayer(shadowLayer)
                        })
                    }
                }
                drawContent()
            },
        }
    })
}

export function innerShadowModifier(shape: () => Shape, shadow: () => InnerShadow | null | undefined): Modifier {
    return M.drawWithCache(({ onDispose }) => {
        const shadowLayer = createRenderLayer()
        shadowLayer.compositingStrategy = CompositingStrategy.Offscreen
        onDispose(() => {
            shadowLayer.dispose()
        })

        return {
            onDrawWithContent(scope: DrawScope, drawContent: () => void) {
                drawContent()

                const cfg = shadow()
                if (!cfg || cfg.alpha <= 0) return

                const currentShape = shape()
                const { size, density } = scope
                if (size.width <= 0 || size.height <= 0) return

                const radiusPx = Math.max(0, density.dpToPx(cfg.radiusDp))
                const offsetX = density.dpToPx(cfg.offset.xDp)
                const offsetY = density.dpToPx(cfg.offset.yDp)

                shadowLayer.alpha = cfg.alpha
                shadowLayer.blendMode = cfg.blendMode
                shadowLayer.renderEffect = radiusPx > 0 ? RenderEffect.blur(radiusPx, radiusPx, TileMode.Decal) : null

                scope.recordLayer(shadowLayer, () => {
                    scope.clipOutline(currentShape, { x: 0, y: 0, width: size.width, height: size.height }, () => {
                        scope.drawOutline(currentShape, {
                            x: 0,
                            y: 0,
                            width: size.width,
                            height: size.height,
                            color: cfg.color,
                        })
                        scope.withTransform({ translationX: offsetX, translationY: offsetY }, () => {
                            scope.drawOutline(currentShape, {
                                x: 0,
                                y: 0,
                                width: size.width,
                                height: size.height,
                                color: 0xffffffff,
                                blendMode: BlendMode.Clear,
                            })
                        })
                    })
                })

                scope.clipOutline(currentShape, { x: 0, y: 0, width: size.width, height: size.height }, () => {
                    scope.drawLayer(shadowLayer)
                })
            },
        }
    })
}
