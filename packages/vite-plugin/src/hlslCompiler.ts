import { readFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { createRequire } from 'node:module'
import { CANTON_HLSL_HEADER } from '@canton/shared'

export interface HlslCompileOptions {
    sourceName: string
    entryPoint?: string
    target?: string
    optimize?: boolean
    baseDir?: string
    onIncludeFile?: (resolvedPath: string) => void
}

export interface HlslCompileResult {
    dxbc: Uint8Array
    warnings: string
}

const D3DCOMPILE_ENABLE_STRICTNESS = 1 << 11
const D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15
const D3D_INCLUDE_LOCAL = 0
const S_OK = 0
const E_FAIL = -2147467259

type KoffiModule = typeof import('koffi')
type KoffiType = import('koffi').IKoffiCType

interface D3DCompilerBinding {
    koffi: KoffiModule
    d3dCompile: (...args: unknown[]) => number
    blobReleaseProto: KoffiType
    blobPtrProto: KoffiType
    blobSizeProto: KoffiType
    openProto: KoffiType
    closeProto: KoffiType
}

let cachedBinding: D3DCompilerBinding | undefined

function loadD3DCompiler(): D3DCompilerBinding {
    if (cachedBinding) return cachedBinding
    if (process.platform !== 'win32') {
        throw new Error('HLSL 编译仅支持 Windows 平台（需要系统 d3dcompiler_47.dll）')
    }
    const require = createRequire(import.meta.url)
    const koffi = require('koffi') as typeof import('koffi')
    const dll = koffi.load('d3dcompiler_47.dll')
    const d3dCompile = dll.func('int32 __stdcall D3DCompile(const uint8* pSrcData, size_t SrcDataSize, const char* pSourceName, void* pDefines, void* pInclude, const char* pEntrypoint, const char* pTarget, uint32 Flags1, uint32 Flags2, _Out_ void** ppCode, _Out_ void** ppErrorMsgs)')
    const blobReleaseProto = koffi.proto('uint32 __stdcall ID3DBlob_Release(void* self)')
    const blobPtrProto = koffi.proto('void* __stdcall ID3DBlob_GetBufferPointer(void* self)')
    const blobSizeProto = koffi.proto('size_t __stdcall ID3DBlob_GetBufferSize(void* self)')
    const openProto = koffi.proto('int32 __stdcall ID3DInclude_Open(void* self, int32 includeType, const char* fileName, const void* parentData, void* ppData, void* pBytes)')
    const closeProto = koffi.proto('int32 __stdcall ID3DInclude_Close(void* self, const void* pData)')
    cachedBinding = { koffi, d3dCompile, blobReleaseProto, blobPtrProto, blobSizeProto, openProto, closeProto }
    return cachedBinding
}

function readAndReleaseBlob(binding: D3DCompilerBinding, blob: unknown): Uint8Array {
    const { koffi, blobReleaseProto, blobPtrProto, blobSizeProto } = binding
    const vtblPtr = koffi.decode(blob, 'void*')
    const vtbl = koffi.decode(vtblPtr, koffi.array('void*', 5)) as unknown[]
    try {
        const size = Number(koffi.call(vtbl[4], blobSizeProto, blob))
        if (size <= 0) return new Uint8Array(0)
        const dataPtr = koffi.call(vtbl[3], blobPtrProto, blob)
        const bytes = koffi.decode(dataPtr, koffi.array('uint8', size)) as Uint8Array
        return new Uint8Array(bytes)
    } finally {
        koffi.call(vtbl[2], blobReleaseProto, blob)
    }
}

export function formatHlslDiagnostics(raw: string, sourceName: string): string {
    const trimmed = raw.trim()
    if (!trimmed) return `${sourceName}: HLSL 编译失败（无详细诊断输出）`
    return trimmed.replace(/\r\n/g, '\n')
}

export function compileHlslToDxbc(source: string, options: HlslCompileOptions): HlslCompileResult {
    const binding = loadD3DCompiler()
    const { koffi, d3dCompile, openProto, closeProto } = binding
    const entryPoint = options.entryPoint ?? 'main'
    const target = options.target ?? 'ps_5_0'
    const flags = D3DCOMPILE_ENABLE_STRICTNESS | (options.optimize === false ? 0 : D3DCOMPILE_OPTIMIZATION_LEVEL3)
    const srcBytes = Buffer.from(source, 'utf8')
    const baseDir = options.baseDir ?? (options.sourceName.includes('/') || options.sourceName.includes('\\') ? dirname(options.sourceName) : process.cwd())
    const allocations = new Map<bigint, { ptr: unknown; dir: string }>()

    const openCb = koffi.register((_self: unknown, includeType: number, fileName: string, parentData: unknown, ppData: unknown, pBytes: unknown): number => {
        try {
            let contentBytes: Uint8Array
            let nextDir = baseDir
            const normalized = fileName.replaceAll('\\', '/')
            if (normalized === 'canton.hlsl') {
                contentBytes = Buffer.from(CANTON_HLSL_HEADER, 'utf8')
            } else if (includeType === D3D_INCLUDE_LOCAL) {
                const parentAddr = parentData ? koffi.address(parentData) : 0n
                const parentDir = allocations.get(parentAddr)?.dir ?? baseDir
                const resolved = resolve(parentDir, fileName)
                options.onIncludeFile?.(resolved)
                contentBytes = readFileSync(resolved)
                nextDir = dirname(resolved)
            } else {
                return E_FAIL
            }
            const allocSize = Math.max(1, contentBytes.byteLength)
            const nativeBuf = koffi.alloc('uint8', allocSize)
            if (contentBytes.byteLength > 0) {
                koffi.encode(nativeBuf, koffi.array('uint8', contentBytes.byteLength), contentBytes)
            }
            const addr = koffi.address(nativeBuf)
            allocations.set(addr, { ptr: nativeBuf, dir: nextDir })
            koffi.encode(ppData, 'uintptr', addr)
            koffi.encode(pBytes, 'uint32', contentBytes.byteLength)
            return S_OK
        } catch {
            return E_FAIL
        }
    }, koffi.pointer(openProto))

    const closeCb = koffi.register((_self: unknown, pData: unknown): number => {
        if (pData) {
            const addr = koffi.address(pData)
            const entry = allocations.get(addr)
            if (entry) {
                allocations.delete(addr)
                koffi.free(entry.ptr)
            }
        }
        return S_OK
    }, koffi.pointer(closeProto))

    const vtbl = koffi.alloc('uintptr', 2)
    koffi.encode(vtbl, koffi.array('uintptr', 2), [koffi.address(openCb), koffi.address(closeCb)])
    const includeObj = koffi.alloc('uintptr', 1)
    koffi.encode(includeObj, 'uintptr', koffi.address(vtbl))

    const outCode: unknown[] = [null]
    const outErrors: unknown[] = [null]
    try {
        const hr = d3dCompile(srcBytes, srcBytes.byteLength, options.sourceName, null, includeObj, entryPoint, target, flags, 0, outCode, outErrors)
        const errorText = outErrors[0] ? Buffer.from(readAndReleaseBlob(binding, outErrors[0])).toString('utf8').replace(/\0+$/, '') : ''
        if (hr < 0 || !outCode[0]) {
            if (outCode[0]) readAndReleaseBlob(binding, outCode[0])
            throw new Error(`HLSL 编译失败 (${options.sourceName}):\n${formatHlslDiagnostics(errorText, options.sourceName)}`)
        }
        const dxbc = readAndReleaseBlob(binding, outCode[0])
        return { dxbc, warnings: errorText.trim() }
    } finally {
        for (const entry of allocations.values()) koffi.free(entry.ptr)
        allocations.clear()
        koffi.free(includeObj)
        koffi.free(vtbl)
        koffi.unregister(openCb)
        koffi.unregister(closeCb)
    }
}
