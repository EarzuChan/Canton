import type { ArrangementName, AxisAlignment, HorizontalAlignment, VerticalAlignment } from '../values/primitives.ts'
import { CANTON_PROTOCOL_VERSION } from '@canton/shared'

export type NodeId = number
export type NativeEventCallback = (...args: unknown[]) => unknown

export type NativeReloadPayload = {
    path?: string
    timestamp?: number
}

// 字号单位 SP 即 em 大小；与 native/src/core 的 defaultFontSize 保持一致
export const defaultFontSize = 12

export type TextStyleProp = Readonly<{
    fontSize?: number
    lineHeight?: number
    color?: number
    // 字体族按序取首个可用者，缺字回落交给平台；省略时用平台默认字体
    fontFamily?: string | readonly string[]
    // 1 至 1000 的数值字重，规则同 CSS；FontWeight 提供常用常量
    fontWeight?: number
    fontStyle?: 'normal' | 'italic'
    // 缺失字重或字形时是否由 Canton 伪造，默认 true；关闭后严格使用选中的字面
    fontSynthesis?: boolean
}>

export type ArrangementProp<A extends AxisAlignment = AxisAlignment, N extends ArrangementName = ArrangementName> = N | Readonly<{
    kind: "spacedBy"
    spaceDp: number
    spacePx: number
    alignment?: A
}>

export type HorizontalArrangementProp = ArrangementProp<HorizontalAlignment, Exclude<ArrangementName, 'Top' | 'Bottom'>>
export type VerticalArrangementProp = ArrangementProp<VerticalAlignment, Exclude<ArrangementName, 'Start' | 'End'>>

export type ResourceRef =
    | string
    | Readonly<{ path: string }>

export type NativePropValue = | string | number | boolean | NativeEventCallback | TextStyleProp | ArrangementProp | ResourceRef

export type NativeBindingHandle = Readonly<{ identity: bigint; generation: bigint }>

export type NativeModifierHandle = NativeBindingHandle & Readonly<{ key: string; kind: string }>

export type WindowPlacement = 'normal' | 'minimized' | 'maximized' | 'fullscreen'
export type WindowPositionPreset = 'platformDefault' | 'centerScreen' | 'centerOwner'
export type WindowPosition = WindowPositionPreset | Readonly<{ x: number; y: number }>
export type WindowChrome = 'system' | 'customTitleBar' | 'frameless'
export type WindowRole = 'standard' | 'utility'
export type WindowModality = 'none' | 'owner' | 'application'
export type WindowBackdrop = 'none' | 'mica' | 'micaAlt' | 'acrylic'
export type WindowCornerPreference = 'default' | 'round' | 'roundSmall' | 'square'
export type CaptionButtonKind = 'minimize' | 'maximize' | 'close'

export type NativeAppConfig = Readonly<{
    exitOnLastWindowClosed: boolean
    appId?: string
    icon?: string
}>

export type NativeWindowConfig = Readonly<{
    ownerWindowId?: number
    title: string
    icon?: string
    visible: boolean
    sizeDp: Readonly<{ width: number; height: number }>
    minSizeDp?: Readonly<{ width: number; height: number }>
    maxSizeDp?: Readonly<{ width: number; height: number }>
    position: WindowPosition
    placement: WindowPlacement
    modality: WindowModality
    chrome: WindowChrome
    role: WindowRole
    resizable: boolean
    minimizable: boolean
    maximizable: boolean
    closable: boolean
    alwaysOnTop: boolean
    showInTaskbar?: boolean
    backdrop: WindowBackdrop
    cornerPreference: WindowCornerPreference
}>

export type NativeWindowGeometryEvent = Readonly<{
    widthPx: number
    heightPx: number
    x?: number
    y?: number
    hasPosition?: boolean
    placement: WindowPlacement
}>

export type NativeWindowDriver = Readonly<{
    onCloseRequest: (windowId: number) => void
    onGeometryChanged: (windowId: number, event: NativeWindowGeometryEvent) => void
    onFocusChanged: (windowId: number, focused: boolean) => void
    onDpiChanged: (windowId: number, dpiScale: number) => void
}>

