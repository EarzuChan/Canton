import { defineArrangable, slotParams } from '../runtime/index.ts'
import type { Arrangable, ArrangableProps, PropType, SlotArgs, SlotTypes, WithSlotTypes } from '../runtime/index.ts'
import { isArrangableDefinition } from '../runtime/apiDefineArrangable.ts'
import { retainContent } from '../runtime/internal.ts'
import type { RearrangeKey } from '../runtime/internal.ts'

export type DynamicArrangableProps = ArrangableProps<typeof DynamicArrangable>
const dynamicArrangable = defineArrangable({
    name: 'DynamicArrangable',
    props: { is: { type: Object as PropType<Arrangable>, required: true, validator: isArrangableDefinition }, props: { type: Object as PropType<Record<string, unknown>>, default: () => ({}) } },
    contentTarget: 'is',
    setup: (props, { call, slots }) => () => call(0, props.is, Object.fromEntries(Object.keys(props.props).map(name => [name, () => props.props[name]])), slots),
})
// 内容原样转交给 is 指向的定义，各槽位的参数类型也取自它
interface DynamicSlots extends SlotTypes { readonly slots: this['parameters'] extends { readonly is: () => infer D } ? SlotArgs<D> : {} }
export const DynamicArrangable = dynamicArrangable as WithSlotTypes<typeof dynamicArrangable, DynamicSlots>

export type KeepAliveProps = ArrangableProps<typeof KeepAlive>
export const KeepAlive = defineArrangable({
    name: 'KeepAlive',
    props: { cacheKey: { type: [String, Number, Symbol, BigInt, null] as PropType<RearrangeKey>, required: true }, max: { type: Number, default: 10, validator: (value: unknown) => Number.isInteger(value) && Number(value) > 0 } },
    slots: { default: slotParams() },
    setup(props, { slot }) {
        const content = slot()
        return () => retainContent(0, props.cacheKey, content, props.max)
    },
})