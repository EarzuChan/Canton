import test from 'node:test'
import assert from 'node:assert/strict'
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import * as runtime from '../../packages/framework/src/index.ts'
import * as internal from '../../packages/framework/src/internal.ts'
import type { Modifier } from '../../packages/framework/src/modifier/modifier.ts'
import { checkCtaProject } from '../../packages/compiler/src/typecheck.ts'
import { compileCantonCta } from '../../packages/vite-plugin/src/ctaTransform.ts'
import { evaluateCta } from './ctaModules.ts'
import { recordingNative, mountFrame, advanceFrames } from './recordingNative.ts'

const container = `<template>
    <Slot name="header" :title="title" :actions="actions" />
    <Slot :app-handle="appHandle" />
    <Slot name="footer" />
</template>
<script>
import { title, actions, appHandle } from './state'
defineSlots<{ default: { appHandle: { name: string } }; header: { title: string; actions: { label: string } } }>()
</script>`

test('零参、单参与多参槽位按值交付，同名、别名与解构都只在对应内容内可见', () => {
    const title = runtime.ref('标题')
    const Container = evaluateCta(container, { './state': { title, actions: { label: '刷新' }, appHandle: { name: '应用' } } })
    const Page = evaluateCta(`<template>
    <App &app-handle>
        <Window :onCloseRequest="appHandle.exitApplication">
            <Container &app-handle="outerApp">
                <Template #header &title &actions="{ label }"><Text :text="title + label" /></Template>
                <Text :text="outerApp.name + title" />
                <Template #footer><Text text="页脚" /></Template>
            </Container>
        </Window>
    </App>
</template>
<script>
import { Container } from './state'
const title = '外层'
</script>`, { './state': { Container } })
    const native = recordingNative(), running = mountFrame(runtime.appBuilder(Page).build(), native.target)
    const initial = native.textNodes()
    // 默认槽没有绑定 title，同名标识符取自 script
    assert.deepEqual(initial.map(node => node.text), ['标题刷新', '应用外层', '页脚'])

    const created = internal.getCantonExecutionStats().instancesCreated
    title.value = '新标题'
    advanceFrames()
    assert.deepEqual(native.textNodes(), [{ id: initial[0].id, text: '新标题刷新' }, initial[1], initial[2]])
    assert.equal(internal.getCantonExecutionStats().instancesCreated, created)
    running.unmount()
})

test('嵌套同一容器时外层起别名、内层用原名可同时访问；同一槽位的多个出口各有实参与内容作用域', () => {
    const first = runtime.ref(1), second = runtime.ref(2)
    const Pair = evaluateCta(`<template><Slot :value="first" /><Slot :value="second" /></template>
<script>
import { first, second } from './state'
defineSlots<{ default: { value: number } }>()
</script>`, { './state': { first, second } })
    const runs: number[] = []
    const Page = evaluateCta(`<template>
    <App &app-handle>
        <Window :onCloseRequest="appHandle.exitApplication">
            <Pair &value="outer">
                <Pair c-if="outer === 1" &value><Text :text="mark(outer * 10 + value)" /></Pair>
                <Text c-else :text="mark(outer)" />
            </Pair>
        </Window>
    </App>
</template>
<script>
import { Pair, mark } from './state'
</script>`, { './state': { Pair, mark: (value: number) => (runs.push(value), String(value)) } })
    const native = recordingNative(), running = mountFrame(runtime.appBuilder(Page).build(), native.target)
    const initial = native.textNodes()
    assert.deepEqual(initial.map(node => node.text), ['11', '12', '2'])

    runs.length = 0
    second.value = 3
    advanceFrames()
    // first 出口的外层内容没有读到 second，不重跑；其余文本节点保留身份
    assert.deepEqual(native.textNodes(), [{ id: initial[0].id, text: '11' }, { id: initial[1].id, text: '13' }, { id: initial[2].id, text: '3' }])
    assert.deepEqual(runs.sort(), [13, 3])
    running.unmount()
})