// 真实宿主（QuickJsNativeApi）必然提供的成员均为必选
export type NativeTransactionTarget = {
    configureApp: (config: NativeAppConfig) => void
    requestAppExit: () => void
    allocateWindowId: () => number
    createWindow: (windowId: number, rootNodeId: NodeId, config: NativeWindowConfig) => void
    updateWindow: (windowId: number, config: NativeWindowConfig) => void
    destroyWindow: (windowId: number) => void
    windowCommand: (windowId: number, command: 'activate' | 'requestClose') => void
    installWindowDriver: (driver: NativeWindowDriver) => void
    focusCommand: (operation: 'request' | 'clear' | 'move' | 'cancel', requester: number, direction?: FocusDirection, windowId?: number) => boolean
    installFrameDriver: (prepare: (time: number) => void, complete: (success: boolean) => void, dispose: () => void) => void
    installLayoutDriver: (materialize: (id: NodeId, indices: readonly number[]) => void) => void
    continueRearrange: () => void
    currentTime: () => number
    requestFrame: (pending: boolean) => void
    beginRearrange: () => void
    submitRearrange: (complete: (error?: string) => void) => void
    abortRearrange: () => void
    // 资源已在本运行时解码过时，句柄携带 ready，Painter 首帧即就绪且不再回调
    acquirePainter: (resource: string, completion: (result: PainterCompletion) => void) => NativeBindingHandle & { ready?: PainterCompletion }
    releasePainter: (handle: NativeBindingHandle) => void
    // 自带字体：字节经 acquirePainter 取得，已到达的字面登记进字体注册表并推进字体环境代际
    defineFontFamily: (name: string, faces: readonly (NativeBindingHandle & Readonly<{ contentVersion: number; weight?: number; style?: 'normal' | 'italic' }>)[]) => void
    removeFontFamily: (name: string) => void
    beginFontPreload: () => void
    endFontPreload: () => void
    createPixelBuffer: (width: number, height: number) => NativeBindingHandle
    // 返回提交后的内容版本
    commitPixelBuffer: (handle: NativeBindingHandle, pixels: Uint32Array) => number
    registerBinding: (id: NodeId, input: string) => NativeBindingHandle
    updateBinding: (handle: NativeBindingHandle, value: NativeBindingValue) => void
    releaseBinding: (handle: NativeBindingHandle) => void
    modifierInstances: (id: NodeId) => readonly NativeModifierHandle[]
    registerModifierBinding: (id: NodeId, instance: NativeModifierHandle) => NativeBindingHandle
    runtimeVersion: number
    readAsset?: { (reference: string, text: true): Promise<string>, (reference: string, text: false): Promise<Uint8Array> }
    createNode: (id: NodeId, type: string, windowId?: number) => void
    deleteNode: (id: NodeId) => void
    insertChild: (parent: NodeId, child: NodeId, index: number) => void
    removeChild: (parent: NodeId, child: NodeId) => void
    unmount: () => void
    log: (level: string, tag: string, args: string[]) => void
    diagnosticsToast: (level: string, tag: string, title: string, args: string[], coalesce: boolean) => void
    diagnosticsRequestReload: (payload?: NativeReloadPayload) => void
    diagnosticsTriggerFakeError: (payload?: { message?: string }) => void
    diagnosticsSetToastsEnabled: (enabled: boolean) => void
}

export type NativeMutation = (target: NativeTransactionTarget) => void

export const CANTON_RUNTIME_VERSION = CANTON_PROTOCOL_VERSION
export type FocusDirection = 'next' | 'previous' | 'up' | 'down' | 'left' | 'right'

export type PainterCompletion = Readonly<{ contentVersion: number; width?: number; height?: number; error?: string }>

// 跨边界的描述形状：上层的 MeasurePolicy、Modifier 元素与降级后的 PX 修饰链都满足这些形状，原生按 Modifier 原生协议表逐字段准入
export type NativeMeasurePolicy = Readonly<{ kind: string }>
export type NativeModifierElement = Readonly<{ type: string; key?: string; value: unknown }>
export type NativeModifierChain = Readonly<{ elements: readonly NativeModifierElement[] }>
export type NativeBindingValue = NativePropValue | NativeMeasurePolicy | NativeModifierChain | NativeModifierElement | null

declare global {
    var __CANTON_NATIVE__: NativeTransactionTarget | undefined
}
