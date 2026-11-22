import type { ParserPlugin } from '@babel/parser'
import type { CTATemplateCompileOptions } from '../template/index.ts'

export interface CTAScriptCompileOptions {
    sourceMap?: boolean
    babelParserPlugins?: ParserPlugin[]
    globalTypeFiles?: string[]
    genDefaultAs?: string
    templateOptions?: Partial<CTATemplateCompileOptions>
    hoistStatic?: boolean
    propsDestructure?: boolean | 'error'
    fs?: {
        fileExists(file: string): boolean
        readFile(file: string): string | undefined
        realpath?(file: string): string
    }
}
