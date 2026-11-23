import type { PxModifier } from './modifierLowering.ts'
import type { ArrangableInstance, RearrangeHost, RearrangeNode, RearrangeSurface } from '../runtime/internal.ts'
import { cantonExecutionStats } from '../runtime/internal.ts'
import type { InjectionKey } from '../runtime/apiInject.ts'
import type { NativeAppConfig, NativeBindingHandle, NativeModifierHandle, NativeMutation, NativeTransactionTarget, NativeWindowConfig, NativeWindowGeometryEvent } from '../native/protocol.ts'
import { modifierStats, type ModifierElement } from '../modifier/modifier.ts'
import { sameWindowPosition, sameWindowSize } from '../modifier/window.ts'
import type { MeasurePolicy } from '../layout/measurePolicy.ts'
import type { LayoutContentProvider } from '../layout/layoutContent.ts'

export interface SurfaceDomain {
    readonly windowId: number
    readonly host: NativeRearrangeHost
}

export const SurfaceDomainKey: InjectionKey<SurfaceDomain> = Symbol('Canton.SurfaceDomain')

export interface WindowSurfaceCallbacks {
    readonly onCloseRequest: () => void
    readonly onGeometryChanged: (event: NativeWindowGeometryEvent) => void
    readonly onFocusChanged: (focused: boolean) => void
    readonly onDpiChanged: (dpiScale: number) => void
    readonly onCommitMount?: () => void
    readonly onRollbackMount?: () => void
    readonly onRetire?: () => void
}

export class NativeRearrangeHost implements RearrangeHost {
    private nextId = 2
    private operations: NativeMutation[] = []
    private commits: (() => void)[] = []
    private aborts: (() => void)[] = []
    private preparing = false
    private applying = false
    private nativeOpen = false
    private transaction: object | undefined
    private sentOperations = 0
    private readonly providers = new Map<number, { node: LayoutRearrangeNode; provider: LayoutContentProvider }>()
    private readonly surfaces = new Set<WindowRearrangeSurface>()
    private readonly activeWindows = new Map<number, WindowSurfaceCallbacks>()
    private readonly appInstances = new Set<ArrangableInstance>()
    private committedAppConfig: NativeAppConfig | undefined
    private pendingAppConfig: NativeAppConfig | undefined
    private appConfigStagedEpoch = 0
    preparationEpoch = 0
    continuing = false

    constructor(readonly native: NativeTransactionTarget) {
        native.installLayoutDriver((id, indices) => {
            const entry = this.providers.get(id)
            if (!entry || entry.node.owner.isUnmounted) throw new Error('子组合目标已退休')
            const session = entry.node.owner.rearrangeSession
            session.continuePreparation(() => {
                entry.provider.prepare(indices)
                entry.node.owner.structure.enqueue(true)
            })
        })
        native.installWindowDriver({
            onCloseRequest: windowId => this.activeWindows.get(windowId)?.onCloseRequest(),
            onGeometryChanged: (windowId, event) => this.activeWindows.get(windowId)?.onGeometryChanged(event),
            onFocusChanged: (windowId, focused) => this.activeWindows.get(windowId)?.onFocusChanged(focused),
            onDpiChanged: (windowId, dpiScale) => this.activeWindows.get(windowId)?.onDpiChanged(dpiScale),
        })
    }

    registerAppInstance(instance: ArrangableInstance): void {
        this.appInstances.add(instance)
        this.onAbort(() => this.appInstances.delete(instance))
    }

    unregisterAppInstance(instance: ArrangableInstance): void {
        this.appInstances.delete(instance)
    }

    updateAppConfig(config: NativeAppConfig): void {
        const current = this.pendingAppConfig ?? this.committedAppConfig
        if (current && sameAppConfig(current, config)) return
        const firstInCandidate = this.pendingAppConfig === undefined
        this.pendingAppConfig = config
        if (this.appConfigStagedEpoch === this.preparationEpoch) return
        this.appConfigStagedEpoch = this.preparationEpoch
        if (firstInCandidate) {
            this.onCommit(() => {
                this.committedAppConfig = this.pendingAppConfig
                this.pendingAppConfig = undefined
                this.appConfigStagedEpoch = 0
            })
            this.onAbort(() => {
                this.pendingAppConfig = undefined
                this.appConfigStagedEpoch = 0
            })
        }
        this.stage(native => {
            if (this.pendingAppConfig) native.configureApp(this.pendingAppConfig)
        })
    }

