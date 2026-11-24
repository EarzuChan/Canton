import type { CallExpression, Expression, Identifier, ImportDefaultSpecifier, ImportNamespaceSpecifier, ImportSpecifier, Node, StringLiteral, TSMethodSignature, TSPropertySignature } from '@babel/types'
import path from 'path'
import type { ParserPlugin } from '@babel/parser'
import { isArray } from '@canton/shared'

export const UNKNOWN_TYPE = 'Unknown'
export const DEFINE_PROPS = 'defineProps'
export const WITH_DEFAULTS = 'withDefaults'
export const DEFINE_SLOTS = 'defineSlots'

export function resolveObjectKey(node: Node, computed: boolean): string | undefined {
    switch (node.type) {
        case 'StringLiteral':
        case 'NumericLiteral':
            return String(node.value)
        case 'Identifier':
            if (!computed) return node.name
    }
    return undefined
}

export function concatStrings(strs: Array<string | null | undefined | false>): string {
    return strs.filter((s): s is string => !!s).join(', ')
}

export function isLiteralNode(node: Node): boolean {
    return node.type.endsWith('Literal')
}

export function isCallOf(node: Node | null | undefined, test: string | ((id: string) => boolean) | null | undefined): node is CallExpression {
    return !!(node && test && node.type === 'CallExpression' && node.callee.type === 'Identifier' && (typeof test === 'string' ? node.callee.name === test : test(node.callee.name)))
}

export function toRuntimeTypeString(types: string[]): string {
    return types.length > 1 ? `[${types.join(', ')}]` : types[0]
}

export function getImportedName(specifier: | ImportSpecifier | ImportDefaultSpecifier | ImportNamespaceSpecifier): string {
    if (specifier.type === 'ImportSpecifier')
        return specifier.imported.type === 'Identifier' ? specifier.imported.name : specifier.imported.value
    else if (specifier.type === 'ImportNamespaceSpecifier') return '*'
    return 'default'
}

export function getId(node: Identifier | StringLiteral): string
export function getId(node: Expression): string | null
export function getId(node: Expression) {
    return node.type === 'Identifier' ? node.name : node.type === 'StringLiteral' ? node.value : null
}

export function getStringLiteralKey(node: TSPropertySignature | TSMethodSignature): string | null {
    return node.computed ? node.key.type === 'TemplateLiteral' && !node.key.expressions.length ? node.key.quasis.map(q => q.value.cooked).join('') : null : node.key.type === 'Identifier' ? node.key.name : node.key.type === 'StringLiteral' ? node.key.value : node.key.type === 'NumericLiteral' ? String(node.key.value) : null
}

const identity = (str: string) => str
const fileNameLowerCaseRegExp = /[^\u0130\u0131\u00DFa-z0-9\\/:\-_\. ]+/g
const toLowerCase = (str: string) => str.toLowerCase()

function toFileNameLowerCase(x: string) {
    return fileNameLowerCaseRegExp.test(x) ? x.replace(fileNameLowerCaseRegExp, toLowerCase) : x
}

export function createGetCanonicalFileName(useCaseSensitiveFileNames: boolean): (str: string) => string {
    return useCaseSensitiveFileNames ? identity : toFileNameLowerCase
}

const normalize = (path.posix || path).normalize
const windowsSlashRE = /\\/g
export function normalizePath(p: string): string {
    return normalize(p.replace(windowsSlashRE, '/'))
}

export const joinPaths: (...paths: string[]) => string = (path.posix || path).join

export const isJS = (...langs: (string | null | undefined)[]): boolean => langs.some(lang => lang === 'js' || lang === 'jsx')
export const isTS = (...langs: (string | null | undefined)[]): boolean => langs.some(lang => lang === 'ts' || lang === 'tsx')

export function resolveParserPlugins(lang: string, userPlugins?: ParserPlugin[], dts = false): ParserPlugin[] {
    const plugins: ParserPlugin[] = []
    if (
        !userPlugins || !userPlugins.some(p => p === 'importAssertions' || p === 'importAttributes' || (isArray(p) && p[0] === 'importAttributes'))
    ) {
        plugins.push('importAttributes')
    }
    if (lang === 'jsx' || lang === 'tsx' || lang === 'mtsx') {
        plugins.push('jsx')
    } else if (userPlugins) {
        userPlugins = userPlugins.filter(p => p !== 'jsx')
    }
    if (
        lang === 'ts' || lang === 'mts' || lang === 'tsx' || lang === 'cts' || lang === 'mtsx'
    ) {
        plugins.push(['typescript', { dts }], 'explicitResourceManagement')
        if (!userPlugins || !userPlugins.includes('decorators')) {
            plugins.push('decorators-legacy')
        }
    }
    if (userPlugins) {
        plugins.push(...userPlugins)
    }
    return plugins
}
