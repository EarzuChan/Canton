import type { NativeNetRequestPayload } from './protocol.ts'

export type HttpMethod = 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE' | 'HEAD' | 'OPTIONS'

export type NetErrorCode = | 'INVALID_URL' | 'NETWORK' | 'TIMEOUT' | 'ABORTED' | 'RESPONSE_TOO_LARGE' | 'IO'

export class NetError extends Error {
    readonly code: NetErrorCode
    readonly url: string
    override readonly cause?: unknown

    constructor(code: NetErrorCode, url: string, message: string, cause?: unknown) {
        super(message)
        this.name = 'NetError'
        this.code = code
        this.url = url
        this.cause = cause
    }
}

export interface HttpRequestOptions {
    readonly method?: HttpMethod
    readonly headers?: Readonly<Record<string, string>>
    readonly query?: Readonly<Record<string, string | number | boolean | undefined>>
    readonly body?: string | Uint8Array | ArrayBuffer
    readonly json?: unknown
    readonly timeoutMs?: number
    readonly maxBytes?: number
    readonly followRedirects?: boolean
    readonly useSystemProxy?: boolean
    readonly signal?: AbortSignal
}

export interface HttpResponse {
    readonly status: number
    readonly ok: boolean
    readonly url: string
    readonly headers: Readonly<Record<string, string>>
    readonly contentType: string
    text(): string
    json<T = unknown>(): T
    bytes(): Uint8Array
}

export interface DownloadProgress {
    readonly receivedBytes: number
    readonly totalBytes: number | null
}

export interface DownloadOptions extends Omit<HttpRequestOptions, 'method' | 'body' | 'json' | 'maxBytes'> {
    readonly destPath: string
    readonly maxBytes?: number
    readonly onProgress?: (progress: DownloadProgress) => void
}

export interface DownloadResult {
    readonly status: number
    readonly ok: boolean
    readonly url: string
    readonly destPath: string
    readonly bytesWritten: number
}

const allowedMethods = new Set<HttpMethod>(['GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD', 'OPTIONS'])
const defaultTimeoutMs = 15000
const defaultMaxBytes = 32 * 1024 * 1024
const defaultMaxRedirects = 10

let nextNetRequestId = 1

function decodeUtf8(bytes: Uint8Array): string {
    if (typeof TextDecoder !== 'undefined') {
        return new TextDecoder('utf-8').decode(bytes)
    }
    // QuickJS 环境回退纯 JS UTF-8 解码
    let out = ''
    let i = 0
    while (i < bytes.length) {
        const c = bytes[i++]!
        if (c < 0x80) {
            out += String.fromCharCode(c)
        } else if ((c & 0xe0) === 0xc0 && i < bytes.length) {
            const c2 = bytes[i++]!
            out += String.fromCharCode(((c & 0x1f) << 6) | (c2 & 0x3f))
        } else if ((c & 0xf0) === 0xe0 && i + 1 < bytes.length) {
            const c2 = bytes[i++]!
            const c3 = bytes[i++]!
            out += String.fromCharCode(((c & 0x0f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f))
        } else if ((c & 0xf8) === 0xf0 && i + 2 < bytes.length) {
            const c2 = bytes[i++]!
            const c3 = bytes[i++]!
            const c4 = bytes[i++]!
            const cp = (((c & 0x07) << 18) | ((c2 & 0x3f) << 12) | ((c3 & 0x3f) << 6) | (c4 & 0x3f)) - 0x10000
            out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff))
        } else {
            out += '\ufffd'
        }
    }
    return out
}

