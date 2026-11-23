import { shallowRef, type ShallowRef } from '@canton/reactivity'
import { requireArgb } from '../values/color.ts'
import { TileMode, tileModeToIndex, type RuntimeShaderLike, type ShaderBrush } from '../values/graphics.ts'
import { Log } from '../native/diagnostics.ts'
import type { NativeBindingHandle, NativeTransactionTarget } from '../native/protocol.ts'
import { isPainter, painterSnapshot, type Painter, type PainterSnapshot } from './painter.ts'
import { registerDrawResourceCleanup } from './disposal.ts'

export interface RuntimeEffectOptions {
    entryPoint?: string
    debugName?: string
}

export interface RuntimeEffect {
    readonly debugName: string
    readonly entryPoint: string
    readonly revision: number
    readonly isDisposed: boolean
    dispose(): void
}

interface RuntimeEffectInternal {
    readonly handle: NativeBindingHandle
    readonly revision: ShallowRef<number>
    disposed: boolean
    entryPoint: string
    debugName: string
}

const effectInternals = new WeakMap<RuntimeEffect, RuntimeEffectInternal>()
const hlslModuleRegistry = new Map<string, RuntimeEffect>()

function requireNative(feature: string): NativeTransactionTarget {
    const native = globalThis.__CANTON_NATIVE__
    if (!native) throw new Error(`${feature} 需要原生运行时`)
    return native
}

function makeRuntimeEffect(handle: NativeBindingHandle, entryPoint: string, debugName: string): RuntimeEffect {
    const internal: RuntimeEffectInternal = {
        handle,
        revision: shallowRef(1),
        disposed: false,
        entryPoint,
        debugName,
    }
    const effect: RuntimeEffect = Object.freeze({
        get debugName() { return internal.debugName },
        get entryPoint() { return internal.entryPoint },
        get revision() { return internal.revision.value },
        get isDisposed() { return internal.disposed },
        dispose() {
            if (internal.disposed) return
            internal.disposed = true
            internal.revision.value++
            globalThis.__CANTON_NATIVE__?.releaseRuntimeEffect(internal.handle)
        },
    })
    effectInternals.set(effect, internal)
    return effect
}

export function isRuntimeEffect(value: unknown): value is RuntimeEffect {
    return typeof value === 'object' && value !== null && effectInternals.has(value as RuntimeEffect)
}

export function runtimeEffectInternal(effect: RuntimeEffect): RuntimeEffectInternal {
    const internal = effectInternals.get(effect)
    if (!internal) throw new TypeError('需要 createRuntimeEffect 或 .hlsl 导入创建的 RuntimeEffect')
    return internal
}

export function createRuntimeEffect(hlslSource: string, options: RuntimeEffectOptions = {}): RuntimeEffect {
    if (typeof hlslSource !== 'string' || !hlslSource.trim()) throw new TypeError('createRuntimeEffect 需要非空 HLSL 源码字符串')

    const entryPoint = options.entryPoint ?? 'main'
    if (typeof entryPoint !== 'string' || !entryPoint) throw new TypeError('entryPoint 需要非空字符串')

    const debugName = options.debugName ?? '<inline.hlsl>'
    const native = requireNative('RuntimeEffect')
    const handle = native.compileRuntimeEffect(hlslSource, entryPoint, debugName)
    return makeRuntimeEffect(handle, entryPoint, debugName)
}

export function registerHlslRuntimeEffect(id: string, source: string, options: RuntimeEffectOptions = {}): RuntimeEffect {
    const existing = hlslModuleRegistry.get(id)
    if (existing && !existing.isDisposed) return existing

    const effect = createRuntimeEffect(source, options)
    hlslModuleRegistry.set(id, effect)
    return effect
}

export function updateHlslRuntimeEffect(id: string, source: string, options: RuntimeEffectOptions = {}): boolean {
    const existing = hlslModuleRegistry.get(id)
    if (!existing || existing.isDisposed) {
        registerHlslRuntimeEffect(id, source, options)
        return true
    }
    const internal = effectInternals.get(existing)!
    const entryPoint = options.entryPoint ?? internal.entryPoint
    const debugName = options.debugName ?? internal.debugName
    const native = requireNative('RuntimeEffect')
    const result = native.updateRuntimeEffect(internal.handle, source, entryPoint, debugName)
    if (result.ok) {
        internal.entryPoint = entryPoint
        internal.debugName = debugName
        internal.revision.value++
        Log.i('RuntimeEffect', 'HLSL 热替换已应用', debugName)
        return true
    }
    const detail = result.error ?? '未知编译错误'
    Log.e('RuntimeEffect', 'HLSL 热替换编译失败，保留上一版着色器', debugName, detail)
    native.diagnosticsToast('e', 'RuntimeEffect', `HLSL 编译失败：${debugName}`, [detail], true)
    return false
}

