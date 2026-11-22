import { nativeAnimationSpec, spring, type AnimationSpec } from '../animation/value.ts'
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

// 某种动画给 null 表示不做；缺省对齐 Compose 的 LazyItemScope.animateItem
export type AnimateItemOptions = Readonly<{ fadeInSpec?: AnimationSpec | null; placementSpec?: AnimationSpec | null; fadeOutSpec?: AnimationSpec | null }>

export interface LazyGridItemHandle {
    // 数据集里新出现的项淡入、位置变化的项滑到新位置、移除的项原地淡出；写在项内容的根上，整项一起动
    animateItem(options?: AnimateItemOptions): Modifier
}

export interface LazyItemHandle extends LazyGridItemHandle {
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

function animateItem(options: AnimateItemOptions = {}): Modifier {
    const spec = (value: AnimationSpec | null | undefined, fallback: AnimationSpec) => value === null ? null : nativeAnimationSpec(value ?? fallback)
    return element('animateItem', {
        fadeInSpec: spec(options.fadeInSpec, defaultFadeSpec),
        placementSpec: spec(options.placementSpec, defaultPlacementSpec),
        fadeOutSpec: spec(options.fadeOutSpec, defaultFadeSpec),
    })
}

const defaultFadeSpec = spring({ stiffness: 400 })
// 位移按像素收敛，停稳阈值取 1 像素
const defaultPlacementSpec = spring({ stiffness: 400, visibilityThreshold: 1 })
const alignByBaseline = () => alignBy(AlignmentLine.FirstBaseline)
const fillCrossAxis = fill('fillCrossAxis')

export const boxHandle: BoxHandle = Object.freeze({ align, matchParentSize: () => element('matchParentSize') })
export const rowHandle: RowHandle = Object.freeze({ weight, align, alignBy, alignByBaseline })
export const columnHandle: ColumnHandle = Object.freeze({ weight, align })
export const flowRowHandle: FlowRowHandle = Object.freeze({ ...rowHandle, fillMaxRowHeight: fillCrossAxis })
export const flowColumnHandle: FlowColumnHandle = Object.freeze({ ...columnHandle, fillMaxColumnWidth: fillCrossAxis })
export const lazyGridItemHandle: LazyGridItemHandle = Object.freeze({ animateItem })
export const lazyItemHandle: LazyItemHandle = Object.freeze({ animateItem, fillParentMaxSize: fill('fillParentMaxSize'), fillParentMaxWidth: fill('fillParentMaxWidth'), fillParentMaxHeight: fill('fillParentMaxHeight') })
