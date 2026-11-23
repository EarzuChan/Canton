import { reactive } from '@canton/reactivity'
import { Dp } from '../values/unit.ts'
import type { CaptionButtonKind, WindowBackdrop, WindowChrome, WindowCornerPreference, WindowModality, WindowPlacement, WindowPosition, WindowPositionPreset, WindowRole } from '../native/protocol.ts'
import { interactionStateInternal, type InteractionState } from './gesture.ts'
import { Modifier, type ModifierElement } from './modifier.ts'

export type { CaptionButtonKind, WindowBackdrop, WindowChrome, WindowCornerPreference, WindowModality, WindowPlacement, WindowPosition, WindowPositionPreset, WindowRole }

export type WindowSize = Readonly<{ width: number; height: number }>

export type WindowStateInit = Readonly<{
    size?: WindowSize
    position?: WindowPosition
    placement?: WindowPlacement
}>

export interface WindowState {
    size: WindowSize
    position: WindowPosition
    placement: WindowPlacement
    readonly focused: boolean
    readonly dpiScale: number
    activate(): void
    requestClose(): void
}

export interface AppHandle {
    exitApplication(): void
}

export interface WindowHandle {
    readonly state: WindowState
    draggableArea(): Modifier
    nonDraggableArea(): Modifier
    captionButton(kind: CaptionButtonKind, options?: Readonly<{ interactionState?: InteractionState }>): Modifier
}

export interface WindowStateInternal {
    bind(owner: symbol, windowId: number, handle: WindowHandle, command: (action: 'activate' | 'requestClose') => void): void
    unbind(owner: symbol, handle: WindowHandle): void
    applyNativeGeometry(size: WindowSize | undefined, position: WindowPosition | undefined, placement: WindowPlacement): void
    applyNativeFocus(focused: boolean): void
    applyNativeDpi(dpiScale: number): void
    readonly nativeGeometry: { size?: WindowSize; position?: WindowPosition; placement?: WindowPlacement }
}

const privateWindowIds = new WeakMap<object, number>()
const stateInternals = new WeakMap<WindowState, WindowStateInternal>()
const windowModifierOwners = new WeakMap<ModifierElement, () => number | undefined>()

const placements = new Set<string>(['normal', 'minimized', 'maximized', 'fullscreen'])
const positionPresets = new Set<string>(['platformDefault', 'centerScreen', 'centerOwner'])
const captionButtons = new Set<string>(['minimize', 'maximize', 'close'])

function unwrapDp(value: unknown, label: string): number {
    const raw = value instanceof Dp ? value.value : value
    if (typeof raw !== 'number' || !Number.isFinite(raw) || raw <= 0) throw new RangeError(`${label} 必须是正有限数值`)
    return raw
}

export function normalizeWindowSize(value: unknown, label = '窗口尺寸'): WindowSize {
    if (!value || typeof value !== 'object') throw new TypeError(`${label} 需要 { width, height } 对象`)
    const input = value as { width?: unknown; height?: unknown }
    return Object.freeze({ width: unwrapDp(input.width, `${label}.width`), height: unwrapDp(input.height, `${label}.height`) })
}

export function normalizeWindowPosition(value: unknown): WindowPosition {
    if (typeof value === 'string') {
        if (!positionPresets.has(value)) throw new TypeError(`窗口位置预设无效：${value}`)
        return value as WindowPositionPreset
    }
    if (!value || typeof value !== 'object') throw new TypeError('窗口位置需要预设名或 { x, y } 物理屏幕坐标')
    const input = value as { x?: unknown; y?: unknown }
    if (typeof input.x !== 'number' || !Number.isFinite(input.x) || typeof input.y !== 'number' || !Number.isFinite(input.y)) {
        throw new RangeError('窗口坐标 { x, y } 必须是有限数值')
    }
    return Object.freeze({ x: Math.round(input.x), y: Math.round(input.y) })
}

export function normalizeWindowPlacement(value: unknown): WindowPlacement {
    if (typeof value !== 'string' || !placements.has(value)) throw new TypeError(`窗口形态无效：${String(value)}`)
    return value as WindowPlacement
}

export function sameWindowPosition(a: WindowPosition | undefined, b: WindowPosition | undefined): boolean {
    if (a === b) return true
    if (!a || !b || typeof a === 'string' || typeof b === 'string') return false
    return a.x === b.x && a.y === b.y
}

export function sameWindowSize(a: WindowSize | undefined, b: WindowSize | undefined): boolean {
    if (a === b) return true
    if (!a || !b) return false
    return Math.abs(a.width - b.width) < 1e-4 && Math.abs(a.height - b.height) < 1e-4
}

