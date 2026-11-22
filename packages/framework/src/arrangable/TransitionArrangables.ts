import { Box } from './LayoutingArrangables.ts'
import { cantonScope, defineArrangable, onMounted, shallowRef, slotParams, watch } from "../runtime/index.ts"
import type { ArrangableProps, PropType, SlotTypes, WithSlotTypes } from "../runtime/index.ts"
import { animatedNumberAsRef, tween } from "../animation/value.ts"
import type { AnimationSpec } from "../animation/value.ts"
import { M } from "../modifier/modifier.ts"

export type VisibilityTransform = Readonly<{ alpha?: number; translationX?: number; translationY?: number; scaleX?: number; scaleY?: number }>
const clampAlpha = (value: number) => Math.max(0, Math.min(1, value))

export type AnimatedVisibilityProps = ArrangableProps<typeof AnimatedVisibility>
export const AnimatedVisibility = defineArrangable({
    name: "AnimatedVisibility",
    slots: { default: slotParams() },
    props: {
        visible: { type: Boolean, required: true },
        appear: Boolean,
        animationSpec: { type: Object as PropType<AnimationSpec>, default: () => tween() },
        enterFrom: { type: Object as PropType<VisibilityTransform>, default: () => ({ alpha: 0 }) },
        exitTo: { type: Object as PropType<VisibilityTransform>, default: () => ({ alpha: 0 }) },
    },
    setup(props, { call, slot }) {
        const content = slot()
        // 内部 Box 的 Handle 不交给外部内容
        const contents = { default: () => content() }
        const retained = shallowRef(props.visible)
        const target = shallowRef(props.visible && !props.appear ? 1 : 0)
        const progress = animatedNumberAsRef(target, { animationSpec: props.animationSpec })
        onMounted(() => { target.value = props.visible ? 1 : 0 })
        watch(() => props.visible, visible => {
            if (visible) retained.value = true
            target.value = visible ? 1 : 0
        }, { flush: "sync" })
        watch(() => [props.visible, progress.isRunning.value, progress.value], () => {
            if (!props.visible && !progress.isRunning.value && progress.value === 0) retained.value = false
        }, { flush: "post" })
        return () => {
            if (retained.value) call(0, Box, {
                // 退出开始时禁用交互，焦点和命中结果随原生发布一起更新
                enabled: () => props.visible,
                modifier: () => {
                    const edge = props.visible ? props.enterFrom : props.exitTo
                    const remaining = 1 - progress.value
                    return M.graphicsLayer({
                        alpha: clampAlpha(1 - (1 - (edge.alpha ?? 0)) * remaining),
                        translationX: (edge.translationX ?? 0) * remaining,
                        translationY: (edge.translationY ?? 0) * remaining,
                        scaleX: 1 - (1 - (edge.scaleX ?? 1)) * remaining,
                        scaleY: 1 - (1 - (edge.scaleY ?? 1)) * remaining,
                    })
                },
            }, contents)
        }
    },
})

type CrossfadeEntry = Readonly<{ id: number; value: unknown; initial: boolean }>
const CrossfadeLayer = defineArrangable({
    props: {
        entry: { type: Object as PropType<CrossfadeEntry>, required: true },
        active: Boolean,
        animationSpec: { type: Object as PropType<AnimationSpec>, required: true },
        retire: { type: Function as PropType<(entry: CrossfadeEntry) => void>, required: true },
    },
    slots: { default: slotParams<{ state: any }>() },
    setup(props, { call, slot }) {
        const content = slot()
        // 每层交付它建立时的状态值，退出中的层不随目标状态改变
        const contents = { default: () => content({ state: props.entry.value }) }
        const target = shallowRef(props.entry.initial ? 1 : 0)
        const alpha = animatedNumberAsRef(target, { animationSpec: props.animationSpec })
        onMounted(() => { target.value = props.active ? 1 : 0 })
        watch(() => props.active, active => { target.value = active ? 1 : 0 }, { flush: "sync" })
        watch(() => [props.active, alpha.isRunning.value, alpha.value], () => {
            if (!props.active && !alpha.isRunning.value && alpha.value === 0) props.retire(props.entry)
        }, { flush: "post" })
        return () => call(0, Box, {
            enabled: () => props.active,
            modifier: () => M.alpha(clampAlpha(alpha.value)),
        }, contents)
    },
})

export type CrossfadeProps = ArrangableProps<typeof Crossfade>
const crossfade = defineArrangable({
    name: "Crossfade",
    props: {
        targetState: { required: true },
        animationSpec: { type: Object as PropType<AnimationSpec>, default: () => tween() },
    },
    slots: { default: slotParams<{ state: any }>() },
    setup(props, { call, slot }) {
        const layerContents = { default: slot() }
        let nextId = 1

        const entry = (initial: boolean): CrossfadeEntry => ({ id: nextId++, value: props.targetState, initial })
        const entries = shallowRef<CrossfadeEntry[]>([entry(true)])
        const retire = (retired: CrossfadeEntry) => { if (!Object.is(retired.value, props.targetState)) entries.value = entries.value.filter(item => item.id !== retired.id) }

        // 目标状态没有对应的层时追加一层；切回仍在退出的状态时沿用原来那层
        watch(() => props.targetState, state => {
            if (!entries.value.some(item => Object.is(item.value, state))) entries.value = [...entries.value, entry(false)]
        })

        return () => call(0, Box, {}, {
            default: () => {
                for (const item of entries.value) cantonScope(0, () => call(0, CrossfadeLayer, {
                    entry: () => item,
                    active: () => Object.is(item.value, props.targetState),
                    animationSpec: () => props.animationSpec,
                    retire: () => retire,
                }, layerContents), item.id)
            }
        })
    },
})
// 默认槽的 state 类型取自 targetState
type CrossfadeSlotArgs<P> = { default: { state: P extends { readonly targetState: () => infer T } ? T : unknown } }
interface CrossfadeSlots extends SlotTypes { readonly slots: CrossfadeSlotArgs<this['parameters']> }
export const Crossfade = crossfade as WithSlotTypes<typeof crossfade, CrossfadeSlots>