    requestAppExit(): void {
        this.native.requestAppExit()
    }

    registerSurface(surface: WindowRearrangeSurface): void {
        this.surfaces.add(surface)
        this.onAbort(() => {
            if (!surface.committed) this.surfaces.delete(surface)
        })
    }

    unregisterSurface(surface: WindowRearrangeSurface): void {
        this.surfaces.delete(surface)
    }

    bindWindowCallbacks(windowId: number, callbacks: WindowSurfaceCallbacks): void {
        this.activeWindows.set(windowId, callbacks)
    }

    unbindWindowCallbacks(windowId: number): void {
        this.activeWindows.delete(windowId)
    }

    syncCandidateSurfaces(): void {
        const activeApps = [...this.appInstances].filter(instance => !instance.isUnmounted && instance.structure?.isActive())
        if (activeApps.length > 1) throw new Error('<App> 是进程内唯一的非布局应用根，不能同时挂载多个实例')
        const surfaceStates: Array<{ surface: WindowRearrangeSurface; active: boolean }> = []
        const activeWindowStates = new Set<object>()
        for (const surface of [...this.surfaces]) {
            const active = !surface.owner.isUnmounted && Boolean(surface.owner.structure?.isActive())
            if (active && surface.state) {
                if (activeWindowStates.has(surface.state)) throw new Error('同一 WindowState 不能同时绑定多个活跃 <Window>')
                activeWindowStates.add(surface.state)
            }
            surfaceStates.push({ surface, active })
        }
        for (const { surface, active } of surfaceStates) surface.syncCandidateActive(active)
    }

    registerProvider(node: LayoutRearrangeNode, provider: LayoutContentProvider): void {
        const id = node.id
        this.providers.set(id, { node, provider })
        this.onAbort(() => this.providers.delete(id))
    }

    unregisterProvider(id: number): void { this.providers.delete(id) }

    currentTime(): number { return this.native.currentTime() }
    requestFrame(pending: boolean): void { this.native.requestFrame(pending) }

    allocateId(): number { return this.nextId++ }

    begin(): void {
        if (this.preparing || this.applying) throw new Error('原生候选提交不能重入')
        this.preparing = true
        this.preparationEpoch++
        this.continuing = false
    }

    onCommit(action: () => void): void { this.commits.push(action) }
    onAbort(action: () => void): void { this.aborts.push(action) }

    stage(operation: NativeMutation, commit?: () => void, abort?: () => void): void {
        this.operations.push(operation)
        if (commit) this.commits.push(commit)
        if (abort) this.aborts.push(abort)
    }

    reconcileRoots(nodes: readonly RearrangeNode[]): void {
        if (nodes.length > 0) throw new Error('Layout 只能挂载在 <Window> 建立的画区作用域内')
    }

    reconcile(parent: number, previous: readonly LayoutRearrangeNode[], next: readonly LayoutRearrangeNode[]): void {
        const retained = new Set(next)
        for (const node of previous) {
            if (retained.has(node)) continue
            const id = node.id
            node.detachCandidate()
            this.stage(native => {
                native.removeChild(parent, id)
                native.deleteNode(id)
                cantonExecutionStats.nativeRemoveOperations++
            })
        }
        const order = previous.filter(node => retained.has(node))
        for (let index = 0; index < next.length; index++) {
            const node = next[index]
            node.ensureAttached()
            if (order[index] === node) continue
            const old = order.indexOf(node)
            if (old >= 0) order.splice(old, 1)
            order.splice(index, 0, node)
            const id = node.id
            this.stage(native => {
                if (node.id !== id) return
                native.insertChild(parent, id, index)
                cantonExecutionStats.nativeInsertOperations++
            })
        }
    }

