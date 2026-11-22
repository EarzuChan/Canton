import { afterEach } from 'node:test'
import { mountApplication, type CantonApplication, type CantonRunningApplication } from '../../packages/framework/src/app/app.ts'
import type { NativeAppConfig, NativeModifierHandle, NativeTransactionTarget, NativeWindowConfig, NativeWindowDriver } from '../../packages/framework/src/native/protocol.ts'
import { Modifier, type ModifierElement } from '../../packages/framework/src/modifier/modifier.ts'
import { createFakeNative } from '../helpers/fakeNative.ts'

const frameSources = new Set<(time?: number) => void>()
const drivers = new WeakMap<NativeTransactionTarget, (time?: number) => void>()
afterEach(() => frameSources.clear())

// 测试只控制宿主帧到达，阶段与任务选择仍由生产调度器执行
export function advanceFrames(): void {
    for (const frame of [...frameSources]) frame()
}

export function mountFrame(application: CantonApplication, target: NativeTransactionTarget): CantonRunningApplication {
    const running = mountApplication(application, target)
    drivers.get(target)?.()
    return running
}

export interface RecordedLayoutNode {
    readonly id: number
    readonly type: string
    readonly windowId: number
    readonly inputs: Map<string, unknown>
    readonly children: number[]
    modifiers: NativeModifierHandle[]
}

export interface RecordedWindow {
    readonly windowId: number
    readonly rootNodeId: number
    config: NativeWindowConfig
}

