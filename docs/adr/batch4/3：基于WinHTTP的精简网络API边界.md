# 003 基于 WinHTTP 的精简网络 API 边界

日期 2026-11-27。

> 本文承接 [batch1 ADR 008](../batch1/8：应用路径、只读资产与文件存储边界.md) 与 [batch2 ADR 007](../batch2/7：第三方依赖的引入规矩.md)，确定 Canton 面向应用层的网络请求、下载落盘与 TS / Native 双侧分工。

## 背景

桌面应用普遍需要 HTTP/HTTPS 请求与文件下载能力。M3 阶段宿主已在 `native/src/runtime/` 中基于 Windows 原生 WinHTTP 实现了同步 `httpGet` 与 `HttpCancellation` 取消句柄，用于 Dev/Live 拉取资源；现需将其整理为正式、简洁、异步且安全的应用级网络接口。

## 决定

- **独立子模块 `@canton/framework/net`（与 `@canton/framework/fs` 成对）**：底层统一使用 Windows 原生 WinHTTP，请求全部在宿主后台 I/O 线程池异步执行，不阻塞 UI 与渲染帧，不引入第三方网络库。
- **简洁桌面 HTTP 语义，不照搬完整浏览器 `fetch()`**：
  - 提供 `request` 及 `http.get` / `post` / `put` / `patch` / `delete` / `head`，支持 `query` 自动编码、`json` 序列化、自定义请求头、总超时、响应字节上限（防撑爆 QuickJS 内存）、可选跟随重定向与 Windows 系统代理；
  - 接入 [ADR 002](2：定时器、异步延迟与取消原语.md) 的 `AbortSignal`，直接透传到底层 WinHTTP 取消句柄即时断连；
  - 响应体回传至 JS 时已完整就绪，`res.text()`、`res.json()`、`res.bytes()` 为同步调用，免去二次 `await`。
- **大文件直落盘 `downloadFile`**：配合 `@canton/framework/fs` 的真实磁盘路径，由后台线程流式写入临时文件并在完成后原子替换目标文件，附带经宿主节流的进度回调，避免大文件经 QuickJS 内存拷贝。
- **结构化错误 `NetError`**：统一抛出带明确 `code` 的 `NetError`（区分非法 URL、网络/TLS 失败、超时、主动取消、响应超限与落盘 I/O 错误）。
- **克制边界与分期落地**：首版聚焦 HTTP/HTTPS，不内置持久化 Cookie 罐、隐式 HTTP 缓存或原始 TCP/UDP Socket（WebSocket 视后续需求再评估是否基于 WinHTTP 原生能力追加）。**暂定 M6 先做 TS 侧，M7 在 `canton-sdk` 补齐 Native 侧 `ctx.net()`**，共用同一套底层 WinHTTP 实现；高吞吐流式传输与自定义协议由 Native 扩展自行使用 Win32 或第三方库处理。

## API 契约

导出自 **`@canton/framework/net`**（M6 先落地 TS 侧）：

