import { onScopeDispose, ref } from '@canton/reactivity'
import { currentInstance, invokeContent, ValueBinding, type ArrangableInstance } from '../runtime/internal.ts'
import { defineArrangable, inject, onMounted, provide, slotParams, watch } from '../runtime/index.ts'
import type { ArrangableProps, InjectionKey, PropType } from '../runtime/index.ts'
import { createWindowState, type WindowSize } from '../modifier/window.ts'
import { M } from '../modifier/modifier.ts'
import type { NativeJumpListCategorySpec, NativeMediaButtonKind, NativeMediaPlaybackStatus, NativeTrayAnchorRect, NativeTrayEvent, NativeTrayMenuItemSpec } from '../native/protocol.ts'
import { NativeRearrangeHost, SurfaceDomainKey } from '../bridge/nativeRearrange.ts'
import { AppScopeKey, hasMountedApp, Window } from './AppAndWindow.ts'
import { Box } from './LayoutingArrangables.ts'

export interface TrayMenuHandle {
    dismiss(): void
}

export interface JumpListTask {
    readonly title: string
    readonly args?: readonly string[]
    readonly icon?: string
    readonly description?: string
}

export interface JumpListCategory {
    readonly title: string
    readonly items: readonly JumpListTask[]
}

export type MediaPlaybackStatus = NativeMediaPlaybackStatus

interface MenuEntryNode {
    readonly kind: 'item' | 'checkbox' | 'radio' | 'separator' | 'submenu'
    readonly commandId: number
    readonly getSpec: () => NativeTrayMenuItemSpec
    readonly onInvoke?: () => void
}

interface MenuContainerContext {
    register(node: MenuEntryNode): void
    unregister(node: MenuEntryNode): void
}

interface TrayMenuHostContext {
    readonly trayId: string
    readonly hasContextMenuCallback: boolean
    registerMenuMode(mode: 'system' | 'custom', owner: symbol, instance: ArrangableInstance, handler: { open: (anchor: NativeTrayAnchorRect) => void }): void
    unregisterMenuMode(owner: symbol): void
}

const TrayMenuHostKey: InjectionKey<TrayMenuHostContext> = Symbol('Canton.TrayMenuHost')
const MenuContainerKey: InjectionKey<MenuContainerContext> = Symbol('Canton.MenuContainer')

let nextTrayAutoId = 1
let nextMenuCommandId = 1

const activeTrayHandlers = new Map<string, (event: NativeTrayEvent) => void>()
let trayDriverInstalled = false

function ensureTrayDriver(host: NativeRearrangeHost): void {
    if (trayDriverInstalled || !host.native.onTrayEvent) return
    trayDriverInstalled = true
    host.native.onTrayEvent(event => {
        activeTrayHandlers.get(event.trayId)?.(event)
    })
}

const activeMediaSessions = new Map<symbol, { instance: ArrangableInstance; handler: (button: NativeMediaButtonKind) => void }>()
const mediaSessionVersion = ref(0)
let mediaDriverInstalled = false

function isInstanceActive(instance: ArrangableInstance): boolean {
    return !instance.isUnmounted && Boolean(instance.structure?.isActive())
}

function ensureMediaDriver(host: NativeRearrangeHost): void {
    if (mediaDriverInstalled || !host.native.onMediaButton) return
    mediaDriverInstalled = true
    host.native.onMediaButton(button => {
        for (const entry of activeMediaSessions.values()) {
            if (isInstanceActive(entry.instance)) {
                entry.handler(button)
                return
            }
        }
    })
}

