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
export type NativeWindowCommand = 'activate' | 'requestClose' | 'requestAttentionInformational' | 'requestAttentionCritical' | 'stopAttention'

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
    transparent?: boolean
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

export type NativeGraphicsEnvironmentSnapshot = Readonly<{
    backend: 'd3d11'
    adapterName: string
    isSoftwareRenderer: boolean
    hardwareAccelerated: boolean
    isRemoteSession: boolean
    systemTransparency: boolean
    systemAnimations: boolean
}>

export type NativeLaunchRequest = Readonly<{
    args: readonly string[]
    workingDirectory: string
}>

export type NativeTrayIconSpec = Readonly<{
    id: string
    guid?: string
    icon?: string
    tooltip?: string
    visible: boolean
}>

export type NativeTrayMenuItemSpec = Readonly<{
    kind: 'item' | 'checkbox' | 'radio' | 'separator' | 'submenu'
    commandId: number
    label?: string
    enabled?: boolean
    checked?: boolean
    children?: readonly NativeTrayMenuItemSpec[]
}>

export type NativeTrayAnchorRect = Readonly<{
    left: number
    top: number
    right: number
    bottom: number
    cursorX: number
    cursorY: number
    workLeft: number
    workTop: number
    workRight: number
    workBottom: number
    dpiScale: number
}>

export type NativeTrayEvent = Readonly<{
    trayId: string
    kind: 'click' | 'doubleClick' | 'contextMenu'
    anchor: NativeTrayAnchorRect
}>

export type NativeTaskbarProgressSpec = Readonly<{
    state: 'none' | 'indeterminate' | 'normal' | 'paused' | 'error'
    value?: number
}>

export type NativeTaskbarBadgeSpec = Readonly<{
    icon?: string
    label?: string
}>

export type NativeTaskbarActionSpec = Readonly<{
    id: string
    icon: string
    tooltip?: string
    enabled?: boolean
    dismissOnClick?: boolean
}>

export type NativeMediaPlaybackStatus = 'closed' | 'stopped' | 'playing' | 'paused'
export type NativeMediaButtonKind = 'play' | 'pause' | 'stop' | 'next' | 'previous'

export type NativeMediaSessionSpec = Readonly<{
    active: boolean
    playbackStatus: NativeMediaPlaybackStatus
    title?: string
    artist?: string
    album?: string
    thumbnail?: string
    canPlay?: boolean
    canPause?: boolean
    canStop?: boolean
    canNext?: boolean
    canPrevious?: boolean
}>

export type NativeJumpListTaskSpec = Readonly<{
    title: string
    args?: readonly string[]
    icon?: string
    description?: string
}>

export type NativeJumpListCategorySpec = Readonly<{
    title: string
    items: readonly NativeJumpListTaskSpec[]
}>

export type NativeFsDirEntry = Readonly<{
    name: string
    isFile: boolean
    isDirectory: boolean
}>

export type NativeFsAsyncResponse = Readonly<{
    ok: boolean
    errorCode?: string
    errorPath?: string
    errorMessage?: string
    text?: string
    bytes?: Uint8Array
    exists?: boolean
    isFile?: boolean
    isDirectory?: boolean
    size?: number
    modifiedTimeMs?: number
    createdTimeMs?: number
    entries?: readonly NativeFsDirEntry[]
}>

export type NativeNetRequestPayload = Readonly<{
    method: string
    url: string
    headers: readonly (readonly [string, string])[]
    body?: string | Uint8Array | ArrayBuffer
    timeoutMs: number
    maxBytes: number
    followRedirects: boolean
    maxRedirects: number
    useSystemProxy: boolean
}>

export type NativeNetProgressPayload = Readonly<{
    receivedBytes: number
    totalBytes: number | null
}>

export type NativeNetAsyncResponse = Readonly<{
    ok: boolean
    errorCode?: string
    errorUrl?: string
    errorMessage?: string
    status?: number
    url?: string
    headers?: Readonly<Record<string, string>>
    contentType?: string
    body?: Uint8Array
    downloadOk?: boolean
    destPath?: string
    bytesWritten?: number
}>

