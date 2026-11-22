import * as core from '@canton/framework'
import * as foundation from '@canton/framework/foundation'
import * as ui from '@canton/framework/ui'
import * as animation from '@canton/framework/animation'
import * as internal from '@canton/framework/internal'
import assert from 'node:assert/strict'
import ts from 'typescript'
import { compileCantonCta } from '../../packages/vite-plugin/src/ctaTransform.ts'

const modules: Record<string, unknown> = { '@canton/framework': core, '@canton/framework/foundation': foundation, '@canton/framework/ui': ui, '@canton/framework/animation': animation, '@canton/framework/internal': internal }

export function requireCtaModule(name: string): unknown {
    if (!Object.hasOwn(modules, name)) throw new Error(`测试未提供 CTA 依赖：${name}`)

    return modules[name]
}

// 只执行正式编译产物，不在测试内复制模板、布局或重排实现
export function evaluateCta(source: string, imports: Record<string, unknown> = {}, filename = '测试.cta'): core.ArrangableDefinition {
    const { code } = compileCantonCta(source, filename)
    const output = ts.transpileModule(code, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } }).outputText
    const exports: { default?: core.ArrangableDefinition } = {}
    new Function('require', 'exports', output)((name: string) => Object.hasOwn(imports, name) ? imports[name] : requireCtaModule(name), exports)
    assert.ok(exports.default, 'CTA 必须生成正式 Arrangable 定义')
    return exports.default
}