function computeCustomPopupPosition(anchor: NativeTrayAnchorRect, widthDp: number, heightDp: number): { x: number; y: number } {
    const scale = anchor.dpiScale > 0 ? anchor.dpiScale : 1
    const w = Math.max(1, Math.round(widthDp * scale))
    const h = Math.max(1, Math.round(heightDp * scale))
    const centerX = anchor.right > anchor.left ? Math.round((anchor.left + anchor.right) / 2) : anchor.cursorX
    const topY = anchor.bottom > anchor.top ? anchor.top : anchor.cursorY
    const bottomY = anchor.bottom > anchor.top ? anchor.bottom : anchor.cursorY
    const margin = 6

    let x = centerX - Math.round(w / 2)
    let y = topY - h - margin
    if (y < anchor.workTop) y = bottomY + margin

    const maxX = Math.max(anchor.workLeft, anchor.workRight - w - margin)
    const maxY = Math.max(anchor.workTop, anchor.workBottom - h - margin)
    x = Math.min(Math.max(x, anchor.workLeft + margin), maxX)
    y = Math.min(Math.max(y, anchor.workTop + margin), maxY)
    return { x, y }
}

export type TrayIconProps = ArrangableProps<typeof TrayIcon>
export const TrayIcon = defineArrangable({
    name: 'TrayIcon',
    props: {
        id: String,
        guid: String,
        icon: String,
        tooltip: { type: String, default: '' },
        visible: { type: Boolean, default: true },
        onClick: Function as PropType<() => void>,
        onDoubleClick: Function as PropType<() => void>,
        onContextMenu: Function as PropType<() => void>,
    },
    slots: { default: slotParams<Record<string, never>>() },
    setup(props, { slot, source }) {
        const instance = currentInstance!
        if (!inject(AppScopeKey, undefined) || inject(SurfaceDomainKey, undefined)) {
            throw new Error('<TrayIcon> 必须直接挂在 <App> 下（不能放在 <Window> 画区内部）')
        }
        const host = instance.appContext.host
        if (!(host instanceof NativeRearrangeHost)) throw new Error('<TrayIcon> 需要正式原生应用宿主')

        ensureTrayDriver(host)
        const trayId = props.id || `tray-${nextTrayAutoId++}`
        // 借助 Canton 事务候选活跃态（instance.structure.isActive()）过滤即将退休的旧分支，
        // 既允许 c-if / c-else 同帧无缝切换双轨菜单，又能在真正并存时于本帧事务提交前立即报错。
        const menuRegistrations = new Map<symbol, { mode: 'system' | 'custom'; instance: ArrangableInstance; open: (anchor: NativeTrayAnchorRect) => void }>()
        const menuVersion = ref(0)

        const activeMenus = () => [...menuRegistrations.values()].filter(entry => isInstanceActive(entry.instance))

        const assertMenuValid = () => {
            const active = activeMenus()
            if (typeof props.onContextMenu === 'function' && active.length > 0) {
                throw new Error('<TrayIcon> 已设置 onContextMenu 回调，不能同时挂载 <SystemTrayMenu> 或 <CustomTrayMenu>')
            }
            if (active.length > 1) {
                throw new Error('同一个 <TrayIcon> 内只能挂载一个 <SystemTrayMenu> 或 <CustomTrayMenu>，二者不可混用')
            }
        }

        const context: TrayMenuHostContext = {
            trayId,
            get hasContextMenuCallback() {
                return typeof props.onContextMenu === 'function'
            },
            registerMenuMode(mode, owner, menuInstance, handler) {
                menuRegistrations.set(owner, { mode, instance: menuInstance, open: handler.open })
                menuVersion.value++
            },
            unregisterMenuMode(owner) {
                if (menuRegistrations.delete(owner)) menuVersion.value++
            },
        }
        provide(TrayMenuHostKey, context)

        activeTrayHandlers.set(trayId, event => {
            if (event.kind === 'click') {
                props.onClick?.()
            } else if (event.kind === 'doubleClick') {
                props.onDoubleClick?.()
            } else if (event.kind === 'contextMenu') {
                assertMenuValid()
                if (typeof props.onContextMenu === 'function') {
                    props.onContextMenu()
                } else {
                    activeMenus()[0]?.open(event.anchor)
                }
            }
        })

        new ValueBinding(
            () => {
                void menuVersion.value
                assertMenuValid()
                return Object.freeze({
                    id: trayId,
                    ...(props.guid ? { guid: props.guid } : {}),
                    ...(props.icon ? { icon: props.icon } : {}),
                    tooltip: props.tooltip,
                    visible: props.visible,
                })
            },
            instance,
            spec => host.native.trayUpsert?.(spec as Parameters<NonNullable<typeof host.native.trayUpsert>>[0]),
            source('tooltip') ?? source('visible'),
        )

        onScopeDispose(() => {
            activeTrayHandlers.delete(trayId)
            host.native.trayRemove?.(trayId)
        })

        const content = slot()
        return () => invokeContent(0, () => content({}))
    },
})

