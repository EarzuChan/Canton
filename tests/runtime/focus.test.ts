import test from 'node:test'
import assert from 'node:assert/strict'
import { mkdirSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { resolve, join } from 'node:path'
import * as core from '@canton/framework'
import { appBuilder, createFocusRequester, FocusManagerKey, inject, provide, ref, onMounted, nextTick, type FocusRequester, type FocusManager } from '@canton/framework'
import { App, Layout, Window } from '@canton/framework/foundation'
import { M, MinSizeMeasurePolicy } from '@canton/framework/ui'
import { defineArrangable, mountApplication } from '@canton/framework/internal'
import { recordingNative } from './recordingNative.ts'
import { compileCantonCta } from '../../packages/vite-plugin/src/ctaTransform.ts'
import { checkCtaProject } from '../../packages/compiler/src/typecheck.ts'
import { evaluateCta } from './ctaModules.ts'

test('焦点请求发送窗口级宿主命令；nextTick 不冒充原生焦点回执，unmount 取消请求', async () => {
    const native = recordingNative()
    const commands: unknown[][] = []
    native.target.focusCommand = (...args) => {
        commands.push(args)
        return true
    }
    let requester!: FocusRequester
    let manager!: FocusManager
    const observed: boolean[] = []
    const Page = defineArrangable({
        setup(_props, { call }) {
            requester = createFocusRequester()
            manager = inject(FocusManagerKey)!
            onMounted(() => assert.equal(requester.requestFocus(), true))
            return () => call(0, Layout, { measurePolicy: () => MinSizeMeasurePolicy, modifier: () => M.focusRequester(requester).onFocusChanged(state => observed.push(state.isFocused)).focusable() })
        }
    })
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Page, {}),
            }),
        }),
    })
    const app = mountApplication(appBuilder(Root).build(), native.target)
    native.frame()
    const winId = [...native.windows.keys()][0]
    assert.equal(commands[0][0], 'request')
    assert.equal(commands[0][3], winId)
    await nextTick()
    assert.deepEqual(observed, [])
    assert.equal(manager.moveFocus('previous'), true)
    manager.clearFocus()
    assert.deepEqual(commands.slice(1), [['move', 0, 'previous', winId], ['clear', 0, undefined, winId]])
    assert.throws(() => manager.moveFocus('diagonal' as never), /方向/)
    app.unmount()
    assert.deepEqual(commands.at(-1), ['cancel', commands[0][1], undefined, winId])
    assert.equal(requester.requestFocus(), false)
    assert.equal(manager.moveFocus('next'), false)
})

test('focusProperties 持有稳定 Requester 身份，普通 Modifier 描述可由 CTA 组合', () => {
    const a = createFocusRequester()
    const b = createFocusRequester()
    const chain = M.focusGroup().focusProperties({ right: b, next: a, canFocus: false }).focusRequester(a).focusable()
    assert.deepEqual(chain.elements.map(element => element.type), ['focusGroup', 'focusProperties', 'focusRequester', 'focusable'])
    assert.equal(chain.elements[1].value.next, chain.elements[2].value.requester)
    assert.notEqual(chain.elements[1].value.right, chain.elements[2].value.requester)
    assert.equal(chain.elements[1].value.canFocus, false)
    assert.throws(() => M.focusRequester({ requestFocus: () => true }), /createFocusRequester/)
    assert.throws(() => M.onFocusChanged(null as never), /函数/)
    assert.throws(() => inject(FocusManagerKey), /活动 Arrangable/)
    assert.equal('useFocusManager' in core, false)
    const compiled = compileCantonCta(`<template><Layout :measurePolicy="MinSizeMeasurePolicy" :modifier="M.focusRequester(target).onFocusChanged(changed).focusProperties({ right: target }).focusGroup().focusable()" /></template><script>
import { createFocusRequester } from '@canton/framework'
import { M, MinSizeMeasurePolicy } from '@canton/framework/ui'
const target = createFocusRequester()
function changed(state: { isFocused: boolean; hasFocus: boolean }) {}
</script>`, '焦点.cta')
    assert.match(compiled.code, /focusRequester.*onFocusChanged.*focusProperties.*focusGroup.*focusable/s)
})

