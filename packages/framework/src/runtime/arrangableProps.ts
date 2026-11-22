import { batchUpdates, isRef, isReadonly, shallowReadonly, shallowRef, pauseTracking, resetTracking, type ShallowRef } from '@canton/reactivity'
import { hasOwn, isArray } from '@canton/shared'
import type { ArrangableInstance, CallMetadata } from './arrangable.ts'
import type { Data, NormalizedProps, PropConstructor, PropGetter, PropInputs, PropOptions } from './propTypes.ts'
import { normalizeDeclaredProps, prepareParameters, resolveParameterPlan } from './propDeclarations.ts'
import { cantonExecutionStats } from './executionStats.ts'
import { ValueBinding } from './valueBinding.ts'
import { queueJob, SchedulerJobFlags, type SchedulerJob } from './scheduler.ts'
import { callWithErrorHandling, ErrorCodes } from './errorHandling.ts'

export class PropStore {
    readonly values: Data = Object.create(null)
    private readonly cells = new Map<string, ShallowRef<unknown>>()
    private readonly bindings = new Map<string, ValueBinding>()
    private readonly defaults = new Map<string, unknown>()
    private readonly pending = new Map<string, unknown>()
    private absent = new Set<string>()
    private readonly job: SchedulerJob
    private readonly refreshDirty = () => {
        this.preserve()
        for (const binding of this.bindings.values()) binding.refreshDirty()
        this.publishValues()
    }
    private readonly updateTask = { scope: () => this.owner.structure, dirty: () => !this.owner.isUnmounted && !this.owner.isDeactivated && [...this.bindings.values()].some(binding => binding.dirty), run: this.refreshDirty }
    private readonly declarations: NormalizedProps
    private constants = new Set<string>()
    private inputs: PropInputs<Data> | undefined
    private sources: Readonly<Record<string, string | undefined>> = {}

    constructor(private readonly owner: ArrangableInstance) {
        this.declarations = normalizeDeclaredProps(owner.type.props)
        this.job = () => {
            if (!owner.isUnmounted && !owner.isDeactivated) callWithErrorHandling(() => owner.rearrangeSession.runValue(this.updateTask), owner, ErrorCodes.ARRANGABLE_UPDATE)
        }
        this.job.i = owner
        for (const name of Object.keys(this.declarations)) {
            const cell = shallowRef<unknown>()
            this.cells.set(name, cell)
            Object.defineProperty(this.values, name, { enumerable: true, get: () => cell.value })
        }
        Object.freeze(this.values)
    }

    private preserve(): void {
        this.owner.rearrangeSession.preserve(this, () => {
            const inputs = this.inputs
            const values = new Map([...this.cells].map(([name, cell]) => [name, cell.value]))
            const defaults = new Map(this.defaults)
            const constants = this.constants
            const sources = this.sources
            const absent = this.absent
            return () => {
                this.inputs = inputs
                this.pending.clear()
                this.defaults.clear()
                for (const [name, value] of defaults) this.defaults.set(name, value)
                this.constants = constants
                this.sources = sources
                this.absent = absent
                batchUpdates(() => {
                    for (const [name, value] of values) this.cells.get(name)!.value = value
                })
            }
        })
    }

    private publishValues(): void {
        if (!this.pending.size) return
        const values = Object.fromEntries([...this.cells].map(([name, cell]) => [name, this.pending.has(name) ? this.pending.get(name) : cell.value]))
        for (const [name, declaration] of Object.entries(this.declarations)) try {
            validateProp(name, values[name], declaration, values, this.absent.has(name))
        } catch (error) {
            const source = this.source(name)
            if (error instanceof Error) error.message += `\nArrangable：${this.owner.type.name ?? this.owner.type.__name ?? '匿名定义'}${source ? `\n来源：${source}` : ''}`
            throw error
        }
        const pending = [...this.pending]
        this.pending.clear()
        batchUpdates(() => {
            for (const [name, value] of pending) this.cells.get(name)!.value = value
        })
    }

    private invalidate = (): void => {
        if (this.owner.rearrangeSession.reverting) return
        if (this.owner.rearrangeSession.preparing) this.owner.rearrangeSession.runValue(this.updateTask)
        else queueJob(this.job)
    }

