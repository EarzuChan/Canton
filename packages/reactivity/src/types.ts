import { ReactiveFlags } from './constants.ts'

// 响应式的公共类型面：Ref、Computed、Reactive 的形状与解包规则，只含类型，不依赖任何实现
export interface Target {
    [ReactiveFlags.SKIP]?: boolean
    [ReactiveFlags.IS_REACTIVE]?: boolean
    [ReactiveFlags.IS_READONLY]?: boolean
    [ReactiveFlags.IS_SHALLOW]?: boolean
    [ReactiveFlags.RAW]?: any
}

declare const RefSymbol: unique symbol
export declare const RawSymbol: unique symbol

export interface Ref<T = any, S = T> {
    get value(): T
    set value(_: S)
    [RefSymbol]: true
}

declare const ShallowRefMarker: unique symbol

export type ShallowRef<T = any, S = T> = Ref<T, S> & {
    [ShallowRefMarker]?: true
}

declare const ComputedRefSymbol: unique symbol
declare const WritableComputedRefSymbol: unique symbol

interface BaseComputedRef<T, S = T> extends Ref<T, S> {
    [ComputedRefSymbol]: true
}

export interface ComputedRef<T = any> extends BaseComputedRef<T> {
    readonly value: T
}

export interface WritableComputedRef<T, S = T> extends BaseComputedRef<T, S> {
    [WritableComputedRefSymbol]: true
}

export type ComputedGetter<T> = (oldValue?: T) => T
export type ComputedSetter<T> = (newValue: T) => void

export interface WritableComputedOptions<T, S = T> {
    get: ComputedGetter<T>
    set: ComputedSetter<S>
}

export type MaybeRef<T = any> = | T | Ref<T> | ShallowRef<T> | WritableComputedRef<T>

export type MaybeRefOrGetter<T = any> = MaybeRef<T> | ComputedRef<T> | (() => T)

export type UnwrapNestedRefs<T> = T extends Ref ? T : UnwrapRefSimple<T>

declare const ReactiveMarkerSymbol: unique symbol

export interface ReactiveMarker {
    [ReactiveMarkerSymbol]?: void
}

export type Reactive<T> = UnwrapNestedRefs<T> &
    (T extends readonly any[] ? ReactiveMarker : {})

declare class ShallowReactiveBrandClass {
    private __shallowReactiveBrand?: never
}

export type ShallowReactiveBrand = ShallowReactiveBrandClass

export type ShallowReactive<T> = T & ShallowReactiveBrand

type Primitive = string | number | boolean | bigint | symbol | undefined | null
export type Builtin = Primitive | Function | Date | Error | RegExp
export type DeepReadonly<T> = T extends Builtin
    ? T
    : T extends Map<infer K, infer V>
    ? ReadonlyMap<DeepReadonly<K>, DeepReadonly<V>>
    : T extends ReadonlyMap<infer K, infer V>
    ? ReadonlyMap<DeepReadonly<K>, DeepReadonly<V>>
    : T extends WeakMap<infer K, infer V>
    ? WeakMap<DeepReadonly<K>, DeepReadonly<V>>
    : T extends Set<infer U>
    ? ReadonlySet<DeepReadonly<U>>
    : T extends ReadonlySet<infer U>
    ? ReadonlySet<DeepReadonly<U>>
    : T extends WeakSet<infer U>
    ? WeakSet<DeepReadonly<U>>
    : T extends Promise<infer U>
    ? Promise<DeepReadonly<U>>
    : T extends Ref<infer U, unknown>
    ? Readonly<Ref<DeepReadonly<U>>>
    : T extends {}
    ? { readonly [K in keyof T]: DeepReadonly<T[K]> }
    : Readonly<T>

export type Raw<T> = T & { [RawSymbol]?: true }

export interface RefUnwrapBailTypes { }

export type ShallowUnwrapRef<T> = T extends ShallowReactiveBrand ? T : { [K in keyof T]: DistributeRef<T[K]> }

type DistributeRef<T> = T extends Ref<infer V, unknown> ? V : T

export type UnwrapRef<T> = T extends ShallowRef<infer V, unknown> ? V : T extends Ref<infer V, unknown> ? UnwrapRefSimple<V> : UnwrapRefSimple<T>

export type UnwrapRefSimple<T> = T extends
    | Builtin
    | Ref
    | RefUnwrapBailTypes[keyof RefUnwrapBailTypes]
    | { [RawSymbol]?: true }
    ? T
    : T extends ShallowReactiveBrand
    ? T
    : T extends Map<infer K, infer V>
    ? Map<K, UnwrapRefSimple<V>> & UnwrapRef<Omit<T, keyof Map<any, any>>>
    : T extends WeakMap<infer K, infer V>
    ? WeakMap<K, UnwrapRefSimple<V>> &
    UnwrapRef<Omit<T, keyof WeakMap<any, any>>>
    : T extends Set<infer V>
    ? Set<UnwrapRefSimple<V>> & UnwrapRef<Omit<T, keyof Set<any>>>
    : T extends WeakSet<infer V>
    ? WeakSet<UnwrapRefSimple<V>> &
    UnwrapRef<Omit<T, keyof WeakSet<any>>>
    : T extends ReadonlyArray<any>
    ? { [K in keyof T]: UnwrapRefSimple<T[K]> }
    : T extends object
    ? {
        [P in keyof T]: P extends symbol ? T[P] : UnwrapRef<T[P]>
    }
    : T
