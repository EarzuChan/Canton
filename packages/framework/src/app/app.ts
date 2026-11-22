import type { AppConfig, AppContext, ArrangableDefinition, Data } from '../runtime/internal.ts'
import { RearrangeSession, isArrangableDefinition } from '../runtime/internal.ts'
import type { InjectionKey } from '../runtime/apiInject.ts'
import { NativeRearrangeHost } from '../bridge/nativeRearrange.ts'
import { App, Window } from '../arrangable/AppAndWindow.ts'
import { Layout } from '../arrangable/Layout.ts'
import { Box, Row, Column, Spacer, FlowRow, FlowColumn } from '../arrangable/LayoutingArrangables.ts'
import { LazyColumn, LazyRow, LazyVerticalGrid, LazyHorizontalGrid } from '../arrangable/LazyArrangables.ts'
import { Text, Input } from '../arrangable/TextAndInput.ts'
import { Image, Icon } from '../arrangable/ImageAndIcon.ts'
import { Canvas } from '../arrangable/Canvas.ts'
import { DynamicArrangable, KeepAlive } from '../arrangable/ToolArrangables.ts'
import { AnimatedVisibility, Crossfade } from '../arrangable/TransitionArrangables.ts'
import { CANTON_RUNTIME_VERSION, type NativeTransactionTarget } from '../native/protocol.ts'

declare global {
    var __CANTON_NATIVE__: NativeTransactionTarget | undefined
    var __CANTON_LAUNCH_APP__: ((application: unknown, target?: NativeTransactionTarget) => CantonRunningApplication) | undefined
}

const applicationSecrets = new WeakMap<CantonApplication, {
    readonly root: ArrangableDefinition
    readonly props: Readonly<Data>
    readonly config: AppConfig
    readonly definitions: Readonly<Record<string, ArrangableDefinition>>
    readonly provides: Readonly<Record<PropertyKey, unknown>>
}>()

export interface CantonApplication {
    readonly root: ArrangableDefinition
    readonly config: Readonly<AppConfig>
}

export interface AppBuilder {
    readonly config: AppConfig
    arrangable(name: string, definition: ArrangableDefinition): AppBuilder
    provide<T>(key: InjectionKey<T> | PropertyKey, value: T): AppBuilder
    build(): CantonApplication
}

export interface CantonRunningApplication {
    unmount(): void
}

const foundationArrangables = {
    App,
    Window,
    Layout,
    Box,
    Row,
    Column,
    Spacer,
    FlowRow,
    FlowColumn,
    LazyColumn,
    LazyRow,
    LazyVerticalGrid,
    LazyHorizontalGrid,
    Text,
    Input,
    Image,
    Icon,
    Canvas,
    DynamicArrangable,
    KeepAlive,
    AnimatedVisibility,
    Crossfade,
}

const mountedTargets = new WeakMap<NativeTransactionTarget, CantonRunningApplication>()

const requiredNativeMethods = [
    'configureApp',
    'requestAppExit',
    'allocateWindowId',
    'createWindow',
    'updateWindow',
    'destroyWindow',
    'windowCommand',
    'installWindowDriver',
    'focusCommand',
    'installFrameDriver',
    'installLayoutDriver',
    'continueRearrange',
    'currentTime',
    'requestFrame',
    'beginRearrange',
    'submitRearrange',
    'abortRearrange',
    'acquirePainter',
    'releasePainter',
    'createNode',
    'deleteNode',
    'insertChild',
    'removeChild',
    'registerBinding',
    'updateBinding',
    'releaseBinding',
    'modifierInstances',
    'registerModifierBinding',
    'unmount',
    'log',
    'diagnosticsToast',
    'diagnosticsRequestReload',
    'diagnosticsTriggerFakeError',
    'diagnosticsSetToastsEnabled',
] as const

