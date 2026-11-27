import test from 'node:test'
import assert from 'node:assert/strict'
import { appBuilder, assets, createWindowState, delay, effectScope, inject, LaunchRequestKey, provide, ref, useInterval, useTimeout, watchEffect } from '@canton/framework'
import { App, Window } from '@canton/framework/foundation'
import { GraphicsEnvironmentKey, type GraphicsEnvironment } from '@canton/framework/ui'
import { CustomTrayMenu, MediaSession, MenuItem, setJumpList, clearJumpList, SystemTrayMenu, TrayIcon } from '@canton/framework/shell'
import { FsError, paths, readText, writeText, exists as fsExists } from '@canton/framework/fs'
import { downloadFile, http, NetError, request } from '@canton/framework/net'
import { CANTON_PROTOCOL_VERSION } from '@canton/shared'
import { defineArrangable, mountApplication } from '@canton/framework/internal'
import type { NativeGraphicsEnvironmentSnapshot, NativeLaunchRequest, NativeTransactionTarget } from '../../packages/framework/src/native/protocol.ts'

function createMockNativeTarget(overrides: Partial<NativeTransactionTarget> = {}) {
    let nextWindowId = 1
    let graphicsListener: ((env: NativeGraphicsEnvironmentSnapshot) => void) | null = null
    let secondInstanceListener: ((req: NativeLaunchRequest) => void) | null = null
    let prepareFrame: ((time: number) => void) | undefined
    let completeFrame: ((success: boolean) => void) | undefined
    let rearrangeComplete: ((error?: string) => void) | undefined
    const recordedTrays: unknown[] = []
    const recordedMedia: unknown[] = []
    const recordedJumpLists: unknown[] = []

    const target: NativeTransactionTarget = {
        runtimeVersion: CANTON_PROTOCOL_VERSION,
        configureApp() { },
        requestAppExit() { },
        allocateWindowId: () => nextWindowId++,
        createWindow() { },
        updateWindow() { },
        destroyWindow() { },
        windowCommand() { },
        installWindowDriver() { },
        focusCommand: () => true,
        installFrameDriver(prepare, complete) {
            prepareFrame = prepare
            completeFrame = complete
        },
        installLayoutDriver() { },
        continueRearrange() { },
        currentTime: () => 0,
        requestFrame() { },
        beginRearrange() { },
        submitRearrange(complete) {
            rearrangeComplete = complete
        },
        abortRearrange() { },
        acquirePainter: () => ({ identity: 1n, generation: 1n }),
        releasePainter() { },
        defineFontFamily() { },
        removeFontFamily() { },
        beginFontPreload() { },
        endFontPreload() { },
        createPixelBuffer: () => ({ identity: 1n, generation: 1n }),
        commitPixelBuffer: () => 1,
        compileRuntimeEffect: () => ({ identity: 1n, generation: 1n }),
        loadRuntimeEffectAsset: () => ({ identity: 1n, generation: 1n }),
        updateRuntimeEffect: () => ({ ok: true }),
        releaseRuntimeEffect() { },
        createRenderLayer: () => ({ identity: 1n, generation: 1n }),
        releaseRenderLayer() { },
        registerBinding: () => ({ identity: 1n, generation: 1n }),
        updateBinding() { },
        releaseBinding() { },
        modifierInstances: () => [],
        registerModifierBinding: () => ({ identity: 1n, generation: 1n }),
        createNode() { },
        deleteNode() { },
        insertChild() { },
        removeChild() { },
        unmount() { },
        log() { },
        diagnosticsToast() { },
        diagnosticsRequestReload() { },
        diagnosticsTriggerFakeError() { },
        diagnosticsSetToastsEnabled() { },
        getGraphicsEnvironment: () => ({
            backend: 'd3d11',
            adapterName: 'RTX Test GPU',
            isSoftwareRenderer: false,
            hardwareAccelerated: true,
            isRemoteSession: false,
            systemTransparency: true,
            systemAnimations: true,
        }),
        onGraphicsEnvironmentChanged(cb) {
            graphicsListener = cb
        },
        getLaunchRequest: () => ({ args: ['--open', 'demo.txt'], workingDirectory: 'E:\\work' }),
        onSecondInstance(cb) {
            secondInstanceListener = cb
        },
        trayUpsert(spec) {
            recordedTrays.push(spec)
        },
        trayRemove() { },
        mediaSessionUpdate(spec) {
            recordedMedia.push(spec)
        },
        setJumpList(categories) {
            recordedJumpLists.push(categories)
        },
        clearJumpList() {
            recordedJumpLists.length = 0
        },
        ...overrides,
    }

    const flushFrame = () => {
        prepareFrame?.(16)
        const cb = rearrangeComplete
        rearrangeComplete = undefined
        cb?.()
        completeFrame?.(true)
    }

    return {
        target,
        flushFrame,
        emitGraphicsChange: (env: NativeGraphicsEnvironmentSnapshot) => graphicsListener?.(env),
        emitSecondInstance: (req: NativeLaunchRequest) => secondInstanceListener?.(req),
        recordedTrays,
        recordedMedia,
        recordedJumpLists,
    }
}

