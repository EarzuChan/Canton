import { createHash } from 'node:crypto'
import { readFileSync } from 'node:fs'
import { basename, dirname, resolve } from 'node:path'
import { compileHlslToDxbc } from './hlslCompiler.ts'

export interface HlslTransformContext {
    addWatchFile?: (file: string) => void
    emitFile?: (file: { type: 'asset'; fileName: string; source: Uint8Array }) => string
}

export function stripQuery(id: string): string {
    const query = id.indexOf('?')
    return query >= 0 ? id.slice(0, query) : id
}

export function isHlslModule(id: string): boolean {
    return stripQuery(id).toLowerCase().endsWith('.hlsl')
}

export function parseHlslEntryPoint(id: string): string {
    const query = id.indexOf('?')
    if (query < 0) return 'main'
    const params = new URLSearchParams(id.slice(query + 1))
    const entry = params.get('entry')?.trim()
    return entry || 'main'
}

// 展开本地相对 #include "..."，保留 #line 以便运行期报错仍指向真实文件与行号；<canton.hlsl> 等尖括号头交由编译器虚拟头处理
export function inlineLocalHlslIncludes(source: string, filePath: string, onIncludeFile?: (file: string) => void, stack = new Set<string>()): string {
    const normalizedPath = resolve(filePath)
    if (stack.has(normalizedPath)) {
        throw new Error(`HLSL 本地 #include 存在循环引用：${normalizedPath}`)
    }
    stack.add(normalizedPath)
    const dir = dirname(normalizedPath)
    const escapedCurrent = normalizedPath.replaceAll('\\', '/')
    const lines = source.replace(/\r\n/g, '\n').split('\n')
    const out: string[] = []
    const includePattern = /^\s*#\s*include\s+"([^"]+)"\s*$/
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i]!
        const match = includePattern.exec(line)
        if (!match) {
            out.push(line)
            continue
        }
        const includedPath = resolve(dir, match[1]!)
        onIncludeFile?.(includedPath)
        const includedSource = readFileSync(includedPath, 'utf8')
        const expanded = inlineLocalHlslIncludes(includedSource, includedPath, onIncludeFile, stack)
        out.push(`#line 1 "${includedPath.replaceAll('\\', '/')}"`)
        out.push(expanded)
        out.push(`#line ${i + 2} "${escapedCurrent}"`)
    }
    stack.delete(normalizedPath)
    return out.join('\n')
}

function sanitizeAssetStem(filePath: string): string {
    const raw = basename(filePath, '.hlsl').replace(/[^a-zA-Z0-9._-]+/g, '_')
    return raw || 'shader'
}

export function transformHlslModule(
    code: string,
    id: string,
    hot: boolean,
    emittedAssets: Set<string>,
    ctx: HlslTransformContext = {},
): string {
    const filePath = stripQuery(id)
    const entryPoint = parseHlslEntryPoint(id)
    const debugName = filePath.replaceAll('\\', '/')
    const stableId = `${debugName}#${entryPoint}`

    if (hot) {
        const expanded = inlineLocalHlslIncludes(code, filePath, file => ctx.addWatchFile?.(file))
        return [
            `import { registerHlslRuntimeEffect as __registerHlsl, updateHlslRuntimeEffect as __updateHlsl } from '@canton/framework/internal'`,
            `const __hlslId = ${JSON.stringify(stableId)}`,
            `export const __hlslSource = ${JSON.stringify(expanded)}`,
            `export const __hlslEntryPoint = ${JSON.stringify(entryPoint)}`,
            `export const __hlslDebugName = ${JSON.stringify(debugName)}`,
            `const __effect = __registerHlsl(__hlslId, __hlslSource, { entryPoint: __hlslEntryPoint, debugName: __hlslDebugName })`,
            `export default __effect`,
            `if (import.meta.hot) {`,
            `    import.meta.hot.accept((next) => {`,
            `        if (next && typeof next.__hlslSource === 'string') {`,
            `            __updateHlsl(__hlslId, next.__hlslSource, { entryPoint: next.__hlslEntryPoint, debugName: next.__hlslDebugName })`,
            `        }`,
            `    })`,
            `}`,
            ``,
        ].join('\n')
    }

    const { dxbc } = compileHlslToDxbc(code, {
        sourceName: filePath,
        entryPoint,
        target: 'ps_5_0',
        optimize: true,
        baseDir: dirname(filePath),
        onIncludeFile: file => ctx.addWatchFile?.(file),
    })
    const hash = createHash('sha256').update(dxbc).digest('hex').slice(0, 8)
    const fileName = `assets/${sanitizeAssetStem(filePath)}-${hash}.dxbc`
    if (!emittedAssets.has(fileName)) {
        emittedAssets.add(fileName)
        if (!ctx.emitFile) {
            throw new Error(`Dist 构建 .hlsl 缺少 emitFile 上下文：${debugName}`)
        }
        ctx.emitFile({ type: 'asset', fileName, source: dxbc })
    }
    return [
        `import { loadHlslRuntimeEffectAsset as __loadHlslAsset } from '@canton/framework/internal'`,
        `export default __loadHlslAsset(${JSON.stringify(fileName)}, { entryPoint: ${JSON.stringify(entryPoint)}, debugName: ${JSON.stringify(debugName)} })`,
        ``,
    ].join('\n')
}
