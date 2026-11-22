import { def, hasOwn, isObject } from '@canton/shared'
import { ReactiveFlags } from './constants.ts'
import type { DeepReadonly, Raw, Ref, Target } from './types.ts'

export const reactiveMap: WeakMap<Target, any> = new WeakMap<Target, any>()
export const shallowReactiveMap: WeakMap<Target, any> = new WeakMap<Target, any>()
export const readonlyMap: WeakMap<Target, any> = new WeakMap<Target, any>()
export const shallowReadonlyMap: WeakMap<Target, any> = new WeakMap<Target, any>()

export function isReactive(value: unknown): boolean {
    if (isReadonly(value)) {
        return isReactive((value as Target)[ReactiveFlags.RAW])
    }
    return !!(value && (value as Target)[ReactiveFlags.IS_REACTIVE])
}

export function isReadonly(value: unknown): boolean {
    return !!(value && (value as Target)[ReactiveFlags.IS_READONLY])
}

export function isShallow(value: unknown): boolean {
    return !!(value && (value as Target)[ReactiveFlags.IS_SHALLOW])
}

export function isProxy(value: any): boolean {
    return value ? !!value[ReactiveFlags.RAW] : false
}

export function toRaw<T>(observed: T): T {
    const raw = observed && (observed as Target)[ReactiveFlags.RAW]
    return raw ? toRaw(raw) : observed
}

export function markRaw<T extends object>(value: T): Raw<T> {
    if (!hasOwn(value, ReactiveFlags.SKIP) && Object.isExtensible(value)) {
        def(value, ReactiveFlags.SKIP, true)
    }
    return value
}

export function isRef<T>(r: Ref<T> | unknown): r is Ref<T>
export function isRef(r: any): r is Ref {
    return r ? r[ReactiveFlags.IS_REF] === true : false
}

// 代理只能由 reactive.ts 创建，它在求值时登记深层转换，处理器经此取用而不反向依赖 reactive.ts
let reactiveProxy: (target: object) => object = () => { throw new Error('响应式代理工厂尚未登记，需先求值 reactive.ts') }
let readonlyProxy: (target: object) => object = reactiveProxy

export function installProxyFactories(reactive: (target: object) => object, readonly: (target: object) => object): void {
    reactiveProxy = reactive
    readonlyProxy = readonly
}

export function toReactive<T>(value: T): T {
    return isObject(value) ? reactiveProxy(value) as T : value
}

export function toReadonly<T>(value: T): DeepReadonly<T> {
    return isObject(value) ? readonlyProxy(value) as DeepReadonly<T> : value as DeepReadonly<T>
}
