import { readFileSync, readdirSync } from 'node:fs'
import { dirname, relative, resolve, sep } from 'node:path'
import ts from 'typescript'

export type ImportEdge = Readonly<{ target: string; typeOnly: boolean }>
export type ModuleGraph = ReadonlyMap<string, readonly ImportEdge[]>

function sourceFiles(directory: string): string[] {
    return readdirSync(directory, { recursive: true, encoding: 'utf8' }).filter(file => file.endsWith('.ts')).map(file => resolve(directory, file))
}

function typeOnlyImport(node: ts.ImportDeclaration): boolean {
    const clause = node.importClause
    if (!clause) return false
    if (clause.isTypeOnly) return true
    const bindings = clause.namedBindings
    return !clause.name && !!bindings && ts.isNamedImports(bindings) && bindings.elements.length > 0 && bindings.elements.every(element => element.isTypeOnly)
}

function typeOnlyExport(node: ts.ExportDeclaration): boolean {
    if (node.isTypeOnly) return true
    const clause = node.exportClause
    return !!clause && ts.isNamedExports(clause) && clause.elements.length > 0 && clause.elements.every(element => element.isTypeOnly)
}

// 只认真实的导入导出声明与 import() 类型，生成代码里的字符串不算依赖
function moduleEdges(file: string): { specifier: string; typeOnly: boolean }[] {
    const source = ts.createSourceFile(file, readFileSync(file, 'utf8'), ts.ScriptTarget.Latest, true)
    const edges: { specifier: string; typeOnly: boolean }[] = []
    const visit = (node: ts.Node) => {
        if (ts.isImportDeclaration(node) && ts.isStringLiteral(node.moduleSpecifier)) edges.push({ specifier: node.moduleSpecifier.text, typeOnly: typeOnlyImport(node) })
        else if (ts.isExportDeclaration(node) && node.moduleSpecifier && ts.isStringLiteral(node.moduleSpecifier)) edges.push({ specifier: node.moduleSpecifier.text, typeOnly: typeOnlyExport(node) })
        else if (ts.isImportTypeNode(node) && ts.isLiteralTypeNode(node.argument) && ts.isStringLiteral(node.argument.literal)) edges.push({ specifier: node.argument.literal.text, typeOnly: true })
        else if (ts.isCallExpression(node) && node.expression.kind === ts.SyntaxKind.ImportKeyword && node.arguments[0] && ts.isStringLiteral(node.arguments[0])) edges.push({ specifier: node.arguments[0].text, typeOnly: false })
        ts.forEachChild(node, visit)
    }
    visit(source)
    return edges
}

// 包内相对依赖，键为相对 src 的 POSIX 路径
export function moduleGraph(sourceRoot: string): ModuleGraph {
    const name = (file: string) => relative(sourceRoot, file).split(sep).join('/')
    const graph = new Map<string, ImportEdge[]>()
    for (const file of sourceFiles(sourceRoot))
        graph.set(name(file), moduleEdges(file).filter(edge => edge.specifier.startsWith('.')).map(edge => ({ target: name(resolve(dirname(file), edge.specifier)), typeOnly: edge.typeOnly })))
    return graph
}

// 包外依赖中的 @canton/* 包名
export function packageImports(sourceRoot: string): ReadonlySet<string> {
    const result = new Set<string>()
    for (const file of sourceFiles(sourceRoot))
        for (const edge of moduleEdges(file)) {
            const found = edge.specifier.match(/^@canton\/[^/]+/)
            if (found) result.add(found[0])
        }
    return result
}

export function findCycles(graph: ModuleGraph, valueOnly = false): string[][] {
    const cycles: string[][] = []
    const state = new Map<string, 'visiting' | 'done'>()
    const stack: string[] = []
    const visit = (node: string) => {
        state.set(node, 'visiting')
        stack.push(node)
        for (const edge of graph.get(node) ?? []) {
            if (valueOnly && edge.typeOnly) continue
            if (state.get(edge.target) === 'visiting') cycles.push([...stack.slice(stack.indexOf(edge.target)), edge.target])
            else if (!state.has(edge.target) && graph.has(edge.target)) visit(edge.target)
        }
        stack.pop()
        state.set(node, 'done')
    }
    for (const node of [...graph.keys()].sort()) if (!state.has(node)) visit(node)
    return cycles
}

// 顶层目录即层；根目录文件记为 '.'
export function layerOf(file: string): string {
    return file.includes('/') ? file.slice(0, file.indexOf('/')) : '.'
}
