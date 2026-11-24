import test from 'node:test'
import assert from 'node:assert/strict'
import ts from 'typescript'
import { HotRuntime, type HotMessage } from '../../packages/framework/src/app/hotRuntime.ts'
import { compileCantonCta } from '../../packages/vite-plugin/src/ctaTransform.ts'
import { applyArrangableHmr } from '../../packages/framework/src/runtime/arrangableHmr.ts'
import { appBuilder, type ArrangableDefinition } from '../../packages/framework/src/index.ts'
import { App, Window } from '../../packages/framework/src/foundation.ts'
import { defineArrangable } from '../../packages/framework/src/internal.ts'
import { requireCtaModule } from './ctaModules.ts'
import { recordingNative, mountFrame, advanceFrames } from './recordingNative.ts'
import { createFakeNative } from '../helpers/fakeNative.ts'

test('同批 HMR 保存旧 accept 边界，dispose/data 在新模块执行前交接', async () => {
    const events: string[] = []
    const sent: unknown[] = []
    const runtime: HotRuntime = new HotRuntime({ send: (...args) => sent.push(args), reload: () => events.push('reload'), report: error => { throw error } }, async url => {
        events.push(url)
        const hot = runtime.context('/counter.ts')
        assert.equal(hot.data.count, 42)
        hot.accept(() => events.push('new accept'))
        return { count: hot.data.count }
    })
    const old = runtime.context('/counter.ts')
    old.dispose(data => {
        data.count = 42
        events.push('dispose')
    })
    old.accept(module => events.push('old accept ' + module?.count))
    await runtime.receive({ type: 'update', updates: [{ type: 'js-update', path: '/counter.ts', acceptedPath: '/counter.ts', timestamp: 123 }] })
    assert.deepEqual(events, ['dispose', '/counter.ts?t=123', 'old accept 42'])
    old.invalidate('contract changed')
    assert.deepEqual(sent, [['vite:invalidate', { path: '/counter.ts', message: 'contract changed', firstInvalidatedBy: '/counter.ts' }]])
})

test('依赖 accept、custom off、prune 与 full reload 遵循独立生命周期', async () => {
    const events: unknown[] = []
    const runtime = new HotRuntime({ send() { }, reload: () => events.push('reload'), report: error => { throw error } }, async () => ({ value: 2 }))
    const parent = runtime.context('/parent')
    parent.accept(['/dep', '/other'], modules => events.push(modules))
    const dep = runtime.context('/dep')
    dep.dispose(data => { data.disposed = true })
    dep.prune(data => { events.push(data.disposed) })
    const listener = () => { events.push('custom') }
    parent.on('test', listener)
    await runtime.receive({ type: 'custom', event: 'test', data: {} })
    parent.off('test', listener)
    await runtime.receive({ type: 'custom', event: 'test', data: {} })
    await runtime.receive({ type: 'update', updates: [{ type: 'js-update', path: '/parent', acceptedPath: '/dep', timestamp: 1 }] })
    await runtime.receive({ type: 'prune', paths: ['/dep'] })
    await runtime.receive({ type: 'full-reload' })
    assert.deepEqual(events, ['custom', [{ value: 2 }, undefined], true, 'reload'])
})

test('模块执行失败保留旧 accept 边界，下一次修复能够接收更新', async () => {
    let broken = true
    const values: unknown[] = []
    const errors: unknown[] = []
    const runtime: HotRuntime = new HotRuntime({ send() { }, reload() { }, report: error => { errors.push(error) } }, async () => {
        runtime.context('/state')
        if (broken) throw new Error('坏模块')
        return { value: 3 }
    })
    runtime.context('/state').accept(next => { values.push(next?.value) })
    const message: HotMessage = { type: 'update', updates: [{ type: 'js-update', path: '/state', acceptedPath: '/state', timestamp: 1 }] }
    await runtime.receive(message)
    broken = false
    await runtime.receive(message)
    assert.deepEqual(values, [undefined, 3])
    assert.equal(errors.length, 1)
})