    apply(complete: (error?: Error) => void): void {
        if (!this.preparing) throw new Error('没有可提交的重排候选')
        try {
            this.syncCandidateSurfaces()
        } catch (error) {
            this.preparing = false
            this.rollback()
            throw error
        }
        this.preparing = false
        this.applying = true
        const transaction = this.transaction = {}
        try {
            this.native.beginRearrange()
            this.nativeOpen = true
            this.flushOperations()
            this.native.submitRearrange(message => {
                if (this.transaction !== transaction) return
                this.nativeOpen = false
                if (message) {
                    complete(new Error(message))
                    return
                }
                this.transaction = undefined
                const commits = this.commits.splice(0)
                this.operations.length = 0
                this.sentOperations = 0
                this.continuing = false
                this.aborts.length = 0
                this.applying = false
                const errors: unknown[] = []
                for (const commit of commits) {
                    try { commit() } catch (error) { errors.push(error) }
                }
                try { complete() } catch (error) { errors.push(error) }
                if (errors.length) throw new AggregateError(errors, '原生已提交，但提交后清理或通知发生错误')
            })
        } catch (error) {
            if (this.transaction === transaction) this.rollback()
            throw error
        }
    }

    beginContinuation(): void {
        if (!this.applying || !this.nativeOpen || this.preparing) throw new Error('子组合没有待提交的原生候选')
        this.preparing = true
        this.continuing = true
        this.preparationEpoch++
    }

    applyContinuation(): void {
        if (!this.preparing || !this.continuing) throw new Error('子组合候选尚未准备')
        this.syncCandidateSurfaces()
        this.native.continueRearrange()
        this.flushOperations()
        this.preparing = false
    }

    private flushOperations(): void {
        while (this.sentOperations < this.operations.length) this.operations[this.sentOperations++](this.native)
    }

    rollback(): void {
        this.transaction = undefined
        const errors: unknown[] = []
        const safely = (operation: () => void) => { try { operation() } catch (error) { errors.push(error) } }
        try {
            if (this.nativeOpen) safely(() => this.native.abortRearrange())
            for (let index = this.aborts.length - 1; index >= 0; index--) safely(this.aborts[index])
        } finally {
            this.nativeOpen = false
            this.operations.length = 0
            this.sentOperations = 0
            this.continuing = false
            this.commits.length = 0
            this.aborts.length = 0
            this.preparing = false
            this.applying = false
        }
        if (errors.length) throw new AggregateError(errors, '原生候选撤销时发生错误')
    }
}

export class WindowRearrangeSurface implements RearrangeSurface {
    windowId: number
    rootId: number
    committed = false
    private retired = false
    private nativeAttached = false
    private candidateAttached = false
    private roots: readonly LayoutRearrangeNode[] = []
    private pendingRoots: readonly LayoutRearrangeNode[] | undefined
    private committedConfig: NativeWindowConfig
    private pendingConfig: NativeWindowConfig | undefined
    private configStagedEpoch = 0

    constructor(readonly owner: ArrangableInstance, private readonly host: NativeRearrangeHost, initialConfig: NativeWindowConfig, private readonly callbacks: WindowSurfaceCallbacks, readonly state?: object) {
        this.windowId = host.native.allocateWindowId()
        this.rootId = host.allocateId()
        this.committedConfig = initialConfig
        this.pendingConfig = initialConfig
        host.registerSurface(this)
        this.stageOpen()
    }

    get activeWindowId(): number | undefined {
        return this.retired || (!this.nativeAttached && !this.candidateAttached) ? undefined : this.windowId
    }

    private stageOpen(): void {
        this.candidateAttached = true
        const windowId = this.windowId
        const rootId = this.rootId
        this.host.bindWindowCallbacks(windowId, this.callbacks)
        this.host.stage(
            native => {
                if (this.retired || !this.candidateAttached || this.windowId !== windowId) return
                const config = this.pendingConfig ?? this.committedConfig
                native.createWindow(windowId, rootId, config)
                native.createNode(rootId, 'Root', windowId)
                cantonExecutionStats.nativeCreateOperations++
            },
            () => {
                this.committed = true
                this.nativeAttached = true
                if (this.pendingConfig) this.committedConfig = this.pendingConfig
                this.pendingConfig = undefined
                this.configStagedEpoch = 0
                this.callbacks.onCommitMount?.()
            },
            () => {
                this.candidateAttached = this.nativeAttached
                this.pendingConfig = undefined
                this.configStagedEpoch = 0
                if (!this.nativeAttached) {
                    this.host.unbindWindowCallbacks(windowId)
                    this.callbacks.onRollbackMount?.()
                }
            },
        )
    }