```ts
export type HttpMethod = 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE' | 'HEAD' | 'OPTIONS'

export type NetErrorCode =
    | 'INVALID_URL'        // 非 http/https 或 URL 格式非法
    | 'NETWORK'            // DNS、连接、TLS 握手失败、传输中断或重定向超限
    | 'TIMEOUT'            // 超过 timeoutMs 总时限
    | 'ABORTED'            // AbortSignal 主动取消
    | 'RESPONSE_TOO_LARGE' // 响应体超过 maxBytes 上限
    | 'IO'                 // downloadFile 本地文件创建、写入或替换失败

export class NetError extends Error {
    readonly code: NetErrorCode
    readonly url: string
    readonly cause?: unknown
}

export interface HttpRequestOptions {
    method?: HttpMethod                                           // 默认 'GET'
    headers?: Record<string, string>                              // 大小写不敏感
    query?: Record<string, string | number | boolean | undefined> // 自动百分号编码并拼接到 URL（忽略 undefined）
    body?: string | Uint8Array | ArrayBuffer                      // 原始请求体（与 json 互斥）
    json?: unknown                                                // 自动 JSON.stringify 并补 Content-Type: application/json
    timeoutMs?: number                                            // 总时限，默认 15000 ms；0 表示不限
    maxBytes?: number                                             // 响应上限，默认 32 MB
    followRedirects?: boolean                                     // 自动跟随 3xx，默认 true（上限 10 次）
    useSystemProxy?: boolean                                      // 跟随 Windows 系统代理，默认 true
    signal?: AbortSignal                                          // 映射到 WinHTTP 取消句柄即时断连
}

export interface HttpResponse {
    readonly status: number
    readonly ok: boolean                                          // 200 <= status < 300
    readonly url: string                                          // 最终 URL（重定向后）
    readonly headers: Readonly<Record<string, string>>            // key 统一小写；同名多头以 ", " 合并
    readonly contentType: string                                  // 归一化小写、不含参数的 MIME 类型
    text(): string                                                // 同步 UTF-8 解码
    json<T = unknown>(): T                                        // 同步 JSON.parse（非法 JSON 抛 SyntaxError）
    bytes(): Uint8Array                                           // 原始字节视图
}

export function request(url: string, options?: HttpRequestOptions): Promise<HttpResponse>

export const http: {
    readonly request: typeof request
    get(url: string, options?: Omit<HttpRequestOptions, 'method' | 'body' | 'json'>): Promise<HttpResponse>
    post(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse>
    put(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse>
    patch(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse>
    delete(url: string, options?: Omit<HttpRequestOptions, 'method'>): Promise<HttpResponse>
    head(url: string, options?: Omit<HttpRequestOptions, 'method' | 'body' | 'json'>): Promise<HttpResponse>
}

export interface DownloadProgress {
    readonly receivedBytes: number
    readonly totalBytes: number | null                            // 无 Content-Length 时为 null
}

export interface DownloadOptions extends Omit<HttpRequestOptions, 'method' | 'body' | 'json' | 'maxBytes'> {
    destPath: string                                              // 目标真实磁盘路径
    maxBytes?: number                                             // 可选下载字节上限（缺省不限）
    onProgress?: (progress: DownloadProgress) => void             // 经宿主节流派发（约 50~100ms 及完成时必发）
}

export interface DownloadResult {
    readonly status: number
    readonly ok: boolean
    readonly url: string
    readonly destPath: string
    readonly bytesWritten: number
}

export function downloadFile(url: string, options: DownloadOptions): Promise<DownloadResult>
```

- **大体行为**：
  - **非 2xx 不自动抛错**：404 / 500 等合法 HTTP 响应正常 resolve，由业务检查 `res.ok` / `res.status`；仅在非法输入、网络/TLS 异常、超时、取消、超限或写盘失败时 reject `NetError`。
  - **参数与安全约束**：同时传入 `body` 与 `json` 或在 `GET`/`HEAD` 上带请求体抛 `TypeError`；仅放行 `http://` 与 `https://`；跨域重定向时自动剥离 `Authorization` 与 `Cookie` 请求头。
  - **取消即时性**：`signal` 已取消时直接 reject `NetError('ABORTED')` 不建连接；进行中取消触发底层 WinHTTP 取消句柄立刻中止阻塞 I/O 并清理资源。
  - **`downloadFile` 原子落盘**：后台线程流式写入 `destPath` 同目录临时文件（不经 QuickJS 内存）；仅当响应 `ok === true` 且传输完整后刷盘并原子替换至 `destPath`；若服务端返回非 2xx 则删除临时文件且不覆盖原目标，resolve `{ ok: false, status, bytesWritten: 0, ... }`；若发生网络错误、超时或取消则清理临时文件并 reject `NetError`。
  - **M7 Native 侧预留（`canton-sdk`）**：`ctx.net()` 提供对称的同步/异步 `request`、`downloadFile` 与取消令牌，共享同一套底层 WinHTTP 封装。

## 为什么不选别的

- **完整实现 WHATWG `fetch()` 与 `ReadableStream`**：规范体量庞大，且 CORS、ServiceWorker、Credentials、Opaque Response 等 Web 机制在桌面客户端中既无实际意义，又带来不必要的封装复杂度和二次 `await` 心智负担。
- **引入 libcurl / OpenSSL 等第三方库**：违背 [batch2 ADR 007](../batch2/7：第三方依赖的引入规矩.md)“系统 API 能做的不引依赖”原则，会增大预编译宿主包体并背上根证书库更新负担；WinHTTP 直接复用 Windows 系统证书与代理配置。

## 代价

- 强依赖浏览器全局 `fetch` 的第三方 JS 包无法直接免改使用，需改用 `@canton/framework/net` 或自行做薄适配。
- 常规 `request` 受响应字节上限约束，大文件必须走 `downloadFile` 或 M7 的 Native 扩展。
