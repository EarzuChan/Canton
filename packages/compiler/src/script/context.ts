import type { BindingMetadata } from '../template/index.ts'
import { generateCodeFrame } from '@canton/shared'
import { parse as babelParse } from '@babel/parser'
import type { CallExpression, Expression, Node, ObjectPattern, Program } from '@babel/types'
import MagicString from 'magic-string'
import type { CTADescriptor, ImportBinding } from '../cta/parse.ts'
import { warn } from '../cta/warn.ts'
import { normalizeCtaUnitSyntax, restoreBabelNodePositions } from '../syntax/unitSyntax.ts'
import type { CTAScriptCompileOptions } from './options.ts'
import type { TypeScope } from './typeScope.ts'
import { resolveParserPlugins } from './utils.ts'

export type PropsDestructureBindings = Record<
    string,
    {
        local: string
        default?: Expression
    }
>

export class ScriptCompileContext {
    readonly isTS = true

    scriptAst: Program | null

    source: string
    filename: string
    s: MagicString
    startOffset: number | undefined
    endOffset: number | undefined

    // import / type 分析
    scope?: TypeScope
    globalScopes?: TypeScope[]
    userImports: Record<string, ImportBinding> = Object.create(null)

    hasDefinePropsCall = false

    // 定义Props
    propsCall: CallExpression | undefined
    propsDecl: Node | undefined
    propsRuntimeDecl: Node | undefined
    propsTypeDecl: Node | undefined
    propsDestructureDecl: ObjectPattern | undefined
    propsDestructuredBindings: PropsDestructureBindings = Object.create(null)
    propsDestructureRestId: string | undefined
    propsRuntimeDefaults: Node | undefined

    // 定义Slots
    slotsCall: CallExpression | undefined

    // 代码生成
    bindingMetadata: BindingMetadata = {}
    helperImports: Set<string> = new Set()
    helper(key: string): string {
        this.helperImports.add(key)
        return `_${key}`
    }

    // 要暴露在编译的脚本块儿以便HMR这那的
    deps?: Set<string>

    // 已解析fs的缓存
    fs?: NonNullable<CTAScriptCompileOptions['fs']>

    constructor(public descriptor: CTADescriptor, public options: Partial<CTAScriptCompileOptions>) {
        this.source = descriptor.source
        this.filename = descriptor.filename
        this.s = new MagicString(this.source)
        this.startOffset = descriptor.script?.loc.start.offset
        this.endOffset = descriptor.script?.loc.end.offset

        const plugins = resolveParserPlugins('ts', options.babelParserPlugins)

        function parse(input: string, offset: number): Program {
            try {
                const normalized = normalizeCtaUnitSyntax(input)
                const program = babelParse(normalized.content, {
                    plugins,
                    sourceType: 'module',
                }).program
                restoreBabelNodePositions(program, normalized.restore)
                return program
            } catch (e: any) {
                e.message = `[Canton/CTA] ${e.message}\n\n${descriptor.filename}\n${generateCodeFrame(descriptor.source, e.pos + offset, e.pos + offset + 1)}`
                throw e
            }
        }

        this.scriptAst = descriptor.script && parse(descriptor.script!.content, this.startOffset!)
    }

    getString(node: Node): string {
        return this.descriptor.script!.content.slice(node.start!, node.end!)
    }

    warn(msg: string, node: Node, scope?: TypeScope): void {
        warn(generateError(msg, node, this, scope))
    }

    error(msg: string, node: Node, scope?: TypeScope): never {
        throw new Error(`[Canton/CTA] ${generateError(msg, node, this, scope)}`)
    }
}

function generateError(msg: string, node: Node, ctx: ScriptCompileContext, scope?: TypeScope) {
    const offset = scope ? scope.offset : ctx.startOffset!
    return `${msg}\n\n${(scope || ctx.descriptor).filename}\n${generateCodeFrame((scope || ctx.descriptor).source, node.start! + offset, node.end! + offset)}`
}
