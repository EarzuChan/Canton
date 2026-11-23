import { shallowRef, type ShallowRef } from '@canton/reactivity'
import type { Density } from '../values/density.ts'
import { BlendMode, blendModeToIndex, CompositingStrategy, compositingStrategyToIndex, isColorFilter, type ColorFilter } from '../values/graphics.ts'
import { RectangleShape, type Shape } from '../values/primitives.ts'
import type { NativeBindingHandle } from '../native/protocol.ts'
import { isRenderEffect, renderEffectSnapshot, type RenderEffect, type RenderEffectSnapshot } from './renderEffect.ts'
import { registerDrawResourceCleanup } from './disposal.ts'

export type TransformOriginValue =
    | 'TopStart'
    | 'TopCenter'
    | 'TopEnd'
    | 'CenterStart'
    | 'Center'
    | 'CenterEnd'
    | 'BottomStart'
    | 'BottomCenter'
    | 'BottomEnd'
    | Readonly<{ x?: number; y?: number }>

export type LayerOffsetPx = Readonly<{ x: number; y: number }>
export type LayerSizePx = Readonly<{ width: number; height: number }>

export interface RenderLayer {
    alpha: number
    blendMode: BlendMode
    colorFilter: ColorFilter | null
    renderEffect: RenderEffect | null
    clip: boolean
    shape: Shape
    translationX: number
    translationY: number
    scaleX: number
    scaleY: number
    rotationZ: number
    transformOrigin: TransformOriginValue
    topLeft: LayerOffsetPx
    compositingStrategy: CompositingStrategy
    readonly size: LayerSizePx
    readonly isReleased: boolean
    readonly isDisposed: boolean
    dispose(): void
}

export type LoweredShapePx =
    | Readonly<{ type: 'rectangle' | 'circle' }>
    | Readonly<{ type: 'rounded'; topStart: number; topEnd: number; bottomEnd: number; bottomStart: number }>

export type RenderLayerSnapshot = Readonly<{
    identity: bigint
    generation: bigint
    version: number
    alpha: number
    blendMode: number
    colorFilter?: ColorFilter
    renderEffect?: RenderEffectSnapshot
    clip: boolean
    shape: LoweredShapePx
    translationX: number
    translationY: number
    scaleX: number
    scaleY: number
    rotationZ: number
    originX: number
    originY: number
    topLeftX: number
    topLeftY: number
    compositingStrategy: number
}>

interface RenderLayerInternal {
    readonly handle: NativeBindingHandle
    readonly version: ShallowRef<number>
    size: LayerSizePx
    disposed: boolean
    snapshot(density: Density): RenderLayerSnapshot
}

const layerInternals = new WeakMap<RenderLayer, RenderLayerInternal>()

function finite(value: number, name: string): number {
    if (typeof value !== 'number' || !Number.isFinite(value)) throw new TypeError(`${name} 需要有限数值`)
    return value
}

export function resolveTransformOrigin(origin: TransformOriginValue): readonly [number, number] {
    if (typeof origin === 'string') {
        switch (origin) {
            case 'TopStart': return [0, 0]
            case 'TopCenter': return [0.5, 0]
            case 'TopEnd': return [1, 0]
            case 'CenterStart': return [0, 0.5]
            case 'Center': return [0.5, 0.5]
            case 'CenterEnd': return [1, 0.5]
            case 'BottomStart': return [0, 1]
            case 'BottomCenter': return [0.5, 1]
            case 'BottomEnd': return [1, 1]
            default: throw new TypeError(`transformOrigin 无效：${String(origin)}`)
        }
    }
    if (typeof origin === 'object' && origin !== null) {
        return [finite(origin.x ?? 0.5, 'transformOrigin.x'), finite(origin.y ?? 0.5, 'transformOrigin.y')]
    }
    throw new TypeError('transformOrigin 需要预设字符串或 { x, y } 对象')
}