test('useTimeout / useInterval 作用域卸载自动取消，delay 响应 AbortSignal 立即中断', async () => {
    let fired = 0
    const scope = effectScope()
    let handle!: ReturnType<typeof useTimeout>
    scope.run(() => {
        handle = useTimeout(() => { fired++ }, 40)
        useInterval(() => { fired++ }, 25)
    })
    assert.equal(handle.active, true)
    scope.stop()
    assert.equal(handle.active, false)
    await delay(70)
    assert.equal(fired, 0)

    const ac = new AbortController()
    const pending = delay(500, ac.signal)
    ac.abort()
    await assert.rejects(pending, (err: unknown) => err instanceof Error && err.name === 'AbortError')

    const alreadyAborted = AbortSignal.abort(new Error('pre-aborted'))
    await assert.rejects(delay(10, alreadyAborted), /pre-aborted/)
})

test('GraphicsEnvironmentKey 响应式更新与子树覆写，LaunchRequestKey 首帧同步可读', () => {
    const mock = createMockNativeTarget()
    const observedTiers: string[] = []
    let observedChildTier = ''
    let observedLaunchArgs: readonly string[] = []
    const receivedSecond: string[][] = []

    const GrandChild = defineArrangable({
        name: 'GrandChild',
        setup() {
            const injected = inject(GraphicsEnvironmentKey)!
            observedChildTier = injected.recommendedTier
            return () => { }
        },
    })

    const ChildOverride = defineArrangable({
        name: 'ChildOverride',
        setup(_props, { call }) {
            const forced: GraphicsEnvironment = Object.freeze({
                backend: 'd3d11',
                adapterName: 'Forced Reduced',
                isSoftwareRenderer: true,
                hardwareAccelerated: false,
                isRemoteSession: false,
                systemTransparency: false,
                systemAnimations: false,
                recommendedTier: 'reduced',
            })
            provide(GraphicsEnvironmentKey, forced)
            return () => call(0, GrandChild, {}, {})
        },
    })

    const RootContent = defineArrangable({
        name: 'RootContent',
        setup(_props, { call }) {
            const env = inject(GraphicsEnvironmentKey)!
            const launch = inject(LaunchRequestKey)!
            observedLaunchArgs = launch.args
            watchEffect(() => {
                observedTiers.push(`${env.adapterName}:${env.recommendedTier}`)
            })
            return () => call(0, ChildOverride, {}, {})
        },
    })

    const Root = defineArrangable({
        name: 'Root',
        setup(_props, { call }) {
            return () => call(0, App, {
                onSecondInstance: () => req => { receivedSecond.push([...req.args]) },
            }, {
                default: () => call(1, RootContent, {}, {}),
            })
        },
    })

    const app = mountApplication(appBuilder(Root).build(), mock.target)
    try {
        mock.flushFrame()
        assert.deepEqual(observedLaunchArgs, ['--open', 'demo.txt'])
        assert.equal(observedChildTier, 'reduced')
        assert.deepEqual(observedTiers, ['RTX Test GPU:full'])

        mock.emitGraphicsChange({
            backend: 'd3d11',
            adapterName: 'Microsoft Basic Render Driver',
            isSoftwareRenderer: true,
            hardwareAccelerated: false,
            isRemoteSession: false,
            systemTransparency: true,
            systemAnimations: true,
        })
        mock.flushFrame()
        assert.deepEqual(observedTiers, ['RTX Test GPU:full', 'Microsoft Basic Render Driver:reduced'])

        mock.emitSecondInstance({ args: ['--wake'], workingDirectory: 'C:\\' })
        assert.deepEqual(receivedSecond, [['--wake']])
    } finally {
        app.unmount()
    }
})

