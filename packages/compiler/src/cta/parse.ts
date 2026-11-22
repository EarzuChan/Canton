import { parse as parseTemplate, type BindingMetadata, type CompilerError, type ElementNode, NodeTypes, type ParserOptions, type RawSourceMap, type RootNode, type SourceLocation, createRoot } from '../template/index.ts'
import { genCacheKey } from '@canton/shared'
import { SourceMapGenerator } from 'source-map-js'
import { createCache } from './cache.ts'

export const DEFAULT_FILENAME = 'anonymous.cta'

export interface ImportBinding {
    isType: boolean
    imported: string
    local: string
    source: string
}

export interface CTAParseOptions {
    filename?: string
    sourceMap?: boolean
    sourceRoot?: string
    templateParseOptions?: ParserOptions
}

export interface CTABlock {
    type: string
    content: string
    loc: SourceLocation
    map?: RawSourceMap
}

export interface CTATemplateBlock extends CTABlock {
    type: 'template'
    ast?: RootNode
}

export interface CTAScriptBlock extends CTABlock {
    type: 'script'
    bindings?: BindingMetadata
    imports?: Record<string, ImportBinding>
    scriptAst?: import('@babel/types').Statement[]
    deps?: string[]
}

export interface CTADescriptor {
    filename: string
    source: string
    template: CTATemplateBlock | null
    script: CTAScriptBlock | null
}

export interface CTAParseResult {
    descriptor: CTADescriptor
    errors: (CompilerError | SyntaxError)[]
}

export const parseCache = createCache<CTAParseResult>()

export function parse(source: string, options: CTAParseOptions = {}): CTAParseResult {
    const key = genCacheKey(source, options)
    const cached = parseCache.get(key)
    if (cached) return cached

    const { filename = DEFAULT_FILENAME, sourceMap = true, sourceRoot = '', templateParseOptions = {} } = options
    const descriptor: CTADescriptor = { filename, source, template: null, script: null }
    const errors: (CompilerError | SyntaxError)[] = []
    const fail = (message: string, loc: SourceLocation) => errors.push(Object.assign(new SyntaxError(message), { loc }))

    const ast = parseTemplate(source, { ...templateParseOptions, parseMode: 'cta', prefixIdentifiers: true, onError: error => errors.push(error) })
    for (const node of ast.children) {
        if (node.type === NodeTypes.COMMENT || node.type === NodeTypes.TEXT && !node.content.trim()) continue

        if (node.type !== NodeTypes.ELEMENT || node.tag !== 'template' && node.tag !== 'script') {
            fail('CTA 顶层只接受 template 与 script 区块', node.loc)
            continue
        }

        if (node.props.length) {
            fail(`CTA 的 <${node.tag}> 不接受属性`, node.props[0].loc)
            continue
        }

        if (descriptor[node.tag]) {
            fail(`CTA 只能包含一个 <${node.tag}> 区块`, node.loc)
            continue
        }

        const block = createBlock(node, source)
        if (sourceMap) block.map = generateSourceMap(filename, source, block, sourceRoot)
        if (node.tag === 'template') descriptor.template = { ...block, type: 'template', ast: createRoot(node.children, source) }
        else descriptor.script = { ...block, type: 'script' }
    }

    if (!descriptor.template && !descriptor.script && !errors.length) errors.push(new SyntaxError(`CTA 至少需要一个 template 或 script 区块：${filename}`))

    const result = { descriptor, errors }
    parseCache.set(key, result)
    return result
}

function createBlock(node: ElementNode, source: string): CTABlock {
    const loc = node.innerLoc!
    return { type: node.tag, content: source.slice(loc.start.offset, loc.end.offset), loc }
}

function generateSourceMap(filename: string, source: string, block: CTABlock, sourceRoot: string): RawSourceMap {
    const map = new SourceMapGenerator({ file: filename.replace(/\\/g, '/'), sourceRoot: sourceRoot.replace(/\\/g, '/') })
    map.setSourceContent(filename, source)
    const lines = block.content.split(/\r?\n/)

    for (let row = 0; row < lines.length; row++) for (let column = 0; column < lines[row].length; column++) map.addMapping({ source: filename, generated: { line: row + 1, column }, original: { line: block.loc.start.line + row, column: column + (row === 0 ? block.loc.start.column - 1 : 0) } })

    return JSON.parse(map.toString())
}