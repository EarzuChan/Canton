import { isColorFilter, TileMode, tileModeToIndex, type ColorFilter } from '../values/graphics.ts'
import { isRuntimeShader, runtimeShaderSnapshot, type RuntimeShader, type RuntimeShaderSnapshot } from './shader.ts'

export type RenderEffectStepSnapshot =
    | Readonly<{ type: 'blur'; radiusX: number; radiusY: number; edgeTreatment: number }>
    | Readonly<{ type: 'offset'; x: number; y: number }>
    | Readonly<{ type: 'colorFilter'; filter: ColorFilter }>
    | Readonly<{ type: 'shader'; shader: RuntimeShaderSnapshot; uniformShaderName: string }>

export type RenderEffectSnapshot = Readonly<{
    steps: readonly RenderEffectStepSnapshot[]
}>

type RenderEffectNode =
    | { kind: 'blur'; radiusX: number; radiusY: number; edgeTreatment: number; input?: RenderEffect }
    | { kind: 'offset'; x: number; y: number; input?: RenderEffect }
    | { kind: 'colorFilter'; filter: ColorFilter; input?: RenderEffect }
    | { kind: 'shader'; shader: RuntimeShader; uniformShaderName: string; input?: RenderEffect }
    | { kind: 'chain'; first: RenderEffect; second: RenderEffect }

const effectNodes = new WeakMap<RenderEffect, RenderEffectNode>()

function finite(value: number, name: string): number {
    if (typeof value !== 'number' || !Number.isFinite(value)) throw new TypeError(`${name} 需要有限数值`)
    return value
}

function nonnegative(value: number, name: string): number {
    if (finite(value, name) < 0) throw new RangeError(`${name} 不能为负数`)
    return value
}

function checkedInput(input: RenderEffect | null | undefined): RenderEffect | undefined {
    if (input === undefined || input === null) return undefined
    if (!isRenderEffect(input)) throw new TypeError('RenderEffect 的 input 必须是 RenderEffect 实例')
    return input
}

export class RenderEffect {
    private constructor(node: RenderEffectNode) {
        effectNodes.set(this, node)
        Object.freeze(this)
    }

    then(next: RenderEffect): RenderEffect {
        if (!isRenderEffect(next)) throw new TypeError('RenderEffect.then 需要 RenderEffect 实例')
        return new RenderEffect({ kind: 'chain', first: this, second: next })
    }

    static blur(radiusX: number, radiusY: number = radiusX, edgeTreatment: TileMode = TileMode.Clamp, input?: RenderEffect | null): RenderEffect {
        return new RenderEffect({
            kind: 'blur',
            radiusX: nonnegative(radiusX, 'RenderEffect.blur radiusX'),
            radiusY: nonnegative(radiusY, 'RenderEffect.blur radiusY'),
            edgeTreatment: tileModeToIndex(edgeTreatment),
            input: checkedInput(input),
        })
    }

    static offset(x: number, y: number, input?: RenderEffect | null): RenderEffect {
        return new RenderEffect({
            kind: 'offset',
            x: finite(x, 'RenderEffect.offset x'),
            y: finite(y, 'RenderEffect.offset y'),
            input: checkedInput(input),
        })
    }

    static colorFilter(colorFilter: ColorFilter, input?: RenderEffect | null): RenderEffect {
        if (!isColorFilter(colorFilter)) throw new TypeError('RenderEffect.colorFilter 需要有效的 ColorFilter')
        return new RenderEffect({
            kind: 'colorFilter',
            filter: colorFilter,
            input: checkedInput(input),
        })
    }

    static shader(runtimeShader: RuntimeShader, uniformShaderName: string, input?: RenderEffect | null): RenderEffect {
        if (!isRuntimeShader(runtimeShader)) throw new TypeError('RenderEffect.shader 需要 RuntimeShader 实例')
        if (typeof uniformShaderName !== 'string' || !uniformShaderName) throw new TypeError('RenderEffect.shader 需要非空纹理槽名 uniformShaderName')
        return new RenderEffect({
            kind: 'shader',
            shader: runtimeShader,
            uniformShaderName,
            input: checkedInput(input),
        })
    }

    static chain(first: RenderEffect, second: RenderEffect, ...rest: RenderEffect[]): RenderEffect {
        if (!isRenderEffect(first) || !isRenderEffect(second)) throw new TypeError('RenderEffect.chain 需要 RenderEffect 实例')
        let current = first.then(second)
        for (const item of rest) current = current.then(item)
        return current
    }
}

export function isRenderEffect(value: unknown): value is RenderEffect {
    return typeof value === 'object' && value !== null && effectNodes.has(value as RenderEffect)
}

function collectSteps(effect: RenderEffect, out: RenderEffectStepSnapshot[]): void {
    const node = effectNodes.get(effect)
    if (!node) throw new TypeError('无效的 RenderEffect')
    if (node.kind === 'chain') {
        collectSteps(node.first, out)
        collectSteps(node.second, out)
        return
    }
    if (node.input) collectSteps(node.input, out)
    if (node.kind === 'blur') {
        out.push(Object.freeze({ type: 'blur', radiusX: node.radiusX, radiusY: node.radiusY, edgeTreatment: node.edgeTreatment }))
    } else if (node.kind === 'offset') {
        out.push(Object.freeze({ type: 'offset', x: node.x, y: node.y }))
    } else if (node.kind === 'colorFilter') {
        out.push(Object.freeze({ type: 'colorFilter', filter: node.filter }))
    } else {
        out.push(Object.freeze({ type: 'shader', shader: runtimeShaderSnapshot(node.shader), uniformShaderName: node.uniformShaderName }))
    }
}

export function renderEffectSnapshot(effect: RenderEffect | null | undefined): RenderEffectSnapshot | undefined {
    if (effect === undefined || effect === null) return undefined
    if (!isRenderEffect(effect)) throw new TypeError('renderEffect 必须是 RenderEffect 或 null')
    const steps: RenderEffectStepSnapshot[] = []
    collectSteps(effect, steps)
    return Object.freeze({ steps: Object.freeze(steps) })
}