test('默认 FocusManager 按 <Window> 独立提供，同一应用内多窗彼此隔离', () => {
    const native = recordingNative()
    const commands: unknown[][] = []
    native.target.focusCommand = (...args) => {
        commands.push(args)
        return true
    }
    const byWindow = { A: [] as FocusManager[], B: [] as FocusManager[] }
    const showSecond = ref(true)
    const Child = defineArrangable({
        props: { tag: { type: String, required: true } },
        setup(props, { call }) {
            byWindow[props.tag as 'A' | 'B'].push(inject(FocusManagerKey)!)
            return () => call(0, Layout, { measurePolicy: () => MinSizeMeasurePolicy })
        }
    })
    const WindowPage = defineArrangable({
        props: { tag: { type: String, required: true } },
        setup(props, { call }) {
            byWindow[props.tag as 'A' | 'B'].push(inject(FocusManagerKey)!)
            return () => {
                call(0, Child, { tag: () => props.tag })
                call(1, Child, { tag: () => props.tag })
            }
        }
    })
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => {
                call(0, Window, { title: () => 'A', onCloseRequest: () => appHandle.exitApplication }, { default: () => call(0, WindowPage, { tag: () => 'A' }) })
                if (showSecond.value) call(1, Window, { title: () => 'B', onCloseRequest: () => () => { showSecond.value = false } }, { default: () => call(0, WindowPage, { tag: () => 'B' }) })
            },
        }),
    })
    const app = mountApplication(appBuilder(Root).build(), native.target)
    native.frame()
    const [winA, winB] = [...native.windows.keys()]
    assert.equal(byWindow.A.length, 3)
    assert.equal(byWindow.B.length, 3)
    assert.equal(byWindow.A[0], byWindow.A[1])
    assert.equal(byWindow.A[0], byWindow.A[2])
    assert.equal(byWindow.B[0], byWindow.B[1])
    assert.equal(byWindow.B[0], byWindow.B[2])
    assert.notEqual(byWindow.A[0], byWindow.B[0])
    assert.equal(byWindow.A[0].moveFocus('right'), true)
    assert.equal(byWindow.B[0].moveFocus('down'), true)
    assert.deepEqual(commands, [['move', 0, 'right', winA], ['move', 0, 'down', winB]])
    showSecond.value = false
    native.frame()
    assert.equal(byWindow.B[0].moveFocus('next'), false)
    assert.equal(byWindow.A[0].moveFocus('previous'), true)
    assert.deepEqual(commands.at(-1), ['move', 0, 'previous', winA])
    app.unmount()
    assert.equal(byWindow.A[0].moveFocus('next'), false)
})

