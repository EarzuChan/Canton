import type { ArrangableDefinition, SetupContext, SlotDeclaration, SlotDeclarations, SlotParams, StructureProgram } from './arrangable.ts'
import type { ArrangableObjectPropsOptions, ExtractPropTypes } from './propTypes.ts'
import { normalizeDeclaredProps } from './propDeclarations.ts'

export type DefineArrangable<P extends Record<string, unknown> = any> = ArrangableDefinition<P>
const definitions = new WeakSet<object>()
const slotDeclaration: SlotDeclaration<any> = Object.freeze({})

// 声明一个槽位，类型实参是它交付给内容的参数
export function slotParams<P = {}>(): SlotDeclaration<P> {
    return slotDeclaration
}

export function defineArrangable<const P extends ArrangableObjectPropsOptions = {}, const S extends SlotDeclarations = {}, const T extends keyof P & string | undefined = undefined>(options: {
    name?: string
    __name?: string
    __file?: string
    __hmrId?: string
    props?: P
    slots?: S
    contentTarget?: T
    setup: (props: Readonly<ExtractPropTypes<P>>, context: SetupContext<SlotParams<S>>) => StructureProgram
}): Omit<ArrangableDefinition<ExtractPropTypes<P>>, 'props' | 'slots'> & { readonly props: P; readonly slots: S; readonly contentTarget: T } {
    if (!options || typeof options !== 'object' || typeof options.setup !== 'function') throw new TypeError('Arrangable 定义必须提供 setup')

    const allowed = new Set(['name', '__name', '__file', '__hmrId', 'props', 'slots', 'contentTarget', 'setup'])
    for (const key of Object.keys(options)) if (!allowed.has(key)) throw new TypeError(`Arrangable 定义不支持字段：${key}`)
    normalizeDeclaredProps(options.props)

    if (options.contentTarget && !(options.contentTarget in (options.props ?? {}))) throw new TypeError('动态内容目标必须引用本定义声明的参数')

    const slots = options.slots ?? {}
    if (typeof slots !== 'object' || Array.isArray(slots)) throw new TypeError('内容声明必须是以槽位名为键的对象')

    const props = Object.fromEntries(Object.entries(options.props ?? {}).map(([name, value]) => {
        if (Array.isArray(value)) return [name, Object.freeze([...value])]
        if (value && typeof value === 'object') return [name, Object.freeze({ ...value, ...(Array.isArray(value.type) ? { type: Object.freeze([...value.type]) } : {}) })]
        return [name, value]
    }))

    const definition = Object.freeze({ ...options, props: Object.freeze(props), slots: Object.freeze({ ...slots }) }) as unknown as Omit<ArrangableDefinition<ExtractPropTypes<P>>, 'props' | 'slots'> & { readonly props: P; readonly slots: S; readonly contentTarget: T }
    definitions.add(definition)

    return definition
}

export function isArrangableDefinition(value: unknown): value is ArrangableDefinition {
    return typeof value === 'object' && value !== null && definitions.has(value)
}