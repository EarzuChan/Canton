import type { TrackOpTypes, TriggerOpTypes } from './constants.ts'

// 追踪内核契约：订阅者、依赖与二者之间链接节点的形状；Dep、ReactiveEffect、ComputedRefImpl 分别实现
export enum EffectFlags {
    ACTIVE = 1 << 0,
    RUNNING = 1 << 1,
    TRACKING = 1 << 2,
    NOTIFIED = 1 << 3,
    DIRTY = 1 << 4,
    ALLOW_RECURSE = 1 << 5,
    PAUSED = 1 << 6,
    EVALUATED = 1 << 7,
}

export type DebuggerEvent = {
    effect: Subscriber
} & DebuggerEventExtraInfo

export type DebuggerEventExtraInfo = {
    target: object
    type: TrackOpTypes | TriggerOpTypes
    key: any
    newValue?: any
    oldValue?: any
    oldTarget?: Map<any, any> | Set<any>
}

export interface DebuggerOptions {
    onTrack?: (event: DebuggerEvent) => void
    onTrigger?: (event: DebuggerEvent) => void
}

export interface Subscriber extends DebuggerOptions {
    deps?: Link
    depsTail?: Link
    flags: EffectFlags
    next?: Subscriber
    notify(): true | void
}

// 同一订阅同时挂在依赖与订阅者两条链上，便于独立退订和复用
export interface Link {
    version: number
    sub: Subscriber
    dep: Dependency
    nextDep?: Link
    prevDep?: Link
    nextSub?: Link
    prevSub?: Link
    prevActiveLink?: Link
}

export interface Dependency {
    version: number
    activeLink?: Link
    subs?: Link
    subsHead?: Link
    map?: { delete(key: unknown): boolean }
    key?: unknown
    sc: number
    readonly computed?: ComputedSubscriber
    notify(debugInfo?: DebuggerEventExtraInfo): void
}

export interface ComputedSubscriber extends Subscriber {
    _value: any
    readonly dep: Dependency
    globalVersion: number
    fn(oldValue?: any): any
}