export function loadHlslRuntimeEffectAsset(assetPath: string, options: RuntimeEffectOptions = {}): RuntimeEffect {
    if (typeof assetPath !== 'string' || !assetPath) throw new TypeError('loadHlslRuntimeEffectAsset 需要非空资产路径')

    const entryPoint = options.entryPoint ?? 'main'
    const debugName = options.debugName ?? assetPath
    const native = requireNative('RuntimeEffect')
    const handle = native.loadRuntimeEffectAsset(assetPath, entryPoint, debugName)
    return makeRuntimeEffect(handle, entryPoint, debugName)
}

export type RuntimeShaderFilter = 'linear' | 'nearest'

export type RuntimeShaderSamplerOptions = Readonly<{
    tileModeX?: TileMode
    tileModeY?: TileMode
    filter?: RuntimeShaderFilter
}>

export interface RuntimeShader extends RuntimeShaderLike {
    readonly effect: RuntimeEffect
    readonly isDisposed: boolean
    setFloat(name: string, values: ArrayLike<number>): this
    setFloat(name: string, ...values: number[]): this
    setInt(name: string, values: ArrayLike<number>): this
    setInt(name: string, ...values: number[]): this
    setColor(name: string, color: number): this
    setPainter(name: string, painter: Painter | null, options?: RuntimeShaderSamplerOptions): this
    dispose(): void
}

export type RuntimeShaderUniformSnapshot =
    | Readonly<{ name: string; kind: 'float'; values: readonly number[] }>
    | Readonly<{ name: string; kind: 'int'; values: readonly number[] }>
    | Readonly<{ name: string; kind: 'color'; color: number }>

export type RuntimeShaderTextureSnapshot = Readonly<{
    name: string
    painter: PainterSnapshot
    tileModeX: number
    tileModeY: number
    nearest: boolean
}>

export type RuntimeShaderSnapshot = Readonly<{
    type: 'shader'
    effect: NativeBindingHandle
    effectRevision: number
    version: number
    uniforms: readonly RuntimeShaderUniformSnapshot[]
    textures: readonly RuntimeShaderTextureSnapshot[]
}>

type UniformEntry =
    | { kind: 'float'; values: number[] }
    | { kind: 'int'; values: number[] }
    | { kind: 'color'; color: number }

type PainterBinding = {
    painter: Painter
    tileModeX: number
    tileModeY: number
    nearest: boolean
}

const shaderSnapshots = new WeakMap<RuntimeShaderLike, () => RuntimeShaderSnapshot>()

export function isRuntimeShader(value: unknown): value is RuntimeShader {
    return typeof value === 'object' && value !== null && shaderSnapshots.has(value as RuntimeShaderLike)
}

export function runtimeShaderSnapshot(shader: RuntimeShaderLike): RuntimeShaderSnapshot {
    const read = shaderSnapshots.get(shader)
    if (!read) throw new TypeError('需要 createRuntimeShader 创建的 RuntimeShader')
    return read()
}

function readNumberList(args: readonly (number | ArrayLike<number>)[], label: string, integer: boolean): number[] {
    if (args.length === 0) throw new TypeError(`${label} 至少需要一个数值`)
    const source: ArrayLike<number> = args.length === 1 && typeof args[0] === 'object' && args[0] !== null ? args[0] : (args as readonly number[])
    if (!source.length) throw new TypeError(`${label} 至少需要一个数值`)
    const out = new Array<number>(source.length)
    for (let i = 0; i < source.length; i++) {
        const value = source[i]
        if (typeof value !== 'number' || !Number.isFinite(value)) throw new TypeError(`${label} 需要有限数值`)
        if (integer && !Number.isInteger(value)) throw new TypeError(`${label} 需要整数`)
        out[i] = integer ? (value | 0) : value
    }
    return out
}

function sameNumbers(a: readonly number[], b: readonly number[]): boolean {
    if (a.length !== b.length) return false
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false
    return true
}

