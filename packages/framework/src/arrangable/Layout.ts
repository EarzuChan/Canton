import { currentInstance, ValueBinding, invokeContent } from '../runtime/internal.ts'
import { defineArrangable, slotParams } from '../runtime/index.ts'
import type { ArrangableProps, PropType } from '../runtime/index.ts'
import { LayoutRearrangeNode, SurfaceDomainKey } from '../bridge/nativeRearrange.ts'
import { isMeasurePolicy } from '../layout/measurePolicy.ts'
import type { MeasurePolicy } from '../layout/measurePolicy.ts'
import { M, Modifier } from '../modifier/modifier.ts'
import { inject } from '../runtime/apiInject.ts'
import { DensityKey } from '../layout/density.ts'
import { ModifierLowering } from '../bridge/modifierLowering.ts'
import type { LayoutContentProvider } from '../layout/layoutContent.ts'

export type LayoutProps = ArrangableProps<typeof Layout>
export const Layout = defineArrangable({
    name: 'Layout',
    props: { modifier: { type: Modifier, default: M }, measurePolicy: { type: Object as PropType<MeasurePolicy>, required: true, validator: isMeasurePolicy }, contentProvider: Object as PropType<LayoutContentProvider>, enabled: Boolean },
    slots: { default: slotParams() },
    setup(props, { slot, source }) {
        const instance = currentInstance!

        const domain = inject(SurfaceDomainKey, undefined)
        if (!domain) throw new Error('Layout 只能挂载在 <Window> 建立的画区作用域内')

        const density = inject(DensityKey)
        if (!density) throw new Error('Layout 缺少所属窗口的 Density 服务')

        const units = new ModifierLowering(density, instance, domain.windowId)

        const node = new LayoutRearrangeNode(instance, domain.host, domain.windowId)
        instance.node = node
        if (props.contentProvider) node.setContentProvider(props.contentProvider)

        new ValueBinding(() => units.measurePolicy(props.measurePolicy), instance, value => node.updateMeasurePolicy(value as MeasurePolicy), source('measurePolicy'))
        new ValueBinding(() => units.modifier(props.modifier), instance, value => node.updateModifier(value as import('../bridge/modifierLowering.ts').PxModifier), source('modifier'))
        new ValueBinding(() => props.enabled, instance, value => node.updateInput('enabled', value as boolean | undefined), source('enabled'))

        const content = slot()
        return () => props.contentProvider ? props.contentProvider.render() : invokeContent(0, content)
    },
})
