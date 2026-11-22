import { isObject, toRawType } from '@canton/shared'
import { mutableHandlers, readonlyHandlers, shallowReactiveHandlers, shallowReadonlyHandlers } from './baseHandlers.ts'
import { mutableCollectionHandlers, readonlyCollectionHandlers, shallowCollectionHandlers, shallowReadonlyCollectionHandlers } from './collectionHandlers.ts'
import { ReactiveFlags } from './constants.ts'
import { installProxyFactories, isReadonly, reactiveMap, readonlyMap, shallowReactiveMap, shallowReadonlyMap } from './target.ts'

// 经 reactive.ts 取用转换，可保证代理工厂已登记
export { toReactive, toReadonly } from './target.ts'
import type { DeepReadonly, Reactive, ShallowReactive, Target, UnwrapNestedRefs } from './types.ts'
import { warn } from './warning.ts'

enum TargetType {
    INVALID = 0,
    COMMON = 1,
    COLLECTION = 2,
}

function targetTypeMap(rawType: string) {
    switch (rawType) {
        case 'Object':
        case 'Array':
            return TargetType.COMMON
        case 'Map':
        case 'Set':
        case 'WeakMap':
        case 'WeakSet':
            return TargetType.COLLECTION
        default:
            return TargetType.INVALID
    }
}

function getTargetType(value: Target) {
    return value[ReactiveFlags.SKIP] || !Object.isExtensible(value) ? TargetType.INVALID : targetTypeMap(toRawType(value))
}

export function reactive<T extends object>(target: T): Reactive<T>
export function reactive(target: object) {
    if (isReadonly(target)) {
        return target
    }
    return createReactiveObject(target, false, mutableHandlers, mutableCollectionHandlers, reactiveMap)
}

export function shallowReactive<T extends object>(target: T): ShallowReactive<T> {
    return createReactiveObject(target, false, shallowReactiveHandlers, shallowCollectionHandlers, shallowReactiveMap)
}

export function readonly<T extends object>(target: T): DeepReadonly<UnwrapNestedRefs<T>> {
    return createReactiveObject(target, true, readonlyHandlers, readonlyCollectionHandlers, readonlyMap)
}

export function shallowReadonly<T extends object>(target: T): Readonly<T> {
    return createReactiveObject(target, true, shallowReadonlyHandlers, shallowReadonlyCollectionHandlers, shallowReadonlyMap)
}

function createReactiveObject(target: Target, isReadonly: boolean, baseHandlers: ProxyHandler<any>, collectionHandlers: ProxyHandler<any>, proxyMap: WeakMap<Target, any>) {
    if (!isObject(target)) {
        if (__DEV__) {
            warn(`value cannot be made ${isReadonly ? 'readonly' : 'reactive'}: ${String(target)}`)
        }
        return target
    }
    if (
        target[ReactiveFlags.RAW] && !(isReadonly && target[ReactiveFlags.IS_REACTIVE])
    ) {
        return target
    }
    const targetType = getTargetType(target)
    if (targetType === TargetType.INVALID) {
        return target
    }
    const existingProxy = proxyMap.get(target)
    if (existingProxy) {
        return existingProxy
    }
    const proxy = new Proxy(target, targetType === TargetType.COLLECTION ? collectionHandlers : baseHandlers)
    proxyMap.set(target, proxy)
    return proxy
}

installProxyFactories(reactive, readonly)