function buildUrlWithQuery(rawUrl: string, query?: Readonly<Record<string, string | number | boolean | undefined>>): string {
    if (typeof rawUrl !== 'string' || rawUrl.trim().length === 0) {
        throw new NetError('INVALID_URL', String(rawUrl ?? ''), 'URL 不能为空')
    }
    const trimmed = rawUrl.trim()
    const lower = trimmed.toLowerCase()
    if (!lower.startsWith('http://') && !lower.startsWith('https://')) {
        throw new NetError('INVALID_URL', trimmed, `仅支持 http:// 与 https:// 协议：${trimmed}`)
    }
    const afterScheme = trimmed.slice(trimmed.indexOf('://') + 3)
    if (!afterScheme || afterScheme.startsWith('/') || /[\s\r\n\0]/.test(trimmed)) {
        throw new NetError('INVALID_URL', trimmed, `URL 格式非法：${trimmed}`)
    }
    if (!query) return trimmed
    const pairs: string[] = []
    for (const [key, value] of Object.entries(query)) {
        if (value === undefined) continue
        pairs.push(`${encodeURIComponent(key)}=${encodeURIComponent(String(value))}`)
    }
    if (pairs.length === 0) return trimmed
    const hashIdx = trimmed.indexOf('#')
    const base = hashIdx >= 0 ? trimmed.slice(0, hashIdx) : trimmed
    const hash = hashIdx >= 0 ? trimmed.slice(hashIdx) : ''
    const sep = base.includes('?') ? (base.endsWith('?') || base.endsWith('&') ? '' : '&') : '?'
    return `${base}${sep}${pairs.join('&')}${hash}`
}

function buildRequestPayload(url: string, options: HttpRequestOptions, defaultMax: number): { finalUrl: string; payload: NativeNetRequestPayload } {
    const method = (options.method ?? 'GET') as HttpMethod
    if (!allowedMethods.has(method)) {
        throw new TypeError(`不支持的 HTTP 方法：${String(method)}`)
    }
    if (options.body !== undefined && options.json !== undefined) {
        throw new TypeError('HttpRequestOptions 的 body 与 json 不能同时传入')
    }
    if ((method === 'GET' || method === 'HEAD') && (options.body !== undefined || options.json !== undefined)) {
        throw new TypeError(`${method} 请求不允许携带请求体`)
    }
    const finalUrl = buildUrlWithQuery(url, options.query)

    const headers: Array<[string, string]> = []
    let hasContentType = false
    if (options.headers) {
        for (const [k, v] of Object.entries(options.headers)) {
            if (/[\r\n\0]/.test(k) || /[\r\n\0]/.test(v)) {
                throw new TypeError(`请求头包含非法换行字符：${k}`)
            }
            if (k.toLowerCase() === 'content-type') hasContentType = true
            headers.push([k, v])
        }
    }

    let body: string | Uint8Array | ArrayBuffer | undefined = options.body
    if (options.json !== undefined) {
        body = JSON.stringify(options.json)
        if (!hasContentType) headers.push(['Content-Type', 'application/json; charset=utf-8'])
    }

    const timeoutMs = options.timeoutMs === undefined ? defaultTimeoutMs : options.timeoutMs
    if (typeof timeoutMs !== 'number' || !Number.isFinite(timeoutMs) || timeoutMs < 0) {
        throw new RangeError('timeoutMs 必须是非负有限数值')
    }

    const maxBytes = options.maxBytes === undefined ? defaultMax : options.maxBytes
    if (typeof maxBytes !== 'number' || !Number.isFinite(maxBytes) || maxBytes < 0) {
        throw new RangeError('maxBytes 必须是非负有限数值')
    }

    return {
        finalUrl,
        payload: {
            method,
            url: finalUrl,
            headers,
            ...(body !== undefined ? { body } : {}),
            timeoutMs: Math.round(timeoutMs),
            maxBytes: Math.round(maxBytes),
            followRedirects: options.followRedirects ?? true,
            maxRedirects: defaultMaxRedirects,
            useSystemProxy: options.useSystemProxy ?? true,
        },
    }
}