export function createWindowState(init: WindowStateInit = {}): WindowState {
    const backing = reactive({
        size: init.size !== undefined ? normalizeWindowSize(init.size, 'WindowState.size') : Object.freeze({ width: 960, height: 640 }),
        position: init.position !== undefined ? normalizeWindowPosition(init.position) : ('platformDefault' as WindowPosition),
        placement: init.placement !== undefined ? normalizeWindowPlacement(init.placement) : ('normal' as WindowPlacement),
        focused: false,
        dpiScale: 1,
    })

    let boundOwner: symbol | undefined
    let sendCommand: ((action: 'activate' | 'requestClose') => void) | undefined
    const nativeGeometry: { size?: WindowSize; position?: WindowPosition; placement?: WindowPlacement } = {}

    const state: WindowState = {
        get size() { return backing.size },
        set size(value: WindowSize) {
            const normalized = normalizeWindowSize(value, 'WindowState.size')
            nativeGeometry.size = undefined
            backing.size = normalized
        },
        get position() { return backing.position },
        set position(value: WindowPosition) {
            const normalized = normalizeWindowPosition(value)
            nativeGeometry.position = undefined
            backing.position = normalized
        },
        get placement() { return backing.placement },
        set placement(value: WindowPlacement) {
            const normalized = normalizeWindowPlacement(value)
            nativeGeometry.placement = undefined
            backing.placement = normalized
        },
        get focused() { return backing.focused },
        get dpiScale() { return backing.dpiScale },
        activate() { sendCommand?.('activate') },
        requestClose() { sendCommand?.('requestClose') },
    }
    Object.freeze(state)

    stateInternals.set(state, {
        nativeGeometry,
        bind(owner, windowId, handle, command) {
            if (boundOwner && boundOwner !== owner) {
                nativeGeometry.size = undefined
                nativeGeometry.position = undefined
                nativeGeometry.placement = undefined
                backing.focused = false
            }
            boundOwner = owner
            sendCommand = command
            privateWindowIds.set(state, windowId)
            privateWindowIds.set(handle, windowId)
        },
        unbind(owner, handle) {
            privateWindowIds.delete(handle)
            if (boundOwner !== owner) return
            boundOwner = undefined
            sendCommand = undefined
            nativeGeometry.size = undefined
            nativeGeometry.position = undefined
            nativeGeometry.placement = undefined
            backing.focused = false
            privateWindowIds.delete(state)
        },
        applyNativeGeometry(size, position, placement) {
            if (size) {
                const normalized = Object.freeze({ width: size.width, height: size.height })
                nativeGeometry.size = normalized
                if (!sameWindowSize(backing.size, normalized)) backing.size = normalized
            }
            if (position) {
                nativeGeometry.position = position
                if (!sameWindowPosition(backing.position, position)) backing.position = position
            }
            nativeGeometry.placement = placement
            if (backing.placement !== placement) backing.placement = placement
        },
        applyNativeFocus(focused) {
            if (backing.focused !== focused) backing.focused = focused
        },
        applyNativeDpi(dpiScale) {
            if (!Number.isFinite(dpiScale) || dpiScale <= 0) return
            if (backing.dpiScale !== dpiScale) backing.dpiScale = dpiScale
        },
    })

    return state
}

export function isWindowState(value: unknown): value is WindowState {
    return typeof value === 'object' && value !== null && stateInternals.has(value as WindowState)
}

export function windowStateInternal(state: WindowState): WindowStateInternal {
    const internal = stateInternals.get(state)
    if (!internal) throw new TypeError('需要 createWindowState() 创建的 WindowState 对象')
    return internal
}

export function lookupPrivateWindowId(target: object): number | undefined {
    return privateWindowIds.get(target)
}

export function createWindowHandle(state: WindowState, activeWindowId: () => number | undefined): WindowHandle {
    const makeElement = (type: string, value: Record<string, unknown> = {}): Modifier => {
        if (activeWindowId() === undefined) throw new Error('WindowHandle 所属窗口已卸载或不在活跃画区内')
        const element: ModifierElement = Object.freeze({ type, value: Object.freeze(value) })
        windowModifierOwners.set(element, activeWindowId)
        return new Modifier([element])
    }
    const handle: WindowHandle = Object.freeze({
        state,
        draggableArea: () => makeElement('draggableArea'),
        nonDraggableArea: () => makeElement('nonDraggableArea'),
        captionButton(kind: CaptionButtonKind, options: Readonly<{ interactionState?: InteractionState }> = {}) {
            if (!captionButtons.has(kind)) throw new TypeError("captionButton 类型只能是 'minimize'、'maximize' 或 'close'")
            const payload: Record<string, unknown> = { button: kind }
            if (options.interactionState) payload.interaction = interactionStateInternal(options.interactionState).pointer
            return makeElement('captionButton', payload)
        },
    })
    return handle
}

export function assertWindowModifierOwner(element: ModifierElement, surfaceWindowId: number | undefined): void {
    if (element.type !== 'draggableArea' && element.type !== 'nonDraggableArea' && element.type !== 'captionButton') return
    const owner = windowModifierOwners.get(element)?.()
    if (owner === undefined || surfaceWindowId === undefined || owner !== surfaceWindowId) {
        throw new Error('WindowHandle 的非客户区 Modifier 只能用于所属 <Window> 画区内')
    }
}