export type SystemTrayMenuProps = ArrangableProps<typeof SystemTrayMenu>
export const SystemTrayMenu = defineArrangable({
    name: 'SystemTrayMenu',
    slots: { default: slotParams<Record<string, never>>() },
    setup(_props, { slot }) {
        const instance = currentInstance!
        const trayHost = inject(TrayMenuHostKey, undefined)
        if (!trayHost) throw new Error('<SystemTrayMenu> 必须声明在 <TrayIcon> 内部')
        const host = instance.appContext.host
        if (!(host instanceof NativeRearrangeHost)) throw new Error('<SystemTrayMenu> 需要正式原生应用宿主')

        const owner = Symbol('Canton.SystemTrayMenu')
        const entries: MenuEntryNode[] = []
        const container: MenuContainerContext = {
            register(node) {
                if (!entries.includes(node)) entries.push(node)
            },
            unregister(node) {
                const idx = entries.indexOf(node)
                if (idx >= 0) entries.splice(idx, 1)
            },
        }
        provide(MenuContainerKey, container)

        const collectCommandMap = (nodes: readonly MenuEntryNode[], map: Map<number, () => void>): NativeTrayMenuItemSpec[] => {
            const out: NativeTrayMenuItemSpec[] = []
            for (const node of nodes) {
                const spec = node.getSpec()
                if (node.onInvoke && spec.commandId > 0) map.set(spec.commandId, node.onInvoke)
                out.push(spec)
            }
            return out
        }

        const subMenuCommandMaps = new Map<number, () => void>()
        trayHost.registerMenuMode('system', owner, instance, {
            open(anchor) {
                subMenuCommandMaps.clear()
                const items = collectCommandMap(entries, subMenuCommandMaps)
                // 递归收集 SubMenu 内部的 command 回调
                const walkInvoke = (specs: readonly NativeTrayMenuItemSpec[]) => {
                    for (const s of specs) {
                        const fn = commandCallbacks.get(s.commandId)
                        if (fn) subMenuCommandMaps.set(s.commandId, fn)
                        if (s.children) walkInvoke(s.children)
                    }
                }
                walkInvoke(items)
                const selected = host.native.trayShowSystemMenu?.(items, anchor.cursorX, anchor.cursorY) ?? 0
                if (selected > 0) subMenuCommandMaps.get(selected)?.()
            },
        })

        onScopeDispose(() => trayHost.unregisterMenuMode(owner))

        const content = slot()
        return () => invokeContent(0, () => content({}))
    },
})

const commandCallbacks = new Map<number, () => void>()

export type MenuItemProps = ArrangableProps<typeof MenuItem>
export const MenuItem = defineArrangable({
    name: 'MenuItem',
    props: {
        label: { type: String, required: true },
        enabled: { type: Boolean, default: true },
        onClick: Function as PropType<() => void>,
    },
    setup(props) {
        const container = inject(MenuContainerKey, undefined)
        if (!container) throw new Error('<MenuItem> 必须声明在 <SystemTrayMenu> 或 <SubMenu> 内部')
        const commandId = nextMenuCommandId++
        const invoke = () => props.onClick?.()
        commandCallbacks.set(commandId, invoke)
        const node: MenuEntryNode = {
            kind: 'item',
            commandId,
            getSpec: () => ({ kind: 'item', commandId, label: props.label, enabled: props.enabled }),
            onInvoke: invoke,
        }
        container.register(node)
        onScopeDispose(() => {
            commandCallbacks.delete(commandId)
            container.unregister(node)
        })
        return () => { }
    },
})

