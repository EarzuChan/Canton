import { BindingTypes, UNREF, compileTemplate } from '../template/index.ts'
import { isFunctionType, unwrapTSNode, walkIdentifiers } from '../syntax/babelUtils.ts'
import { generateCodeFrame } from '@canton/shared'
import type { ArrayPattern, CallExpression, Declaration, Identifier, LVal, Node, ObjectPattern, Statement } from '@babel/types'
import { walk } from 'estree-walker'
import { type RawSourceMap, SourceMapConsumer, SourceMapGenerator } from 'source-map-js'
import { DEFAULT_FILENAME, type CTADescriptor, type CTAScriptBlock, type ImportBinding } from '../cta/parse.ts'
import { warnOnce } from '../cta/warn.ts'
import { ScriptCompileContext } from './context.ts'
import { genRuntimeProps, processDefineProps } from './defineProps.ts'
import { processDefineSlots } from './defineSlots.ts'
import { transformDestructuredProps } from './definePropsDestructure.ts'
import type { CTAScriptCompileOptions } from './options.ts'
import { DEFINE_PROPS, DEFINE_SLOTS, WITH_DEFAULTS, getImportedName, isCallOf, isLiteralNode } from './utils.ts'

const MACROS = [
    DEFINE_PROPS,
    WITH_DEFAULTS,
    DEFINE_SLOTS,
]

// 模块内描述各槽位参数的类型别名，模板出口与定义的槽位声明共用
const SLOTS_TYPE = '__Slots'

