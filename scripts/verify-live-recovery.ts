import { resolve } from 'node:path'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession, nativeSessionStart } from './native-session.ts'
import { repoRoot } from './common.ts'
import { buildFixtureDist } from './fixture-dist.ts'
import { createFixtureServer } from './fixture-server.ts'

// Live 的断线与恢复：Dev 宿主以 lfd 策略启动，DevServer 先不在 → 回退 Dist 并提示；
// 起 DevServer → 自动切到 Live；停 DevServer → 再回退 Dist；重启 DevServer → 再切回 Live。全程不重启宿主
const port = 9000 + Math.floor(Math.random() * 500)

await withNativeSession(async () => {
    const buildDir = nativeTestDirectory()
    await configureNativeBuild(repoRoot, buildDir, 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
    await nativeCmake(['--build', buildDir, '--target', 'canton_host_dev', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])
    const packages = await buildFixtureDist('showcase', resolve(repoRoot, '.canton/verification/live-recovery'))

    let log = '', cursor = 0, exited = false
    const child = nativeSessionStart({
        command: nativeExecutable(resolve(buildDir, 'native'), 'canton-host-dev'), args: [],
        env: { CANTON_DEV_SOURCE: 'lfd', CANTON_DEV_SERVER: `http://127.0.0.1:${port}`, CANTON_DEV_DIST: packages.js },
        onOutput: chunk => { log += chunk.toString() },
    })
    void child.completion.then(() => { exited = true }, () => { exited = true })
    const waitFor = async (text: string) => {
        const deadline = Date.now() + 30000
        while (true) {
            const found = log.indexOf(text, cursor)
            if (found >= 0) {
                cursor = found + text.length
                return
            }
            if (exited) throw new Error(`宿主提前退出：${log}`)
            if (Date.now() > deadline) throw new Error(`没有等到“${text}”：${log.slice(cursor)}`)
            await new Promise(done => setTimeout(done, 50))
        }
    }
    let server: Awaited<ReturnType<typeof createFixtureServer>> | undefined
    try {
        await waitFor('已回退到 Dist')
        await waitFor('首帧已呈现 dist')
        console.log('[CantonLiveRecovery]', 'DevServer 不在：回退到 Dist 并提示')
        for (const round of ['首次', '重启后']) {
            server = await createFixtureServer('showcase', port)
            await server.listen()
            await waitFor('Live App 已启动')
            console.log('[CantonLiveRecovery]', `DevServer ${round}起来：自动切到 Live`)
            await server.close()
            server = undefined
            await waitFor('已回退到 Dist')
            console.log('[CantonLiveRecovery]', 'DevServer 停掉：回退到 Dist 并提示')
        }
    } finally {
        await server?.close()
        await child.stop()
    }
    console.log('[CantonLiveRecovery]', 'lfd 的回退与自动恢复全部通过')
}, [nativeTestDirectory()])