// 真实宿主（QuickJsNativeApi）必然提供的成员均为必选
export type NativeTransactionTarget = {
    configureApp: (config: NativeAppConfig) => void
    requestAppExit: () => void
    allocateWindowId: () => number
    createWindow: (windowId: number, rootNodeId: NodeId, config: NativeWindowConfig) => void
    updateWindow: (windowId: number, config: NativeWindowConfig) => void
    destroyWindow: (windowId: number) => void
    windowCommand: (windowId: number, command: NativeWindowCommand) => void
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
    compileRuntimeEffect: (source: string, entryPoint: string, debugName: string) => NativeBindingHandle
    loadRuntimeEffectAsset: (assetPath: string, entryPoint: string, debugName: string) => NativeBindingHandle
    updateRuntimeEffect: (handle: NativeBindingHandle, source: string, entryPoint: string, debugName: string) => Readonly<{ ok: boolean; error?: string }>
    releaseRuntimeEffect: (handle: NativeBindingHandle) => void
    createRenderLayer: () => NativeBindingHandle
    releaseRenderLayer: (handle: NativeBindingHandle) => void
    registerBinding: (id: NodeId, input: string) => NativeBindingHandle
    updateBinding: (handle: NativeBindingHandle, value: NativeBindingValue) => void
    releaseBinding: (handle: NativeBindingHandle) => void
    modifierInstances: (id: NodeId) => readonly NativeModifierHandle[]
    registerModifierBinding: (id: NodeId, instance: NativeModifierHandle) => NativeBindingHandle
    runtimeVersion: number
    readAsset?: { (reference: string, text: true): Promise<string>, (reference: string, text: false): Promise<Uint8Array> }
    assetExists?: (reference: string) => Promise<boolean>
    getGraphicsEnvironment?: () => NativeGraphicsEnvironmentSnapshot
    onGraphicsEnvironmentChanged?: (callback: ((env: NativeGraphicsEnvironmentSnapshot) => void) | null) => void
    getLaunchRequest?: () => NativeLaunchRequest
    onSecondInstance?: (callback: ((request: NativeLaunchRequest) => void) | null) => void
    getKnownPath?: (kind: string) => string
    fsAsync?: (op: string, path: string, path2?: string, bytes?: string | Uint8Array | ArrayBuffer, flag?: boolean) => Promise<NativeFsAsyncResponse>
    netRequest?: (reqId: number, req: NativeNetRequestPayload) => Promise<NativeNetAsyncResponse>
    netDownload?: (reqId: number, req: NativeNetRequestPayload, destPath: string, onProgress?: (progress: NativeNetProgressPayload) => void) => Promise<NativeNetAsyncResponse>
    netCancel?: (reqId: number) => void
    trayUpsert?: (spec: NativeTrayIconSpec) => void
    trayRemove?: (trayId: string) => void
    trayShowSystemMenu?: (items: readonly NativeTrayMenuItemSpec[], screenX: number, screenY: number) => number
    trayQueryAnchor?: (trayId: string) => NativeTrayAnchorRect
    onTrayEvent?: (callback: ((event: NativeTrayEvent) => void) | null) => void
    setWindowTaskbarProgress?: (windowId: number, spec: NativeTaskbarProgressSpec) => void
    setWindowTaskbarBadge?: (windowId: number, spec: NativeTaskbarBadgeSpec | null) => void
    setWindowTaskbarActions?: (windowId: number, actions: readonly NativeTaskbarActionSpec[]) => void
    onWindowTaskbarAction?: (callback: ((windowId: number, actionId: string) => void) | null) => void
    mediaSessionUpdate?: (spec: NativeMediaSessionSpec) => void
    onMediaButton?: (callback: ((button: NativeMediaButtonKind) => void) | null) => void
    setJumpList?: (categories: readonly NativeJumpListCategorySpec[]) => void
    clearJumpList?: () => void
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