test('HotRuntime 通过 Canton Log 记录更新接收与应用', async () => {
    const previous = globalThis.__CANTON_NATIVE__
    const events: unknown[][] = []
    globalThis.__CANTON_NATIVE__ = createFakeNative({ log: (...args: unknown[]) => { events.push(args) } })
    try {
        const runtime = new HotRuntime({
            send() { }, reload() { }, report: error => { throw error },
        }, async () => ({ value: 2 }))
        runtime.context('/logged').accept(module => assert.equal(module?.value, 2))
        await runtime.receive({ type: 'update', updates: [{ type: 'js-update', path: '/logged', acceptedPath: '/logged', timestamp: 1 }] })
    } finally {
        globalThis.__CANTON_NATIVE__ = previous
    }
    assert.deepEqual(events.map(event => [event[0], event[1], event[2]]), [
        ['i', 'HotRuntime', ['收到热更新', '1', '个模块', '/logged']],
        ['i', 'HotRuntime', ['热更新已应用', '1', '个模块', '/logged']],
    ])
})

function evaluate(source: string, id: string, state: object = {}): ArrangableDefinition {
    Object.defineProperty(state, '__esModule', { value: true, configurable: true })
    const { code } = compileCantonCta(source, id, true)
    const output = ts.transpileModule(code.replaceAll('import.meta.hot', 'undefined'), { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } }).outputText
    const exports: { default?: ArrangableDefinition } = {}
    new Function('require', 'exports', output)((name: string) => name === './state' ? state : requireCtaModule(name), exports)
    return exports.default!
}

test('子组件兼容热更新后，新挂载的父级按新导入定义预备的参数计划仍可调用沿用旧身份的子组件', () => {
    const childId = 'hmr-plan-child.cta'
    const child = (text: string) => `<script>const { label } = defineProps<{ label: string }>()</script><template><Text :text="label + '${text}'" /></template>`
    evaluate(child('旧'), childId)
    const next = evaluate(child('新'), childId)
    applyArrangableHmr(childId, next)
    const { code } = compileCantonCta(`<script>import Child from './Child.cta'</script><template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Child label="甲" /></Window></App></template>`, 'hmr-plan-parent.cta', true)
    assert.match(code, /prepareParameters/)
    const output = ts.transpileModule(code.replaceAll('import.meta.hot', 'undefined'), { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } }).outputText
    const exports: { default?: ArrangableDefinition } = {}
    const childModule = Object.defineProperty({ default: next }, '__esModule', { value: true })
    new Function('require', 'exports', output)((name: string) => name === './Child.cta' ? childModule : requireCtaModule(name), exports)
    const native = recordingNative()
    const app = mountFrame(appBuilder(exports.default!).build(), native.target)
    assert.deepEqual(native.textNodes().map(node => node.text), ['甲新'])
    app.unmount()
})

test('CTA 不增加对 console API 的额外编译期拦截', () => {
    assert.doesNotThrow(() => compileCantonCta('<script>console.log("未定义 API 会在运行时自然失败")</script>', 'console.cta'))
})

test('CTA 模板更新保留 <Window> 身份、WindowState、ref、setup 生命周期与 native 节点', () => {
    const id = 'hmr-template.cta'
    let count: any
    let setups = 0
    const state = {
        capture: (value: any) => {
            count = value
            setups++
        }
    }
    const script = `<script>import {ref, createWindowState} from '@canton/framework'\nimport {capture} from './state'\nconst winState = createWindowState({ size: { width: 800.dp, height: 500.dp } })\nconst count = ref(1)\nconst extra = ref('!')\ncapture(count)</script>`
    const root = evaluate(script + '<template><App &appHandle><Window :state="winState" :onCloseRequest="appHandle.exitApplication"><Text :text="String(count)" /></Window></App></template>', id, state)
    assert.equal(root.__hmrId, id)
    const native = recordingNative()
    const app = mountFrame(appBuilder(root).build(), native.target)
    const initialWindowId = [...native.windows.keys()][0]
    count.value = 9
    advanceFrames()
    const initial = native.textNodes()[0]
    const next = evaluate(script + '<template><App &appHandle><Window :state="winState" title="HMR 标题" :onCloseRequest="appHandle.exitApplication"><Text :text="String(count) + extra" /></Window></App></template>', id, state)
    applyArrangableHmr(id, next)
    advanceFrames()
    assert.deepEqual([...native.windows.keys()], [initialWindowId], '热更新不能销毁重建已有窗口')
    assert.equal(native.windows.get(initialWindowId)!.config.title, 'HMR 标题')
    assert.deepEqual(native.textNodes(), [{ id: initial.id, text: '9!' }])
    assert.equal(setups, 1)
    app.unmount()
})