export function appBuilder(root: ArrangableDefinition, props: Data = {}): AppBuilder {
    if (!isArrangableDefinition(root)) throw new TypeError('应用根必须是 Arrangable 定义')

    const config: AppConfig = {}
    const definitions: Record<string, ArrangableDefinition> = { ...foundationArrangables }
    const provides: Record<PropertyKey, unknown> = Object.create(null)

    const builder: AppBuilder = {
        config,

        arrangable(name, definition) {
            if (!name || typeof name !== 'string') throw new TypeError('Arrangable 登记名称必须是非空字符串')
            if (!isArrangableDefinition(definition)) throw new TypeError('注册目标必须是 Arrangable 定义')
            definitions[name] = definition
            return builder
        },

        provide(key, value) {
            provides[key as PropertyKey] = value
            return builder
        },

        build() {
            const frozenConfig: AppConfig = { ...config }
            const frozenDefinitions = Object.freeze({ ...definitions })
            const frozenProvides = Object.create(null) as Record<PropertyKey, unknown>
            for (const key of Reflect.ownKeys(provides)) frozenProvides[key] = provides[key]
            Object.freeze(frozenProvides)
            const frozenProps = Object.freeze({ ...props })
            const application: CantonApplication = Object.freeze({
                root,
                config: Object.freeze({ ...frozenConfig }),
            })
            applicationSecrets.set(application, {
                root,
                props: frozenProps,
                config: frozenConfig,
                definitions: frozenDefinitions,
                provides: frozenProvides,
            })
            return application
        },
    }

    return builder
}

export function isCantonApplication(value: unknown): value is CantonApplication {
    return typeof value === 'object' && value !== null && applicationSecrets.has(value as CantonApplication)
}

export function mountApplication(application: CantonApplication, target: NativeTransactionTarget | undefined = globalThis.__CANTON_NATIVE__): CantonRunningApplication {
    const secret = applicationSecrets.get(application)
    if (!secret) throw new TypeError('宿主启动需要 appBuilder(Root).build() 产出的应用定义')
    if (!target) throw new Error('Canton 缺少原生运行时')
    if (mountedTargets.has(target)) throw new Error('原生宿主已由另一个应用会话占用')
    if (target.runtimeVersion !== CANTON_RUNTIME_VERSION) throw new Error('Canton 脚本与原生协议版本不一致')

    for (const name of requiredNativeMethods) {
        if (typeof target[name] !== 'function') throw new TypeError(`原生运行时缺少正式入口：${name}`)
    }

    let rearrangeSession: RearrangeSession | undefined
    let activeTarget: NativeTransactionTarget | undefined = target

    const running: CantonRunningApplication = Object.freeze({
        unmount() {
            const current = activeTarget
            if (!current) return
            activeTarget = undefined
            try {
                rearrangeSession?.dispose()
            } finally {
                rearrangeSession = undefined
                try {
                    current.unmount()
                } finally {
                    mountedTargets.delete(current)
                }
            }
        },
    })

    mountedTargets.set(target, running)

    try {
        const ownerProvides = Object.create(secret.provides) as Record<PropertyKey, unknown>
        const host = new NativeRearrangeHost(target)
        const context: AppContext = {
            config: secret.config,
            definitions: { ...secret.definitions },
            provides: ownerProvides,
            host,
        }
        rearrangeSession = new RearrangeSession(
            context,
            secret.root,
            Object.fromEntries(Object.entries(secret.props).map(([name, value]) => [name, () => value])),
        )
        target.installFrameDriver(rearrangeSession.scheduler.prepare, rearrangeSession.scheduler.complete, () => running.unmount())
        rearrangeSession.mount()
        return running
    } catch (error) {
        activeTarget = undefined
        try {
            rearrangeSession?.dispose()
        } finally {
            rearrangeSession = undefined
            mountedTargets.delete(target)
        }
        throw error
    }
}

globalThis.__CANTON_LAUNCH_APP__ = (application: unknown, target?: NativeTransactionTarget) => {
    const resolvedTarget = target ?? globalThis.__CANTON_NATIVE__
    if (resolvedTarget && mountedTargets.has(resolvedTarget)) {
        mountedTargets.get(resolvedTarget)!.unmount()
    }
    return mountApplication(application as CantonApplication, resolvedTarget)
}
