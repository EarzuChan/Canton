import test from 'node:test'
import assert from 'node:assert/strict'
import { painter, M } from '../../packages/framework/src/ui.ts'
import { App, Column, Icon, Text, Window } from '../../packages/framework/src/foundation.ts'
import { appBuilder, diagnostics, DiagnosticsToast, Log, ref, createScrollState, onScopeDispose } from '../../packages/framework/src/index.ts'
import { scrollStateInternal } from '../../packages/framework/src/layout/state.ts'
import { defineArrangable } from '../../packages/framework/src/internal.ts'
import type { NativeTransactionTarget } from '../../packages/framework/src/internal.ts'
import type { Modifier } from '../../packages/framework/src/ui.ts'
import { recordingNative, mountFrame, advanceFrames } from './recordingNative.ts'

function diagnosticNative(): NativeTransactionTarget & { calls: (readonly [string, ...unknown[]])[] } {
    const calls: (readonly [string, ...unknown[]])[] = []
    return {
        ...recordingNative().target,
        calls,
        log: (level, tag, args) => calls.push(['log', level, tag, args]),
        diagnosticsToast: (level, tag, title, args, coalesce) => calls.push(['diagnosticsToast', { level, tag, title, args, coalesce }]),
        diagnosticsRequestReload: payload => calls.push(['diagnosticsRequestReload', payload]),
        diagnosticsSetToastsEnabled: enabled => calls.push(['diagnosticsSetToastsEnabled', enabled]),
    }
}

test('App 与 Window 经 Layout 结构调用提交原生父子关系、Policy 与 Modifier', () => {
    const native = recordingNative()
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Column, { modifier: () => M.padding(8, 0) }, { default: () => call(0, Text, { text: () => '正文' }) }),
            }),
        }),
    })
    const app = mountFrame(appBuilder(Root).build(), native.target)
    const root = native.windowRoot(0)
    assert.equal(root.children.length, 1)
    const columnId = root.children[0]
    const textId = native.nodes.get(columnId)!.children[0]
    assert.deepEqual(native.textNodes(), [{ id: textId, text: '正文' }])
    assert.equal((native.nodes.get(columnId)!.inputs.get('modifier') as Modifier).elements[0].type, 'padding')
    assert.ok(native.nodes.get(columnId)!.inputs.get('measurePolicy'))
    app.unmount()
})

test('反应式文字通过文本 Modifier 更新既有受体', async () => {
    const text = ref('原文')
    const native = recordingNative()
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Text, { text: () => text.value }),
            }),
        }),
    })
    const app = mountFrame(appBuilder(Root).build(), native.target)
    const id = native.textNodes()[0].id
    text.value = '新文'
    advanceFrames()
    assert.deepEqual(native.textNodes(), [{ id, text: '新文' }])
    app.unmount()
})

test('同批多受体值变化只产生一次候选，等待 apply 期间的新值整体进入下一次提交', async () => {
    const first = ref('甲')
    const second = ref('乙')
    const native = recordingNative(false)
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => {
                    call(0, Text, { text: () => first.value })
                    call(1, Text, { text: () => second.value })
                },
            }),
        }),
    })
    const app = mountFrame(appBuilder(Root).build(), native.target)
    native.finish()
    const initialSubmissions = native.submissions

    first.value = '丙'
    second.value = '丁'
    advanceFrames()
    assert.equal(native.submissions, initialSubmissions + 1)
    assert.deepEqual(native.textNodes().map(node => node.text), ['甲', '乙'])

    first.value = '戊'
    second.value = '己'
    advanceFrames()
    assert.equal(native.submissions, initialSubmissions + 1)
    native.finish()
    assert.deepEqual(native.textNodes().map(node => node.text), ['丙', '丁'])
    advanceFrames()
    assert.equal(native.submissions, initialSubmissions + 2)
    native.finish()
    advanceFrames()
    assert.deepEqual(native.textNodes().map(node => node.text), ['戊', '己'])
    assert.equal(native.submissions, initialSubmissions + 2)
    app.unmount()
})

test('父范围退出时取消同批旧子参数求值，候选内替换的 Layout 不留下原生孤儿', async () => {
    const shown = ref(true)
    const invalid = ref(false)
    const native = recordingNative()
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => {
                    if (shown.value) call(0, Text, {
                        text: () => {
                            if (invalid.value) throw new Error('已退出的参数不应求值')
                            return '旧内容'
                        }
                    })
                },
            }),
        }),
    })
    const app = mountFrame(appBuilder(Root).build(), native.target)
    invalid.value = true
    shown.value = false
    advanceFrames()
    assert.equal(native.nodes.size, 1)
    app.unmount()

    const phase = ref(0)
    const Trigger = defineArrangable({ setup: () => () => { if (phase.value === 0) phase.value = 1 } })
    const ReplacementRoot = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => {
                    if (phase.value === 0) call(0, Text, { text: () => '未提交内容' })
                    else call(1, Text, { text: () => '最终内容' })
                    call(2, Trigger, {})
                },
            }),
        }),
    })
    const replacement = mountFrame(appBuilder(ReplacementRoot).build(), native.target)
    assert.equal(native.nodes.size, 2)
    assert.deepEqual(native.textNodes().map(node => node.text), ['最终内容'])
    replacement.unmount()
})

