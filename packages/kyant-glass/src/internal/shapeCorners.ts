import type { Density, DrawSize, Shape } from '@canton/framework/ui'

// 计算形状在给定尺寸与像素密度下的拟合四角像素半径：[左上, 右上, 右下, 左下]
export function resolveShapeCornerRadiiPx(shape: Shape, size: DrawSize, density: Density): readonly [number, number, number, number] {
    const maxRadius = Math.max(0, Math.min(size.width, size.height) * 0.5)
    if (shape.type === 'circle') {
        return [maxRadius, maxRadius, maxRadius, maxRadius]
    }
    if (shape.type !== 'rounded') {
        return [0, 0, 0, 0]
    }
    const toPx = (dp: number, px: number) => Math.max(0, density.dpToPx(dp) + px)
    let tl = toPx(shape.topStartDp, shape.topStartPx)
    let tr = toPx(shape.topEndDp, shape.topEndPx)
    let br = toPx(shape.bottomEndDp, shape.bottomEndPx)
    let bl = toPx(shape.bottomStartDp, shape.bottomStartPx)

    const fitPair = (a: number, b: number, span: number) => (a + b > span && a + b > 0 ? span / (a + b) : 1)
    const factor = Math.min(1, fitPair(tl, tr, size.width), fitPair(bl, br, size.width), fitPair(tl, bl, size.height), fitPair(tr, br, size.height))
    tl *= factor
    tr *= factor
    br *= factor
    bl *= factor
    return [
        Math.min(tl, maxRadius),
        Math.min(tr, maxRadius),
        Math.min(br, maxRadius),
        Math.min(bl, maxRadius),
    ]
}
