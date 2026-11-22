import { AlignmentLine } from '../values/primitives.ts'
import type { AlignmentLineValue, BoxAlignment, HorizontalAlignment, VerticalAlignment } from '../values/primitives.ts'
import { Modifier } from './modifier.ts'

// 布局经默认槽交付 Handle，由它构造只对该类父布局有意义的 Modifier 片段；调用方用 then() 把片段插入链中，实际父级由原生布局校验
export type WeightOptions = Readonly<{ fill?: boolean }>

export interface BoxHandle {
    align(alignment: BoxAlignment): Modifier
    matchParentSize(): Modifier
}

export interface RowHandle {
    weight(weight: number, options?: WeightOptions): Modifier
    align(alignment: VerticalAlignment): Modifier
    // 与同一行内其它按线对齐的兄弟项对齐到同一条线
    alignBy(line: AlignmentLineValue): Modifier
    alignByBaseline(): Modifier
}

export interface ColumnHandle {
    weight(weight: number, options?: WeightOptions): Modifier
    align(alignment: HorizontalAlignment): Modifier
}

export interface FlowRowHandle extends RowHandle {
    // 占所在行最高一项高度的比例
    fillMaxRowHeight(fraction?: number): Modifier
}

export interface FlowColumnHandle extends ColumnHandle {
    // 占所在列最宽一项宽度的比例
    fillMaxColumnWidth(fraction?: number): Modifier
}

export interface LazyItemHandle {
    // 占所在 Lazy 容器视口（扣除 contentPadding）的比例
    fillParentMaxSize(fraction?: number): Modifier
    fillParentMaxWidth(fraction?: number): Modifier
    fillParentMaxHeight(fraction?: number): Modifier
}

function element(type: string, value: Record<string, unknown> = {}): Modifier {
    return new Modifier([Object.freeze({ type, value: Object.freeze(value) })])
}

function weight(weight: number, options: WeightOptions = {}): Modifier {
    if (!(weight > 0)) throw new RangeError('weight 的权重必须大于零')
    return element('weight', { weight, fill: options.fill ?? true })
}

function align(alignment: BoxAlignment | HorizontalAlignment | VerticalAlignment): Modifier {
    return element('align', { alignment })
}

function alignBy(line: AlignmentLineValue): Modifier {
    return element('alignBy', { line })
}

function fill(type: string): (fraction?: number) => Modifier {
    return (fraction = 1) => {
        if (!(fraction >= 0 && fraction <= 1)) throw new RangeError('填充比例必须在 0..1 之间')
        return element(type, { fraction })
    }
}

const alignByBaseline = () => alignBy(AlignmentLine.FirstBaseline)
const fillCrossAxis = fill('fillCrossAxis')

export const boxHandle: BoxHandle = Object.freeze({ align, matchParentSize: () => element('matchParentSize') })
export const rowHandle: RowHandle = Object.freeze({ weight, align, alignBy, alignByBaseline })
export const columnHandle: ColumnHandle = Object.freeze({ weight, align })
export const flowRowHandle: FlowRowHandle = Object.freeze({ ...rowHandle, fillMaxRowHeight: fillCrossAxis })
export const flowColumnHandle: FlowColumnHandle = Object.freeze({ ...columnHandle, fillMaxColumnWidth: fillCrossAxis })
export const lazyItemHandle: LazyItemHandle = Object.freeze({ fillParentMaxSize: fill('fillParentMaxSize'), fillParentMaxWidth: fill('fillParentMaxWidth'), fillParentMaxHeight: fill('fillParentMaxHeight') })