export type MenuCheckboxProps = ArrangableProps<typeof MenuCheckbox>
export const MenuCheckbox = defineArrangable({
    name: 'MenuCheckbox',
    props: {
        label: { type: String, required: true },
        checked: { type: Boolean, default: false },
        enabled: { type: Boolean, default: true },
        onChange: Function as PropType<(checked: boolean) => void>,
    },
    setup(props) {
        const container = inject(MenuContainerKey, undefined)
        if (!container) throw new Error('<MenuCheckbox> 必须声明在 <SystemTrayMenu> 或 <SubMenu> 内部')
        const commandId = nextMenuCommandId++
        const invoke = () => props.onChange?.(!props.checked)
        commandCallbacks.set(commandId, invoke)
        const node: MenuEntryNode = {
            kind: 'checkbox',
            commandId,
            getSpec: () => ({ kind: 'checkbox', commandId, label: props.label, enabled: props.enabled, checked: props.checked }),
            onInvoke: invoke,
        }
        container.register(node)
        onScopeDispose(() => {
            commandCallbacks.delete(commandId)
            container.unregister(node)
        })
        return () => { }
    },
})

export type MenuRadioProps = ArrangableProps<typeof MenuRadio>
export const MenuRadio = defineArrangable({
    name: 'MenuRadio',
    props: {
        label: { type: String, required: true },
        selected: { type: Boolean, default: false },
        enabled: { type: Boolean, default: true },
        onSelect: Function as PropType<() => void>,
    },
    setup(props) {
        const container = inject(MenuContainerKey, undefined)
        if (!container) throw new Error('<MenuRadio> 必须声明在 <SystemTrayMenu> 或 <SubMenu> 内部')
        const commandId = nextMenuCommandId++
        const invoke = () => props.onSelect?.()
        commandCallbacks.set(commandId, invoke)
        const node: MenuEntryNode = {
            kind: 'radio',
            commandId,
            getSpec: () => ({ kind: 'radio', commandId, label: props.label, enabled: props.enabled, checked: props.selected }),
            onInvoke: invoke,
        }
        container.register(node)
        onScopeDispose(() => {
            commandCallbacks.delete(commandId)
            container.unregister(node)
        })
        return () => { }
    },
})

export type MenuSeparatorProps = ArrangableProps<typeof MenuSeparator>
export const MenuSeparator = defineArrangable({
    name: 'MenuSeparator',
    setup() {
        const container = inject(MenuContainerKey, undefined)
        if (!container) throw new Error('<MenuSeparator> 必须声明在 <SystemTrayMenu> 或 <SubMenu> 内部')
        const node: MenuEntryNode = {
            kind: 'separator',
            commandId: 0,
            getSpec: () => ({ kind: 'separator', commandId: 0 }),
        }
        container.register(node)
        onScopeDispose(() => container.unregister(node))
        return () => { }
    },
})

export type SubMenuProps = ArrangableProps<typeof SubMenu>
export const SubMenu = defineArrangable({
    name: 'SubMenu',
    props: {
        label: { type: String, required: true },
        enabled: { type: Boolean, default: true },
    },
    slots: { default: slotParams<Record<string, never>>() },
    setup(props, { slot }) {
        const parent = inject(MenuContainerKey, undefined)
        if (!parent) throw new Error('<SubMenu> 必须声明在 <SystemTrayMenu> 或另一个 <SubMenu> 内部')
        const children: MenuEntryNode[] = []
        const subContainer: MenuContainerContext = {
            register(node) {
                if (!children.includes(node)) children.push(node)
            },
            unregister(node) {
                const idx = children.indexOf(node)
                if (idx >= 0) children.splice(idx, 1)
            },
        }
        provide(MenuContainerKey, subContainer)
        const node: MenuEntryNode = {
            kind: 'submenu',
            commandId: 0,
            getSpec: () => ({
                kind: 'submenu',
                commandId: 0,
                label: props.label,
                enabled: props.enabled,
                children: children.map(child => child.getSpec()),
            }),
        }
        parent.register(node)
        onScopeDispose(() => parent.unregister(node))
        const content = slot()
        return () => invokeContent(0, () => content({}))
    },
})