export async function request(url: string, options: HttpRequestOptions = {}): Promise<HttpResponse> {
    const { finalUrl, payload } = buildRequestPayload(url, options, defaultMaxBytes)
    const signal = options.signal
    if (signal?.aborted) {
        throw new NetError('ABORTED', finalUrl, '请求在发起前已被取消', signal.reason)
    }

    const native = globalThis.__CANTON_NATIVE__
    if (!native?.netRequest) {
        throw new NetError('NETWORK', finalUrl, '当前运行时未挂载 WinHTTP 网络能力')
    }

    const reqId = nextNetRequestId++
    const onAbort = () => {
        native.netCancel?.(reqId)
    }
    signal?.addEventListener('abort', onAbort, { once: true })

    try {
        const res = await native.netRequest(reqId, payload)
        if (!res.ok) {
            const code = (res.errorCode ?? 'NETWORK') as NetErrorCode
            throw new NetError(code, res.errorUrl ?? finalUrl, res.errorMessage ?? '网络请求失败', code === 'ABORTED' ? signal?.reason : undefined)
        }
        const status = res.status ?? 0
        const rawBytes = res.body ?? new Uint8Array(0)
        const frozenHeaders = Object.freeze({ ...(res.headers ?? {}) })
        let cachedText: string | undefined

        return Object.freeze({
            status,
            ok: status >= 200 && status < 300,
            url: res.url ?? finalUrl,
            headers: frozenHeaders,
            contentType: res.contentType ?? '',
            text(): string {
                if (cachedText === undefined) cachedText = decodeUtf8(rawBytes)
                return cachedText
            },
            json<T = unknown>(): T {
                if (cachedText === undefined) cachedText = decodeUtf8(rawBytes)
                return JSON.parse(cachedText) as T
            },
            bytes(): Uint8Array {
                return rawBytes
            },
        })
    } finally {
        signal?.removeEventListener('abort', onAbort)
    }
}

export const http = Object.freeze({
    request,
    get(url: string, options?: Omit<HttpRequestOptions, 'method' | 'body' | 'json'>): Promise<HttpResponse> {
        return request(url, { ...options, method: 'GET' })
    },
    post(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse> {
        return request(url, { ...options, method: 'POST' })
    },
    put(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse> {
        return request(url, { ...options, method: 'PUT' })
    },
    patch(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse> {
        return request(url, { ...options, method: 'PATCH' })
    },
    delete(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse> {
        return request(url, { ...options, method: 'DELETE' })
    },
    head(url: string, options?: Omit<HttpRequestOptions, 'method' | 'body' | 'json'>): Promise<HttpResponse> {
        return request(url, { ...options, method: 'HEAD' })
    },
})

export async function downloadFile(url: string, options: DownloadOptions): Promise<DownloadResult> {
    if (!options || typeof options.destPath !== 'string' || options.destPath.trim().length === 0 || options.destPath.includes('\0')) {
        throw new TypeError('downloadFile 需要有效的 destPath 真实磁盘路径')
    }
    const { finalUrl, payload } = buildRequestPayload(url, { ...options, method: 'GET' }, 0)
    const signal = options.signal
    if (signal?.aborted) {
        throw new NetError('ABORTED', finalUrl, '下载在发起前已被取消', signal.reason)
    }

    const native = globalThis.__CANTON_NATIVE__
    if (!native?.netDownload) {
        throw new NetError('NETWORK', finalUrl, '当前运行时未挂载 WinHTTP 下载能力')
    }

    const reqId = nextNetRequestId++
    const onAbort = () => {
        native.netCancel?.(reqId)
    }
    signal?.addEventListener('abort', onAbort, { once: true })

    try {
        const res = await native.netDownload(
            reqId,
            payload,
            options.destPath,
            options.onProgress
                ? progress => options.onProgress?.(Object.freeze({ receivedBytes: progress.receivedBytes, totalBytes: progress.totalBytes }))
                : undefined,
        )
        if (!res.ok) {
            const code = (res.errorCode ?? 'NETWORK') as NetErrorCode
            throw new NetError(code, res.errorUrl ?? finalUrl, res.errorMessage ?? '文件下载失败', code === 'ABORTED' ? signal?.reason : undefined)
        }
        return Object.freeze({
            status: res.status ?? 0,
            ok: Boolean(res.downloadOk),
            url: res.url ?? finalUrl,
            destPath: res.destPath ?? options.destPath,
            bytesWritten: res.bytesWritten ?? 0,
        })
    } finally {
        signal?.removeEventListener('abort', onAbort)
    }
}