    syncCandidateActive(active: boolean): void {
        if (active) {
            if (!this.candidateAttached && !this.retired) {
                this.windowId = this.host.native.allocateWindowId()
                this.rootId = this.host.allocateId()
                this.stageOpen()
                for (const [index, child] of (this.pendingRoots ?? this.roots).entries()) {
                    child.rebindWindow(this.windowId)
                    const childId = child.id
                    const rootId = this.rootId
                    this.host.stage(native => {
                        native.insertChild(rootId, childId, index)
                        cantonExecutionStats.nativeInsertOperations++
                    })
                }
            }
            return
        }
        if (!this.candidateAttached) return
        this.candidateAttached = false
        const previousRoots = this.pendingRoots ?? this.roots
        for (const node of previousRoots) node.detachCandidate()
        const windowId = this.windowId
        const rootId = this.rootId
        const wasNativeAttached = this.nativeAttached
        this.host.stage(
            native => {
                if (!wasNativeAttached) return
                for (const node of previousRoots) {
                    native.removeChild(rootId, node.id)
                    native.deleteNode(node.id)
                    cantonExecutionStats.nativeRemoveOperations++
                }
                native.deleteNode(rootId)
                native.destroyWindow(windowId)
            },
            () => {
                this.nativeAttached = false
                this.roots = []
                this.pendingRoots = undefined
                this.host.unbindWindowCallbacks(windowId)
                if (this.retired || !this.owner.structure || this.owner.isUnmounted) {
                    this.host.unregisterSurface(this)
                    this.callbacks.onRetire?.()
                }
            },
            () => {
                this.candidateAttached = wasNativeAttached
            },
        )
    }

    reconcileRoots(nodes: readonly RearrangeNode[]): void {
        const next = nodes as readonly LayoutRearrangeNode[]
        if (!this.candidateAttached) return
        this.host.reconcile(this.rootId, this.pendingRoots ?? this.roots, next)
        if (!this.pendingRoots) {
            this.host.onCommit(() => {
                this.roots = this.pendingRoots ?? this.roots
                this.pendingRoots = undefined
            })
            this.host.onAbort(() => { this.pendingRoots = undefined })
        }
        this.pendingRoots = next
    }

    updateConfig(config: NativeWindowConfig): void {
        if (this.retired) return
        const current = this.pendingConfig ?? this.committedConfig
        if (sameWindowConfig(current, config)) return
        const firstInEpoch = this.pendingConfig === undefined
        this.pendingConfig = config
        if (!this.nativeAttached) return
        if (this.configStagedEpoch === this.host.preparationEpoch) return
        this.configStagedEpoch = this.host.preparationEpoch
        if (firstInEpoch) {
            this.host.onCommit(() => {
                if (this.pendingConfig) this.committedConfig = this.pendingConfig
                this.pendingConfig = undefined
                this.configStagedEpoch = 0
            })
            this.host.onAbort(() => {
                this.pendingConfig = undefined
                this.configStagedEpoch = 0
            })
        }
        const windowId = this.windowId
        this.host.stage(native => {
            if (this.retired || !this.candidateAttached || this.windowId !== windowId || !this.pendingConfig) return
            native.updateWindow(windowId, this.pendingConfig)
        })
    }

    noteNativeGeometry(sizeDp: Readonly<{ width: number; height: number }> | undefined, position: NativeWindowConfig['position'] | undefined, placement: NativeWindowConfig['placement']): void {
        this.committedConfig = {
            ...this.committedConfig,
            ...(sizeDp ? { sizeDp } : {}),
            ...(position ? { position } : {}),
            placement,
        }
    }

    deactivate(): void {
        // KeepAlive 停用已在 syncCandidateActive(false) 中卸载原生窗口并保留逻辑实例
    }