test('setJumpList / clearJumpList 在 App 挂载前调用直接报错，挂载后可正常写入', () => {
    assert.throws(() => setJumpList([]), /必须在 <App> 挂载后调用/)
    assert.throws(() => clearJumpList(), /必须在 <App> 挂载后调用/)

    const mock = createMockNativeTarget()
    const prevGlobal = globalThis.__CANTON_NATIVE__
    globalThis.__CANTON_NATIVE__ = mock.target

    const Root = defineArrangable({
        name: 'JumpRoot',
        setup(_props, { call }) {
            return () => call(0, App, {}, { default: () => { } })
        },
    })
    const app = mountApplication(appBuilder(Root).build(), mock.target)
    try {
        mock.flushFrame()
        setJumpList([{ title: 'Tasks', items: [{ title: 'New Note', args: ['--new'] }] }])
        assert.equal(mock.recordedJumpLists.length, 1)
        clearJumpList()
        assert.equal(mock.recordedJumpLists.length, 0)
    } finally {
        app.unmount()
        globalThis.__CANTON_NATIVE__ = prevGlobal
    }
})

test('<TrayIcon> 菜单互斥报错与 <MediaSession> 多实例报错', () => {
    const mock = createMockNativeTarget()

    // 1. 同一 <TrayIcon> 同时声明 <SystemTrayMenu> 与 <CustomTrayMenu> 必须报错
    const ConflictingTrayRoot = defineArrangable({
        name: 'ConflictingTrayRoot',
        setup(_props, { call }) {
            return () => call(0, App, {}, {
                default: () => call(1, TrayIcon, { tooltip: () => 'demo' }, {
                    default: () => {
                        call(2, SystemTrayMenu, {}, { default: () => call(3, MenuItem, { label: () => 'A' }, {}) })
                        call(4, CustomTrayMenu, {}, { default: () => { } })
                    },
                }),
            })
        },
    })
    assert.throws(() => {
        const running = mountApplication(appBuilder(ConflictingTrayRoot).build(), mock.target)
        try {
            mock.flushFrame()
        } finally {
            running.unmount()
        }
    }, /不可混用/)

    // 2. 同时挂载两个 <MediaSession> 必须报错
    const DualMediaRoot = defineArrangable({
        name: 'DualMediaRoot',
        setup(_props, { call }) {
            return () => call(0, App, {}, {
                default: () => {
                    call(1, MediaSession, { playbackStatus: () => 'playing' as const }, {})
                    call(2, MediaSession, { playbackStatus: () => 'paused' as const }, {})
                },
            })
        },
    })
    assert.throws(() => {
        const running = mountApplication(appBuilder(DualMediaRoot).build(), mock.target)
        try {
            mock.flushFrame()
        } finally {
            running.unmount()
        }
    }, /最多只能挂载一个活跃的 <MediaSession>/)
    // 3. 同一 <TrayIcon> 通过条件分支在 <SystemTrayMenu> 与 <CustomTrayMenu> 间切换不得误报
    const mode = ref<'system' | 'custom'>('system')
    const SwitchableTrayRoot = defineArrangable({
        name: 'SwitchableTrayRoot',
        setup(_props, { call }) {
            return () => call(0, App, {}, {
                default: () => call(1, TrayIcon, { tooltip: () => 'switchable' }, {
                    default: () => {
                        if (mode.value === 'system') {
                            call(2, SystemTrayMenu, {}, { default: () => call(3, MenuItem, { label: () => 'A' }, {}) })
                        } else {
                            call(4, CustomTrayMenu, {}, { default: () => { } })
                        }
                    },
                }),
            })
        },
    })
    const switchApp = mountApplication(appBuilder(SwitchableTrayRoot).build(), mock.target)
    try {
        mock.flushFrame()
        mode.value = 'custom'
        mock.flushFrame()
        mode.value = 'system'
        mock.flushFrame()
    } finally {
        switchApp.unmount()
    }
})