const TrayPopupActivator = defineArrangable({
    name: 'TrayPopupActivator',
    props: {
        onReady: { type: Function as PropType<() => void>, required: true },
    },
    setup(props) {
        onMounted(() => {
            props.onReady()
        })
        return () => { }
    },
})

export type CustomTrayMenuProps = ArrangableProps<typeof CustomTrayMenu>
export const CustomTrayMenu = defineArrangable({
    name: 'CustomTrayMenu',
    props: {
        size: { type: Object as PropType<WindowSize>, default: () => ({ width: 240, height: 220 }) },
        transparent: { type: Boolean, default: true },
    },
    slots: { default: slotParams<{ menuHandle: TrayMenuHandle }>() },
    setup(props, { call, slot }) {
        const instance = currentInstance!
        const trayHost = inject(TrayMenuHostKey, undefined)
        if (!trayHost) throw new Error('<CustomTrayMenu> 必须声明在 <TrayIcon> 内部')

        const owner = Symbol('Canton.CustomTrayMenu')
        const open = ref(false)
        let sawFocused = false
        const popupState = createWindowState({ size: props.size, position: { x: 0, y: 0 } })

        const dismiss = () => {
            open.value = false
            sawFocused = false
        }
        const menuHandle: TrayMenuHandle = Object.freeze({ dismiss })

        watch(() => popupState.focused, focused => {
            if (!open.value) return
            if (focused) {
                sawFocused = true
            } else if (sawFocused) {
                dismiss()
            }
        })

        trayHost.registerMenuMode('custom', owner, instance, {
            open(anchor) {
                popupState.size = props.size
                popupState.position = computeCustomPopupPosition(anchor, props.size.width, props.size.height)
                sawFocused = false
                open.value = true
                // 若弹窗已挂载则立刻激活；首次弹出则在 <Window> 提交挂载后的 TrayPopupActivator.onMounted 中激活
                popupState.activate()
            },
        })
        onScopeDispose(() => {
            open.value = false
            trayHost.unregisterMenuMode(owner)
        })

        const content = slot()
        return () => {
            if (!open.value) return
            call(0, Window, {
                state: () => popupState,
                onCloseRequest: () => dismiss,
                title: () => '',
                visible: () => true,
                owner: () => 'none' as const,
                chrome: () => 'frameless' as const,
                role: () => 'utility' as const,
                resizable: () => false,
                minimizable: () => false,
                maximizable: () => false,
                closable: () => true,
                alwaysOnTop: () => true,
                showInTaskbar: () => false,
                transparent: () => props.transparent,
                cornerPreference: () => (props.transparent ? 'square' as const : 'roundSmall' as const),
            }, {
                default: () => {
                    call(1, TrayPopupActivator, {
                        onReady: () => () => {
                            if (open.value) popupState.activate()
                        },
                    }, {})
                    call(2, Box, {
                        modifier: () => M.fillMaxSize().onKeyEvent(event => {
                            if (event.type === 'down' && event.key === 'Escape') {
                                dismiss()
                                return true
                            }
                            return false
                        }),
                    }, {
                        default: () => content({ menuHandle }),
                    })
                },
            })
        }
    },
})

