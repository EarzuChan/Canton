import ts from 'typescript'
import { unitFieldContracts, type UnitFields, type ValueUnit } from '@canton/shared'

export type ValueRule = ValueUnit | UnitFields
export interface CtaValueContract {
    value?: ValueUnit
    fields?: UnitFields
    arguments?: readonly (ValueRule | undefined)[]
    result?: ValueUnit
    modifier?: boolean
    modifierCall?: boolean
    call?: boolean
    checkProps?: boolean
    unref?: boolean
    parameterInputs?: boolean
    resolve?: boolean
}

// 契约绑定正式模块的声明身份，重导出和别名共享身份，局部同名声明保持原始含义
export function createCtaValueContracts(checker: ts.TypeChecker, source: ts.SourceFile) {
    const symbols = new Map<ts.Symbol, CtaValueContract>()
    const declarations = new Map<ts.Node, CtaValueContract>()
    const actual = (symbol: ts.Symbol | undefined): ts.Symbol | undefined => symbol && symbol.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(symbol) : symbol
    const register = (symbol: ts.Symbol | undefined, contract: CtaValueContract): void => {
        symbol = actual(symbol)
        if (!symbol) return
        symbols.set(symbol, { ...symbols.get(symbol), ...contract })
        for (const declaration of symbol.declarations ?? []) declarations.set(declaration, symbols.get(symbol)!)
        for (const signature of checker.getTypeOfSymbolAtLocation(symbol, source).getCallSignatures()) if (signature.declaration && symbol.declarations?.some(declaration => declaration === signature.declaration!.parent)) declarations.set(signature.declaration, symbols.get(symbol)!)
    }
    const modules = new Map<string, ts.Symbol>()
    for (const statement of source.statements) if (ts.isImportDeclaration(statement) && ts.isStringLiteral(statement.moduleSpecifier)) {
        const symbol = checker.getSymbolAtLocation(statement.moduleSpecifier)
        if (symbol) modules.set(statement.moduleSpecifier.text, symbol)
    }
    const exported = (module: string, name: string): ts.Symbol | undefined => {
        const symbol = modules.get(module)
        return symbol && actual(checker.getExportsOfModule(symbol).find(item => item.name === name))
    }
    const property = (symbol: ts.Symbol | undefined, name: string): ts.Symbol | undefined => symbol && checker.getTypeOfSymbolAtLocation(symbol, source).getProperty(name)
    const resultType = (symbol: ts.Symbol | undefined): ts.Type | undefined => {
        const signature = symbol && checker.getTypeOfSymbolAtLocation(symbol, source).getCallSignatures()[0]
        return signature && checker.getReturnTypeOfSignature(signature)
    }
    const animation = '@canton/framework/animation'
    const ui = '@canton/framework/ui'
    const root = '@canton/framework'
    const internal = '@canton/framework/internal'
    for (const [name, value] of [['Dp', 'dp'], ['Sp', 'sp'], ['Px', 'px']] as const) register(exported(ui, name), { value })
    const color = exported(ui, 'Color')
    register(color, { value: 'color' })
    register(property(color, 'hsl'), { value: 'color' })
    const fields = {
        Shape: 'shape', CornerSizes: 'shape', Brush: 'brush', SolidColor: 'brush', LinearGradient: 'brush', RadialGradient: 'brush', SweepGradient: 'brush', ShaderBrush: 'brush', PaddingValue: 'padding', Padding: 'padding', SizeRange: 'sizeRange', BorderOptions: 'border', ShadowOptions: 'shadow', PaintOptions: 'paint', TextOptions: 'text', TextFieldOptions: 'textField', GraphicsLayerOptions: 'graphics', TextStyleProp: 'style', ArrangementProp: 'arrangement', RowPolicyOptions: 'row', ColumnPolicyOptions: 'column', FlowRowPolicyOptions: 'flow', FlowColumnPolicyOptions: 'flow', PainterSize: 'pxSize', ColorFilter: 'colorFilter', TintColorFilter: 'colorFilter', LightingColorFilter: 'colorFilter', RenderLayer: 'renderLayer', LayerOffsetPx: 'pxOffset', LayerSizePx: 'pxSize', PxOffset: 'pxOffset', PxSize: 'pxSize', PxRect: 'pxRect', LayoutCoordinates: 'layoutCoordinates',
    }
    for (const [name, field] of Object.entries(fields)) register(exported(ui, name), { fields: unitFieldContracts[field] })
    for (const [name, field] of [['DrawSize', 'pxSize'], ['DrawRect', 'drawRect'], ['DrawCorners', 'pxCorners'], ['DrawPaint', 'drawPaint'], ['DrawTransform', 'graphics']] as const) register(exported(ui, name), { fields: unitFieldContracts[field] })
    const colorFilterSymbol = exported(ui, 'ColorFilter')
    register(property(colorFilterSymbol, 'tint'), { arguments: ['color'] })
    register(property(colorFilterSymbol, 'lighting'), { arguments: ['color', 'color'] })
    const renderEffectSymbol = exported(ui, 'RenderEffect')
    register(property(renderEffectSymbol, 'blur'), { arguments: ['px', 'px'] })
    register(property(renderEffectSymbol, 'offset'), { arguments: ['px', 'px'] })
    const runtimeShader = exported(ui, 'RuntimeShader')
    const runtimeShaderType = runtimeShader && checker.getDeclaredTypeOfSymbol(runtimeShader)
    register(runtimeShaderType?.getProperty('setColor'), { arguments: [undefined, 'color'] })
    const coordinates = exported(ui, 'LayoutCoordinates')
    const coordinatesType = coordinates && checker.getDeclaredTypeOfSymbol(coordinates)
    register(coordinatesType?.getProperty('localToWindow'), { arguments: [unitFieldContracts.pxOffset] })
    register(coordinatesType?.getProperty('windowToLocal'), { arguments: [unitFieldContracts.pxOffset] })
    register(coordinatesType?.getProperty('localPositionOf'), { arguments: [undefined, unitFieldContracts.pxOffset] })
    const drawScope = exported(ui, 'DrawScope')
    const drawType = drawScope && checker.getDeclaredTypeOfSymbol(drawScope)
    const drawRect = { ...unitFieldContracts.drawRect, ...unitFieldContracts.drawPaint }
    for (const method of ['drawRect', 'drawOval']) register(drawType?.getProperty(method), { arguments: [drawRect] })
    register(drawType?.getProperty('drawRoundRect'), { arguments: [unitFieldContracts.drawRoundRect] })
    register(drawType?.getProperty('drawOutline'), { arguments: [undefined, unitFieldContracts.drawOutline] })
    register(drawType?.getProperty('drawCircle'), { arguments: [unitFieldContracts.drawCircle] })
    register(drawType?.getProperty('drawLine'), { arguments: [unitFieldContracts.drawLine] })
    register(drawType?.getProperty('drawPath'), { arguments: [undefined, unitFieldContracts.drawPaint] })
    register(drawType?.getProperty('drawText'), { arguments: [unitFieldContracts.drawText] })
    register(drawType?.getProperty('drawPainter'), { arguments: [undefined, unitFieldContracts.drawPainter] })
    register(drawType?.getProperty('recordLayer'), { arguments: [undefined, unitFieldContracts.pxSize] })
    for (const method of ['clipRect', 'clipOval']) register(drawType?.getProperty(method), { arguments: [unitFieldContracts.drawRect] })
    register(drawType?.getProperty('clipRoundRect'), { arguments: [{ ...unitFieldContracts.drawRect, radius: 'px', corners: unitFieldContracts.pxCorners }] })
    register(drawType?.getProperty('clipOutline'), { arguments: [undefined, unitFieldContracts.drawRect] })
    register(drawType?.getProperty('withTransform'), { arguments: [unitFieldContracts.graphics] })
    for (const [name, field] of [['Offset', 'dpOffset'], ['Size', 'dpSize'], ['Rect', 'dpRect']] as const) register(exported(animation, name), { fields: unitFieldContracts[field] })
    register(exported(animation, 'VisibilityTransform'), { fields: unitFieldContracts.graphics })
    const scroll = exported(root, 'ScrollState')
    register(scroll, { fields: unitFieldContracts.scroll })
    const scrollType = scroll && checker.getDeclaredTypeOfSymbol(scroll)
    register(scrollType?.getProperty('scrollTo'), { arguments: ['px'] })
    register(exported(root, 'createScrollState'), { arguments: [unitFieldContracts.scrollOptions] })
    register(property(exported(ui, 'GridCells'), 'Adaptive'), { arguments: ['length'] })
    const lazyState = exported(root, 'LazyState')
    register(lazyState, { fields: { ...unitFieldContracts.scroll, firstVisibleItemScrollOffsetPx: 'px' } })
    const lazyType = lazyState && checker.getDeclaredTypeOfSymbol(lazyState)
    register(lazyType?.getProperty('scrollToItem'), { arguments: [undefined, 'px'] })
    register(lazyType?.getProperty('animateScrollToItem'), { arguments: [undefined, 'px'] })
    for (const method of ['createLazyState', 'createLazyListState', 'createLazyGridState']) register(exported(root, method), { arguments: [{ firstVisibleItemScrollOffsetPx: 'px' }] })
    register(exported(root, 'WindowSize'), { fields: unitFieldContracts.dpSize })
    register(exported(ui, 'WindowSize'), { fields: unitFieldContracts.dpSize })
    register(exported(root, 'createWindowState'), { arguments: [{ size: unitFieldContracts.dpSize }] })
    register(exported(root, 'unref'), { unref: true })
    register(exported(ui, 'Modifier'), { modifier: true })
    register(exported(internal, 'cantonModifier'), { modifierCall: true })
    register(exported(internal, 'callArrangable'), { call: true })
    register(exported(internal, 'parameterInputs'), { parameterInputs: true })
    register(exported(internal, 'resolveArrangable'), { resolve: true })
    register(exported(ui, 'colorToHex'), { arguments: ['color'] })
    register(exported(ui, 'solidColor'), { arguments: ['color'] })
    register(exported(ui, 'rounded'), { arguments: ['length'] })
    register(exported(ui, 'roundedCorners'), { arguments: [unitFieldContracts.shape] })
    register(exported(ui, 'PaddingValues'), { arguments: [unitFieldContracts.padding] })
    const spacedBy = property(exported(ui, 'Arrangement'), 'spacedBy')
    register(spacedBy, { arguments: ['length'] })
    const arrangement = resultType(spacedBy)
    // 返回值的映射别名可能是通用 Readonly，字段契约只登记在正式字段的原始声明节点
    for (const field of arrangement?.getProperties() ?? []) for (const declaration of field.declarations ?? []) declarations.set(declaration.parent, { fields: unitFieldContracts.arrangement })
    const animatedValues = { dp: 'dp', color: 'color', offset: unitFieldContracts.dpOffset, size: unitFieldContracts.dpSize, rect: unitFieldContracts.dpRect } as const
    for (const [name, rule] of [['animatedDpAsRef', 'dp'], ['animatedColorAsRef', 'color'], ['animatedOffsetAsRef', 'offset'], ['animatedSizeAsRef', 'size'], ['animatedRectAsRef', 'rect']] as const) {
        register(exported(animation, name), { arguments: [animatedValues[rule]], result: rule === 'dp' || rule === 'color' ? rule : undefined })
    }
    const transition = resultType(exported(animation, 'createTransition'))
    for (const [method, rule] of [['animatedDp', 'dp'], ['animatedColor', 'color'], ['animatedOffset', 'offset'], ['animatedSize', 'size'], ['animatedRect', 'rect']] as const) {
        register(transition?.getProperty(method), { arguments: [undefined, animatedValues[rule]], result: rule === 'dp' || rule === 'color' ? rule : undefined })
    }
    const infinite = resultType(exported(animation, 'createInfiniteTransition'))
    for (const [method, rule] of [['animatedDp', 'dp'], ['animatedColor', 'color']] as const) {
        register(infinite?.getProperty(method), { arguments: [undefined, rule, rule], result: rule })
    }
    for (const statement of source.statements) if (ts.isFunctionDeclaration(statement) && statement.name?.text === '__cantonCheck' && statement.pos >= source.text.indexOf('\nimport * as __CantonUnitsFoundation')) register(checker.getSymbolAtLocation(statement.name), { checkProps: true })
    const forSymbol = (symbol: ts.Symbol | undefined): CtaValueContract | undefined => {
        symbol = actual(symbol)
        return symbol && symbols.get(symbol)
    }
    const forDeclaration = (node: ts.Node | undefined): CtaValueContract | undefined => node && declarations.get(node)
    const forExpression = (node: ts.Node, seen = new Set<ts.Node>()): CtaValueContract | undefined => {
        if (seen.has(node)) return
        seen.add(node)
        if (ts.isParenthesizedExpression(node) || ts.isAsExpression(node) || ts.isNonNullExpression(node) || ts.isSatisfiesExpression(node)) return forExpression(node.expression, seen)
        const symbol = actual(checker.getSymbolAtLocation(node))
        const contract = forSymbol(symbol)
        if (contract) return contract
        const declaration = symbol?.valueDeclaration
        const initializer = declaration && (ts.isVariableDeclaration(declaration) || ts.isPropertyAssignment(declaration)) && declaration.initializer
        if (initializer && (ts.isIdentifier(initializer) || ts.isPropertyAccessExpression(initializer) || ts.isElementAccessExpression(initializer) || ts.isParenthesizedExpression(initializer) || ts.isAsExpression(initializer) || ts.isNonNullExpression(initializer) || ts.isSatisfiesExpression(initializer))) return forExpression(initializer, seen)
    }
    const forCall = (node: ts.CallExpression): CtaValueContract | undefined => forExpression(node.expression) ?? forDeclaration(checker.getResolvedSignature(node)?.declaration)
    const fieldsOf = (type: ts.Type): UnitFields | undefined => {
        const fields = forSymbol(type.aliasSymbol)?.fields ?? forSymbol(type.symbol)?.fields
        if (fields) return fields
        for (const property of type.getProperties()) for (const declaration of property.declarations ?? []) {
            let parent: ts.Node | undefined = declaration.parent
            while (parent && !ts.isSourceFile(parent)) {
                const fields = forDeclaration(parent)?.fields
                if (fields) return fields
                parent = parent.parent
            }
        }
        return type.isUnion() ? type.types.map(fieldsOf).find(Boolean) : undefined
    }
    return { forSymbol, forDeclaration, forExpression, forCall, fieldsOf }
}

export type CtaValueContracts = ReturnType<typeof createCtaValueContracts>
