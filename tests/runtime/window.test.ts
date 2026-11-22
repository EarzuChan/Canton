import test from 'node:test'
import assert from 'node:assert/strict'
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import * as framework from '@canton/framework'
import { appBuilder, createInteractionState, createWindowState, inject, ref, type WindowHandle } from '@canton/framework'
import { App, Box, Column, Input, Layout, Text, Window } from '@canton/framework/foundation'
import { DensityKey, M, MinSizeMeasurePolicy, type Density, type Modifier } from '@canton/framework/ui'
import { defineArrangable, lookupPrivateWindowId, mountApplication } from '@canton/framework/internal'
import { checkCtaProject } from '../../packages/compiler/src/typecheck.ts'
import { evaluateCta } from './ctaModules.ts'
import { recordingNative, mountFrame, advanceFrames } from './recordingNative.ts'

test('公开入口仅保留 appBuilder(Root).build() 产出不可变应用定义', () => {
    assert.equal(('create' + 'App') in framework, false)
    assert.equal(('Canton' + 'App') in framework, false)
    const Root = defineArrangable({ setup: (_p, { call }) => () => call(0, App, {}) })
    const built = appBuilder(Root).build()
    assert.equal(framework.isCantonApplication(built), true)
    assert.equal(Object.isFrozen(built), true)
    assert.equal('mount' in (built as object), false)
    assert.throws(() => mountApplication({} as never, recordingNative().target), /appBuilder/)
})

test('画区作用域守卫：画区外使用 <Layout>、<Window> 脱离 <App>、多 <App> 或跨窗混用 WindowHandle 直接报错', () => {
    // <Layout> 直接写在 <App> 下（无 <Window> 画区）
    const BareLayoutInApp = defineArrangable({
        setup: (_p, { call }) => () => call(0, App, {}, {
            default: () => call(0, Box, {}),
        }),
    })
    assert.throws(() => mountFrame(appBuilder(BareLayoutInApp).build(), recordingNative().target), /画区作用域/)

    // <Window> 脱离 <App>
    const BareWindow = defineArrangable({
        setup: (_p, { call }) => () => call(0, Window, { onCloseRequest: () => () => { } }),
    })
    assert.throws(() => mountFrame(appBuilder(BareWindow).build(), recordingNative().target), /<App>/)

    // 同一应用挂载两个 <App>
    const DoubleApp = defineArrangable({
        setup: (_p, { call }) => () => {
            call(0, App, {})
            call(1, App, {})
        },
    })
    assert.throws(() => mountFrame(appBuilder(DoubleApp).build(), recordingNative().target), /唯一的非布局应用根/)

    // 跨窗混用另一个窗口的 windowHandle.draggableArea()
    let leakedHandle!: WindowHandle
    const CrossWindowHandle = defineArrangable({
        setup: (_p, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => {
                call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                    default: ({ windowHandle }) => {
                        leakedHandle = windowHandle
                        call(0, Text, { text: () => '窗一' })
                    },
                })
                call(1, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                    default: () => call(0, Box, { modifier: () => leakedHandle.draggableArea() }),
                })
            },
        }),
    })
    assert.throws(() => mountFrame(appBuilder(CrossWindowHandle).build(), recordingNative().target), /所属 <Window> 画区内/)
})