export function recordingNative(autoApply = true) {
    let prepare: ((time: number) => void) | undefined
    let completeFrame: ((success: boolean) => void) | undefined
    let windowDriver: NativeWindowDriver | undefined
    let requested = false
    let preparing = false
    let time = 0
    let materialize: ((id: number, indices: readonly number[]) => void) | undefined
    let materializing = false
    let nextWindowId = 1

    const frame = (timestamp = time + 16) => {
        if (!requested || completion) return
        requested = false
        time = timestamp
        preparing = true
        try { prepare?.(time) } finally { preparing = false }
        if (completion && autoApply) finish()
        else if (!completion) completeFrame?.(true)
    }
    frameSources.add(frame)

    let nodes = new Map<number, RecordedLayoutNode>()
    let windows = new Map<number, RecordedWindow>()
    let bindings = new Map<bigint, { id: number; input: string }>()
    let appConfig: NativeAppConfig | undefined
    let candidate: Map<number, RecordedLayoutNode> | undefined
    let candidateWindows: Map<number, RecordedWindow> | undefined
    let candidateBindings: typeof bindings | undefined
    let candidateAppConfig: NativeAppConfig | undefined
    let completion: ((error?: string) => void) | undefined
    let identity = 1n
    let submissions = 0
    let writes = 0
    let appExitRequests = 0
    const windowCommands: { windowId: number; command: 'activate' | 'requestClose' }[] = []
    const windowLifecycleEvents: { kind: 'create' | 'update' | 'destroy'; windowId: number }[] = []

    const live = () => {
        if (!candidate) throw new Error('原生操作必须属于候选事务')
        return candidate
    }
    const liveWindows = () => {
        if (!candidateWindows) throw new Error('原生窗口操作必须属于候选事务')
        return candidateWindows
    }
    const node = (id: number) => {
        const result = live().get(id)
        if (!result) throw new Error(`布局节点不存在：${id}`)
        return result
    }
    const finish = (error?: string) => {
        if (!completion) throw new Error('当前没有待应用事务')
        const callback = completion
        completion = undefined
        if (!error) {
            nodes = candidate!
            windows = candidateWindows!
            bindings = candidateBindings!
            appConfig = candidateAppConfig
        }
        candidate = undefined
        candidateWindows = undefined
        candidateBindings = undefined
        candidateAppConfig = undefined
        try { callback(error) } finally { completeFrame?.(!error) }
    }
    const target: NativeTransactionTarget = createFakeNative({
        configureApp(config) {
            if (candidate) candidateAppConfig = config
            else appConfig = config
        },
        requestAppExit() { appExitRequests++ },
        allocateWindowId: () => nextWindowId++,
        createWindow(windowId, rootNodeId, config) {
            if (liveWindows().has(windowId)) throw new Error(`窗口身份重复：${windowId}`)
            liveWindows().set(windowId, { windowId, rootNodeId, config })
            windowLifecycleEvents.push({ kind: 'create', windowId })
        },
        updateWindow(windowId, config) {
            const win = liveWindows().get(windowId)
            if (!win) throw new Error(`更新目标窗口不存在：${windowId}`)
            win.config = config
            windowLifecycleEvents.push({ kind: 'update', windowId })
        },
        destroyWindow(windowId) {
            if (!liveWindows().delete(windowId)) throw new Error(`销毁目标窗口不存在：${windowId}`)
            windowLifecycleEvents.push({ kind: 'destroy', windowId })
        },
        windowCommand(windowId, command) {
            windowCommands.push({ windowId, command })
            if (command === 'requestClose') windowDriver?.onCloseRequest(windowId)
        },
        installWindowDriver(driver) {
            windowDriver = driver
        },
        installFrameDriver(prepareCallback, completeCallback) {
            prepare = prepareCallback
            completeFrame = completeCallback
            frameSources.add(frame)
        },
        currentTime: () => time,
        requestFrame(pending) { requested = pending },
        installLayoutDriver(callback) { materialize = callback },
        continueRearrange() {
            if (!materializing || !candidate || !completion) throw new Error('子组合只能继续当前候选')
        },
        beginRearrange() {
            if (candidate) throw new Error('候选事务不能重入')
            candidate = new Map([...nodes].map(([id, value]) => [id, { ...value, inputs: new Map(value.inputs), children: [...value.children], modifiers: [...value.modifiers] }]))
            candidateWindows = new Map([...windows].map(([id, win]) => [id, { ...win }]))
            candidateBindings = new Map(bindings)
            candidateAppConfig = appConfig
        },
        submitRearrange(callback) {
            submissions++
            completion = callback
            if (autoApply && !preparing) throw new Error('事务提交必须属于准备阶段')
        },
        abortRearrange() {
            candidate = undefined
            candidateWindows = undefined
            candidateBindings = undefined
            candidateAppConfig = undefined
            completion = undefined
        },
        createNode(id, type, windowId = 0) {
            if (live().has(id)) throw new Error(`布局节点身份重复：${id}`)
            live().set(id, { id, type, windowId, inputs: new Map(), children: [], modifiers: [] })
        },
        deleteNode(id) {
            const pending = [id]
            while (pending.length) {
                const current = pending.pop()!
                pending.push(...node(current).children)
                live().delete(current)
                for (const [handle, binding] of candidateBindings!) if (binding.id === current) candidateBindings!.delete(handle)
            }
        },
        insertChild(parent, child, index) {
            const c = node(child)
            const p = node(parent)
            if (c.windowId !== p.windowId) throw new Error('不能跨窗口挂载节点')
            for (const other of live().values()) {
                const previous = other.children.indexOf(child)
                if (previous >= 0) other.children.splice(previous, 1)
            }
            p.children.splice(index, 0, child)
        },
        removeChild(parent, child) {
            const children = node(parent).children
            const index = children.indexOf(child)
            if (index < 0) throw new Error('移除目标不属于该父节点')
            children.splice(index, 1)
        },
        registerBinding(id, input) {
            node(id)
            const handle = { identity: identity++, generation: 1n }
            candidateBindings!.set(handle.identity, { id, input })
            return handle
        },
        updateBinding(handle, value) {
            const binding = candidateBindings!.get(handle.identity)
            if (!binding) throw new Error('拒绝已退休绑定')
            const owner = node(binding.id)
            if (binding.input.startsWith('实例:')) {
                const targetId = BigInt(binding.input.slice(3))
                const index = owner.modifiers.findIndex(item => item.identity === targetId)
                if (index < 0) throw new Error('Modifier 实例已退休')
                const elements = [...(owner.inputs.get('modifier') as Modifier).elements]
                elements[index] = value as ModifierElement
                owner.inputs.set('modifier', new Modifier(elements))
            } else {
                if (binding.input === 'measurePolicy') {
                    const next = value as { kind?: string; version?: number; keys?: unknown }
                    const previous = owner.inputs.get(binding.input) as typeof next | undefined
                    if (next.kind === 'Lazy' && !next.keys && previous?.kind === 'Lazy' && previous.version === next.version) value = { ...previous, ...next } as typeof value
                }
                owner.inputs.set(binding.input, value)
                if (binding.input === 'modifier') owner.modifiers = (value as Modifier).elements.map((element, index) => {
                    const previous = owner.modifiers[index]
                    const kind = element.type === 'absoluteOffset' ? 'offset' : element.type
                    return previous?.kind === kind && previous.key === (element.key ?? '') ? previous : { identity: identity++, generation: 1n, kind, key: element.key ?? '' }
                })
            }
            writes++
        },
        modifierInstances(id) { return nodes.get(id)?.modifiers ?? [] },
        registerModifierBinding(id, instance) {
            if (!node(id).modifiers.some(handle => handle.identity === instance.identity && handle.generation === instance.generation)) throw new Error('Modifier 实例不属于受体')
            const handle = { identity: identity++, generation: 1n }
            candidateBindings!.set(handle.identity, { id, input: `实例:${instance.identity}` })
            return handle
        },
        releaseBinding(handle) { candidateBindings!.delete(handle.identity) },
        unmount() {
            frameSources.delete(frame)
            requested = false
            prepare = undefined
            completeFrame = undefined
            windowDriver = undefined
            nodes.clear()
            windows.clear()
            bindings.clear()
            target.abortRearrange()
        },
    })
    drivers.set(target, frame)
    return {
        target,
        frame,
        get time() { return time },
        get pendingFrame() { return requested },
        finish,
        get nodes() { return nodes },
        get windows() { return windows },
        get candidateNodes() { return candidate },
        get candidateWindows() { return candidateWindows },
        get appConfig() { return appConfig },
        get appExitRequests() { return appExitRequests },
        get windowCommands() { return windowCommands },
        get windowLifecycleEvents() { return windowLifecycleEvents },
        get windowDriver() {
            if (!windowDriver) throw new Error('窗口驱动尚未安装')
            return windowDriver
        },
        windowRoot(index = 0): RecordedLayoutNode {
            const win = [...windows.values()][index]
            if (!win) throw new Error(`第 ${index} 个窗口不存在`)
            const root = nodes.get(win.rootNodeId)
            if (!root) throw new Error(`窗口 ${win.windowId} 的根节点不存在`)
            return root
        },
        materialize(id: number, indices: readonly number[]) {
            if (!candidate || !completion || !materialize) throw new Error('材料化必须属于已提交候选')
            materializing = true
            try { materialize(id, indices) } finally { materializing = false }
        },
        get submissions() { return submissions },
        get writes() { return writes },
        textNodes(windowId?: number) {
            const result: { id: number; text: string }[] = []
            const roots = windowId !== undefined
                ? (windows.has(windowId) ? [windows.get(windowId)!.rootNodeId] : [])
                : windows.size > 0
                    ? [...windows.values()].map(w => w.rootNodeId).filter(id => nodes.has(id))
                    : (nodes.has(1) ? [1] : [])
            const pending = [...roots].reverse()
            while (pending.length) {
                const value = nodes.get(pending.pop()!)!
                for (const element of (value.inputs.get('modifier') as Modifier | undefined)?.elements ?? []) {
                    if (element.type === 'text') result.push({ id: value.id, text: String(element.value.text) })
                    if (element.type === 'textField') result.push({ id: value.id, text: String(element.value.value) })
                }
                pending.push(...[...value.children].reverse())
            }
            return result
        },
    }
}
