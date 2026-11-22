import { spawn } from 'node:child_process'
import { resolve } from 'node:path'
import { createFixtureServer } from './fixture-server.ts'
import { NativeReadiness } from '../cli/src/feature/platform/NativeReadiness.ts'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession } from './native-session.ts'
import { repoRoot } from './common.ts'

// 开窗跑夹具：起 Vite DevServer，按正式的加载规矩经 CANTON_DEV_* 拉起 Dev 宿主，弹出真窗口；改夹具的 .cta 会热更新。
// 用法：pnpm fixture:window [夹具名，默认 showcase] [--source lfd|live|dist] [--dist <目录或 ZIP>] [--exit-after-ready]
// 关掉窗口或按 Ctrl+C 结束。
const args = process.argv.slice(2)
const option = (name: string) => {
    const index = args.indexOf(name)
    return index >= 0 ? args[index + 1] : undefined
}
const valued = new Set(['--source', '--dist'])
const fixture = args.find((argument, index) => !argument.startsWith('--') && !valued.has(args[index - 1] ?? '')) ?? 'showcase'
const source = option('--source') ?? 'lfd'
if (!['lfd', 'live', 'dist'].includes(source)) throw new Error('--source 只接受 lfd、live 或 dist')
await withNativeSession(async () => {
    const buildDir = nativeTestDirectory()
    await configureNativeBuild(repoRoot, buildDir, 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
    await nativeCmake(['--build', buildDir, '--target', 'canton_host_dev', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])
}, [nativeTestDirectory()])

// dist 策略不连 DevServer，也就不起它
const server = source === 'dist' ? undefined : await createFixtureServer(fixture)
await server?.listen()
const url = server?.resolvedUrls?.local[0]
const readiness = await NativeReadiness.open()
const environment: NodeJS.ProcessEnv = { ...process.env, ...readiness.environment, CANTON_DEV_SOURCE: source }
if (url) environment.CANTON_DEV_SERVER = url
const dist = option('--dist')
if (dist) environment.CANTON_DEV_DIST = resolve(dist)
console.log('[CantonFixtureWindow]', `夹具 ${fixture}，策略 ${source}${url ? `，DevServer ${url}` : ''}`)

// 不走带 windowsHide 的执行器：窗口要真的弹出来
const host = spawn(nativeExecutable(resolve(nativeTestDirectory(), 'native'), 'canton-host-dev'), [], { env: environment, stdio: ['ignore', 'inherit', 'inherit'] })
const exited = new Promise<number>(done => host.once('exit', code => done(code ?? 1)))
const abort = new AbortController()
const stop = () => {
    abort.abort()
    host.kill()
}
process.once('SIGINT', stop)
process.once('SIGTERM', stop)
try {
    const ready = readiness.wait(abort.signal).then(() => 'ready' as const, () => 'aborted' as const)
    const first = await Promise.race([ready, exited])
    if (first === 'ready') {
        console.log('[CantonFixtureWindow]', '首帧已就绪')
        if (args.includes('--exit-after-ready')) host.kill()
    } else if (typeof first === 'number') console.log('[CantonFixtureWindow]', `宿主在首帧前退出，退出码 ${first}`)
    process.exitCode = first === 'ready' ? 0 : 1
    await exited
} finally {
    abort.abort()
    await readiness.close()
    await server?.close()
}
