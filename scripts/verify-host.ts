import assert from 'node:assert/strict'
import { cp, copyFile, mkdir, readFile, rm } from 'node:fs/promises'
import { resolve } from 'node:path'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession, nativeSessionStart } from './native-session.ts'
import { repoRoot } from './common.ts'
import { buildFixtureDist } from './fixture-dist.ts'

// 宿主验收：
// 1. Dist 宿主的剥离——产物里查不到开发期开关和 Live 客户端的字样（Dev 宿主里必须查得到，检查才不是空转）；
// 2. Dist 宿主对目录包、ZIP 包、字节码包各冒烟一次，以日志里的“首帧已呈现”为准；
// 3. 随包位置同时有目录包和 ZIP 包时报错。
const devOnlyMarkers = [
    'CANTON_DEV_',
    '/@canton/modules',
    'vite-hmr',
    'LiveModuleClient',
    '__CANTON_HOT_TRANSPORT__',
    '__CANTON_HOT_RECEIVE__',
    'Live 热更新失败',
    '热更新已恢复',
    'DIST FALLBACK',
    '气泡之《',
    'UI IDLE',
    'frag %llu/%llu',
    'frame %.2f ms  paint %.2f ms',
    '帧时间面板 (F9)',
    '重画区闪红 (F10)',
    '布局框框与命中高亮 (F11)',
    '快捷键请求重新加载 (F5)',
    'CANTON_DEV_CRASH_LOG',
    'StackWalk64',
]

function contains(binary: Buffer, text: string): boolean {
    return binary.includes(Buffer.from(text, 'utf8')) || binary.includes(Buffer.from(text, 'utf16le'))
}

async function launch(directory: string, expected: string): Promise<void> {
    let log = ''
    const child = nativeSessionStart({ command: resolve(directory, 'canton-host.exe'), args: [], cwd: directory, onOutput: chunk => { log += chunk.toString() } })
    let exited = false
    void child.completion.then(() => { exited = true }, () => { exited = true })
    try {
        const deadline = Date.now() + 30000
        while (!log.includes(expected)) {
            if (exited) throw new Error(`宿主提前退出：${log}`)
            if (Date.now() > deadline) throw new Error(`没有等到“${expected}”：${log}`)
            await new Promise(done => setTimeout(done, 50))
        }
    } finally {
        await child.stop()
    }
}

await withNativeSession(async () => {
    const buildDir = nativeTestDirectory()
    await configureNativeBuild(repoRoot, buildDir, 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
    await nativeCmake(['--build', buildDir, '--target', 'canton_host', 'canton_host_dev', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])
    const distHost = nativeExecutable(resolve(buildDir, 'native'), 'canton-host')
    const devHost = nativeExecutable(resolve(buildDir, 'native'), 'canton-host-dev')

    const [dist, dev] = await Promise.all([readFile(distHost), readFile(devHost)])
    for (const marker of devOnlyMarkers) {
        assert.ok(contains(dev, marker), `Dev 宿主里应当有 ${marker}，否则剥离检查没有意义`)
        assert.ok(!contains(dist, marker), `Dist 宿主里不应出现 ${marker}`)
    }
    console.log('[CantonHostVerify]', 'Dist 宿主不含开发期开关与 Live 客户端')

    const work = resolve(repoRoot, '.canton/verification/host')
    await rm(work, { recursive: true, force: true })
    const packages = await buildFixtureDist('showcase', resolve(work, 'packages'))
    const stage = async (name: string, place: (directory: string) => Promise<void>) => {
        const directory = resolve(work, name)
        await mkdir(directory, { recursive: true })
        await copyFile(distHost, resolve(directory, 'canton-host.exe'))
        await place(directory)
        return directory
    }
    await launch(await stage('directory', directory => cp(packages.js, resolve(directory, 'ui'), { recursive: true })), '首帧已呈现 dist')
    console.log('[CantonHostVerify]', '目录包冒烟通过')
    await launch(await stage('zip', directory => copyFile(packages.jsZip, resolve(directory, 'ui.zip'))), '首帧已呈现 dist')
    console.log('[CantonHostVerify]', 'ZIP 包冒烟通过')
    await launch(await stage('bytecode', directory => cp(packages.bytecode, resolve(directory, 'ui'), { recursive: true })), '首帧已呈现 dist')
    console.log('[CantonHostVerify]', '字节码包冒烟通过')
    await launch(await stage('bytecode-zip', directory => copyFile(packages.bytecodeZip, resolve(directory, 'ui.zip'))), '首帧已呈现 dist')
    console.log('[CantonHostVerify]', '字节码 ZIP 包冒烟通过')
    await launch(await stage('both', async directory => {
        await cp(packages.js, resolve(directory, 'ui'), { recursive: true })
        await copyFile(packages.jsZip, resolve(directory, 'ui.zip'))
    }), '只能保留一个')
    console.log('[CantonHostVerify]', '目录包与 ZIP 包并存时报错')
}, [nativeTestDirectory()])
