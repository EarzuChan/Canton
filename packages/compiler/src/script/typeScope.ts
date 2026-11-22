import type { Node, TSModuleDeclaration } from '@babel/types'
import type { ImportBinding } from '../cta/parse.ts'

export type Import = Pick<ImportBinding, 'source' | 'imported'>

export interface WithScope {
    _ownerScope: TypeScope
}

export type ScopeTypeNode = Node &
    WithScope & { _ns?: TSModuleDeclaration & WithScope }

export class TypeScope {
    constructor(public filename: string, public source: string, public offset: number = 0, public imports: Record<string, Import> = Object.create(null), public types: Record<string, ScopeTypeNode> = Object.create(null), public declares: Record<string, ScopeTypeNode> = Object.create(null)) { }
    isGenericScope = false
    resolvedImportSources: Record<string, string> = Object.create(null)
    exportedTypes: Record<string, ScopeTypeNode> = Object.create(null)
    exportedDeclares: Record<string, ScopeTypeNode> = Object.create(null)
}

export interface MaybeWithScope {
    _ownerScope?: TypeScope
}
