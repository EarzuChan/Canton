import type { CTADescriptor, CTAScriptBlock } from './cta/parse.ts'
import { generateCtaScript } from './script/generateScript.ts'
import type { CTAScriptCompileOptions } from './script/options.ts'
import { lowerCtaValues } from './values/lowerValues.ts'

// 唯一的 TS setup 脚本模式，模板直接共享实例词法作用域
export function compileScript(cta: CTADescriptor, options: CTAScriptCompileOptions): CTAScriptBlock {
    const script = generateCtaScript(cta, options)
    const lowered = lowerCtaValues(script.content, cta.filename, script.map)
    return { ...script, content: lowered.content, map: lowered.map, deps: [...new Set([...script.deps ?? [], ...lowered.deps])] }
}