test('槽位参数绑定的位置与命名错误在编译期报出', () => {
    const compile = (template: string) => () => compileCantonCta(`<template>${template}</template>`, '槽位参数.cta')
    assert.throws(compile('<Slot &value />'), /槽位参数只能绑定在 Arrangable 标签或 <Template #name> 上/)
    assert.throws(compile('<Template &value><Text text="x" /></Template>'), /槽位参数只能绑定在 Arrangable 标签或 <Template #name> 上/)
    assert.throws(compile('<Box &a.b><Text text="x" /></Box>'), /槽位参数名不是合法标识符/)
    assert.throws(compile('<Box &box-handle &boxHandle><Text text="x" /></Box>'), /重复绑定槽位参数/)
    assert.throws(compile('<Box &a="same" &b="{ same }"><Text text="x" /></Box>'), /局部名重复：same/)
    assert.throws(compile('<Box &a="{ broken"><Text text="x" /></Box>'), /TS 表达式解析失败/)
    assert.throws(compile('<Slot c-bind="args" />'), /Slot 出口只接受静态 name 和具名实参/)
    assert.throws(() => compileCantonCta('<template><Slot /></template><script>const slots = defineSlots<{ default: {} }>()</script>', '槽位参数.cta'), /defineSlots.+只能写成顶层语句/)
})

test('离线类型检查按槽位签名推导绑定类型，并诊断使用方与提供方的错误', () => {
    const directory = mkdtempSync(resolve('tmp-refs/cta-slots-'))
    const check = (name: string, source: string) => {
        writeFileSync(join(directory, name), source)
        return checkCtaProject(resolve('tsconfig.json'), [join(directory, name)]).map(item => `${item.line}:${item.message.split('\n')[0]}`)
    }
    try {
        writeFileSync(join(directory, 'Container.cta'), container.replace("import { title, actions, appHandle } from './state'", "const title = '标题', actions = { label: '刷新' }, appHandle = { name: '应用' }"))
        const imports = "<script>\nimport { M, Alignment } from '@canton/framework/ui'\nimport Container from './Container.cta'\n</script>"
        assert.deepEqual(check('Good.cta', `<template>
    <Container &app-handle="{ name }">
        <Template #header &title &actions><Text :text="title.toUpperCase() + actions.label" /></Template>
        <Column &column-handle><Text :text="name" :modifier="M.then(columnHandle.align(Alignment.End)).padding(8.dp)" /></Column>
    </Container>
</template>${imports}`), [])

        const consumer = check('Bad.cta', `<template>
    <Container &missing>
        <Template #header &title><Text :text="title.toFixed()" /></Template>
        <Row &row-handle><Text text="x" :modifier="rowHandle.align(Alignment.End)" /></Row>
    </Container>
</template>${imports}`)
        assert.equal(consumer.length, 3, JSON.stringify(consumer))
        assert.match(consumer.join('\n'), /2:Property 'missing' does not exist/)
        assert.match(consumer.join('\n'), /3:Property 'toFixed' does not exist on type 'string'/)
        assert.match(consumer.join('\n'), /4:Argument of type '"End"' is not assignable/)

        const provider = check('Provider.cta', `<template>
    <Slot name="header" :title="1" :actions="() => {}" />
    <Slot name="header" title="缺少 actions" />
    <Slot :extra="2" />
</template>
<script>
defineSlots<{ header: { title: string; actions: () => void } }>()
</script>`)
        assert.equal(provider.length, 3, JSON.stringify(provider))
        assert.match(provider.join('\n'), /2:Type 'number' is not assignable to type 'string'/)
        assert.match(provider.join('\n'), /3:Argument of type '\{ title: string; \}' is not assignable/)
        assert.match(provider.join('\n'), /4:Expected 0 arguments/)
    } finally {
        rmSync(directory, { recursive: true })
    }
})

test('布局 Handle 产出的片段按 then 的位置进入 Modifier 链，经参数转交后不变', () => {
    const Item = evaluateCta(`<template><Text text="项" :modifier="M.then(placement).padding(4.dp)" /></template>
<script>
import { M, type Modifier } from '@canton/framework/ui'
defineProps<{ placement: Modifier }>()
</script>`)
    const Page = evaluateCta(`<template>
    <App &app-handle>
        <Window :onCloseRequest="appHandle.exitApplication">
            <Row &row-handle>
                <Text text="直接" :modifier="M.padding(8.dp).then(rowHandle.weight(2, { fill: false })).then(rowHandle.alignByBaseline())" />
                <Box &box-handle><Item :placement="boxHandle.align(Alignment.Center).then(boxHandle.matchParentSize())" /></Box>
            </Row>
        </Window>
    </App>
</template>
<script>
import { M, Alignment } from '@canton/framework/ui'
import { Item } from './state'
</script>`, { './state': { Item } })
    const native = recordingNative(), running = mountFrame(runtime.appBuilder(Page).build(), native.target)
    const chain = (text: string) => (native.nodes.get(native.textNodes().find(node => node.text === text)!.id)!.inputs.get('modifier') as Modifier).elements.map(element => [element.type, element.value])
    assert.deepEqual(chain('直接').slice(0, 3).map(([type]) => type), ['padding', 'weight', 'alignBy'])
    assert.deepEqual(chain('直接')[1][1], { weight: 2, fill: false })
    assert.deepEqual(chain('直接')[2][1], { line: 'FirstBaseline' })
    assert.deepEqual(chain('项').slice(0, 3), [['align', { alignment: 'Center' }], ['matchParentSize', {}], ['padding', { start: 4, top: 4, end: 4, bottom: 4 }]])
    running.unmount()
})
