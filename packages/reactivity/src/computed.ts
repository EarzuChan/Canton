import { isFunction } from '@canton/shared'
import { ReactiveFlags, TrackOpTypes } from './constants.ts'
import { Dep } from './dep.ts'
import { activeSub, batch, globalVersion, refreshComputed } from './effect.ts'
import { type ComputedSubscriber, type DebuggerEvent, type DebuggerOptions, EffectFlags, type Link, type Subscriber } from './system.ts'
import type { ComputedGetter, ComputedRef, ComputedSetter, WritableComputedOptions, WritableComputedRef } from './types.ts'
import { warn } from './warning.ts'

// 响应式核心实现，不从 Framework 用户入口导出
export class ComputedRefImpl<T = any> implements ComputedSubscriber {
    _value: any = undefined
    readonly dep: Dep = new Dep(this)
    readonly [ReactiveFlags.IS_REF] = true
    readonly [ReactiveFlags.IS_READONLY]: boolean
    deps?: Link = undefined
    depsTail?: Link = undefined
    flags: EffectFlags = EffectFlags.DIRTY
    globalVersion: number = globalVersion - 1
    next?: Subscriber = undefined

    onTrack?: (event: DebuggerEvent) => void
    onTrigger?: (event: DebuggerEvent) => void

    constructor(public fn: ComputedGetter<T>, private readonly setter: ComputedSetter<T> | undefined) {
        this[ReactiveFlags.IS_READONLY] = !setter

    }

    notify(): true | void {
        this.flags |= EffectFlags.DIRTY
        if (
            !(this.flags & EffectFlags.NOTIFIED) && activeSub !== this
        ) {
            batch(this, true)
            return true
        }
    }

    get value(): T {
        const link = __DEV__
            ? this.dep.track({
                target: this,
                type: TrackOpTypes.GET,
                key: 'value',
            })
            : this.dep.track()
        refreshComputed(this)
        if (link) {
            link.version = this.dep.version
        }
        return this._value
    }

    set value(newValue) {
        if (this.setter) {
            this.setter(newValue)
        } else if (__DEV__) {
            warn('Write operation failed: computed value is readonly')
        }
    }
}

export function computed<T>(getter: ComputedGetter<T>, debugOptions?: DebuggerOptions): ComputedRef<T>
export function computed<T, S = T>(options: WritableComputedOptions<T, S>, debugOptions?: DebuggerOptions): WritableComputedRef<T, S>
export function computed<T>(getterOrOptions: ComputedGetter<T> | WritableComputedOptions<T>, debugOptions?: DebuggerOptions) {
    let getter: ComputedGetter<T>
    let setter: ComputedSetter<T> | undefined

    if (isFunction(getterOrOptions)) {
        getter = getterOrOptions
    } else {
        getter = getterOrOptions.get
        setter = getterOrOptions.set
    }

    const cRef = new ComputedRefImpl(getter, setter)

    if (__DEV__ && debugOptions) {
        cRef.onTrack = debugOptions.onTrack
        cRef.onTrigger = debugOptions.onTrigger
    }

    return cRef as any
}
