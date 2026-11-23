import { getCurrentScope, onScopeDispose } from '@canton/reactivity'

// drawWithCache 构建期间托管内部创建的 RuntimeShader / RenderLayer，确保缓存重算或卸载自动释放
const cacheCleanupStack: Array<Array<() => void>> = []

export function runWithDrawCacheDisposal<T>(cleanups: Array<() => void>, fn: () => T): T {
    cacheCleanupStack.push(cleanups)
    try {
        return fn()
    } finally {
        cacheCleanupStack.pop()
    }
}

export function registerDrawResourceCleanup(cleanup: () => void): void {
    const activeCache = cacheCleanupStack[cacheCleanupStack.length - 1]
    if (activeCache) {
        activeCache.push(cleanup)
        return
    }
    if (getCurrentScope()) onScopeDispose(cleanup)
}