test('CTA 结构热更新在打开的子 <Window> 前插入同级 <Window> 时平滑交接 WindowState', () => {
    const id = 'hmr-structural-window.cta'
    const script = `<script>import {ref, createWindowState} from '@canton/framework'\nconst mainState = createWindowState({ size: { width: 800.dp, height: 500.dp } })\nconst childState = createWindowState({ size: { width: 300.dp, height: 200.dp } })\nconst showExtra = ref(false)\nconst showChild = ref(true)</script>`
    const initialRoot = evaluate(script + '<template><App &appHandle><Window key="main" :state="mainState" :onCloseRequest="appHandle.exitApplication"><Window c-if="showChild" key="child" title="子窗" :state="childState" :onCloseRequest="appHandle.exitApplication"><Text text="旧子窗" /></Window></Window></App></template>', id)
    const native = recordingNative()
    const app = mountFrame(appBuilder(initialRoot).build(), native.target)
    assert.equal(native.windows.size, 2)
    const [mainId, oldChildId] = [...native.windows.keys()]

    // 在已有打开的子 <Window> 前面插入同级 <Window>，改变其编译期调用位置
    const updatedRoot = evaluate(script + '<template><App &appHandle><Window key="main" :state="mainState" :onCloseRequest="appHandle.exitApplication"><Window c-if="showExtra" key="extra" title="新插入" :onCloseRequest="appHandle.exitApplication" /><Window c-if="showChild" key="child" title="子窗" :state="childState" :onCloseRequest="appHandle.exitApplication"><Text text="新子窗" /></Window></Window></App></template>', id)
    applyArrangableHmr(id, updatedRoot)
    advanceFrames()
    assert.equal(native.windows.size, 2)
    assert.ok(native.windows.has(mainId), '主窗身份应保持不变')
    assert.ok(!native.windows.has(oldChildId), '旧调用位置的子窗应已销毁')
    assert.deepEqual(native.textNodes().map(node => node.text), ['新子窗'])
    app.unmount()
})

test('CTA 模板更新会刷新静态参数且复用 native 节点', () => {
    const id = 'hmr-static-prop.cta'
    const Page = evaluate('<template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Text text="旧文案" /></Window></App></template>', id)
    const native = recordingNative()
    const app = mountFrame(appBuilder(Page).build(), native.target)
    const initial = native.textNodes()[0]
    applyArrangableHmr(id, evaluate('<template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Text text="新文案" /></Window></App></template>', id))
    advanceFrames()
    assert.deepEqual(native.textNodes(), [{ id: initial.id, text: '新文案' }])
    app.unmount()
})

test('CTA 接受依赖模块更新时重建 setup，模板更新仍复用原 setup', () => {
    const id = 'hmr-dependency.cta'
    const source = `<script>import { read } from './state'\nconst text = read()</script><template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Text :text="text" /></Window></App></template>`
    const native = recordingNative()
    const app = mountFrame(appBuilder(evaluate(source, id, { read: () => '旧依赖' })).build(), native.target)
    const node = native.textNodes()[0].id
    applyArrangableHmr(id, evaluate(source, id, { read: () => '新依赖' }))
    advanceFrames()
    assert.deepEqual(native.textNodes(), [{ id: node, text: '新依赖' }])
    app.unmount()
})