export function lowerShapeToPx(shape: Shape, density: Density): LoweredShapePx {
    if (shape.type === 'rounded') {
        return Object.freeze({
            type: 'rounded',
            topStart: density.dpToPx(shape.topStartDp) + shape.topStartPx,
            topEnd: density.dpToPx(shape.topEndDp) + shape.topEndPx,
            bottomEnd: density.dpToPx(shape.bottomEndDp) + shape.bottomEndPx,
            bottomStart: density.dpToPx(shape.bottomStartDp) + shape.bottomStartPx,
        })
    }
    return Object.freeze({ type: shape.type })
}

export function isRenderLayer(value: unknown): value is RenderLayer {
    return typeof value === 'object' && value !== null && layerInternals.has(value as RenderLayer)
}

export function renderLayerHandle(layer: RenderLayer): NativeBindingHandle {
    const internal = layerInternals.get(layer)
    if (!internal) throw new TypeError('需要 createRenderLayer 创建的 RenderLayer')
    if (internal.disposed) throw new Error('RenderLayer 已释放')
    return internal.handle
}

export function setRenderLayerRecordedSize(layer: RenderLayer, width: number, height: number): void {
    const internal = layerInternals.get(layer)
    if (!internal) throw new TypeError('需要 createRenderLayer 创建的 RenderLayer')
    if (internal.disposed) throw new Error('RenderLayer 已释放')
    if (internal.size.width !== width || internal.size.height !== height) {
        internal.size = Object.freeze({ width, height })
    }
}

export function renderLayerSnapshot(layer: RenderLayer, density: Density): RenderLayerSnapshot {
    const internal = layerInternals.get(layer)
    if (!internal) throw new TypeError('需要 createRenderLayer 创建的 RenderLayer')
    return internal.snapshot(density)
}