test('FocusManagerKey 支持窗口子树内祖先局部覆盖，兄弟分支保持窗口默认服务', () => {
    const native = recordingNative()
    const calls: string[] = []
    native.target.focusCommand = (op, _req, dir) => {
        calls.push(`窗口:${op}:${dir ?? ''}`)
        return true
    }
    const localManager: FocusManager = {
        clearFocus: () => { calls.push('局部清除') }, moveFocus: direction => {
            calls.push(`局部:${direction}`)
            return false
        }
    }
    const seen = new Map<string, FocusManager>()
    const Consumer = defineArrangable({
        props: { branch: { type: String, required: true } },
        setup(props, { call }) {
            seen.set(props.branch, inject(FocusManagerKey)!)
            return () => call(0, Layout, { measurePolicy: () => MinSizeMeasurePolicy })
        }
    })
    const Provider = defineArrangable({
        setup(_props, { call }) {
            seen.set('提供者', inject(FocusManagerKey)!)
            provide(FocusManagerKey, localManager)
            return () => call(0, Consumer, { branch: () => '局部' })
        }
    })
    const Page = defineArrangable({
        setup(_props, { call }) {
            seen.set('根', inject(FocusManagerKey)!)
            return () => {
                call(0, Provider, {})
                call(1, Consumer, { branch: () => '兄弟' })
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
    const app = mountApplication(appBuilder(Root).build(), native.target)
    native.frame()
    assert.equal(seen.get('根'), seen.get('提供者'))
    assert.equal(seen.get('根'), seen.get('兄弟'))
    assert.equal(seen.get('局部'), localManager)
    assert.equal(seen.get('局部')!.moveFocus('left'), false)
    seen.get('局部')!.clearFocus()
    assert.equal(seen.get('兄弟')!.moveFocus('next'), true)
    seen.get('兄弟')!.clearFocus()
    assert.deepEqual(calls, ['局部:left', '局部清除', '窗口:move:next', '窗口:clear:'])
    app.unmount()
})

test('宿主安装失败后可重挂载，窗口默认 FocusManager 绑定成功安装的宿主', () => {
    const failed = recordingNative()
    const successful = recordingNative()
    const failedCommands: unknown[][] = []
    const commands: unknown[][] = []
    const installFailedDriver = failed.target.installFrameDriver
    failed.target.focusCommand = (...args) => {
        failedCommands.push(args)
        return true
    }
    failed.target.installFrameDriver = () => { throw new Error('宿主拒绝安装') }
    successful.target.focusCommand = (...args) => {
        commands.push(args)
        return true
    }
    const managers: FocusManager[] = []
    const Page = defineArrangable({
        setup(_props, { call }) {
            managers.push(inject(FocusManagerKey)!)
            return () => call(0, Layout, { measurePolicy: () => MinSizeMeasurePolicy })
        }
    })
    const Root = defineArrangable({
        setup: (_props, { call }) => () => call(0, App, {}, {
            default: ({ appHandle }) => call(0, Window, { onCloseRequest: () => appHandle.exitApplication }, {
                default: () => call(0, Page, {}),
            }),
        }),
    })
    const definition = appBuilder(Root).build()
    assert.throws(() => mountApplication(definition, failed.target), /宿主拒绝安装/)
    assert.equal(managers.length, 0)
    const firstRun = mountApplication(definition, successful.target)
    successful.frame()
    const win1 = [...successful.windows.keys()][0]
    assert.equal(managers.length, 1)
    assert.equal(managers[0].moveFocus('next'), true)
    assert.deepEqual(failedCommands, [])
    assert.deepEqual(commands, [['move', 0, 'next', win1]])
    firstRun.unmount()
    assert.equal(managers[0].moveFocus('next'), false)
    failed.target.installFrameDriver = installFailedDriver
    const secondRun = mountApplication(definition, failed.target)
    failed.frame()
    const win2 = [...failed.windows.keys()][0]
    assert.equal(managers.length, 2)
    assert.notEqual(managers[0], managers[1])
    assert.equal(managers[1].moveFocus('previous'), true)
    assert.deepEqual(failedCommands, [['move', 0, 'previous', win2]])
    secondRun.unmount()
})

test('CTA 通过 typed FocusManagerKey 注入消费，方向参数保留真实类型', () => {
    const source = `<template><App &appHandle><Window :onCloseRequest="appHandle.exitApplication"><Child /></Window></App></template><script>
import Child from './Child.cta'
</script>`
    const childSource = `<template><Layout :measurePolicy="MinSizeMeasurePolicy" :modifier="M.focusRequester(target).focusable()" /></template><script>
import { createFocusRequester, FocusManagerKey, inject, onMounted, type FocusManager } from '@canton/framework'
import { M, MinSizeMeasurePolicy } from '@canton/framework/ui'
const target = createFocusRequester()
const manager: FocusManager | undefined = inject(FocusManagerKey)
onMounted(() => manager?.moveFocus('next'))
</script>`
    mkdirSync(resolve('tmp-refs'), { recursive: true })
    const directory = mkdtempSync(resolve('tmp-refs/cta-focus-'))
    try {
        const childFile = join(directory, 'Child.cta')
        writeFileSync(childFile, childSource)
        assert.deepEqual(checkCtaProject(resolve('tsconfig.json'), [childFile]), [])
        writeFileSync(childFile, childSource.replace("const manager: FocusManager | undefined", 'const manager').replace("moveFocus('next')", "moveFocus('diagonal')"))
        assert.ok(checkCtaProject(resolve('tsconfig.json'), [childFile]).some(diagnostic => /diagonal.*FocusDirection/.test(diagnostic.message)))
    } finally {
        rmSync(directory, { recursive: true })
    }
    const native = recordingNative()
    const commands: unknown[][] = []
    native.target.focusCommand = (...args) => {
        commands.push(args)
        return true
    }
    const Child = evaluateCta(childSource, {}, '焦点注入子项.cta')
    const Root = evaluateCta(source, { './Child.cta': { default: Child } }, '焦点注入.cta')
    const app = mountApplication(appBuilder(Root).build(), native.target)
    native.frame()
    const winId = [...native.windows.keys()][0]
    assert.deepEqual(commands, [['move', 0, 'next', winId]])
    app.unmount()
})