test('CTA 脚本更新迁移显式 ref，重建 computed 与 setup 清理，不兼容 props 则重置状态', () => {
    const id = 'hmr-script.cta'
    let count: any
    let disposed = 0
    const state = { capture: (value: any) => { count = value }, dispose: () => { disposed++ } }
    const source = (factor: number, props = '') => `<script>import {ref, computed, onScopeDispose} from '@canton/framework'\nimport {capture, dispose} from './state'\n${props}\nconst count = ref(1)\nconst display = computed(() => count.value * ${factor})\ncapture(count)\nonScopeDispose(dispose)</script><template><Text :text="String(display)" /></template>`
    const Leaf = evaluate(source(2), id, state)
    const Shell = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Leaf, {}),
            }),
        }),
    })
    const native = recordingNative()
    const app = mountFrame(appBuilder(Shell).build(), native.target)
    count.value = 9
    advanceFrames()
    const previous = count
    const previousNode = native.textNodes()[0].id
    applyArrangableHmr(id, evaluate(source(3), id, state))
    advanceFrames()
    assert.equal(native.textNodes()[0].text, '27')
    assert.equal(count, previous)
    assert.equal(native.textNodes()[0].id, previousNode)
    assert.equal(disposed, 1)
    applyArrangableHmr(id, evaluate(source(4, "defineProps<{ title?: string }>()"), id, state))
    advanceFrames()
    assert.equal(native.textNodes()[0].text, '4')
    assert.notEqual(count, previous)
    app.unmount()
    assert.equal(disposed, 3)
})

for (const failure of ['setup', 'native'] as const) test(`CTA ${failure} 失败回滚候选状态表，修复后重新初始化新增状态`, () => {
    const id = `hmr-rollback-${failure}.cta`
    let count: any
    const errors: unknown[] = []
    const state = { capture: (value: any) => { count = value } }
    const source = (bonus?: number, broken = false) => `<script>import {ref} from '@canton/framework'\nimport {capture} from './state'\nconst count = ref(1)\ncapture(count)\n${bonus === undefined ? '' : `const bonus = ref(${bonus})`}\n${broken ? "throw new Error('候选 setup 失败')" : ''}</script><template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Text :text="String(count${bonus === undefined ? '' : ' + bonus'})" /></Window></App></template>`
    const builder = appBuilder(evaluate(source(), id, state))
    builder.config.errorHandler = error => { errors.push(error) }
    const native = recordingNative(false)
    const app = mountFrame(builder.build(), native.target)
    native.finish()
    const previous = count
    const initial = native.textNodes()[0]
    applyArrangableHmr(id, evaluate(source(10, failure === 'setup'), id, state))
    advanceFrames()
    if (failure === 'native') native.finish('候选 native 失败')
    assert.equal(errors.length, 1)
    assert.deepEqual(native.textNodes(), [initial])
    applyArrangableHmr(id, evaluate(source(20), id, state))
    advanceFrames()
    native.finish()
    assert.deepEqual(native.textNodes(), [{ id: initial.id, text: '21' }])
    assert.equal(count, previous)
    app.unmount()
})

test('CTA 模板热更新增删槽位参数绑定与出口实参，保留 ref、setup 与 native 节点，槽位局部名不被当作 script 变量', () => {
    const providerId = 'hmr-slot-provider.cta', consumerId = 'hmr-slot-consumer.cta'
    let count: any
    let setups = 0
    const providerState = {
        capture: (value: any) => {
            count = value
            setups++
        }
    }
    const provider = (outlet: string) => `<script>import {ref} from '@canton/framework'\nimport {capture} from './state'\nconst count = ref(1)\ncapture(count)\ndefineSlots<{ default: { value: number; unit?: string } }>()</script><template>${outlet}</template>`
    const consumer = (content: string) => `<script>import {Provider} from './state'\nconst value = '外层'</script><template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Text :text="value" />${content}</Window></App></template>`
    const Provider = evaluate(provider('<Slot :value="count" />'), providerId, providerState)
    const native = recordingNative()
    const app = mountFrame(appBuilder(evaluate(consumer('<Provider &value><Text :text="String(value)" /></Provider>'), consumerId, { Provider })).build(), native.target)
    count.value = 5
    advanceFrames()
    const initial = native.textNodes()
    assert.deepEqual(initial.map(node => node.text), ['外层', '5'])

    applyArrangableHmr(consumerId, evaluate(consumer('<Provider &value="current" &unit><Text :text="value + current + (unit ?? \'\')" /></Provider>'), consumerId, { Provider }))
    advanceFrames()
    assert.deepEqual(native.textNodes(), [initial[0], { id: initial[1].id, text: '外层5' }])

    applyArrangableHmr(providerId, evaluate(provider('<Slot :value="count" unit="个" />'), providerId, providerState))
    advanceFrames()
    assert.deepEqual(native.textNodes(), [initial[0], { id: initial[1].id, text: '外层5个' }])
    assert.equal(setups, 1)
    app.unmount()
})