test('<Window :taskbarBadge> 支持在对象与 null 之间切换，WindowState.requestAttention 正确派发，taskbarActions 超过 7 项直接报错', () => {
    const windowCommands: Array<{ windowId: number; command: string }> = []
    const mock = createMockNativeTarget({
        windowCommand(windowId, command) {
            windowCommands.push({ windowId, command })
        },
    })
    const badge = ref<{ icon?: string; label?: string } | null>({ icon: '/badge.svg', label: '1' })
    const winState = createWindowState()
    const BadgeRoot = defineArrangable({
        name: 'TaskbarBadgeToggleRoot',
        setup(_props, { call }) {
            return () => call(0, App, {}, {
                default: () => call(1, Window, {
                    state: () => winState,
                    onCloseRequest: () => () => { },
                    taskbarBadge: () => badge.value,
                }, { default: () => { } }),
            })
        },
    })
    const badgeApp = mountApplication(appBuilder(BadgeRoot).build(), mock.target)
    try {
        mock.flushFrame()
        winState.requestAttention()
        winState.requestAttention('critical')
        winState.requestAttention('stop')
        assert.deepEqual(windowCommands.map(c => c.command), [
            'requestAttentionInformational',
            'requestAttentionCritical',
            'stopAttention',
        ])
        badge.value = null
        mock.flushFrame()
        badge.value = { icon: '/badge.svg', label: '2' }
        mock.flushFrame()
    } finally {
        badgeApp.unmount()
    }

    const eightActions = Array.from({ length: 8 }, (_, i) => ({ id: `a${i}`, icon: '/icon.png' }))
    const Root = defineArrangable({
        name: 'TaskbarLimitRoot',
        setup(_props, { call }) {
            return () => call(0, App, {}, {
                default: () => call(1, Window, {
                    onCloseRequest: () => () => { },
                    taskbarActions: () => eightActions,
                }, { default: () => { } }),
            })
        },
    })
    assert.throws(() => {
        const running = mountApplication(appBuilder(Root).build(), mock.target)
        try {
            mock.flushFrame()
        } finally {
            running.unmount()
        }
    }, /最多支持 7 个/)
})

