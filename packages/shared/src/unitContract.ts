export type ValueUnit = 'dp' | 'sp' | 'px' | 'color' | 'length'
export type UnitFields = Readonly<{ [name: string]: ValueUnit | UnitFields }>

const shape: UnitFields = { topStart: 'length', topEnd: 'length', bottomEnd: 'length', bottomStart: 'length' }
const brush: UnitFields = { color: 'color' }
const style: UnitFields = { fontSize: 'sp', lineHeight: 'sp', color: 'color' }
const offset: UnitFields = { x: 'length', y: 'length' }
const size: UnitFields = { width: 'length', height: 'length' }
const range: UnitFields = { min: 'length', max: 'length' }
const sizeRange: UnitFields = { minWidth: 'length', maxWidth: 'length', minHeight: 'length', maxHeight: 'length' }
const padding: UnitFields = { start: 'length', top: 'length', end: 'length', bottom: 'length', horizontal: 'length', vertical: 'length' }
const colorFilterValue: UnitFields = { tint: 'color', color: 'color', multiply: 'color', add: 'color' }
const graphics: UnitFields = { translationX: 'px', translationY: 'px', shape, colorFilter: colorFilterValue }
const pxOffset: UnitFields = { x: 'px', y: 'px' }
const pxSize: UnitFields = { width: 'px', height: 'px' }
const pxRect: UnitFields = { x: 'px', y: 'px', width: 'px', height: 'px' }
const pxCorners: UnitFields = { topStart: 'px', topEnd: 'px', bottomEnd: 'px', bottomStart: 'px' }

// 单位形状由 Modifier 原生协议表与值契约共用
export const unitShapes = { shape, brush, style, offset, size, range, sizeRange, padding, graphics, colorFilter: colorFilterValue } as const

export const unitFieldContracts: Readonly<Record<string, UnitFields>> = {
    style, shape, brush, padding, offset, size, rect: { ...offset, ...size }, range, sizeRange, graphics,
    border: { width: 'length', brush, shape },
    shadow: { shape, radius: 'length', spread: 'length', offset, color: 'color' },
    text: { style },
    textField: { textStyle: style },
    paint: { colorFilter: colorFilterValue },
    colorFilter: colorFilterValue,
    arrangement: { space: 'length' },
    row: { horizontalArrangement: { space: 'length' } },
    column: { verticalArrangement: { space: 'length' } },
    flow: { horizontalArrangement: { space: 'length' }, verticalArrangement: { space: 'length' } },
    scroll: { valuePx: 'px', maxValuePx: 'px', viewportSizePx: 'px', contentSizePx: 'px' },
    scrollOptions: { initialPx: 'px' },
    pxOffset,
    pxSize,
    pxRect,
    pxCorners,
    drawRect: pxRect,
    drawPaint: { color: 'color', strokeWidth: 'px', colorFilter: colorFilterValue },
    drawRoundRect: { ...pxRect, radius: 'px', corners: pxCorners, color: 'color', strokeWidth: 'px', colorFilter: colorFilterValue },
    drawOutline: { ...pxRect, color: 'color', strokeWidth: 'px', colorFilter: colorFilterValue },
    drawCircle: { centerX: 'px', centerY: 'px', radius: 'px', color: 'color', strokeWidth: 'px', colorFilter: colorFilterValue },
    drawLine: { startX: 'px', startY: 'px', endX: 'px', endY: 'px', color: 'color', strokeWidth: 'px', colorFilter: colorFilterValue },
    drawText: { x: 'px', y: 'px', width: 'px', color: 'color', fontSize: 'px', lineHeight: 'px' },
    drawPainter: { ...pxRect, tint: 'color', colorFilter: colorFilterValue },
    renderLayer: { translationX: 'px', translationY: 'px', topLeft: pxOffset, size: pxSize, shape, colorFilter: colorFilterValue },
    layoutCoordinates: { size: pxSize },
    dpOffset: { x: 'dp', y: 'dp' },
    dpSize: { width: 'dp', height: 'dp' },
    dpRect: { x: 'dp', y: 'dp', width: 'dp', height: 'dp' },
}
