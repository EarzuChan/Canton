import { compileScript, invalidateTypeCache, parse } from "@canton/compiler"
import type { RawSourceMap } from 'source-map-js'
import { withCtaHmr } from './cta-hmr.ts'

export type CantonCtaCompileResult = {
    code: string
    map?: RawSourceMap
    dependencies: string[]
}

export function normalizePath(id: unknown): string {
    return String(id ?? "").split("?")[0].replace(/\\/g, "/")
}

export function isCtaModule(id: unknown): boolean {
    const normalized = normalizePath(id)
    return normalized.endsWith(".cta") || normalized.includes(".cta&")
}

export function isCtaQueryModule(id: unknown): boolean {
    const text = String(id ?? "")
    if (!isCtaModule(text)) return false
    const query = text.indexOf('?')
    return query >= 0 && new URLSearchParams(text.slice(query + 1)).has('type')
}

function stripBom(code: string): string {
    return code.charCodeAt(0) === 0xfeff ? code.slice(1) : code
}

export function compileCantonCta(code: string, id: string, hot = false): CantonCtaCompileResult {
    const filename = normalizePath(id)
    const source = stripBom(code)
    const describeError = (error: unknown) => {
        const located = error as { message?: string; loc?: { start: { line: number; column: number } } }
        const position = located.loc?.start
        return `${filename}${position ? `:${position.line}:${position.column}` : ''}：${located.message ?? String(error)}`
    }
    const descriptorResult = parse(source, { filename })
    if (descriptorResult.errors.length) {
        throw new Error(descriptorResult.errors.map(describeError).join("\n"))
    }

    let descriptor = descriptorResult.descriptor

    const compilerOptions = { runtimeModuleName: "@canton/framework/internal" }
    if (!descriptor.script) descriptor = parse(`${source}\n<script></script>`, { filename }).descriptor

    if (descriptor.script) {
        const script = compileScript(descriptor, {
            genDefaultAs: "_cta_main",
            templateOptions: { compilerOptions },
        })

        let output = script.content
        if (!descriptor.template || !script.content.includes("export default")) output += "\nexport default _cta_main"
        const compiled = hot ? withCtaHmr(output, filename, descriptor.script.content, script.map) : { code: output, map: script.map }
        return {
            code: compiled.code,
            map: compiled.map,
            dependencies: script.deps ?? [],
        }
    }

    throw new Error(`Canton CTA ${filename} 未能建立脚本编译入口`)
}

export function invalidateCtaTypeDependency(filename: string): void {
    invalidateTypeCache(normalizePath(filename))
}