test('assets.exists 与 @canton/framework/fs 路径工具及结构化错误', async () => {
    assert.equal(paths.join('C:\\Users\\Alice', 'Docs', '../Notes', 'a.txt'), 'C:\\Users\\Alice\\Notes\\a.txt')
    assert.equal(paths.dirname('C:\\Users\\Alice\\a.txt'), 'C:\\Users\\Alice')
    assert.equal(paths.basename('C:\\Users\\Alice\\a.txt'), 'a.txt')
    assert.equal(paths.basename('C:\\Users\\Alice\\a.txt', '.txt'), 'a')
    assert.equal(paths.extname('C:\\Users\\Alice\\a.txt'), '.txt')

    const prevGlobal = globalThis.__CANTON_NATIVE__
    const fakeStore = new Map<string, string>()
    const mock = createMockNativeTarget({
        assetExists: async ref => ref === '/logo.png',
        getKnownPath: kind => `C:\\Mock\\${kind}`,
        fsAsync: async (op, path, _path2, bytes) => {
            if (op === 'writeText') {
                fakeStore.set(path, String(bytes ?? ''))
                return { ok: true }
            }
            if (op === 'readText') {
                if (!fakeStore.has(path)) return { ok: false, errorCode: 'NOT_FOUND', errorPath: path, errorMessage: 'not found' }
                return { ok: true, text: fakeStore.get(path) }
            }
            if (op === 'exists') return { ok: true, exists: fakeStore.has(path) }
            return { ok: true }
        },
    })
    globalThis.__CANTON_NATIVE__ = mock.target
    try {
        assert.equal(await assets.exists('/logo.png'), true)
        assert.equal(await assets.exists('/missing.png'), false)
        assert.equal(paths.appData(), 'C:\\Mock\\appData')
        const file = paths.join(paths.appData(), 'note.txt')
        await writeText(file, 'hello atomic', { atomic: true })
        assert.equal(await fsExists(file), true)
        assert.equal(await readText(file), 'hello atomic')
        await assert.rejects(readText('C:\\Mock\\missing.txt'), (err: unknown) => err instanceof FsError && err.code === 'NOT_FOUND')
    } finally {
        globalThis.__CANTON_NATIVE__ = prevGlobal
    }
})

test('@canton/framework/net 选项校验、query 编码、AbortSignal 与同步响应读取', async () => {
    await assert.rejects(request('ftp://example.com/a'), (err: unknown) => err instanceof NetError && err.code === 'INVALID_URL')
    await assert.rejects(request('https://example.com', { method: 'POST', body: 'a', json: { b: 1 } }), TypeError)
    await assert.rejects(request('https://example.com', { method: 'GET', body: 'x' }), TypeError)
    await assert.rejects(
        http.get('https://example.com', { signal: AbortSignal.abort() }),
        (err: unknown) => err instanceof NetError && err.code === 'ABORTED',
    )

    const prevGlobal = globalThis.__CANTON_NATIVE__
    let capturedUrl = ''
    let capturedHeaders: readonly (readonly [string, string])[] = []
    const mock = createMockNativeTarget({
        netRequest: async (_reqId, req) => {
            capturedUrl = req.url
            capturedHeaders = req.headers
            return {
                ok: true,
                status: 201,
                url: req.url,
                contentType: 'application/json',
                headers: { 'content-type': 'application/json; charset=utf-8' },
                body: new TextEncoder().encode('{"ok":true,"n":7}'),
            }
        },
        netDownload: async (_reqId, req, destPath, onProgress) => {
            onProgress?.({ receivedBytes: 512, totalBytes: 1024 })
            onProgress?.({ receivedBytes: 1024, totalBytes: 1024 })
            return { ok: true, status: 200, downloadOk: true, url: req.url, destPath, bytesWritten: 1024 }
        },
    })
    globalThis.__CANTON_NATIVE__ = mock.target
    try {
        const res = await http.post('https://api.example.com/items', {
            query: { tag: 'a b', skip: undefined, page: 2 },
            json: { name: 'Canton' },
        })
        assert.equal(capturedUrl, 'https://api.example.com/items?tag=a%20b&page=2')
        assert.ok(capturedHeaders.some(([k, v]) => k.toLowerCase() === 'content-type' && v.includes('application/json')))
        assert.equal(res.status, 201)
        assert.equal(res.ok, true)
        assert.equal(res.text(), '{"ok":true,"n":7}')
        assert.deepEqual(res.json(), { ok: true, n: 7 })
        assert.equal(res.bytes().length, 17)

        const progressEvents: number[] = []
        const dl = await downloadFile('https://api.example.com/file.bin', {
            destPath: 'C:\\Temp\\file.bin',
            onProgress: p => progressEvents.push(p.receivedBytes),
        })
        assert.equal(dl.ok, true)
        assert.equal(dl.bytesWritten, 1024)
        assert.deepEqual(progressEvents, [512, 1024])
    } finally {
        globalThis.__CANTON_NATIVE__ = prevGlobal
    }
})