test('多窗口共享响应式状态、同帧切窗单事务提交、私有身份随卸载立即摘除且 WindowState 保留几何快照', () => {
    const native = recordingNative()
    const phase = ref<'login' | 'main'>('login')
    const sharedCount = ref(10)
    const loginState = createWindowState({ size: { width: 420, height: 300 }, position: 'centerScreen' })
    const mainState = createWindowState({ size: { width: 1080, height: 720 } })
    const toolState = createWindowState({ size: { width: 320, height: 240 } })
    const showTool = ref(true)
    let capturedLoginHandle!: WindowHandle

    const Root = evaluateCta(`<template>
    <App :exitOnLastWindowClosed="true" appId="Canton.Test.MultiWindow" &app-handle>
        <Window c-if="phase === 'login'" key="login" title="登录" :state="loginState" :onCloseRequest="() => { phase = 'main' }" &window-handle>
            <Column :modifier="windowHandle.draggableArea()">
                <Text :text="capture(windowHandle, '登录:' + sharedCount)" />
            </Column>
        </Window>
        <Template c-else>
            <Window key="main" title="主工作台" chrome="customTitleBar" backdrop="mica" :state="mainState" :onCloseRequest="appHandle.exitApplication" &window-handle>
                <Box :modifier="windowHandle.captionButton('maximize')">
                    <Text :text="'主窗:' + sharedCount" />
                </Box>
                <Window c-if="showTool" key="tool" title="工具面板" role="utility" :state="toolState" :onCloseRequest="() => { showTool = false }">
                    <Text :text="'工具:' + sharedCount" />
                </Window>
            </Window>
        </Template>
    </App>
</template>
<script>
import type { WindowHandle } from '@canton/framework'
import { phase, sharedCount, loginState, mainState, toolState, showTool, recordHandle } from './state'
function capture(h: WindowHandle, label: string) {
    recordHandle(h)
    return label
}
</script>`, {
        './state': {
            phase,
            sharedCount,
            loginState,
            mainState,
            toolState,
            showTool,
            recordHandle: (h: WindowHandle) => { capturedLoginHandle = h },
        },
    })

    const app = mountFrame(appBuilder(Root).build(), native.target)
    assert.deepEqual(native.appConfig, { exitOnLastWindowClosed: true, appId: 'Canton.Test.MultiWindow' })
    assert.equal(native.windows.size, 1)
    const loginWinId = lookupPrivateWindowId(loginState)!
    assert.ok(loginWinId >= 1)
    assert.equal(lookupPrivateWindowId(capturedLoginHandle), loginWinId)
    assert.deepEqual(native.textNodes(), [{ id: native.textNodes()[0].id, text: '登录:10' }])

    // 原生几何回写保留至卸载后
    native.windowDriver.onGeometryChanged(loginWinId, { widthPx: 500, heightPx: 360, x: 120, y: 80, hasPosition: true, placement: 'normal' })
    advanceFrames()
    assert.deepEqual(loginState.size, { width: 500, height: 360 })
    assert.deepEqual(loginState.position, { x: 120, y: 80 })

    // 拦截关闭请求转入同帧切窗：一次重排内同时销毁登录窗并打开主窗与嵌套工具窗
    const submissionsBeforeSwitch = native.submissions
    loginState.requestClose()
    advanceFrames()
    assert.equal(native.submissions, submissionsBeforeSwitch + 1, '同帧切窗应在单次重排事务内完成')
    assert.equal(lookupPrivateWindowId(loginState), undefined, '卸载窗口必须立即从私有句柄表摘除')
    assert.throws(() => capturedLoginHandle.draggableArea(), /已卸载/)
    assert.deepEqual(loginState.size, { width: 500, height: 360 }, '卸载后 WindowState 保留最后几何快照供下次复开')

    const mainWinId = lookupPrivateWindowId(mainState)!
    const toolWinId = lookupPrivateWindowId(toolState)!
    assert.ok(mainWinId > loginWinId && toolWinId > mainWinId, '私有窗口身份单调递增不复用')
    assert.equal(native.windows.get(toolWinId)!.config.ownerWindowId, mainWinId, '嵌套 <Window> 默认继承最近祖先窗口为 owner')
    // 嵌套 <Window> 对父窗 LayoutTree 透明，不混入父窗节点树
    assert.equal(native.nodes.get(native.windows.get(mainWinId)!.rootNodeId)!.children.length, 1)

    // 共享响应式状态同时驱动主窗与工具窗更新
    sharedCount.value = 42
    advanceFrames()
    assert.deepEqual(native.textNodes(mainWinId).map(n => n.text), ['主窗:42'])
    assert.deepEqual(native.textNodes(toolWinId).map(n => n.text), ['工具:42'])

    app.unmount()
    assert.equal(lookupPrivateWindowId(mainState), undefined)
    assert.equal(lookupPrivateWindowId(toolState), undefined)
})

test('modality="owner" 无宿主报错，同一 WindowState 同时绑定两窗报错', () => {
    const TopLevelOwnerModal = defineArrangable({
        setup: (_p, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { modality: () => 'owner' as const, onCloseRequest: () => appHandle.exitApplication }),
        }),
    })
    assert.throws(() => mountFrame(appBuilder(TopLevelOwnerModal).build(), recordingNative().target), /modality="owner"/)

    const OwnerNoneModal = defineArrangable({
        setup: (_p, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Window, { owner: () => 'none' as const, modality: () => 'owner' as const, onCloseRequest: () => appHandle.exitApplication }),
            }),
        }),
    })
    assert.throws(() => mountFrame(appBuilder(OwnerNoneModal).build(), recordingNative().target), /modality="owner"/)

    const sharedState = createWindowState()
    const DuplicateState = defineArrangable({
        setup: (_p, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => {
                call(0, Window, { state: () => sharedState, onCloseRequest: () => appHandle.exitApplication })
                call(1, Window, { state: () => sharedState, onCloseRequest: () => appHandle.exitApplication })
            },
        }),
    })
    assert.throws(() => mountFrame(appBuilder(DuplicateState).build(), recordingNative().target), /同一 WindowState/)
})