export function createRenderLayer(): RenderLayer {
    const native = globalThis.__CANTON_NATIVE__
    if (!native) throw new Error('RenderLayer 需要原生运行时')
    const handle = native.createRenderLayer()
    const version = shallowRef(1)
    let alpha = 1
    let blendMode: BlendMode = BlendMode.SrcOver
    let colorFilter: ColorFilter | null = null
    let renderEffect: RenderEffect | null = null
    let clip = false
    let shape: Shape = RectangleShape
    let translationX = 0
    let translationY = 0
    let scaleX = 1
    let scaleY = 1
    let rotationZ = 0
    let transformOrigin: TransformOriginValue = 'Center'
    let topLeft: LayerOffsetPx = Object.freeze({ x: 0, y: 0 })
    let compositingStrategy: CompositingStrategy = CompositingStrategy.Auto

    const internal: RenderLayerInternal = {
        handle,
        version,
        size: Object.freeze({ width: 0, height: 0 }),
        disposed: false,
        snapshot(density: Density): RenderLayerSnapshot {
            const currentVersion = version.value
            if (internal.disposed) throw new Error('RenderLayer 已释放')
            const [originX, originY] = resolveTransformOrigin(transformOrigin)
            return Object.freeze({
                identity: handle.identity,
                generation: handle.generation,
                version: currentVersion,
                alpha,
                blendMode: blendModeToIndex(blendMode),
                ...(colorFilter ? { colorFilter } : {}),
                ...(renderEffect ? { renderEffect: renderEffectSnapshot(renderEffect) } : {}),
                clip,
                shape: lowerShapeToPx(shape, density),
                translationX,
                translationY,
                scaleX,
                scaleY,
                rotationZ,
                originX,
                originY,
                topLeftX: topLeft.x,
                topLeftY: topLeft.y,
                compositingStrategy: compositingStrategyToIndex(compositingStrategy),
            })
        },
    }

    const assertAlive = () => {
        if (internal.disposed) throw new Error('RenderLayer 已释放')
    }

    const layer: RenderLayer = {
        get alpha() { return alpha },
        set alpha(value: number) {
            assertAlive()
            const next = finite(value, 'RenderLayer.alpha')
            if (next < 0 || next > 1) throw new RangeError('RenderLayer.alpha 需要在 0 到 1 之间')
            if (Object.is(alpha, next)) return
            alpha = next
            version.value++
        },
        get blendMode() { return blendMode },
        set blendMode(value: BlendMode) {
            assertAlive()
            blendModeToIndex(value)
            if (blendMode === value) return
            blendMode = value
            version.value++
        },
        get colorFilter() { return colorFilter },
        set colorFilter(value: ColorFilter | null) {
            assertAlive()
            if (value !== null && !isColorFilter(value)) throw new TypeError('RenderLayer.colorFilter 需要 ColorFilter 或 null')
            if (colorFilter === value) return
            colorFilter = value
            version.value++
        },
        get renderEffect() { return renderEffect },
        set renderEffect(value: RenderEffect | null) {
            assertAlive()
            if (value !== null && !isRenderEffect(value)) throw new TypeError('RenderLayer.renderEffect 需要 RenderEffect 或 null')
            if (renderEffect === value) return
            renderEffect = value
            version.value++
        },
        get clip() { return clip },
        set clip(value: boolean) {
            assertAlive()
            const next = Boolean(value)
            if (clip === next) return
            clip = next
            version.value++
        },
        get shape() { return shape },
        set shape(value: Shape) {
            assertAlive()
            if (!value || typeof value !== 'object' || (value.type !== 'rectangle' && value.type !== 'circle' && value.type !== 'rounded')) {
                throw new TypeError('RenderLayer.shape 需要有效的 Shape')
            }
            if (shape === value) return
            shape = value
            version.value++
        },
        get translationX() { return translationX },
        set translationX(value: number) {
            assertAlive()
            const next = finite(value, 'RenderLayer.translationX')
            if (Object.is(translationX, next)) return
            translationX = next
            version.value++
        },
        get translationY() { return translationY },
        set translationY(value: number) {
            assertAlive()
            const next = finite(value, 'RenderLayer.translationY')
            if (Object.is(translationY, next)) return
            translationY = next
            version.value++
        },
        get scaleX() { return scaleX },
        set scaleX(value: number) {
            assertAlive()
            const next = finite(value, 'RenderLayer.scaleX')
            if (Object.is(scaleX, next)) return
            scaleX = next
            version.value++
        },
        get scaleY() { return scaleY },
        set scaleY(value: number) {
            assertAlive()
            const next = finite(value, 'RenderLayer.scaleY')
            if (Object.is(scaleY, next)) return
            scaleY = next
            version.value++
        },
        get rotationZ() { return rotationZ },
        set rotationZ(value: number) {
            assertAlive()
            const next = finite(value, 'RenderLayer.rotationZ')
            if (Object.is(rotationZ, next)) return
            rotationZ = next
            version.value++
        },
        get transformOrigin() { return transformOrigin },
        set transformOrigin(value: TransformOriginValue) {
            assertAlive()
            resolveTransformOrigin(value)
            if (transformOrigin === value) return
            transformOrigin = value
            version.value++
        },
        get topLeft() { return topLeft },
        set topLeft(value: LayerOffsetPx) {
            assertAlive()
            if (!value || typeof value !== 'object') throw new TypeError('RenderLayer.topLeft 需要 { x, y }')
            const x = finite(value.x, 'RenderLayer.topLeft.x')
            const y = finite(value.y, 'RenderLayer.topLeft.y')
            if (Object.is(topLeft.x, x) && Object.is(topLeft.y, y)) return
            topLeft = Object.freeze({ x, y })
            version.value++
        },
        get compositingStrategy() { return compositingStrategy },
        set compositingStrategy(value: CompositingStrategy) {
            assertAlive()
            compositingStrategyToIndex(value)
            if (compositingStrategy === value) return
            compositingStrategy = value
            version.value++
        },
        get size() { return internal.size },
        get isReleased() { return internal.disposed },
        get isDisposed() { return internal.disposed },
        dispose() {
            if (internal.disposed) return
            internal.disposed = true
            version.value++
            globalThis.__CANTON_NATIVE__?.releaseRenderLayer(handle)
        },
    }

    layerInternals.set(layer, internal)
    registerDrawResourceCleanup(() => layer.dispose())
    return layer
}
