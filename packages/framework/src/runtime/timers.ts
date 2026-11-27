import { getCurrentScope, onScopeDispose } from '@canton/reactivity'

export interface TimerHandle {
    readonly active: boolean
    cancel(): void
}

function sanitizeDelay(ms: number): number {
    return typeof ms === 'number' && Number.isFinite(ms) && ms > 0 ? ms : 0
}

function createAbortError(reason?: unknown): unknown {
    if (reason !== undefined) return reason
    const error = new Error('The operation was aborted')
    error.name = 'AbortError'
    return error
}

export function useTimeout(fn: () => void, delayMs: number): TimerHandle {
    if (typeof fn !== 'function') throw new TypeError('useTimeout 回调必须是函数')
    let active = true
    const id = setTimeout(() => {
        active = false
        fn()
    }, sanitizeDelay(delayMs))

    const cancel = () => {
        if (!active) return
        active = false
        clearTimeout(id)
    }

    if (getCurrentScope()) {
        onScopeDispose(cancel)
    }

    return Object.freeze({
        get active() { return active },
        cancel,
    })
}

export function useInterval(fn: () => void, intervalMs: number): TimerHandle {
    if (typeof fn !== 'function') throw new TypeError('useInterval 回调必须是函数')
    let active = true
    const id = setInterval(() => {
        if (!active) return
        fn()
    }, sanitizeDelay(intervalMs))

    const cancel = () => {
        if (!active) return
        active = false
        clearInterval(id)
    }

    if (getCurrentScope()) {
        onScopeDispose(cancel)
    }

    return Object.freeze({
        get active() { return active },
        cancel,
    })
}

export function delay(ms: number, signal?: AbortSignal): Promise<void> {
    if (signal?.aborted) {
        return Promise.reject(createAbortError(signal.reason))
    }

    return new Promise<void>((resolve, reject) => {
        const onAbort = () => {
            clearTimeout(id)
            reject(createAbortError(signal?.reason))
        }
        const id = setTimeout(() => {
            signal?.removeEventListener('abort', onAbort)
            resolve()
        }, sanitizeDelay(ms))

        signal?.addEventListener('abort', onAbort, { once: true })
    })
}