    source(name: string): string | undefined {
        return this.sources[name] ?? this.owner.source ?? this.owner.type.__file
    }

    invalidateConstants(): void {
        if (!this.constants.size) return
        this.preserve()
        this.constants.clear()
    }

    update(inputs: Record<string, PropGetter<unknown>>, metadata: CallMetadata = {}): void {
        const prepared = metadata.parameters ? resolveParameterPlan(metadata.parameters, this.owner.type, metadata.source) : undefined
        if (inputs === this.inputs && Object.isFrozen(inputs)) {
            this.refreshDirty()
            return
        }
        const plan = prepared ?? prepareParameters(this.owner.type, Object.keys(inputs), [], metadata.source)
        const entries = Object.entries(inputs)
        if (entries.length !== plan.names.length) throw new TypeError('参数组与声明位置计划不一致')
        const getters = entries.map(([name, getter], index) => {
            if (name !== plan.names[index]) throw new TypeError(`参数组与声明位置计划不一致：${name}`)
            if (typeof getter !== 'function') throw new TypeError(`内部参数 ${name} 必须提供求值函数`)
            return getter
        })

        this.preserve()
        this.inputs = inputs
        this.sources = metadata.sources ?? {}
        this.absent = new Set(plan.fields.filter(field => field.position < 0).map(field => field.name))
        const nextConstants = new Set(metadata.constants ?? [])
        pauseTracking()
        try {
            for (const { name, position } of plan.fields) {
                const declaration = this.declarations[name]
                if (this.constants.has(name) && nextConstants.has(name)) continue
                const read = () => {
                    if (position >= 0) cantonExecutionStats.parameterPositionReads++
                    let value = position < 0 ? undefined : getters[position]()
                    if (value === undefined && hasOwn(declaration, 'default')) {
                        if (!this.defaults.has(name)) {
                            const factory = declaration.default
                            this.defaults.set(name, typeof factory === 'function' && declaration.type !== Function && !declaration.skipFactory ? factory(this.values) : factory)
                        }
                        value = this.defaults.get(name)
                    }
                    return value
                }
                const existing = this.bindings.get(name)
                if (existing) existing.refresh(read, this.source(name))
                else this.bindings.set(name, new ValueBinding(read, this.owner, value => this.pending.set(name, value), this.source(name), this.invalidate))
            }

            this.constants = nextConstants
            this.publishValues()
        } finally {
            resetTracking()
        }
    }

    stop(): void {
        this.job.flags = (this.job.flags ?? 0) | SchedulerJobFlags.DISPOSED
        for (const binding of this.bindings.values()) binding.stop()
        this.bindings.clear()
    }
}

function validateProp(name: string, value: unknown, declaration: PropOptions, props: Data, absent: boolean): void {
    if (absent && declaration.required && !hasOwn(declaration, 'default')) throw new TypeError(`缺少必需参数：${name}`)
    if (value === undefined && !declaration.required) return
    if (declaration.refKind && (!isRef(value) || declaration.refKind === 'writable' && isReadonly(value))) throw new TypeError(`参数 ${name} 要求${declaration.refKind === 'writable' ? '可写' : '只读'} Ref 本体`)
    const { type, validator } = declaration
    if (type != null && type !== true) {
        const types = isArray(type) ? type : [type]
        if (!types.some(candidate => matchesType(value, candidate))) throw new TypeError(`参数 ${name} 类型错误：要求 ${types.map(candidate => candidate?.name ?? 'null').join(' | ')}，实际为 ${value === null ? 'null' : isArray(value) ? 'Array' : typeof value}`)
    }
    if (validator && !validator(value, shallowReadonly(props))) throw new TypeError(`参数 ${name} 未通过声明校验`)
}

function matchesType(value: unknown, type: PropConstructor | null): boolean {
    if (type === null) return value === null

    switch (type) {
        case String:
            return typeof value === 'string'

        case Number:
            return typeof value === 'number'

        case Boolean:
            return typeof value === 'boolean'

        case Function:
            return typeof value === 'function'

        case Symbol:
            return typeof value === 'symbol'

        case BigInt:
            return typeof value === 'bigint'

        case Object:
            return value !== null && typeof value === 'object' && !isArray(value)

        case Array:
            return isArray(value)

        default:
            return value instanceof type
    }
}