test('窗口级 DPI 倍率独立驱动所属窗口 Density，Input readOnly 与非客户区 Modifier 正确降解', () => {
    const native = recordingNative()
    const stateA = createWindowState({ size: { width: 400, height: 300 } })
    const stateB = createWindowState({ size: { width: 400, height: 300 } })
    const captionInteraction = createInteractionState()
    let densityA!: Density, densityB!: Density

    const ProbeA = defineArrangable({
        setup(_p, { call }) {
            densityA = inject(DensityKey)!
            return () => call(0, Layout, { measurePolicy: () => MinSizeMeasurePolicy, modifier: () => M.width(100, 0) })
        },
    })
    const ProbeB = defineArrangable({
        setup(_p, { call }) {
            densityB = inject(DensityKey)!
            return () => call(0, Input, { value: () => '只读文本', readOnly: () => true, modifier: () => M.width(100, 0) })
        },
    })
    const Root = defineArrangable({
        setup: (_p, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => {
                call(0, Window, { state: () => stateA, chrome: () => 'customTitleBar' as const, onCloseRequest: () => appHandle.exitApplication }, {
                    default: ({ windowHandle }) => {
                        call(0, Box, { modifier: () => windowHandle.draggableArea().then(windowHandle.nonDraggableArea()).then(windowHandle.captionButton('close', { interactionState: captionInteraction })) })
                        call(1, ProbeA, {})
                    },
                })
                call(1, Window, { state: () => stateB, onCloseRequest: () => appHandle.exitApplication }, {
                    default: () => call(0, ProbeB, {}),
                })
            },
        }),
    })

    const app = mountFrame(appBuilder(Root).build(), native.target)
    const winA = lookupPrivateWindowId(stateA)!
    const winB = lookupPrivateWindowId(stateB)!
    assert.notEqual(densityA, densityB)

    // 窗口 A 切换至 1.5 倍 DPI：仅窗口 A 的 DP 值重算为 150px，窗口 B 保持 100px
    native.windowDriver.onDpiChanged(winA, 1.5)
    native.windowDriver.onFocusChanged(winA, true)
    advanceFrames()
    assert.equal(stateA.dpiScale, 1.5)
    assert.equal(stateA.focused, true)
    assert.equal(stateB.dpiScale, 1)

    const nodesA = [...native.nodes.values()].filter(n => n.windowId === winA && n.type === 'LayoutNode')
    const nodesB = [...native.nodes.values()].filter(n => n.windowId === winB && n.type === 'LayoutNode')
    const chromeElements = (nodesA[0].inputs.get('modifier') as Modifier).elements
    assert.deepEqual(chromeElements.map(e => e.type), ['draggableArea', 'nonDraggableArea', 'captionButton'])
    assert.equal(chromeElements[2].value.button, 'close')
    assert.equal(typeof chromeElements[2].value.interaction, 'function')

    const widthA = (nodesA[1].inputs.get('modifier') as Modifier).elements[0].value.value
    const inputElementsB = (nodesB[0].inputs.get('modifier') as Modifier).elements
    assert.equal(widthA, 150)
    assert.equal(inputElementsB[0].value.value, 100)
    assert.equal(inputElementsB[1].value.readOnly, true)

    app.unmount()
})

test('CTA 类型检查支持 <App> / <Window> 槽位句柄与 createWindowState DP 单位契约', () => {
    const directory = mkdtempSync(resolve('tmp-refs/cta-window-typecheck-'))
    try {
        const good = join(directory, 'GoodWindow.cta')
        writeFileSync(good, `<template>
    <App &app-handle>
        <Window title="标题" chrome="customTitleBar" :state="win" :minSize="{ width: 320.dp, height: 240.dp }" :onCloseRequest="appHandle.exitApplication" &window-handle>
            <Box :modifier="windowHandle.draggableArea().fillMaxWidth().height(36.dp)">
                <Spacer :modifier="windowHandle.captionButton('close').size(40.dp, 36.dp)" />
            </Box>
            <Input value="只读" :readOnly="true" />
        </Window>
    </App>
</template>
<script>
import { createWindowState } from '@canton/framework'
import { M } from '@canton/framework/ui'
const win = createWindowState({ size: { width: 800.dp, height: 600.dp }, position: 'centerScreen' })
</script>`)
        assert.deepEqual(checkCtaProject(resolve('tsconfig.json'), [good]), [])

        const badUnit = join(directory, 'BadUnitWindow.cta')
        writeFileSync(badUnit, `<template>
    <App &app-handle>
        <Window :state="win" :onCloseRequest="appHandle.exitApplication" />
    </App>
</template>
<script>
import { createWindowState } from '@canton/framework'
const win = createWindowState({ size: { width: 800, height: 600 } })
</script>`)
        const unitDiag = checkCtaProject(resolve('tsconfig.json'), [badUnit])
        assert.ok(unitDiag.some(d => /要求 DP/.test(d.message)), JSON.stringify(unitDiag))

        const badButton = join(directory, 'BadButtonWindow.cta')
        writeFileSync(badButton, `<template>
    <App &app-handle>
        <Window :onCloseRequest="appHandle.exitApplication" &window-handle>
            <Spacer :modifier="windowHandle.captionButton('invalid')" />
        </Window>
    </App>
</template>`)
        const btnDiag = checkCtaProject(resolve('tsconfig.json'), [badButton])
        assert.ok(btnDiag.some(d => /invalid/.test(d.message)), JSON.stringify(btnDiag))
    } finally {
        rmSync(directory, { recursive: true })
    }
})
