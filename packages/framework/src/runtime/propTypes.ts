import type { IfAny } from '@canton/shared'

export type Data = Record<string, unknown>

export type ArrangablePropsOptions<P = Data> = | ArrangableObjectPropsOptions<P> | string[]

export type ArrangableObjectPropsOptions<P = Data> = { [K in keyof P]: Prop<P[K]> | null }

export type Prop<T, D = T> = PropOptions<T, D> | PropType<T>

type DefaultFactory<T> = (props: Data) => T | null | undefined

export interface PropOptions<T = any, D = T> {
    refKind?: 'writable' | 'readonly'
    type?: PropType<T> | true | null
    required?: boolean
    default?: D | DefaultFactory<D> | null | undefined | object

    validator?(value: unknown, props: Data): boolean

    skipFactory?: boolean // 内部
}

export type PropType<T> = PropConstructor<T> | (PropConstructor<T> | null)[]

export type PropConstructor<T = any> =
    | { new(...args: any[]): T & {} }
    | { (): T }
    | BigIntConstructor
    | PropMethod<T>

type PropMethod<T, TConstructor = any> = [T] extends [
    ((...args: any) => any) | undefined,
]
    ? { new(): TConstructor; (): T; readonly prototype: TConstructor }
    : never

type RequiredKeys<T> = {
    [K in keyof T]: T[K] extends | { required: true }
    | { default: any }
    ? T[K] extends { default: undefined | (() => undefined) }
    ? never
    : K
    : never
}[keyof T]

type OptionalKeys<T> = Exclude<keyof T, RequiredKeys<T>>

type DefaultKeys<T> = { [K in keyof T]: T[K] extends { default: any } ? K : never }[keyof T]

type InferPropType<T, NullAsAny = true> = [T] extends [null]
    ? NullAsAny extends true
    ? any
    : null
    : [T] extends [{ type: null | true }]
    ? any
    : [T] extends [ObjectConstructor | { type: ObjectConstructor }]
    ? Record<string, any>
    : [T] extends [BooleanConstructor | { type: BooleanConstructor }]
    ? boolean
    : [T] extends [DateConstructor | { type: DateConstructor }]
    ? Date
    : [T] extends [(infer U)[] | { type: (infer U)[] }]
    ? U extends DateConstructor
    ? Date | InferPropType<U, false>
    : InferPropType<U, false>
    : [T] extends [Prop<infer V, infer D>]
    ? unknown extends V
    ? keyof V extends never
    ? IfAny<V, V, D>
    : V
    : V
    : T

export type ExtractPropTypes<O> = {
    [K in keyof Pick<O, RequiredKeys<O>>]: O[K] extends { default: any }
    ? Exclude<InferPropType<O[K]>, undefined>
    : InferPropType<O[K]>
} & {
    [K in keyof Pick<O, OptionalKeys<O>>]?: InferPropType<O[K]>
}

type PublicRequiredKeys<T> = {
    [K in keyof T]: T[K] extends { required: true } ? K : never
}[keyof T]

type PublicOptionalKeys<T> = Exclude<keyof T, PublicRequiredKeys<T>>

export type ExtractPublicPropTypes<O> = { [K in keyof Pick<O, PublicRequiredKeys<O>>]: InferPropType<O[K]> } & { [K in keyof Pick<O, PublicOptionalKeys<O>>]?: InferPropType<O[K]> }

// 默认值只来自声明，布尔参数不自动补值或转换
export type ExtractDefaultPropTypes<O> = O extends object ? { [K in keyof Pick<O, DefaultKeys<O>>]: InferPropType<O[K]> } : {}
export type NormalizedProps = Record<string, PropOptions>
export type NormalizedPropsOptions = [NormalizedProps, string[]] | []

export type PropGetter<T> = () => T
export type PropInputs<P> = { readonly [K in keyof P]: PropGetter<P[K]> }
