import { shallowRef, type ShallowRef } from '@canton/framework'
import { M, createRenderLayer, type Density, type DrawCallback, type DrawContentCallback, type DrawScope, type LayoutCoordinates, type Modifier, type RenderLayer } from '@canton/framework/ui'
import { InverseLayerScope, type BackdropLayerBlock } from './internal/inverseLayerScope.ts'

export interface BackdropDrawContext {
    readonly scope: DrawScope
    readonly density: Density
    readonly coordinates: LayoutCoordinates | null
    readonly layerBlock?: BackdropLayerBlock
}

export interface Backdrop {
    readonly isCoordinatesDependent: boolean
    drawBackdrop(context: BackdropDrawContext): void
}

const defaultLayerDraw: DrawContentCallback = (_scope, drawContent) => {
    drawContent()
}

export class LayerBackdrop implements Backdrop {
    readonly isCoordinatesDependent = true
    readonly graphicsLayer: RenderLayer
    readonly onDraw: DrawContentCallback
    readonly layerCoordinates: ShallowRef<LayoutCoordinates | null> = shallowRef(null)
    private inverseLayerScope: InverseLayerScope | null = null

    constructor(graphicsLayer: RenderLayer, onDraw: DrawContentCallback = defaultLayerDraw) {
        this.graphicsLayer = graphicsLayer
        this.onDraw = onDraw
    }

    drawBackdrop(context: BackdropDrawContext): void {
        const { scope, density, coordinates, layerBlock } = context
        const layerCoords = this.layerCoordinates.value
        if (!coordinates || !layerCoords || !coordinates.isAttached || !layerCoords.isAttached) return

        let offsetX = 0
        let offsetY = 0
        let affineInverse: { a: number; b: number; c: number; d: number; tx: number; ty: number } | null = null
        let resolvedFromCoordinates = false
        try {
            const origin = layerCoords.localPositionOf(coordinates, { x: 0, y: 0 })
            const unitX = layerCoords.localPositionOf(coordinates, { x: 1, y: 0 })
            const unitY = layerCoords.localPositionOf(coordinates, { x: 0, y: 1 })
            offsetX = origin.x
            offsetY = origin.y
            const a = unitX.x - origin.x
            const b = unitX.y - origin.y
            const c = unitY.x - origin.x
            const d = unitY.y - origin.y
            const isIdentityLinear = Math.abs(a - 1) < 1e-6 && Math.abs(b) < 1e-6 && Math.abs(c) < 1e-6 && Math.abs(d - 1) < 1e-6
            if (!isIdentityLinear) {
                const det = a * d - b * c
                if (!Number.isFinite(det) || Math.abs(det) < 1e-8) return
                const invDet = 1 / det
                const invA = d * invDet
                const invB = -b * invDet
                const invC = -c * invDet
                const invD = a * invDet
                affineInverse = {
                    a: invA,
                    b: invB,
                    c: invC,
                    d: invD,
                    tx: -(invA * offsetX + invC * offsetY),
                    ty: -(invB * offsetX + invD * offsetY),
                }
            }
            resolvedFromCoordinates = true
        } catch {
            const p = coordinates.positionInWindow()
            const lp = layerCoords.positionInWindow()
            offsetX = p.x - lp.x
            offsetY = p.y - lp.y
        }

        if (affineInverse) {
            scope.withTransform(affineInverse, () => {
                scope.drawLayer(this.graphicsLayer)
            })
            return
        }

        let inverseMatrix = null
        if (!resolvedFromCoordinates && layerBlock) {
            const inv = this.inverseLayerScope ?? (this.inverseLayerScope = new InverseLayerScope())
            inv.evaluate(scope.size, density, layerBlock)
            inverseMatrix = inv.computeInverseMatrix()
        }

        const drawTranslated = () => {
            scope.withTransform({ translationX: -offsetX, translationY: -offsetY }, () => {
                scope.drawLayer(this.graphicsLayer)
            })
        }

        if (inverseMatrix) scope.withTransform(inverseMatrix, drawTranslated)
        else drawTranslated()
    }

    dispose(): void {
        this.layerCoordinates.value = null
        this.graphicsLayer.dispose()
    }
}

export function createLayerBackdrop(options?: { graphicsLayer?: RenderLayer; onDraw?: DrawContentCallback }): LayerBackdrop {
    const layer = options?.graphicsLayer ?? createRenderLayer()
    return new LayerBackdrop(layer, options?.onDraw ?? defaultLayerDraw)
}

export function layerBackdrop(backdrop: LayerBackdrop): Modifier {
    return M.onGloballyPositioned((coords) => {
        backdrop.layerCoordinates.value = coords.isAttached ? coords : null
    }).drawWithContent((scope, drawContent) => {
        scope.recordLayer(backdrop.graphicsLayer, () => {
            backdrop.onDraw(scope, drawContent)
        })
        scope.drawLayer(backdrop.graphicsLayer)
    })
}

export function createCanvasBackdrop(onDraw: DrawCallback): Backdrop {
    return Object.freeze({
        isCoordinatesDependent: false,
        drawBackdrop({ scope }: BackdropDrawContext) {
            onDraw(scope)
        },
    })
}

export function createCombinedBackdrop(...backdrops: readonly Backdrop[]): Backdrop {
    const list = [...backdrops]
    return Object.freeze({
        isCoordinatesDependent: list.some((b) => b.isCoordinatesDependent),
        drawBackdrop(context: BackdropDrawContext) {
            for (const backdrop of list) {
                backdrop.drawBackdrop(context)
            }
        },
    })
}

export function createBackdrop(
    draw: (context: BackdropDrawContext) => void,
    options?: { isCoordinatesDependent?: boolean },
): Backdrop {
    return Object.freeze({
        isCoordinatesDependent: options?.isCoordinatesDependent ?? true,
        drawBackdrop: draw,
    })
}

export const emptyBackdrop: Backdrop = Object.freeze({
    isCoordinatesDependent: false,
    drawBackdrop() {}, // 空背景不绘制任何内容
})