// 生成保有单位声明的中间 TS，供当前文件及跨 CTA 契约分析共同使用
export function generateCtaScript(cta: CTADescriptor, options: CTAScriptCompileOptions): CTAScriptBlock {
    const { script, source, filename } = cta
    if (!script) throw new Error('CTA 没有可编译的 script 区块')
    const hoistStatic = options.hoistStatic !== false
    const ctx = new ScriptCompileContext(cta, options)
    const setupBindings: Record<string, BindingTypes> = Object.create(null)
    const startOffset = ctx.startOffset!
    const endOffset = ctx.endOffset!

    function hoistNode(node: Statement) {
        const start = node.start! + startOffset
        let end = node.end! + startOffset
        if (node.trailingComments && node.trailingComments.length > 0) {
            const lastCommentNode = node.trailingComments[node.trailingComments.length - 1]
            end = lastCommentNode.end! + startOffset
        }
        while (end <= source.length) {
            if (!/\s/.test(source.charAt(end))) {
                break
            }
            end++
        }
        ctx.s.move(start, end, 0)
    }

    function registerUserImport(source: string, local: string, imported: string, isType: boolean) {
        ctx.userImports[local] = { isType, imported, local, source }
    }

    function checkInvalidScopeReference(node: Node | undefined, method: string) {
        if (!node) return
        walkIdentifiers(node, id => {
            const binding = setupBindings[id.name]
            if (binding && binding !== BindingTypes.LITERAL_CONST) {
                ctx.error(`${method}() 的声明会提升到模块，不能读取实例局部变量；共享定义请移入独立 TS 模块`, id)
            }
        })
    }

    const scriptAst = ctx.scriptAst!

    for (const node of scriptAst.body) {
        if (node.type === 'ImportDeclaration') {
            hoistNode(node)

            let removed = 0
            const removeSpecifier = (i: number) => {
                const removeLeft = i > removed
                removed++
                const current = node.specifiers[i]
                const next = node.specifiers[i + 1]
                ctx.s.remove(removeLeft ? node.specifiers[i - 1].end! + startOffset : current.start! + startOffset, next && !removeLeft ? next.start! + startOffset : current.end! + startOffset)
            }

            for (let i = 0; i < node.specifiers.length; i++) {
                const specifier = node.specifiers[i]
                const local = specifier.local.name
                const imported = getImportedName(specifier)
                const source = node.source.value
                const existing = ctx.userImports[local]
                if ((source === '@canton/framework/internal' || source === '@canton/framework') && MACROS.includes(imported)) {
                    if (local === imported) {
                        warnOnce(`\`${imported}\` is a compiler macro and no longer needs to be imported.`)
                    } else {
                        ctx.error(`\`${imported}\` is a compiler macro and cannot be aliased to ` + `a different name.`, specifier)
                    }
                    removeSpecifier(i)
                } else if (existing) {
                    if (existing.source === source && existing.imported === imported) {
                        removeSpecifier(i)
                    } else {
                        ctx.error(`different imports aliased to same local name.`, specifier)
                    }
                } else {
                    registerUserImport(source, local, imported, node.importKind === 'type' || (specifier.type === 'ImportSpecifier' && specifier.importKind === 'type'))
                }
            }
            if (node.specifiers.length && removed === node.specifiers.length) {
                ctx.s.remove(node.start! + startOffset, node.end! + startOffset)
            }
        }
    }

    const cantonImportAliases: Record<string, string> = {}
    for (const key in ctx.userImports) {
        const { source, imported, local } = ctx.userImports[key]
        if ((source === '@canton/framework')) cantonImportAliases[imported] = local
    }

    for (const node of scriptAst.body) {
        if (node.type === 'ExpressionStatement' && (processDefineProps(ctx, unwrapTSNode(node.expression)) || processDefineSlots(ctx, unwrapTSNode(node.expression)))) {
            ctx.s.remove(node.start! + startOffset, node.end! + startOffset)
        }

        if (node.type === 'VariableDeclaration' && !node.declare) {
            const total = node.declarations.length
            let left = total
            let lastNonRemoved: number | undefined

            for (let i = 0; i < total; i++) {
                const decl = node.declarations[i]
                const init = decl.init && unwrapTSNode(decl.init)
                if (init) {
                    if (isCallOf(init, DEFINE_SLOTS)) ctx.error(`${DEFINE_SLOTS}() 只能写成顶层语句，没有返回值；要读取槽位请用 useSlots()`, init)
                    const isDefineProps = processDefineProps(ctx, init, decl.id as LVal)
                    if (ctx.propsDestructureRestId) {
                        setupBindings[ctx.propsDestructureRestId] = BindingTypes.SETUP_REACTIVE_CONST
                    }

                    if (
                        isDefineProps && !ctx.propsDestructureRestId && ctx.propsDestructureDecl
                    ) {
                        if (left === 1) {
                            ctx.s.remove(node.start! + startOffset, node.end! + startOffset)
                        } else {
                            let start = decl.start! + startOffset
                            let end = decl.end! + startOffset
                            if (i === total - 1) {
                                start = node.declarations[lastNonRemoved!].end! + startOffset
                            } else {
                                end = node.declarations[i + 1].start! + startOffset
                            }
                            ctx.s.remove(start, end)
                            left--
                        }
                    } else {
                        lastNonRemoved = i
                    }
                }
            }
        }

        let isAllLiteral = false
        if (
            (node.type === 'VariableDeclaration' || node.type === 'FunctionDeclaration' || node.type === 'ClassDeclaration' || node.type === 'TSEnumDeclaration') && !node.declare
        ) {
            isAllLiteral = walkDeclaration(node, setupBindings, cantonImportAliases, hoistStatic, !!ctx.propsDestructureDecl)
        }

        if (hoistStatic && isAllLiteral) {
            hoistNode(node)
        }

        if (
            (node.type === 'VariableDeclaration' && !node.declare) || node.type.endsWith('Statement')
        ) {
            walk(node, {
                enter(child: Node) {
                    if (isFunctionType(child)) {
                        this.skip()
                    }
                    if (child.type === 'AwaitExpression' || child.type === 'ForOfStatement' && child.await) {
                        ctx.error('CTA 初始化必须同步；异步加载请通过明确状态与控制流表达', child)
                    }
                },
            })
        }

        if (
            node.type === 'ExportNamedDeclaration' || node.type === 'ExportAllDeclaration' || node.type === 'ExportDefaultDeclaration'
        ) {
            ctx.error('CTA script 不接受模块导出，请将共享逻辑和类型导出放入独立 TS 模块', node)
        }

        if (ctx.isTS) {
            if (
                node.type.startsWith('TS') || (node.type === 'ExportNamedDeclaration' && node.exportKind === 'type') || (node.type === 'VariableDeclaration' && node.declare)
            ) {
                if (node.type !== 'TSEnumDeclaration') {
                    hoistNode(node)
                }
            }
        }
    }

    if (ctx.propsDestructureDecl) {
        transformDestructuredProps(ctx, cantonImportAliases)
    }

    checkInvalidScopeReference(ctx.propsRuntimeDecl, DEFINE_PROPS)
    checkInvalidScopeReference(ctx.propsRuntimeDefaults, DEFINE_PROPS)
    checkInvalidScopeReference(ctx.propsDestructureDecl, DEFINE_PROPS)
    ctx.s.remove(0, startOffset)
    ctx.s.remove(endOffset, source.length)

    for (const [key, { isType, imported, source }] of Object.entries(ctx.userImports)) {
        if (isType) continue
        if (source === '@canton/framework/ui' && imported === 'M') {
            (ctx.bindingMetadata.__cantonModifierRoots ??= []).push(key)
        }
        ctx.bindingMetadata[key] = imported === '*' || (imported === 'default' && source.endsWith('.cta')) || (source === '@canton/framework') ? BindingTypes.SETUP_CONST : BindingTypes.SETUP_MAYBE_REF
    }
    for (const key in setupBindings) {
        ctx.bindingMetadata[key] = setupBindings[key]
    }

    // 整理 setup 参数
    let args = ctx.propsTypeDecl ? `__props: ${ctx.getString(ctx.propsTypeDecl)}` : '__props'
    if (ctx.propsDecl) {
        if (ctx.propsDestructureRestId) {
            ctx.s.overwrite(startOffset + ctx.propsCall!.start!, startOffset + ctx.propsCall!.end!, `${ctx.helper(`createPropsRestProxy`)}(__props, ${JSON.stringify(Object.keys(ctx.propsDestructuredBindings))})`)
            ctx.s.overwrite(startOffset + ctx.propsDestructureDecl!.start!, startOffset + ctx.propsDestructureDecl!.end!, ctx.propsDestructureRestId)
        } else if (!ctx.propsDestructureDecl) {
            ctx.s.overwrite(startOffset + ctx.propsCall!.start!, startOffset + ctx.propsCall!.end!, '__props')
        }
    }

    let templateMap
    let slotNames: readonly string[] = []
    let returned
    const propsDecl = genRuntimeProps(ctx)
    if (cta.template) {

        const { code, ast, preamble, tips, errors, map } = compileTemplate({
            filename,
            ast: cta.template.ast,
            source: cta.template.content,
            inMap: cta.template.map,
            ...options.templateOptions,
            compilerOptions: {
                ...(options.templateOptions && options.templateOptions.compilerOptions),
                inline: true,
                isTS: ctx.isTS,
                bindingMetadata: ctx.bindingMetadata,
                slotsType: SLOTS_TYPE,
            },
        })
        slotNames = ast?.slotNames ?? []
        templateMap = map
        if (tips.length) {
            tips.forEach(warnOnce)
        }
        const err = errors[0]
        if (typeof err === 'string') {
            throw new Error(err)
        } else if (err) {
            if (err.loc) {
                err.message += `\n\n` + cta.filename + '\n' + generateCodeFrame(source, err.loc.start.offset, err.loc.end.offset) + `\n`
            }
            throw err
        }
        if (preamble) {
            ctx.s.prepend(preamble)
        }

        if (ast && ast.helpers.has(UNREF)) {
            ctx.helperImports.delete('unref')
        }
        returned = code
    } else {
        returned = `() => {}`
    }

    ctx.s.appendRight(endOffset, `\nreturn ${returned}\n}\n\n`)

    const genDefaultAs = options.genDefaultAs ? `const ${options.genDefaultAs} =` : `export default`

    let runtimeOptions = `\n    __file: ${JSON.stringify(filename)},`
    let slotTypes = ''
    if (slotNames.length) {
        // 模板出口决定有哪些槽位，defineSlots 没有写到的槽位没有参数
        const declared = ctx.slotsCall?.typeParameters ? ctx.getString(ctx.slotsCall.typeParameters.params[0]) : '{}'
        slotTypes = `type __DefinedSlots = ${declared}\ntype ${SLOTS_TYPE} = { [K in ${slotNames.map(name => JSON.stringify(name)).join(' | ')}]: K extends keyof __DefinedSlots ? __DefinedSlots[K] : {} }\n`
        runtimeOptions += `\n    slots: { ${slotNames.map(name => `${JSON.stringify(name)}: ${ctx.helper('slotParams')}<${SLOTS_TYPE}[${JSON.stringify(name)}]>()`).join(', ')} },`
    }
    if (filename && filename !== DEFAULT_FILENAME) {
        const match = filename.match(/([^/\\]+)\.\w+$/)
        if (match) {
            runtimeOptions += `\n  __name: '${match[1]}',`
        }
    }

    if (propsDecl) runtimeOptions += `\n  props: ${propsDecl},`

    ctx.s.prependLeft(startOffset, `\n${slotTypes}${genDefaultAs} ${ctx.helper('defineArrangable')}({${runtimeOptions}\n    setup(${args}) {\n`)
    ctx.s.appendRight(endOffset, `})`)

    // 汇总 Canton 编译辅助函数导入
    if (ctx.helperImports.size > 0) {
        const runtimeModuleName = options.templateOptions?.compilerOptions?.runtimeModuleName
        const importSrc = runtimeModuleName ? JSON.stringify(runtimeModuleName) : `'@canton/framework/internal'`
        ctx.s.prepend(
            `
import { ${[...ctx.helperImports]
                .map(h => `${h} as _${h}`)
                .join(', ')} } from ${importSrc}\n`,
        )
    }

    const content = ctx.s.toString()
    let map =
        options.sourceMap !== false
            ? (ctx.s.generateMap({
                source: filename,
                hires: true,
                includeContent: true,
            }) as unknown as RawSourceMap)
            : undefined
    if (templateMap && map) {
        const offset = content.indexOf(returned)
        const templateLineOffset = content.slice(0, offset).split(/\r?\n/).length - 1
        map = mergeSourceMaps(map, templateMap, templateLineOffset)
    }
    return {
        ...script,
        bindings: ctx.bindingMetadata,
        imports: ctx.userImports,
        content,
        map,
        scriptAst: scriptAst?.body,
        deps: ctx.deps ? [...ctx.deps] : undefined,
    }
}

