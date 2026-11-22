import { pauseTracking, resetTracking } from '@canton/reactivity'
import { isPromise } from '@canton/shared'
import { LifecycleHooks } from './enums.ts'

// 错误上报只需要父链、捕获钩子、来源与应用级处理器
export interface ErrorOwner {
    readonly parent: ErrorOwner | null
    readonly hooks: ReadonlyMap<LifecycleHooks, Function[]>
    readonly source?: string
    readonly appContext: { readonly config: { errorHandler?: (error: unknown, source: string | undefined, phase: string) => void } }
}

export enum ErrorCodes {
    SCHEDULER = 10,
    ARRANGABLE_UPDATE,
}

export type ErrorTypes = ErrorCodes | LifecycleHooks | number

export function callWithErrorHandling<T>(fn: (...args: any[]) => T, instance: ErrorOwner | null | undefined, type: ErrorTypes, args: unknown[] = []): T | undefined {
    try { return fn(...args) } catch (error) { handleError(error, instance, type) }
}

export function callWithAsyncErrorHandling(fn: Function | Function[], instance: ErrorOwner | null | undefined, type: ErrorTypes, args: unknown[] = []): any {
    if (Array.isArray(fn)) return fn.map(callback => callWithAsyncErrorHandling(callback, instance, type, args))
    const result = callWithErrorHandling((...values) => fn(...values), instance, type, args)
    if (isPromise(result)) result.catch((error: unknown) => handleError(error, instance, type))
    return result
}

export function handleError(error: unknown, instance: ErrorOwner | null | undefined, type: ErrorTypes): void {
    pauseTracking()
    try {
        for (let parent = instance?.parent; parent; parent = parent.parent) {
            for (const hook of parent.hooks.get(LifecycleHooks.ERROR_CAPTURED) ?? []) if (hook(error, instance?.source, String(type)) === false) return
        }
        if (instance?.appContext.config.errorHandler) {
            instance.appContext.config.errorHandler(error, instance.source, String(type))
            return
        }
        throw error
    } finally {
        resetTracking()
    }
}
