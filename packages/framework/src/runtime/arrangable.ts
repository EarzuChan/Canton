import { effectScope, type EffectScope, type Ref } from '@canton/reactivity'
import type { IfAny } from '@canton/shared'
import type { ArrangableObjectPropsOptions, Data, ExtractPublicPropTypes, PropInputs } from './propTypes.ts'
import type { LifecycleHooks } from './enums.ts'
import type { FrameScheduler } from './scheduler.ts'

export type { Data }
export type StructureProgram = () => void
// 内容按值接收出口交付的参数对象；没有参数的槽位不接收实参
export type Content<P = any> = [keyof P] extends [never] ? () => void : (params: P) => void
export type Contents = Readonly<Record<string, Content | undefined>>
// 槽位声明只在类型层面携带参数，运行期只用到槽位名
export interface SlotDeclaration<P = {}> { readonly params?: P }
export type SlotDeclarations = Readonly<Record<string, SlotDeclaration>>
export type SlotParams<S> = { readonly [K in keyof S]: S[K] extends SlotDeclaration<infer P> ? P : never }
// 槽位参数的类型随调用参数变化的定义（如 Lazy 的 item 取自 items）实现本接口，以 this['parameters'] 读取调用参数的类型，并经 WithSlotTypes 挂到定义的类型上；运行期不存在
export interface SlotTypes { readonly parameters: unknown; readonly slots: object }
export type WithSlotTypes<D, F extends SlotTypes> = D & { readonly slotTypes: F }
// 给定定义与调用参数，各槽位交付给内容的参数
export type SlotArgs<D, P = unknown> = D extends { readonly slotTypes: infer F extends SlotTypes } ? (F & { readonly parameters: P })['slots'] : D extends { readonly slots: infer S } ? SlotParams<S> : never
// 编译产物用它约束一次调用的内容表：D 可以是定义或装着定义的 Ref，类型未知的定义不作约束
export type ContentsOf<D, P = unknown> = IfAny<D, Contents, D extends Ref<infer V> ? ContentsOf<V, P> : { readonly [K in keyof SlotArgs<D, P>]?: Content<SlotArgs<D, P>[K]> }>
export type CallPosition = string | number
export type RearrangeKey = string | number | symbol | bigint | null | undefined

export interface ArrangableDefinition<P extends Data = any> {
    readonly name?: string
    readonly __name?: string
    readonly __file?: string
    readonly __hmrId?: string
    readonly props: ArrangableObjectPropsOptions<P>
    readonly slots: SlotDeclarations
    readonly contentTarget?: string
    readonly setup: (props: Readonly<P>, context: SetupContext) => StructureProgram
}

export type Arrangable<P extends Data = any> = ArrangableDefinition<P>
export type ArrangableProps<D extends ArrangableDefinition> = string extends keyof D['props'] ? Data : ExtractPublicPropTypes<D['props']>

export interface ParameterPlan {
    readonly definition: ArrangableDefinition
    readonly names: readonly string[]
    readonly fields: readonly { readonly name: string; readonly position: number }[]
}

export interface CallMetadata {
    readonly parameters?: ParameterPlan
    readonly key?: RearrangeKey
    readonly source?: string
    readonly sources?: Readonly<Record<string, string | undefined>>
    readonly constants?: readonly string[]
}

export interface SetupContext<S = Record<string, any>> {
    readonly slots: { readonly [K in keyof S]: Content<S[K]> }
    readonly call: <D extends ArrangableDefinition>(position: CallPosition, definition: D, inputs: PropInputs<ArrangableProps<D>>, contents?: Contents, metadata?: CallMetadata) => void
    readonly slot: <K extends keyof S & string = 'default' & keyof S>(name?: K) => Content<S[K]>
    readonly source: (name: string) => string | undefined
}

export interface AppConfig {
    errorHandler?: (error: unknown, source: string | undefined, phase: string) => void
    warnHandler?: (message: string) => void
}

export interface AppContext {
    readonly config: AppConfig
    readonly provides: Record<PropertyKey, unknown>
    readonly definitions: Record<string, ArrangableDefinition>
    readonly host: RearrangeHost
}

// 节点只由 Layout 实现创建，这里声明重排与后端应用的职责边界
export interface RearrangeNode {
    readonly owner: ArrangableInstance
    readonly children: readonly RearrangeNode[]
    reconcileChildren(children: readonly RearrangeNode[]): void
    deactivate(): void
    activate(): void
    retire(): void
}

// 应用原生候选并交付回执；RearrangeSession 负责前端实例和结构账本
export interface RearrangeHost {
    currentTime(): number
    requestFrame(pending: boolean): void
    begin(): void
    reconcileRoots(roots: readonly RearrangeNode[]): void
    apply(complete: (error?: Error) => void): void
    beginContinuation?(): void
    applyContinuation?(): void
    rollback(): void
}

// 实例只经以下两个端口接触重组引擎，实现均在 rearrange：会话随实例创建即可用，结构账本在 setup 返回结构程序后建立
export interface StructureScope {
    readonly parent: StructureScope | null
    // 热更新后让此范围内所有实例的常量参数重新求值
    invalidateConstants(): void
    // 排入下一帧重组
    schedule(force?: boolean): void
    // 并入正在准备的候选
    enqueue(force?: boolean): void
    isActive(): boolean
}

export interface ValueTask {
    scope(): StructureScope
    dirty(): boolean
    run(): void
}

export interface InstanceSession {
    readonly scheduler: FrameScheduler
    readonly preparing: boolean
    readonly reverting: boolean
    readonly isDisposed: boolean
    preserve(target: object, checkpoint: () => () => void): void
    onCommit(callback: () => void): void
    onCommitCleanup(callback: () => void): void
    beforeApply(callback: () => void): void
    onRollback(callback: () => void): void
    runValue(update: ValueTask): void
    continuePreparation(work: () => void): void
}

export class ArrangableInstance {
    readonly uid = nextInstanceId++
    readonly scope: EffectScope = effectScope(true)
    readonly provides: Record<PropertyKey, unknown>
    readonly hooks = new Map<LifecycleHooks, Function[]>()
    readonly ids: [string, number, number] = ['', 0, 0]
    rearrangeSession!: InstanceSession
    props!: Readonly<Data>
    structure!: StructureScope
    node: RearrangeNode | null = null
    contents: Contents = Object.freeze({})
    setupContext!: SetupContext
    isMounted = false
    isUnmounted = false
    isDeactivated = false

    constructor(readonly type: ArrangableDefinition, readonly parent: ArrangableInstance | null, readonly appContext: AppContext, readonly source?: string) {
        this.provides = Object.create(parent?.provides ?? appContext.provides)
    }
}

let nextInstanceId = 1
export let currentInstance: ArrangableInstance | null = null

export function getCurrentInstance(): ArrangableInstance | null {
    return currentInstance
}

export function setCurrentInstance(instance: ArrangableInstance, scope: EffectScope = instance.scope): () => void {
    const previous = currentInstance
    currentInstance = instance
    scope.on()

    return () => {
        scope.off()
        currentInstance = previous
    }
}

export function getArrangableName(definition: ArrangableDefinition): string | undefined {
    return definition.name ?? definition.__name
}