export type MediaSessionProps = ArrangableProps<typeof MediaSession>
export const MediaSession = defineArrangable({
    name: 'MediaSession',
    props: {
        playbackStatus: { type: String as PropType<MediaPlaybackStatus>, default: 'closed' },
        title: { type: String, default: '' },
        artist: { type: String, default: '' },
        album: { type: String, default: '' },
        thumbnail: { type: String, default: '' },
        canPlay: { type: Boolean, default: false },
        canPause: { type: Boolean, default: false },
        canStop: { type: Boolean, default: false },
        canNext: { type: Boolean, default: false },
        canPrevious: { type: Boolean, default: false },
        onPlay: Function as PropType<() => void>,
        onPause: Function as PropType<() => void>,
        onStop: Function as PropType<() => void>,
        onNext: Function as PropType<() => void>,
        onPrevious: Function as PropType<() => void>,
    },
    setup(props, { source }) {
        const instance = currentInstance!
        if (!inject(AppScopeKey, undefined) || inject(SurfaceDomainKey, undefined)) {
            throw new Error('<MediaSession> 必须直接挂在 <App> 下（不能放在 <Window> 画区内部）')
        }
        const host = instance.appContext.host
        if (!(host instanceof NativeRearrangeHost)) throw new Error('<MediaSession> 需要正式原生应用宿主')

        const owner = Symbol('Canton.MediaSession')
        ensureMediaDriver(host)
        activeMediaSessions.set(owner, {
            instance,
            handler: button => {
                switch (button) {
                    case 'play': props.onPlay?.()
                        break
                    case 'pause': props.onPause?.()
                        break
                    case 'stop': props.onStop?.()
                        break
                    case 'next': props.onNext?.()
                        break
                    case 'previous': props.onPrevious?.()
                        break
                }
            },
        })
        mediaSessionVersion.value++

        new ValueBinding(
            () => {
                void mediaSessionVersion.value
                let activeCount = 0
                for (const entry of activeMediaSessions.values()) {
                    if (isInstanceActive(entry.instance)) activeCount++
                }
                if (activeCount > 1) {
                    throw new Error('同一时刻进程内最多只能挂载一个活跃的 <MediaSession>')
                }
                return Object.freeze({
                    active: true,
                    playbackStatus: props.playbackStatus,
                    title: props.title,
                    artist: props.artist,
                    album: props.album,
                    thumbnail: props.thumbnail,
                    canPlay: props.canPlay,
                    canPause: props.canPause,
                    canStop: props.canStop,
                    canNext: props.canNext,
                    canPrevious: props.canPrevious,
                })
            },
            instance,
            spec => host.native.mediaSessionUpdate?.(spec as Parameters<NonNullable<typeof host.native.mediaSessionUpdate>>[0]),
            source('playbackStatus') ?? source('title'),
        )

        onScopeDispose(() => {
            if (activeMediaSessions.delete(owner)) {
                mediaSessionVersion.value++
                let remainingActive = 0
                for (const entry of activeMediaSessions.values()) {
                    if (isInstanceActive(entry.instance)) remainingActive++
                }
                if (remainingActive === 0) {
                    host.native.mediaSessionUpdate?.({ active: false, playbackStatus: 'closed' })
                }
            }
        })

        return () => { }
    },
})

export function setJumpList(categories: readonly JumpListCategory[]): void {
    if (!hasMountedApp()) {
        throw new Error('setJumpList 必须在 <App> 挂载后调用')
    }
    const native = globalThis.__CANTON_NATIVE__
    if (!native?.setJumpList) throw new Error('当前运行时未挂载 Jump List 能力')
    const normalized: NativeJumpListCategorySpec[] = categories.map(cat => ({
        title: cat.title,
        items: cat.items.map(item => ({
            title: item.title,
            ...(item.args ? { args: [...item.args] } : {}),
            ...(item.icon ? { icon: item.icon } : {}),
            ...(item.description ? { description: item.description } : {}),
        })),
    }))
    native.setJumpList(normalized)
}

export function clearJumpList(): void {
    if (!hasMountedApp()) {
        throw new Error('clearJumpList 必须在 <App> 挂载后调用')
    }
    const native = globalThis.__CANTON_NATIVE__
    if (!native?.clearJumpList) throw new Error('当前运行时未挂载 Jump List 能力')
    native.clearJumpList()
}
