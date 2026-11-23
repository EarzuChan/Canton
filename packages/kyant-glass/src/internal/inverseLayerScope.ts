import { BlendMode, CompositingStrategy, RectangleShape, type ColorFilter, type Density, type DrawMatrixTransform, type DrawSize, type GraphicsLayerOptions, type RenderEffect, type Shape, type TransformOriginValue } from '@canton/framework/ui'

// 对齐 kyant-backdrop 的 GraphicsLayerScope：供 drawBackdrop 的 layerBlock 配置前景变换与图层属性
export interface BackdropLayerScope extends Density {
    readonly size: DrawSize
    scaleX: number
    scaleY: number
    alpha: number
    translationX: number
    translationY: number
    rotationZ: number
    transformOrigin: TransformOriginValue
    shape: Shape
    clip: boolean
    renderEffect: RenderEffect | null
    blendMode: BlendMode
    colorFilter: ColorFilter | null
    compositingStrategy: CompositingStrategy
}

export type BackdropLayerBlock = (scope: BackdropLayerScope) => void

// 逆向图层作用域：求前景 rotationZ / scaleX / scaleY 的 2D 逆线性矩阵，用于采样背景时抵消前景自身形变
export class InverseLayerScope implements BackdropLayerScope {
    size: DrawSize = { width: 0, height: 0 }
    private currentDensity: Density = { dpToPx: (v) => v, pxToDp: (v) => v, spToPx: (v) => v, pxToSp: (v) => v }
    scaleX = 1
    scaleY = 1
    alpha = 1
    translationX = 0
    translationY = 0
    rotationZ = 0
    transformOrigin: TransformOriginValue = 'Center'
    shape: Shape = RectangleShape
    clip = false
    renderEffect: RenderEffect | null = null
    blendMode: BlendMode = BlendMode.SrcOver
    colorFilter: ColorFilter | null = null
    compositingStrategy: CompositingStrategy = CompositingStrategy.Auto

    dpToPx(dp: number): number {
        return this.currentDensity.dpToPx(dp)
    }
    pxToDp(px: number): number {
        return this.currentDensity.pxToDp(px)
    }
    spToPx(sp: number): number {
        return this.currentDensity.spToPx(sp)
    }
    pxToSp(px: number): number {
        return this.currentDensity.pxToSp(px)
    }

    reset(): void {
        this.size = { width: 0, height: 0 }
        this.currentDensity = { dpToPx: (v) => v, pxToDp: (v) => v, spToPx: (v) => v, pxToSp: (v) => v }
        this.scaleX = 1
        this.scaleY = 1
        this.alpha = 1
        this.translationX = 0
        this.translationY = 0
        this.rotationZ = 0
        this.transformOrigin = 'Center'
        this.shape = RectangleShape
        this.clip = false
        this.renderEffect = null
        this.blendMode = BlendMode.SrcOver
        this.colorFilter = null
        this.compositingStrategy = CompositingStrategy.Auto
    }

    evaluate(size: DrawSize, density: Density, block: BackdropLayerBlock): void {
        this.reset()
        this.size = size
        this.currentDensity = density
        block(this)
    }

    toGraphicsLayerOptions(): GraphicsLayerOptions {
        return {
            translationX: this.translationX,
            translationY: this.translationY,
            scaleX: this.scaleX,
            scaleY: this.scaleY,
            rotationZ: this.rotationZ,
            alpha: this.alpha,
            clip: this.clip,
            shape: this.shape,
            renderEffect: this.renderEffect,
            blendMode: this.blendMode,
            colorFilter: this.colorFilter,
            compositingStrategy: this.compositingStrategy,
            transformOrigin: this.transformOrigin,
        }
    }

    computeInverseMatrix(): DrawMatrixTransform | null {
        const sx = this.scaleX
        const sy = this.scaleY
        const rzDeg = this.rotationZ
        if (sx === 1 && sy === 1 && rzDeg === 0) return null
        if (rzDeg === 0) {
            if (sx === 0 || sy === 0) return null
            return { a: 1 / sx, b: 0, c: 0, d: 1 / sy, tx: 0, ty: 0 }
        }
        const rz = (rzDeg * Math.PI) / 180
        const sinZ = Math.sin(rz)
        const cosZ = Math.cos(rz)
        // 正向 2D 线性矩阵（顺时针旋转 + 缩放）：x' = cos*sx*x - sin*sy*y, y' = sin*sx*x + cos*sy*y
        const a = cosZ * sx
        const b = sinZ * sx
        const c = -sinZ * sy
        const d = cosZ * sy
        const det = a * d - b * c
        if (!Number.isFinite(det) || Math.abs(det) < 1e-8) return null
        const invDet = 1 / det
        return {
            a: d * invDet,
            b: -b * invDet,
            c: -c * invDet,
            d: a * invDet,
            tx: 0,
            ty: 0,
        }
    }
}