function registerBinding(bindings: Record<string, BindingTypes>, node: Identifier, type: BindingTypes) {
    bindings[node.name] = type
}

function walkDeclaration(node: Declaration, bindings: Record<string, BindingTypes>, userImportAliases: Record<string, string>, hoistStatic: boolean, isPropsDestructureEnabled = false): boolean {
    let isAllLiteral = false

    if (node.type === 'VariableDeclaration') {
        const isConst = node.kind === 'const'
        isAllLiteral = isConst && node.declarations.every(decl => decl.id.type === 'Identifier' && isStaticNode(decl.init!))

        for (const { id, init: _init } of node.declarations) {
            const init = _init && unwrapTSNode(_init)
            const isConstMacroCall = isConst && isCallOf(init, c => c === DEFINE_PROPS || c === WITH_DEFAULTS)
            if (id.type === 'Identifier') {
                let bindingType
                const userReactiveBinding = userImportAliases['reactive']
                if (
                    hoistStatic && (isAllLiteral || (isConst && isStaticNode(init!)))
                ) {
                    bindingType = BindingTypes.LITERAL_CONST
                } else if (isCallOf(init, userReactiveBinding)) {
                    bindingType = isConst ? BindingTypes.SETUP_REACTIVE_CONST : BindingTypes.SETUP_LET
                } else if (
                    isConstMacroCall || (isConst && canNeverBeRef(init!, userReactiveBinding))
                ) {
                    bindingType = isCallOf(init, DEFINE_PROPS) ? BindingTypes.SETUP_REACTIVE_CONST : BindingTypes.SETUP_CONST
                } else if (isConst) {
                    if (
                        isCallOf(init, m => m === userImportAliases['ref'] || m === userImportAliases['computed'] || m === userImportAliases['shallowRef'] || m === userImportAliases['customRef'] || m === userImportAliases['toRef'])
                    ) {
                        bindingType = BindingTypes.SETUP_REF
                    } else {
                        bindingType = BindingTypes.SETUP_MAYBE_REF
                    }
                } else {
                    bindingType = BindingTypes.SETUP_LET
                }
                registerBinding(bindings, id, bindingType)
            } else {
                if (isCallOf(init, DEFINE_PROPS) && isPropsDestructureEnabled) {
                    continue
                }
                if (id.type === 'ObjectPattern') {
                    walkObjectPattern(id, bindings, isConst, isConstMacroCall)
                } else if (id.type === 'ArrayPattern') {
                    walkArrayPattern(id, bindings, isConst, isConstMacroCall)
                }
            }
        }
    } else if (node.type === 'TSEnumDeclaration') {
        isAllLiteral = node.members.every(member => !member.initializer || isStaticNode(member.initializer))
        bindings[node.id!.name] = isAllLiteral ? BindingTypes.LITERAL_CONST : BindingTypes.SETUP_CONST
    } else if (
        node.type === 'FunctionDeclaration' || node.type === 'ClassDeclaration'
    ) {
        bindings[node.id!.name] = BindingTypes.SETUP_CONST
    }

    return isAllLiteral
}