    activate(): void {
        // KeepAlive 恢复将在下一次 prepareCandidate 的 syncCandidateActive(true) 中重新建窗
    }

    retire(): void {
        if (this.retired) return
        this.retired = true
        this.host.unregisterSurface(this)
        this.host.unbindWindowCallbacks(this.windowId)
        this.callbacks.onRetire?.()
        this.roots = []
        this.pendingRoots = undefined
    }
}

// 此类的唯一构造调用位于 Layout setup，普通定义不接入节点应用层
export class LayoutRearrangeNode implements RearrangeNode {
    id: number
    children: readonly LayoutRearrangeNode[] = []
    private retired = false
    private attached = true
    private nativeDetached = false
    private readonly bindings = new Map<string, NativeBindingHandle>()
    private readonly inputs = new Map<string, unknown>()
    private readonly pendingInputs = new Map<string, unknown>()
    private readonly sentInputs = new Map<string, unknown>()
    private readonly stagedEpochs = new Map<string, number>()
    private pendingChildren: readonly LayoutRearrangeNode[] | undefined
    private provider: LayoutContentProvider | undefined
    private readonly modifierBindings = new Map<bigint, NativeBindingHandle>()

    constructor(readonly owner: ArrangableInstance, private readonly host: NativeRearrangeHost, private windowId: number) {
        this.id = host.allocateId()
        cantonExecutionStats.rearrangeNodesCreated++
        this.stageCreation()
    }

    private stageCreation(): void {
        const id = this.id
        const windowId = this.windowId
        this.host.stage(native => {
            if (this.retired || this.id !== id) return
            native.createNode(id, 'LayoutNode', windowId)
            cantonExecutionStats.nativeCreateOperations++
        })
    }

    rebindWindow(windowId: number): void {
        this.windowId = windowId
        this.nativeDetached = true
        this.ensureAttached()
    }

    reconcileChildren(children: readonly RearrangeNode[]): void {
        const next = children as readonly LayoutRearrangeNode[]
        this.host.reconcile(this.id, this.pendingChildren ?? this.children, next)
        if (!this.pendingChildren) {
            this.host.onCommit(() => {
                this.children = this.pendingChildren!
                this.pendingChildren = undefined
            })
            this.host.onAbort(() => { this.pendingChildren = undefined })
        }
        this.pendingChildren = next
    }

    setContentProvider(provider: LayoutContentProvider): void {
        this.provider = provider
        this.host.registerProvider(this, provider)
    }

    updateMeasurePolicy(policy: MeasurePolicy): void {
        this.update('measurePolicy', policy)
    }

    updateModifier(modifier: PxModifier): void {
        this.update('modifier', modifier)
    }

    updateInput(name: 'enabled', value: boolean | undefined): void {
        this.update(name, value ?? null)
    }

    private update(name: string, value: unknown, force = false): void {
        if (this.retired) throw new Error('不能更新已退休的重排节点')
        const current = this.pendingInputs.has(name) ? this.pendingInputs : this.inputs
        if (!force && current.has(name) && Object.is(current.get(name), value)) return
        const staged = this.pendingInputs.has(name)
        this.pendingInputs.set(name, value)
        if (this.stagedEpochs.get(name) === this.host.preparationEpoch) return
        this.stagedEpochs.set(name, this.host.preparationEpoch)
        if (!staged) {
            this.host.onCommit(() => {
                this.inputs.set(name, this.pendingInputs.get(name))
                this.pendingInputs.delete(name)
                this.stagedEpochs.delete(name)
                this.sentInputs.delete(name)
            })
            this.host.onAbort(() => {
                this.pendingInputs.delete(name)
                this.stagedEpochs.delete(name)
                this.sentInputs.delete(name)
            })
        }

        const id = this.id
        this.host.stage(native => {
            if (this.retired || this.id !== id) return
            const value = this.pendingInputs.get(name)
            if (name === 'modifier' && this.bindings.has(name) && !this.host.continuing && this.writeModifier(native, value as PxModifier)) return
            let binding = this.bindings.get(name)
            const freshBinding = !binding
            if (!binding) {
                binding = native.registerBinding(this.id, name)
                this.bindings.set(name, binding)
                this.host.onAbort(() => { this.bindings.delete(name) })
            }
            let input = value
            if (name === 'measurePolicy') {
                const current = value as MeasurePolicy
                const previous = freshBinding ? undefined : (this.sentInputs.get(name) ?? this.inputs.get(name)) as MeasurePolicy | undefined
                if (current.kind === 'Lazy' && previous?.kind === 'Lazy' && current.version === previous.version) {
                    const { keys, contentTypes, spans, ...reference } = current
                    input = reference
                }
                this.sentInputs.set(name, value)
            }
            try { native.updateBinding(binding, input as Parameters<NativeTransactionTarget['updateBinding']>[1]) } catch (error) {
                const source = this.owner.setupContext.source(name)
                if (error instanceof Error && source && !error.message.includes('来源：')) error.message += `\n来源：${source}`
                throw error
            }
            if (name === 'modifier') {
                modifierStats.chainWrites++
                const previousBindings = [...this.modifierBindings.values()]
                for (const handle of previousBindings) native.releaseBinding(handle)
                this.host.onCommit(() => this.modifierBindings.clear())
            }
        })
    }

