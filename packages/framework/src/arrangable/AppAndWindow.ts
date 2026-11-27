import { onScopeDispose } from '@canton/reactivity'
import { currentInstance, invokeContent, ValueBinding } from '../runtime/internal.ts'
import { defineArrangable, inject, provide, slotParams } from '../runtime/index.ts'
import type { ArrangableProps, InjectionKey, PropType } from '../runtime/index.ts'
import { createDensity, DensityKey } from '../layout/density.ts'
import { createGraphicsEnvironment, GraphicsEnvironmentKey } from '../layout/graphicsEnvironment.ts'
import { freezeLaunchRequest, LaunchRequestKey, type LaunchRequest } from '../layout/launchRequest.ts'
import { createNativeFocusManager, FocusManagerKey, WindowFocusIdKey } from '../modifier/focus.ts'
import { createWindowHandle, createWindowState, isWindowState, normalizeWindowSize, windowStateInternal, type AppHandle, type WindowBackdrop, type WindowChrome, type WindowCornerPreference, type WindowHandle, type WindowModality, type WindowRole, type WindowSize, type WindowState } from '../modifier/window.ts'
import type { NativeAppConfig, NativeTaskbarActionSpec, NativeTaskbarBadgeSpec, NativeTaskbarProgressSpec, NativeWindowConfig } from '../native/protocol.ts'
import { NativeRearrangeHost, SurfaceDomainKey, WindowRearrangeSurface } from '../bridge/nativeRearrange.ts'

export const AppScopeKey: InjectionKey<true> = Symbol('Canton.AppScope')
const AncestorWindowIdKey: InjectionKey<number> = Symbol('Canton.AncestorWindowId')

let mountedAppCount = 0
export function hasMountedApp(): boolean {
    return mountedAppCount > 0
}

const windowTaskbarActionCallbacks = new Map<number, Map<string, () => void>>()
let taskbarActionDriverInstalled = false

function ensureTaskbarActionDriver(host: NativeRearrangeHost): void {
    if (taskbarActionDriverInstalled || !host.native.onWindowTaskbarAction) return
    taskbarActionDriverInstalled = true
    host.native.onWindowTaskbarAction((windowId, actionId) => {
        windowTaskbarActionCallbacks.get(windowId)?.get(actionId)?.()
    })
}

export interface TaskbarProgress {
    readonly state: 'none' | 'indeterminate' | 'normal' | 'paused' | 'error'
    readonly value?: number
}

export interface TaskbarBadge {
    readonly icon?: string
    readonly label?: string
}

export interface TaskbarAction {
    readonly id: string
    readonly icon: string
    readonly tooltip?: string
    readonly enabled?: boolean
    readonly dismissOnClick?: boolean
    readonly onClick?: () => void
}

const taskbarProgressStates = new Set(['none', 'indeterminate', 'normal', 'paused', 'error'])

const ownerModes = new Set(['auto', 'none'])
const modalities = new Set(['none', 'owner', 'application'])
const chromes = new Set(['system', 'customTitleBar', 'frameless'])
const roles = new Set(['standard', 'utility'])
const backdrops = new Set(['none', 'mica', 'micaAlt', 'acrylic'])
const cornerPreferences = new Set(['default', 'round', 'roundSmall', 'square'])