function walkObjectPattern(node: ObjectPattern, bindings: Record<string, BindingTypes>, isConst: boolean, isDefineCall = false) {
    for (const p of node.properties) {
        if (p.type === 'ObjectProperty') {
            if (p.key.type === 'Identifier' && p.key === p.value) {
                const type = isDefineCall ? BindingTypes.SETUP_CONST : isConst ? BindingTypes.SETUP_MAYBE_REF : BindingTypes.SETUP_LET
                registerBinding(bindings, p.key, type)
            } else {
                walkPattern(p.value, bindings, isConst, isDefineCall)
            }
        } else {
            const type = isConst ? BindingTypes.SETUP_CONST : BindingTypes.SETUP_LET
            registerBinding(bindings, p.argument as Identifier, type)
        }
    }
}

function walkArrayPattern(node: ArrayPattern, bindings: Record<string, BindingTypes>, isConst: boolean, isDefineCall = false) {
    for (const e of node.elements) {
        e && walkPattern(e, bindings, isConst, isDefineCall)
    }
}

function walkPattern(node: Node, bindings: Record<string, BindingTypes>, isConst: boolean, isDefineCall = false) {
    if (node.type === 'Identifier') {
        const type = isDefineCall ? BindingTypes.SETUP_CONST : isConst ? BindingTypes.SETUP_MAYBE_REF : BindingTypes.SETUP_LET
        registerBinding(bindings, node, type)
    } else if (node.type === 'RestElement') {
        const type = isConst ? BindingTypes.SETUP_CONST : BindingTypes.SETUP_LET
        registerBinding(bindings, node.argument as Identifier, type)
    } else if (node.type === 'ObjectPattern') {
        walkObjectPattern(node, bindings, isConst)
    } else if (node.type === 'ArrayPattern') {
        walkArrayPattern(node, bindings, isConst)
    } else if (node.type === 'AssignmentPattern') {
        if (node.left.type === 'Identifier') {
            const type = isDefineCall ? BindingTypes.SETUP_CONST : isConst ? BindingTypes.SETUP_MAYBE_REF : BindingTypes.SETUP_LET
            registerBinding(bindings, node.left, type)
        } else {
            walkPattern(node.left, bindings, isConst)
        }
    }
}