    detachCandidate(): void {
        if (this.nativeDetached) return
        this.nativeDetached = true
        this.host.onAbort(() => { this.nativeDetached = false })
        const id = this.id
        this.host.onCommit(() => { if (this.nativeDetached || this.id !== id) this.host.unregisterProvider(id) })
        for (const child of this.pendingChildren ?? this.children) child.detachCandidate()
    }

    ensureAttached(): void {
        if (!this.nativeDetached || this.retired) return
        const previousId = this.id
        const bindings = new Map(this.bindings)
        const modifierBindings = new Map(this.modifierBindings)
        const sentInputs = new Map(this.sentInputs)
        const stagedEpochs = new Map(this.stagedEpochs)
        const values = new Map([...this.inputs, ...this.pendingInputs])
        this.nativeDetached = false
        this.id = this.host.allocateId()
        this.bindings.clear()
        this.modifierBindings.clear()
        this.sentInputs.clear()
        this.stagedEpochs.clear()
        this.stageCreation()
        if (this.provider) this.host.registerProvider(this, this.provider)
        this.host.onAbort(() => {
            this.id = previousId
            this.nativeDetached = true
            this.bindings.clear()
            this.modifierBindings.clear()
            this.sentInputs.clear()
            this.stagedEpochs.clear()
            for (const [key, value] of bindings) this.bindings.set(key, value)
            for (const [key, value] of modifierBindings) this.modifierBindings.set(key, value)
            for (const [key, value] of sentInputs) this.sentInputs.set(key, value)
            for (const [key, value] of stagedEpochs) this.stagedEpochs.set(key, value)
        })
        for (const [name, value] of values) this.update(name, value, true)
        for (const [index, child] of (this.pendingChildren ?? this.children).entries()) {
            child.windowId = this.windowId
            child.ensureAttached()
            const id = child.id
            const parent = this.id
            this.host.stage(native => {
                native.insertChild(parent, id, index)
                cantonExecutionStats.nativeInsertOperations++
            })
        }
    }

    private writeModifier(native: NativeTransactionTarget, next: PxModifier): boolean {
        const previous = this.inputs.get('modifier') as PxModifier | undefined
        if (!previous) return false
        if (previous.elements.length === next.elements.length && previous.elements.every((element, index) => sameModifierElement(element, next.elements[index]))) {
            modifierStats.equalWritesSkipped++
            return true
        }
        if (previous.elements.length !== next.elements.length || previous.elements.some((item, index) => item.type !== next.elements[index].type || item.key !== next.elements[index].key)) return false
        const handles = native.modifierInstances(this.id)
        if (handles.length !== next.elements.length) throw new Error('已提交 Modifier 描述与原生实例不一致')
        for (let index = 0; index < next.elements.length; index++) {
            const element = next.elements[index]
            if (sameModifierElement(previous.elements[index], element)) continue
            const handle = handles[index]
            if (handle.kind !== nativeModifierKind(element) || handle.key !== (element.key ?? '')) throw new Error('Modifier 参数更新目标与已提交实例不一致')
            this.writeModifierInstance(native, handle, element)
        }
        return true
    }

