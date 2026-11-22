import { Namespaces } from './ast.ts'
import type { ParserOptions } from './options.ts'

export const parserOptions: ParserOptions = {
    parseMode: 'base',
    isVoidTag: () => false,
    isPreTag: () => false,
    getNamespace: () => Namespaces.HTML,
}
