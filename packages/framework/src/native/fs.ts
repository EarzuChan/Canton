import type { NativeFsAsyncResponse } from './protocol.ts'

export type FsErrorCode = | 'NOT_FOUND' | 'ALREADY_EXISTS' | 'PERMISSION_DENIED' | 'IS_DIRECTORY' | 'NOT_DIRECTORY' | 'INVALID_PATH' | 'IO'

export class FsError extends Error {
    readonly code: FsErrorCode
    readonly path: string

    constructor(code: FsErrorCode, path: string, message: string) {
        super(message)
        this.name = 'FsError'
        this.code = code
        this.path = path
    }
}

export interface FsStat {
    readonly isFile: boolean
    readonly isDirectory: boolean
    readonly size: number
    readonly modifiedTimeMs: number
    readonly createdTimeMs: number
}

export interface FsDirEntry {
    readonly name: string
    readonly isFile: boolean
    readonly isDirectory: boolean
}

export interface WriteFileOptions {
    readonly atomic?: boolean
}

export interface MkdirOptions {
    readonly recursive?: boolean
}

export interface RemoveOptions {
    readonly recursive?: boolean
}

export interface CopyOptions {
    readonly overwrite?: boolean
}

function normalizePathSlashes(input: string): string {
    return input.replace(/\//g, '\\')
}

function normalizePath(input: string): string {
    if (!input) return '.'
    const replaced = normalizePathSlashes(input)
    let prefix = ''
    let rest = replaced
    if (rest.startsWith('\\\\?\\UNC\\')) {
        prefix = '\\\\?\\UNC\\'
        rest = rest.slice(8)
    } else if (rest.startsWith('\\\\?\\')) {
        prefix = '\\\\?\\'
        rest = rest.slice(4)
    } else if (rest.startsWith('\\\\')) {
        prefix = '\\\\'
        rest = rest.slice(2)
    } else if (/^[a-zA-Z]:/.test(rest)) {
        prefix = rest.slice(0, 2)
        rest = rest.slice(2)
        if (rest.startsWith('\\')) {
            prefix += '\\'
            rest = rest.slice(1)
        }
    } else if (rest.startsWith('\\')) {
        prefix = '\\'
        rest = rest.slice(1)
    }

    const isAbsolute = prefix.length > 0
    const segments = rest.split('\\')
    const stack: string[] = []
    for (const seg of segments) {
        if (!seg || seg === '.') continue
        if (seg === '..') {
            if (stack.length > 0 && stack[stack.length - 1] !== '..') {
                stack.pop()
            } else if (!isAbsolute) {
                stack.push('..')
            }
        } else {
            stack.push(seg)
        }
    }
    const joined = stack.join('\\')
    if (!prefix) return joined || '.'
    if (!joined) return prefix
    return prefix.endsWith('\\') ? prefix + joined : prefix + '\\' + joined
}

function joinPath(...parts: readonly string[]): string {
    const filtered = parts.filter(part => part.length > 0)
    if (filtered.length === 0) return '.'
    return normalizePath(filtered.join('\\'))
}

function splitDirBase(input: string): { dir: string; base: string } {
    const norm = normalizePath(input)
    if (norm === '.' || norm === '\\' || /^[a-zA-Z]:\\?$/.test(norm)) {
        return { dir: norm, base: '' }
    }
    const trimmed = norm.endsWith('\\') ? norm.slice(0, -1) : norm
    const idx = trimmed.lastIndexOf('\\')
    if (idx < 0) return { dir: '.', base: trimmed }
    const dir = idx === 0 ? '\\' : (/^[a-zA-Z]:$/.test(trimmed.slice(0, idx)) ? trimmed.slice(0, idx + 1) : trimmed.slice(0, idx))
    return { dir, base: trimmed.slice(idx + 1) }
}

function dirnamePath(input: string): string {
    return splitDirBase(input).dir
}

function basenamePath(input: string, ext?: string): string {
    const base = splitDirBase(input).base
    if (ext && base.endsWith(ext) && base.length > ext.length) {
        return base.slice(0, base.length - ext.length)
    }
    return base
}

function extnamePath(input: string): string {
    const base = splitDirBase(input).base
    const dot = base.lastIndexOf('.')
    if (dot <= 0) return ''
    return base.slice(dot)
}

function queryKnownPath(kind: string): string {
    const native = globalThis.__CANTON_NATIVE__
    if (!native?.getKnownPath) throw new FsError('IO', '', '当前运行时未挂载路径解析能力')
    return native.getKnownPath(kind)
}

export const paths = Object.freeze({
    appData: () => queryKnownPath('appData'),
    temp: () => queryKnownPath('temp'),
    downloads: () => queryKnownPath('downloads'),
    documents: () => queryKnownPath('documents'),
    desktop: () => queryKnownPath('desktop'),
    pictures: () => queryKnownPath('pictures'),
    music: () => queryKnownPath('music'),
    videos: () => queryKnownPath('videos'),
    join: joinPath,
    dirname: dirnamePath,
    basename: basenamePath,
    extname: extnamePath,
    normalize: normalizePath,
})

function assertPathString(path: unknown, label = 'path'): string {
    if (typeof path !== 'string' || path.length === 0 || path.includes('\0')) {
        throw new FsError('INVALID_PATH', String(path ?? ''), `${label} 必须是非空有效路径字符串`)
    }
    return path
}

async function callFs(op: string, path: string, path2 = '', bytes?: string | Uint8Array | ArrayBuffer, flag = false): Promise<NativeFsAsyncResponse> {
    const native = globalThis.__CANTON_NATIVE__
    if (!native?.fsAsync) throw new FsError('IO', path, '当前运行时未挂载异步文件系统')
    const res = await native.fsAsync(op, path, path2, bytes, flag)
    if (!res.ok) {
        const code = (res.errorCode ?? 'IO') as FsErrorCode
        throw new FsError(code, res.errorPath ?? path, res.errorMessage ?? `文件操作 ${op} 失败`)
    }
    return res
}

export async function readText(path: string): Promise<string> {
    const p = assertPathString(path)
    const res = await callFs('readText', p)
    return res.text ?? ''
}

export async function readBytes(path: string): Promise<Uint8Array> {
    const p = assertPathString(path)
    const res = await callFs('readBytes', p)
    return res.bytes ?? new Uint8Array(0)
}

export async function writeText(path: string, content: string, options?: WriteFileOptions): Promise<void> {
    const p = assertPathString(path)
    if (typeof content !== 'string') throw new TypeError('writeText 内容必须是字符串')
    await callFs('writeText', p, '', content, Boolean(options?.atomic))
}

export async function writeBytes(path: string, data: Uint8Array | ArrayBuffer, options?: WriteFileOptions): Promise<void> {
    const p = assertPathString(path)
    if (!(data instanceof Uint8Array) && !(data instanceof ArrayBuffer)) {
        throw new TypeError('writeBytes 数据必须是 Uint8Array 或 ArrayBuffer')
    }
    await callFs('writeBytes', p, '', data, Boolean(options?.atomic))
}

export async function exists(path: string): Promise<boolean> {
    if (typeof path !== 'string' || path.length === 0 || path.includes('\0')) return false
    const res = await callFs('exists', path)
    return Boolean(res.exists)
}

export async function stat(path: string): Promise<FsStat> {
    const p = assertPathString(path)
    const res = await callFs('stat', p)
    return Object.freeze({
        isFile: Boolean(res.isFile),
        isDirectory: Boolean(res.isDirectory),
        size: res.size ?? 0,
        modifiedTimeMs: res.modifiedTimeMs ?? 0,
        createdTimeMs: res.createdTimeMs ?? 0,
    })
}

export async function readDir(path: string): Promise<readonly FsDirEntry[]> {
    const p = assertPathString(path)
    const res = await callFs('readDir', p)
    return Object.freeze((res.entries ?? []).map(entry => Object.freeze({ ...entry })))
}

export async function mkdir(path: string, options?: MkdirOptions): Promise<void> {
    const p = assertPathString(path)
    await callFs('mkdir', p, '', undefined, options?.recursive ?? true)
}

export async function remove(path: string, options?: RemoveOptions): Promise<void> {
    const p = assertPathString(path)
    await callFs('remove', p, '', undefined, Boolean(options?.recursive))
}

export async function rename(fromPath: string, toPath: string): Promise<void> {
    const from = assertPathString(fromPath, 'fromPath')
    const to = assertPathString(toPath, 'toPath')
    await callFs('rename', from, to)
}

export async function copy(fromPath: string, toPath: string, options?: CopyOptions): Promise<void> {
    const from = assertPathString(fromPath, 'fromPath')
    const to = assertPathString(toPath, 'toPath')
    await callFs('copy', from, to, undefined, Boolean(options?.overwrite))
}