export function createRuntimeShader(effect: RuntimeEffect): RuntimeShader {
    const effectInfo = runtimeEffectInternal(effect)
    if (effectInfo.disposed) throw new Error('不能为已释放的 RuntimeEffect 创建 RuntimeShader')
    const uniforms = new Map<string, UniformEntry>()
    const textures = new Map<string, PainterBinding>()
    const version = shallowRef(1)
    let disposed = false

    const assertAlive = () => {
        if (disposed) throw new Error('RuntimeShader 已释放')
        if (effectInfo.disposed) throw new Error('RuntimeShader 关联的 RuntimeEffect 已释放')
    }

    const checkName = (name: string) => {
        if (typeof name !== 'string' || !name) throw new TypeError('着色器参数名必须是非空字符串')
    }

    const shader: RuntimeShader = Object.freeze({
        __cantonRuntimeShader: true as const,
        get effect() { return effect },
        get isDisposed() { return disposed },
        setFloat(name: string, ...args: (number | ArrayLike<number>)[]) {
            assertAlive()
            checkName(name)
            const values = readNumberList(args, `setFloat(${name})`, false)
            const prev = uniforms.get(name)
            if (prev && prev.kind === 'float' && sameNumbers(prev.values, values)) return shader
            uniforms.set(name, { kind: 'float', values })
            version.value++
            return shader
        },
        setInt(name: string, ...args: (number | ArrayLike<number>)[]) {
            assertAlive()
            checkName(name)
            const values = readNumberList(args, `setInt(${name})`, true)
            const prev = uniforms.get(name)
            if (prev && prev.kind === 'int' && sameNumbers(prev.values, values)) return shader
            uniforms.set(name, { kind: 'int', values })
            version.value++
            return shader
        },
        setColor(name: string, color: number) {
            assertAlive()
            checkName(name)
            const argb = requireArgb(color)
            const prev = uniforms.get(name)
            if (prev && prev.kind === 'color' && prev.color === argb) return shader
            uniforms.set(name, { kind: 'color', color: argb })
            version.value++
            return shader
        },
        setPainter(name: string, painter: Painter | null, options: RuntimeShaderSamplerOptions = {}) {
            assertAlive()
            checkName(name)
            if (painter === null) {
                if (textures.delete(name)) version.value++
                return shader
            }
            if (!isPainter(painter)) throw new TypeError(`setPainter(${name}) 需要有效的 Painter 或 null`)
            const tileModeX = tileModeToIndex(options.tileModeX ?? TileMode.Clamp)
            const tileModeY = tileModeToIndex(options.tileModeY ?? TileMode.Clamp)
            const filter = options.filter ?? 'linear'
            if (filter !== 'linear' && filter !== 'nearest') throw new TypeError(`setPainter(${name}) filter 只能是 'linear' 或 'nearest'`)
            const nearest = filter === 'nearest'
            const prev = textures.get(name)
            if (prev && prev.painter === painter && prev.tileModeX === tileModeX && prev.tileModeY === tileModeY && prev.nearest === nearest) return shader
            textures.set(name, { painter, tileModeX, tileModeY, nearest })
            version.value++
            return shader
        },
        dispose() {
            if (disposed) return
            disposed = true
            uniforms.clear()
            textures.clear()
            version.value++
        },
    })

    shaderSnapshots.set(shader, () => {
        const currentVersion = version.value
        const effectRevision = effectInfo.revision.value
        assertAlive()
        const uniformList: RuntimeShaderUniformSnapshot[] = []
        for (const [name, entry] of uniforms) {
            if (entry.kind === 'float') uniformList.push(Object.freeze({ name, kind: 'float', values: Object.freeze([...entry.values]) }))
            else if (entry.kind === 'int') uniformList.push(Object.freeze({ name, kind: 'int', values: Object.freeze([...entry.values]) }))
            else uniformList.push(Object.freeze({ name, kind: 'color', color: entry.color }))
        }
        const textureList: RuntimeShaderTextureSnapshot[] = []
        for (const [name, binding] of textures) {
            const snapshot = painterSnapshot(binding.painter)
            textureList.push(Object.freeze({ name, painter: snapshot, tileModeX: binding.tileModeX, tileModeY: binding.tileModeY, nearest: binding.nearest }))
        }
        return Object.freeze({
            type: 'shader',
            effect: effectInfo.handle,
            effectRevision,
            version: currentVersion,
            uniforms: Object.freeze(uniformList),
            textures: Object.freeze(textureList),
        })
    })

    registerDrawResourceCleanup(() => shader.dispose())
    return shader
}

export function shaderBrush(shader: RuntimeShader): ShaderBrush {
    if (!isRuntimeShader(shader)) throw new TypeError('shaderBrush 需要 createRuntimeShader 创建的 RuntimeShader')
    return Object.freeze({ type: 'shader', shader })
}
