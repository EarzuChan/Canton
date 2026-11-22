import { defineArrangable, slotParams } from '../runtime/index.ts'
import type { ArrangableProps, Contents, PropType } from '../runtime/index.ts'
import { BoxMeasurePolicy, ColumnMeasurePolicy, MinSizeMeasurePolicy, RowMeasurePolicy, FlowRowMeasurePolicy, FlowColumnMeasurePolicy } from '../layout/measurePolicy.ts'
import { M, Modifier } from '../modifier/modifier.ts'
import { boxHandle, columnHandle, flowColumnHandle, flowRowHandle, rowHandle } from '../modifier/layoutHandle.ts'
import type { BoxHandle, ColumnHandle, FlowColumnHandle, FlowRowHandle, RowHandle } from '../modifier/layoutHandle.ts'
import type { HorizontalArrangementProp, VerticalArrangementProp } from '../native/protocol.ts'
import type { BoxAlignment, HorizontalAlignment, VerticalAlignment } from '../values/primitives.ts'
import { Layout } from './Layout.ts'

// 把默认槽的内容连同本布局的 Handle 转交给内部 Layout
function withHandle<P>(content: (params: P) => void, params: P): Contents {
    return { default: () => content(params) }
}

export type BoxProps = ArrangableProps<typeof Box>
export const Box = defineArrangable({
    name: 'Box',
    props: { modifier: { type: Modifier, default: M }, contentAlignment: String as PropType<BoxAlignment>, propagateMinConstraints: Boolean, enabled: Boolean },
    slots: { default: slotParams<{ boxHandle: BoxHandle }>() },
    setup(props, { call, slot, source }) {
        const contents = withHandle(slot(), { boxHandle })
        return () => call(0, Layout, {
            measurePolicy: () => BoxMeasurePolicy({ contentAlignment: props.contentAlignment, propagateMinConstraints: props.propagateMinConstraints }),
            modifier: () => props.modifier,
            enabled: () => props.enabled,
        }, contents, { sources: { modifier: source('modifier'), measurePolicy: source('contentAlignment') ?? source('propagateMinConstraints'), enabled: source('enabled') } })
    },
})

export type RowProps = ArrangableProps<typeof Row>
export const Row = defineArrangable({
    name: 'Row',
    props: { modifier: { type: Modifier, default: M }, horizontalArrangement: [String, Object] as PropType<HorizontalArrangementProp>, verticalAlignment: String as PropType<VerticalAlignment | 'Baseline'> },
    slots: { default: slotParams<{ rowHandle: RowHandle }>() },
    setup(props, { call, slot, source }) {
        const contents = withHandle(slot(), { rowHandle })
        return () => call(0, Layout, {
            measurePolicy: () => RowMeasurePolicy({ horizontalArrangement: props.horizontalArrangement, verticalAlignment: props.verticalAlignment }),
            modifier: () => props.modifier,
        }, contents, { sources: { modifier: source('modifier'), measurePolicy: source('horizontalArrangement') ?? source('verticalAlignment') } })
    },
})

export type ColumnProps = ArrangableProps<typeof Column>
export const Column = defineArrangable({
    name: 'Column',
    props: { modifier: { type: Modifier, default: M }, verticalArrangement: [String, Object] as PropType<VerticalArrangementProp>, horizontalAlignment: String as PropType<HorizontalAlignment> },
    slots: { default: slotParams<{ columnHandle: ColumnHandle }>() },
    setup(props, { call, slot, source }) {
        const contents = withHandle(slot(), { columnHandle })
        return () => call(0, Layout, {
            measurePolicy: () => ColumnMeasurePolicy({ verticalArrangement: props.verticalArrangement, horizontalAlignment: props.horizontalAlignment }),
            modifier: () => props.modifier,
        }, contents, { sources: { modifier: source('modifier'), measurePolicy: source('verticalArrangement') ?? source('horizontalAlignment') } })
    },
})

export type SpacerProps = ArrangableProps<typeof Spacer>
export const Spacer = defineArrangable({
    name: 'Spacer',
    props: { modifier: { type: Modifier, default: M } },
    setup: (props, { call, source }) => () => call(0, Layout, { measurePolicy: () => MinSizeMeasurePolicy, modifier: () => props.modifier }, {}, { sources: { modifier: source('modifier') } }),
})

export type FlowRowProps = ArrangableProps<typeof FlowRow>
export const FlowRow = defineArrangable({
    name: 'FlowRow',
    props: { modifier: { type: Modifier, default: M }, horizontalArrangement: [String, Object] as PropType<HorizontalArrangementProp>, verticalArrangement: [String, Object] as PropType<VerticalArrangementProp>, itemVerticalAlignment: String as PropType<VerticalAlignment>, maxItemsInEachRow: Number },
    slots: { default: slotParams<{ flowRowHandle: FlowRowHandle }>() },
    setup(props, { call, slot, source }) {
        const contents = withHandle(slot(), { flowRowHandle })
        return () => call(0, Layout, {
            measurePolicy: () => FlowRowMeasurePolicy({ horizontalArrangement: props.horizontalArrangement, verticalArrangement: props.verticalArrangement, itemVerticalAlignment: props.itemVerticalAlignment, maxItemsInEachRow: props.maxItemsInEachRow }),
            modifier: () => props.modifier,
        }, contents, { sources: { modifier: source('modifier'), measurePolicy: source('maxItemsInEachRow') ?? source('horizontalArrangement') ?? source('verticalArrangement') } })
    },
})

export type FlowColumnProps = ArrangableProps<typeof FlowColumn>
export const FlowColumn = defineArrangable({
    name: 'FlowColumn',
    props: { modifier: { type: Modifier, default: M }, verticalArrangement: [String, Object] as PropType<VerticalArrangementProp>, horizontalArrangement: [String, Object] as PropType<HorizontalArrangementProp>, itemHorizontalAlignment: String as PropType<HorizontalAlignment>, maxItemsInEachColumn: Number },
    slots: { default: slotParams<{ flowColumnHandle: FlowColumnHandle }>() },
    setup(props, { call, slot, source }) {
        const contents = withHandle(slot(), { flowColumnHandle })
        return () => call(0, Layout, {
            measurePolicy: () => FlowColumnMeasurePolicy({ verticalArrangement: props.verticalArrangement, horizontalArrangement: props.horizontalArrangement, itemHorizontalAlignment: props.itemHorizontalAlignment, maxItemsInEachColumn: props.maxItemsInEachColumn }),
            modifier: () => props.modifier,
        }, contents, { sources: { modifier: source('modifier'), measurePolicy: source('maxItemsInEachColumn') ?? source('verticalArrangement') ?? source('horizontalArrangement') } })
    },
})