function canNeverBeRef(node: Node, userReactiveImport?: string): boolean {
    if (isCallOf(node, userReactiveImport)) {
        return true
    }
    switch (node.type) {
        case 'UnaryExpression':
        case 'BinaryExpression':
        case 'ArrayExpression':
        case 'ObjectExpression':
        case 'FunctionExpression':
        case 'ArrowFunctionExpression':
        case 'UpdateExpression':
        case 'ClassExpression':
        case 'TaggedTemplateExpression':
            return true
        case 'SequenceExpression':
            return canNeverBeRef(node.expressions[node.expressions.length - 1], userReactiveImport)
        default:
            if (isLiteralNode(node)) {
                return true
            }
            return false
    }
}

function isStaticNode(node: Node): boolean {
    node = unwrapTSNode(node)

    switch (node.type) {
        case 'UnaryExpression':
            return isStaticNode(node.argument)

        case 'LogicalExpression': // 1 > 2
        case 'BinaryExpression': // 1 + 2
            return isStaticNode(node.left) && isStaticNode(node.right)

        case 'ConditionalExpression': {
            // 1 ? 2 : 3
            return (isStaticNode(node.test) && isStaticNode(node.consequent) && isStaticNode(node.alternate))
        }

        case 'SequenceExpression': // (1, 2)
        case 'TemplateLiteral':
            return node.expressions.every(expr => isStaticNode(expr))

        case 'ParenthesizedExpression': // (1)
            return isStaticNode(node.expression)

        case 'StringLiteral':
        case 'NumericLiteral':
        case 'BooleanLiteral':
        case 'NullLiteral':
        case 'BigIntLiteral':
            return true
    }
    return false
}

export function mergeSourceMaps(scriptMap: RawSourceMap, templateMap: RawSourceMap, templateLineOffset: number): RawSourceMap {
    const generator = new SourceMapGenerator()
    const addMapping = (map: RawSourceMap, lineOffset = 0) => {
        const consumer = new SourceMapConsumer(map);

        (consumer as any).sources.forEach((sourceFile: string) => {
            (generator as any)._sources.add(sourceFile)

            const sourceContent = consumer.sourceContentFor(sourceFile)
            if (sourceContent != null) generator.setSourceContent(sourceFile, sourceContent)
        })

        consumer.eachMapping(m => {
            if (m.originalLine == null) return

            generator.addMapping({
                generated: {
                    line: m.generatedLine + lineOffset,
                    column: m.generatedColumn,
                },
                original: {
                    line: m.originalLine,
                    column: m.originalColumn!,
                },
                source: m.source,
                name: m.name,
            })
        })
    }

    addMapping(scriptMap)
    addMapping(templateMap, templateLineOffset);

    (generator as any)._sourceRoot = scriptMap.sourceRoot;
    (generator as any)._file = scriptMap.file

    return (generator as any).toJSON()
}