export type AppProps = ArrangableProps<typeof App>
export const App = defineArrangable({
    name: 'App',
    props: {
        exitOnLastWindowClosed: { type: Boolean, default: true },
        appId: String,
        icon: String,
        onSecondInstance: Function as PropType<(request: LaunchRequest) => void>,
    },
    slots: { default: slotParams<{ appHandle: AppHandle }>() },
    setup(props, { slot, source }) {
        const instance = currentInstance!
        if (inject(AppScopeKey, undefined) || inject(SurfaceDomainKey, undefined)) {
            throw new Error('<App> 是进程内唯一的顶层非布局应用根，不能嵌套在 <App> 或 <Window> 内部')
        }
        const host = instance.appContext.host
        if (!(host instanceof NativeRearrangeHost)) throw new Error('<App> 需要正式原生应用宿主')

        host.registerAppInstance(instance)
        mountedAppCount++

        const initialGraphics = host.native.getGraphicsEnvironment?.()
        const graphicsController = createGraphicsEnvironment(initialGraphics)
        host.native.onGraphicsEnvironmentChanged?.(snapshot => {
            graphicsController.update(snapshot)
        })

        const launchRequest = inject(LaunchRequestKey, undefined) ?? freezeLaunchRequest(host.native.getLaunchRequest?.())
        host.native.onSecondInstance?.(raw => {
            props.onSecondInstance?.(freezeLaunchRequest(raw))
        })

        onScopeDispose(() => {
            mountedAppCount = Math.max(0, mountedAppCount - 1)
            host.native.onGraphicsEnvironmentChanged?.(null)
            host.native.onSecondInstance?.(null)
            host.unregisterAppInstance(instance)
        })

        provide(AppScopeKey, true)
        provide(GraphicsEnvironmentKey, graphicsController.environment)
        provide(LaunchRequestKey, launchRequest)

        new ValueBinding(
            (): NativeAppConfig => Object.freeze({
                exitOnLastWindowClosed: props.exitOnLastWindowClosed,
                ...(props.appId ? { appId: props.appId } : {}),
                ...(props.icon ? { icon: props.icon } : {}),
            }),
            instance,
            config => host.updateAppConfig(config as NativeAppConfig),
            source('exitOnLastWindowClosed'),
        )

        const appHandle: AppHandle = Object.freeze({
            exitApplication: () => host.requestAppExit(),
        })
        const content = slot()
        return () => invokeContent(0, () => content({ appHandle }))
    },
})