test('原生宿主拒绝双应用会话，清理异常仍释放宿主以便重新挂载', () => {
    const native = recordingNative()
    const FirstRoot = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Text, { text: () => '第一份' }),
            }),
        }),
    })
    const SecondRoot = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Text, { text: () => '第二份' }),
            }),
        }),
    })
    const first = appBuilder(FirstRoot).build()
    const second = appBuilder(SecondRoot).build()
    const firstRunning = mountFrame(first, native.target)
    assert.throws(() => mountFrame(second, native.target), /已经|占用/)
    assert.deepEqual(native.textNodes().map(node => node.text), ['第一份'])
    firstRunning.unmount()
    const secondRunning = mountFrame(second, native.target)
    assert.deepEqual(native.textNodes().map(node => node.text), ['第二份'])
    secondRunning.unmount()

    const FailingLeaf = defineArrangable({
        setup() {
            onScopeDispose(() => { throw new Error('清理失败') })
            return () => { }
        }
    })
    const FailingRoot = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, FailingLeaf, {}),
            }),
        }),
    })
    const failing = mountFrame(appBuilder(FailingRoot).build(), native.target)
    assert.throws(() => failing.unmount(), AggregateError)
    assert.equal(native.nodes.size, 0)
    const afterFailure = mountFrame(second, native.target)
    afterFailure.unmount()
})

test('ScrollState 接收原生对象快照并更新文本和滚动 Modifier', async () => {
    const native = recordingNative()
    const scroll = createScrollState()
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Column, { modifier: () => M.verticalScroll(scroll) }, { default: () => call(0, Text, { text: () => `滚动 ${scroll.valuePx}` }) }),
            }),
        }),
    })
    const app = mountFrame(appBuilder(Root).build(), native.target)
    scrollStateInternal(scroll).native({ value: 12, maxValue: 40, viewportSize: 80, contentSize: 120 })
    advanceFrames()
    assert.equal(native.textNodes()[0].text, '滚动 12')
    assert.equal(scroll.maxValuePx, 40)
    const colId = native.windowRoot(0).children[0]
    assert.equal((native.nodes.get(colId)!.inputs.get('modifier') as Modifier).elements[0].value.value, 12)
    app.unmount()
})

test('App 拒绝不一致的协议版本', () => {
    const Root = defineArrangable({ setup: () => () => { } })
    assert.throws(() => mountFrame(appBuilder(Root).build(), { ...recordingNative().target, runtimeVersion: 999 }), /脚本与原生协议版本不一致/)
})

test("诊断 API 交付明确原生入口", () => {
    const native = diagnosticNative()
    globalThis.__CANTON_NATIVE__ = native
    try {
        Log.w("Demo", "测试警告", "测试详情")
        Log.v("Demo", "详细")
        Log.d("Demo", "调试")
        Log.i("Demo", "信息")
        Log.e("Demo", "错误")
        DiagnosticsToast.e("Demo", "提示", "详情")
        diagnostics.setToastsEnabled(false)
        diagnostics.requestReload("src/App.cta")
    } finally {
        delete globalThis.__CANTON_NATIVE__
    }
    assert.ok(native.calls.some((call) => call[0] === "log" && call[1] === "w" && call[2] === "Demo" && JSON.stringify(call[3]) === '["测试警告","测试详情"]'))
    assert.deepEqual(native.calls.filter(call => call[0] === 'log').map(call => call[1]), ['w', 'v', 'd', 'i', 'e'])
    assert.deepEqual(native.calls.find((call) => call[0] === "diagnosticsToast"), [
        "diagnosticsToast",
        { level: "e", tag: "Demo", title: "提示", args: ["详情"], coalesce: true },
    ])
    assert.ok(native.calls.some((call) => call[0] === "diagnosticsRequestReload"))
})

test("Log 在原生调用前拒绝空 TAG", () => {
    const native = diagnosticNative()
    globalThis.__CANTON_NATIVE__ = native
    try {
        assert.throws(() => Log.i(" ", "bad"), /Tag/)
    } finally {
        delete globalThis.__CANTON_NATIVE__
    }
    assert.deepEqual(native.calls, [])
})

test('Icon 未指定 tint 保留 Painter 原色，指定 tint 只增加 colorFilter', () => {
    const native = recordingNative()
    const retired: bigint[] = []
    native.target.acquirePainter = (_resource, complete) => {
        complete({ contentVersion: 1, width: 24, height: 24 })
        return { identity: 99n, generation: 1n }
    }
    native.target.releasePainter = handle => retired.push(handle.identity)
    const previous = globalThis.__CANTON_NATIVE__
    globalThis.__CANTON_NATIVE__ = native.target
    try {
        const Page = defineArrangable({
            setup(_props, { call }) {
                const image = painter('icons/play.svg')
                return () => {
                    call(0, Icon, { painter: () => image })
                    call(1, Icon, { painter: () => image, tint: () => 0xffe8eaed })
                }
            }
        })
        const Root = defineArrangable({
            setup: (_props, { call }) => () => call(0, App, {}, {
                default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                    default: () => call(0, Page, {}),
                }),
            }),
        })
        const app = mountFrame(appBuilder(Root).build(), native.target)
        const paints = [...native.nodes.values()].flatMap(node => (node.inputs.get('modifier') as Modifier | undefined)?.elements ?? []).filter(element => element.type === 'paint')
        assert.equal(paints.length, 2)
        assert.equal(paints[0].value.colorFilter, undefined)
        assert.deepEqual(paints[1].value.colorFilter, { tint: 0xffe8eaed })
        app.unmount()
        assert.deepEqual(retired, [99n])
    } finally {
        globalThis.__CANTON_NATIVE__ = previous
    }
})
