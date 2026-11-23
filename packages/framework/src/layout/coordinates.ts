export type PxOffset = Readonly<{ x: number; y: number }>
export type PxSize = Readonly<{ width: number; height: number }>
export type PxRect = Readonly<{ x: number; y: number; width: number; height: number }>
export type BoundsOptions = Readonly<{ clipBounds?: boolean }>

export interface LayoutCoordinates {
    readonly size: PxSize
    readonly isAttached: boolean
    positionInWindow(): PxOffset
    positionInParent(): PxOffset
    boundsInParent(): PxRect
    boundsInWindow(options?: BoundsOptions): PxRect
    localToWindow(relativeToLocal: PxOffset): PxOffset
    windowToLocal(relativeToWindow: PxOffset): PxOffset
    localPositionOf(sourceCoordinates: LayoutCoordinates, relativeToSource?: PxOffset): PxOffset
    localBoundingBoxOf(sourceCoordinates: LayoutCoordinates, options?: BoundsOptions): PxRect
}

export type NativePositionedPayload = Readonly<{
    windowId: number
    width: number
    height: number
    parentX: number
    parentY: number
    a: number
    b: number
    c: number
    d: number
    tx: number
    ty: number
    clipX: number
    clipY: number
    clipWidth: number
    clipHeight: number
    attached?: boolean
}>

export interface LayoutCoordinatesAttachToken {
    attached: boolean
}

interface CoordinatesInternal {
    readonly windowId: number
    readonly token: LayoutCoordinatesAttachToken
    readonly payload: NativePositionedPayload
}

const coordinateInternals = new WeakMap<LayoutCoordinates, CoordinatesInternal>()

function finite(value: number, name: string): number {
    if (typeof value !== 'number' || !Number.isFinite(value)) throw new TypeError(`${name} 需要有限数值`)
    return value
}

function projectPoint(p: NativePositionedPayload, x: number, y: number): PxOffset {
    return Object.freeze({
        x: p.a * x + p.c * y + p.tx,
        y: p.b * x + p.d * y + p.ty,
    })
}

function unprojectPoint(p: NativePositionedPayload, x: number, y: number): PxOffset {
    const det = p.a * p.d - p.b * p.c
    if (!Number.isFinite(det) || Math.abs(det) < 1e-12) {
        throw new Error('LayoutCoordinates 变换矩阵不可逆（缩放为零）')
    }
    const dx = x - p.tx
    const dy = y - p.ty
    return Object.freeze({
        x: (p.d * dx - p.c * dy) / det,
        y: (-p.b * dx + p.a * dy) / det,
    })
}

function boundingRect(points: readonly PxOffset[]): PxRect {
    let minX = points[0]!.x
    let minY = points[0]!.y
    let maxX = minX
    let maxY = minY
    for (let i = 1; i < points.length; i++) {
        const pt = points[i]!
        if (pt.x < minX) minX = pt.x
        if (pt.y < minY) minY = pt.y
        if (pt.x > maxX) maxX = pt.x
        if (pt.y > maxY) maxY = pt.y
    }
    return Object.freeze({ x: minX, y: minY, width: maxX - minX, height: maxY - minY })
}