export type WindowProps = ArrangableProps<typeof Window>
export const Window = defineArrangable({
    name: 'Window',
    props: {
        state: { type: Object as PropType<WindowState>, validator: isWindowState },
        onCloseRequest: { type: Function as PropType<() => void>, required: true },
        title: { type: String, default: '' },
        icon: String,
        visible: { type: Boolean, default: true },
        minSize: Object as PropType<WindowSize>,
        maxSize: Object as PropType<WindowSize>,
        owner: { type: String as PropType<'auto' | 'none'>, default: 'auto', validator: (v: unknown) => typeof v === 'string' && ownerModes.has(v) },
        modality: { type: String as PropType<WindowModality>, default: 'none', validator: (v: unknown) => typeof v === 'string' && modalities.has(v) },
        chrome: { type: String as PropType<WindowChrome>, default: 'system', validator: (v: unknown) => typeof v === 'string' && chromes.has(v) },
        role: { type: String as PropType<WindowRole>, default: 'standard', validator: (v: unknown) => typeof v === 'string' && roles.has(v) },
        resizable: { type: Boolean, default: true },
        minimizable: { type: Boolean, default: true },
        maximizable: { type: Boolean, default: true },
        closable: { type: Boolean, default: true },
        alwaysOnTop: { type: Boolean, default: false },
        showInTaskbar: Boolean,
        transparent: { type: Boolean, default: false },
        backdrop: { type: String as PropType<WindowBackdrop>, default: 'none', validator: (v: unknown) => typeof v === 'string' && backdrops.has(v) },
        cornerPreference: { type: String as PropType<WindowCornerPreference>, default: 'default', validator: (v: unknown) => typeof v === 'string' && cornerPreferences.has(v) },
        taskbarProgress: Object as PropType<TaskbarProgress>,
        taskbarBadge: { type: [Object, null] as PropType<TaskbarBadge | null>, default: undefined },
        taskbarActions: Array as PropType<readonly TaskbarAction[]>,
    },
    slots: { default: slotParams<{ windowHandle: WindowHandle }>() },
    setup(props, { slot, source }) {
        const instance = currentInstance!
        if (!inject(AppScopeKey, undefined)) throw new Error('<Window> 必须声明在 <App> 内部')

        const host = instance.appContext.host
        if (!(host instanceof NativeRearrangeHost)) throw new Error('<Window> 需要正式原生应用宿主')

        const ancestorWindowId = inject(AncestorWindowIdKey, undefined)
        const initialOwnerMode = props.owner
        const state = props.state ?? createWindowState()
        const stateInternal = windowStateInternal(state)
        const bindingOwner = Symbol('Canton.WindowBinding')

        const density = createDensity(state.dpiScale, state.dpiScale)

        const resolveConfig = (): NativeWindowConfig => {
            if (props.state !== undefined && props.state !== state) {
                throw new Error('<Window> 不支持在挂载期间动态更换 WindowState 实例，请使用 key 重建窗口')
            }
            if (props.owner !== initialOwnerMode && ancestorWindowId !== undefined) {
                throw new Error('<Window> 不支持在挂载期间动态切换 Win32 owner 关系')
            }
            const ownerWindowId = props.owner === 'none' ? undefined : ancestorWindowId
            if (props.modality === 'owner' && ownerWindowId === undefined) {
                throw new Error('modality="owner" 必须存在有效宿主窗口（不能无祖先 <Window> 或设置 owner="none"）')
            }
            if (props.transparent && (props.chrome !== 'frameless' || props.backdrop !== 'none' || props.cornerPreference === 'round' || props.cornerPreference === 'roundSmall')) {
                throw new Error('透明 <Window> 要求 chrome="frameless"、无背景材质且不使用系统圆角')
            }
            const minSizeDp = props.minSize ? normalizeWindowSize(props.minSize, 'minSize') : undefined
            const maxSizeDp = props.maxSize ? normalizeWindowSize(props.maxSize, 'maxSize') : undefined
            if (minSizeDp && maxSizeDp && (minSizeDp.width > maxSizeDp.width || minSizeDp.height > maxSizeDp.height)) {
                throw new RangeError('Window 的 minSize 不能大于 maxSize')
            }
            return Object.freeze({
                ...(ownerWindowId !== undefined ? { ownerWindowId } : {}),
                title: props.title,
                ...(props.icon !== undefined ? { icon: props.icon } : {}),
                visible: props.visible,
                sizeDp: state.size,
                ...(minSizeDp ? { minSizeDp } : {}),
                ...(maxSizeDp ? { maxSizeDp } : {}),
                position: state.position,
                placement: state.placement,
                modality: props.modality,
                chrome: props.chrome,
                role: props.role,
                resizable: props.resizable,
                minimizable: props.minimizable,
                maximizable: props.maximizable,
                closable: props.closable,
                alwaysOnTop: props.alwaysOnTop,
                ...(props.showInTaskbar !== undefined ? { showInTaskbar: props.showInTaskbar } : {}),
                transparent: props.transparent,
                backdrop: props.backdrop,
                cornerPreference: props.cornerPreference,
            })
        }

        let surface!: WindowRearrangeSurface
        const windowHandle = createWindowHandle(state, () => surface?.activeWindowId)

        const initialConfig = resolveConfig()
        surface = new WindowRearrangeSurface(instance, host, initialConfig, {
            onCloseRequest: () => props.onCloseRequest(),
            onGeometryChanged: event => {
                const sizeDp = event.placement !== 'minimized' && event.widthPx > 0 && event.heightPx > 0
                    ? Object.freeze({ width: density.pxToDp(event.widthPx), height: density.pxToDp(event.heightPx) })
                    : undefined
                const position = event.hasPosition && event.x !== undefined && event.y !== undefined
                    ? Object.freeze({ x: Math.round(event.x), y: Math.round(event.y) })
                    : undefined
                surface.noteNativeGeometry(sizeDp, position, event.placement)
                stateInternal.applyNativeGeometry(sizeDp, position, event.placement)
            },
            onFocusChanged: focused => stateInternal.applyNativeFocus(focused),
            onDpiChanged: dpiScale => {
                stateInternal.applyNativeDpi(dpiScale)
                density.dpScale = dpiScale
                density.spScale = dpiScale
            },
            onCommitMount: () => {
                stateInternal.bind(bindingOwner, surface.windowId, windowHandle, action => {
                    const activeId = surface.activeWindowId
                    if (activeId !== undefined) host.native.windowCommand(activeId, action)
                })
            },
            onRollbackMount: () => stateInternal.unbind(bindingOwner, windowHandle),
            onRetire: () => stateInternal.unbind(bindingOwner, windowHandle),
        }, state)
        instance.surface = surface

        const focus = createNativeFocusManager(host.native, surface.windowId)
        ensureTaskbarActionDriver(host)

        const syncTaskbarEnhancements = () => {
            const winId = surface.activeWindowId
            if (winId === undefined) return
            if (props.taskbarProgress !== undefined) {
                const stateName = props.taskbarProgress.state
                if (!taskbarProgressStates.has(stateName)) {
                    throw new TypeError(`taskbarProgress.state 无效：${String(stateName)}`)
                }
                const spec: NativeTaskbarProgressSpec = {
                    state: stateName,
                    ...(props.taskbarProgress.value !== undefined ? { value: props.taskbarProgress.value } : {}),
                }
                host.native.setWindowTaskbarProgress?.(winId, spec)
            } else {
                host.native.setWindowTaskbarProgress?.(winId, { state: 'none', value: 0 })
            }

            if (props.taskbarBadge !== undefined) {
                const badgeSpec: NativeTaskbarBadgeSpec | null = props.taskbarBadge
                    ? { ...(props.taskbarBadge.icon ? { icon: props.taskbarBadge.icon } : {}), ...(props.taskbarBadge.label ? { label: props.taskbarBadge.label } : {}) }
                    : null
                host.native.setWindowTaskbarBadge?.(winId, badgeSpec)
            }

            if (props.taskbarActions !== undefined) {
                if (props.taskbarActions.length > 7) {
                    throw new RangeError('Window.taskbarActions 最多支持 7 个缩略图工具栏按钮')
                }
                const callbackMap = new Map<string, () => void>()
                const nativeActions: NativeTaskbarActionSpec[] = props.taskbarActions.map(action => {
                    if (action.onClick) callbackMap.set(action.id, action.onClick)
                    return {
                        id: action.id,
                        icon: action.icon,
                        ...(action.tooltip !== undefined ? { tooltip: action.tooltip } : {}),
                        enabled: action.enabled ?? true,
                        dismissOnClick: action.dismissOnClick ?? false,
                    }
                })
                windowTaskbarActionCallbacks.set(winId, callbackMap)
                host.native.setWindowTaskbarActions?.(winId, nativeActions)
            }
        }

        onScopeDispose(() => {
            windowTaskbarActionCallbacks.delete(surface.windowId)
            focus.dispose()
            stateInternal.unbind(bindingOwner, windowHandle)
        })

        provide(DensityKey, density)
        provide(FocusManagerKey, focus.manager)
        provide(WindowFocusIdKey, surface.windowId)
        provide(AncestorWindowIdKey, surface.windowId)
        provide(SurfaceDomainKey, Object.freeze({ windowId: surface.windowId, host }))

        new ValueBinding(resolveConfig, instance, value => surface.updateConfig(value as NativeWindowConfig), source('modality') ?? source('title'))
        new ValueBinding(
            () => ({
                progress: props.taskbarProgress,
                badge: props.taskbarBadge,
                actions: props.taskbarActions,
            }),
            instance,
            () => syncTaskbarEnhancements(),
            source('taskbarProgress') ?? source('taskbarActions'),
        )

        const content = slot()
        return () => invokeContent(0, () => content({ windowHandle }))
    },
})
