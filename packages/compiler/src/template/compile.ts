import type { RootNode } from './ast.ts'
import type { CompilerOptions, ParserOptions } from './options.ts'
import { baseParse } from './parser.ts'
import { generate, type CodegenResult } from './codegen.ts'
import { parserOptions } from './parserOptions.ts'
import { validateTemplateContract } from './templateContract.ts'

export function baseCompile(source: string | RootNode, options: CompilerOptions = {}): CodegenResult {
    return generate(typeof source === 'string' ? baseParse(source, options) : source, options)
}

export function compile(source: string | RootNode, options: CompilerOptions = {}): CodegenResult {
    const resolved = { ...parserOptions, ...options, isTS: true }
    const ast = typeof source === 'string' ? baseParse(source, resolved) : source
    ast.slotNames = validateTemplateContract(ast, resolved)
    return baseCompile(ast, resolved)
}

export function parse(template: string, options: ParserOptions = {}): RootNode {
    return baseParse(template, { ...parserOptions, ...options })
}