export function createLayoutCoordinates(payload: NativePositionedPayload, token: LayoutCoordinatesAttachToken): LayoutCoordinates {
    const size = Object.freeze({ width: payload.width, height: payload.height })
    const isAttached = (): boolean => token.attached && payload.attached !== false

    const requireAttached = (target: LayoutCoordinates, label: string): CoordinatesInternal => {
        const internal = coordinateInternals.get(target)
        if (!internal) throw new TypeError(`${label} 需要有效的 LayoutCoordinates`)
        if (!internal.token.attached || internal.payload.attached === false) {
            throw new Error(`${label} 对应的节点已卸载，无法换算坐标`)
        }
        return internal
    }

    const coordinates: LayoutCoordinates = Object.freeze({
        get size() { return size },
        get isAttached() { return isAttached() },
        positionInWindow() {
            requireAttached(coordinates, 'LayoutCoordinates')
            return Object.freeze({ x: payload.tx, y: payload.ty })
        },
        positionInParent() {
            requireAttached(coordinates, 'LayoutCoordinates')
            return Object.freeze({ x: payload.parentX, y: payload.parentY })
        },
        boundsInParent() {
            requireAttached(coordinates, 'LayoutCoordinates')
            return Object.freeze({ x: payload.parentX, y: payload.parentY, width: payload.width, height: payload.height })
        },
        boundsInWindow(options: BoundsOptions = {}) {
            requireAttached(coordinates, 'LayoutCoordinates')
            if (options.clipBounds !== false) {
                return Object.freeze({ x: payload.clipX, y: payload.clipY, width: payload.clipWidth, height: payload.clipHeight })
            }
            const corners = [
                projectPoint(payload, 0, 0),
                projectPoint(payload, payload.width, 0),
                projectPoint(payload, 0, payload.height),
                projectPoint(payload, payload.width, payload.height),
            ]
            return boundingRect(corners)
        },
        localToWindow(relativeToLocal: PxOffset) {
            requireAttached(coordinates, 'LayoutCoordinates')
            return projectPoint(payload, finite(relativeToLocal.x, 'relativeToLocal.x'), finite(relativeToLocal.y, 'relativeToLocal.y'))
        },
        windowToLocal(relativeToWindow: PxOffset) {
            requireAttached(coordinates, 'LayoutCoordinates')
            return unprojectPoint(payload, finite(relativeToWindow.x, 'relativeToWindow.x'), finite(relativeToWindow.y, 'relativeToWindow.y'))
        },
        localPositionOf(sourceCoordinates: LayoutCoordinates, relativeToSource: PxOffset = { x: 0, y: 0 }) {
            const self = requireAttached(coordinates, '目标 LayoutCoordinates')
            const source = requireAttached(sourceCoordinates, '源 LayoutCoordinates')
            if (self.windowId !== source.windowId) {
                throw new Error('不能跨窗口计算 LayoutCoordinates 相对坐标')
            }
            const win = projectPoint(source.payload, finite(relativeToSource.x, 'relativeToSource.x'), finite(relativeToSource.y, 'relativeToSource.y'))
            return unprojectPoint(self.payload, win.x, win.y)
        },
        localBoundingBoxOf(sourceCoordinates: LayoutCoordinates, options: BoundsOptions = {}) {
            const self = requireAttached(coordinates, '目标 LayoutCoordinates')
            const source = requireAttached(sourceCoordinates, '源 LayoutCoordinates')
            if (self.windowId !== source.windowId) {
                throw new Error('不能跨窗口计算 LayoutCoordinates 相对包围盒')
            }
            if (options.clipBounds !== false) {
                const winRect = sourceCoordinates.boundsInWindow({ clipBounds: true })
                if (winRect.width <= 0 || winRect.height <= 0) {
                    const pt = unprojectPoint(self.payload, winRect.x, winRect.y)
                    return Object.freeze({ x: pt.x, y: pt.y, width: 0, height: 0 })
                }
                const corners = [
                    unprojectPoint(self.payload, winRect.x, winRect.y),
                    unprojectPoint(self.payload, winRect.x + winRect.width, winRect.y),
                    unprojectPoint(self.payload, winRect.x, winRect.y + winRect.height),
                    unprojectPoint(self.payload, winRect.x + winRect.width, winRect.y + winRect.height),
                ]
                return boundingRect(corners)
            }
            const sw = source.payload.width
            const sh = source.payload.height
            const corners = [
                unprojectPoint(self.payload, projectPoint(source.payload, 0, 0).x, projectPoint(source.payload, 0, 0).y),
                unprojectPoint(self.payload, projectPoint(source.payload, sw, 0).x, projectPoint(source.payload, sw, 0).y),
                unprojectPoint(self.payload, projectPoint(source.payload, 0, sh).x, projectPoint(source.payload, 0, sh).y),
                unprojectPoint(self.payload, projectPoint(source.payload, sw, sh).x, projectPoint(source.payload, sw, sh).y),
            ]
            return boundingRect(corners)
        },
    })

    coordinateInternals.set(coordinates, { windowId: payload.windowId, token, payload })
    return coordinates
}