    private writeModifierInstance(native: NativeTransactionTarget, handle: NativeModifierHandle, element: ModifierElement): void {
        let binding = this.modifierBindings.get(handle.identity)
        if (!binding) {
            binding = native.registerModifierBinding(this.id, handle)
            this.modifierBindings.set(handle.identity, binding)
            this.host.onAbort(() => { this.modifierBindings.delete(handle.identity) })
        }
        native.updateBinding(binding, element)
        modifierStats.instanceWrites++
    }

    deactivate(): void {
        this.host.unregisterProvider(this.id)
        this.attached = false
        this.nativeDetached = true
        this.bindings.clear()
        this.modifierBindings.clear()
        this.children = []
    }

    activate(): void {
        if (this.attached || this.retired) return
        this.attached = true
        this.host.onAbort(() => { this.attached = false })
        this.ensureAttached()
    }

    retire(): void {
        if (this.retired) return
        this.retired = true
        this.host.unregisterProvider(this.id)
        cantonExecutionStats.rearrangeNodesRetired++
        this.bindings.clear()
        this.inputs.clear()
        this.pendingInputs.clear()
        this.modifierBindings.clear()
        this.children = []
    }
}

function sameAppConfig(left: NativeAppConfig, right: NativeAppConfig): boolean {
    return left.exitOnLastWindowClosed === right.exitOnLastWindowClosed && left.appId === right.appId && left.icon === right.icon
}

function sameWindowConfig(left: NativeWindowConfig, right: NativeWindowConfig): boolean {
    return (left.ownerWindowId === right.ownerWindowId && left.title === right.title && left.icon === right.icon && left.visible === right.visible && sameWindowSize(left.sizeDp, right.sizeDp) && sameWindowSize(left.minSizeDp, right.minSizeDp) && sameWindowSize(left.maxSizeDp, right.maxSizeDp) && sameWindowPosition(left.position, right.position) && left.placement === right.placement && left.modality === right.modality && left.chrome === right.chrome && left.role === right.role && left.resizable === right.resizable && left.minimizable === right.minimizable && left.maximizable === right.maximizable && left.closable === right.closable && left.alwaysOnTop === right.alwaysOnTop && left.showInTaskbar === right.showInTaskbar && left.backdrop === right.backdrop && left.cornerPreference === right.cornerPreference)
}

function nativeModifierKind(element: ModifierElement): string {
    return element.type === 'absoluteOffset' ? 'offset' : element.type
}

function sameDescriptorFields(left: unknown, right: unknown): boolean {
    if (Object.is(left, right)) return true
    if (!left || !right || typeof left !== 'object' || typeof right !== 'object') return false
    const a = left as Record<string, unknown>
    const b = right as Record<string, unknown>
    const keys = Object.keys(a)
    return keys.length === Object.keys(b).length && keys.every(key => Object.prototype.hasOwnProperty.call(b, key) && Object.is(a[key], b[key]))
}

// 只比较正式 Modifier 的值字段，业务对象与回调仍按身份判断，不递归探测任意对象
const descriptorFields: Readonly<Record<string, readonly string[]>> = {
    background: ['brush', 'shape'], border: ['brush', 'shape'], clip: ['shape'], graphicsLayer: ['transformOrigin'],
    text: ['style'], textField: ['textStyle'], paint: ['painter', 'colorFilter'],
    animateContentSize: ['animationSpec'],
    animateItem: ['fadeInSpec', 'placementSpec', 'fadeOutSpec'],
}

function sameModifierElement(left: ModifierElement, right: ModifierElement): boolean {
    if (left === right) return true
    if (left.type !== right.type || left.key !== right.key) return false
    const fields = descriptorFields[left.type] ?? []
    const keys = Object.keys(left.value)
    return keys.length === Object.keys(right.value).length && keys.every(key => Object.prototype.hasOwnProperty.call(right.value, key) && (Object.is(left.value[key], right.value[key]) || fields.includes(key) && sameDescriptorFields(left.value[key], right.value[key])))
}
