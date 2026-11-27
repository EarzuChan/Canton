import { spawn } from 'node:child_process'
import { createHash } from 'node:crypto'
import { copyFile, cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession, nativeSessionRun as run, nativeSessionStart } from './native-session.ts'
import { repoRoot } from './common.ts'
import { stageUiPackage, verifyUiBuildInput, verifyUiPackage } from '../cli/src/feature/packing/UiPackage.ts'
import { uiManifestName } from '../cli/src/CliMetadata.ts'
import { readCantonVersionContract } from './version-contract.ts'

// 把夹具构建成 Dist 包与可运行应用目录（canton-host.exe + ui.zip / ui/），并默认直接拉起窗口（与 fixture:live 对仗）。
// 用法：pnpm fixture:dist [夹具名，默认 showcase] [--out <目录>] [--format bytecode-zip|bytecode|js-zip|js] [--build-only]
// 产物：<out>/<夹具>-js/、<夹具>-js.zip、<夹具>-bytecode/、<夹具>-bytecode.zip 与 <out>/<夹具>-app/（内含 canton-host.exe + ui.zip 或 ui/）
export async function buildFixtureDist(fixture: string, out: string): Promise<{ js: string, jsZip: string, bytecode: string, bytecodeZip: string }> {
    const framework = readCantonVersionContract().frameworkVersion
    const buildDir = nativeTestDirectory()
    const stage = resolve(out, '.stage')
    await rm(stage, { recursive: true, force: true })
    const builder = resolve(repoRoot, 'scripts/build-runtime-fixtures.ts')
    const result = { js: resolve(out, `${fixture}-js`), jsZip: resolve(out, `${fixture}-js.zip`), bytecode: resolve(out, `${fixture}-bytecode`), bytecodeZip: resolve(out, `${fixture}-bytecode.zip`) }
    for (const path of Object.values(result)) await rm(path, { recursive: true, force: true })

    // js：Vite 的输出本身就是一个目录包
    await run(process.execPath, ['--import', 'tsx', builder, resolve(stage, 'js'), fixture])
    const js = await verifyUiPackage(resolve(stage, 'js', fixture), framework)
    await stageUiPackage(resolve(stage, 'js', fixture), result.js, framework, 'directory', js.digest)
    await stageUiPackage(resolve(stage, 'js', fixture), result.jsZip, framework, 'zip', js.digest)

    // 字节码：先出以 .cbc 命名的 ESM，再用与宿主同一份引擎的 canton_ui_bytecode 编译
    const esm = resolve(stage, 'esm', fixture)
    await run(process.execPath, ['--import', 'tsx', builder, resolve(stage, 'esm'), fixture], { ...process.env, CANTON_UI_BUILD_STAGE: esm, CANTON_UI_BUILD_SCRIPT_FORMAT: 'bytecode' })
    const { manifest, entries } = await verifyUiBuildInput(esm, framework, 'bytecode')
    const assembled = resolve(stage, 'bytecode')
    await mkdir(assembled, { recursive: true })
    const modules = resolve(stage, 'modules.txt')
    await writeFile(modules, entries.filter(row => row.kind === 'module').map(row => row.path).join('\n') + '\n')
    await nativeCmake(['--build', buildDir, '--target', 'canton_ui_bytecode', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])
    let output = ''
    const compiled = await nativeSessionStart({ command: nativeExecutable(resolve(buildDir, 'native'), 'canton_ui_bytecode'), args: [esm, assembled, modules], onOutput: (chunk, channel) => { if (channel === 'stdout') output += chunk.toString(); else process.stderr.write(chunk) } }).completion
    if (compiled.exitCode !== 0) throw new Error(`canton_ui_bytecode 失败，退出码 ${compiled.exitCode}`)
    const engine = output.trim().split(/\r?\n/).at(-1)!
    for (const row of entries) {
        if (row.kind === 'module') {
            const bytes = await readFile(join(assembled, row.path))
            row.size = bytes.length
            row.sha256 = createHash('sha256').update(bytes).digest('hex')
        } else {
            await mkdir(dirname(join(assembled, row.path)), { recursive: true })
            await copyFile(join(esm, row.path), join(assembled, row.path))
        }
    }
    await writeFile(join(assembled, uiManifestName), JSON.stringify({ ...manifest, scriptFormat: 'bytecode', engine, entries }) + '\n')
    const bytecode = await verifyUiPackage(assembled, framework)
    await stageUiPackage(assembled, result.bytecode, framework, 'directory', bytecode.digest)
    await stageUiPackage(assembled, result.bytecodeZip, framework, 'zip', bytecode.digest)
    await rm(stage, { recursive: true, force: true })
    return result
}

if (import.meta.filename === resolve(process.argv[1] ?? '')) {
    const args = process.argv.slice(2)
    const option = (name: string) => {
        const idx = args.indexOf(name)
        return idx >= 0 ? args[idx + 1] : undefined
    }
    const valued = new Set(['--out', '--format'])
    const out = resolve(option('--out') ?? resolve(repoRoot, '.canton/verification/dist'))
    const format = option('--format') ?? 'bytecode-zip'
    if (!['bytecode-zip', 'bytecode', 'js-zip', 'js'].includes(format)) {
        throw new Error('--format 只接受 bytecode-zip、bytecode、js-zip 或 js')
    }
    const buildOnly = args.includes('--build-only')
    const fixture = args.find((argument, index) => !argument.startsWith('--') && !valued.has(args[index - 1] ?? '')) ?? 'showcase'

    let appExe = ''
    await withNativeSession(async () => {
        const buildDir = nativeTestDirectory()
        await configureNativeBuild(repoRoot, buildDir, 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
        const result = await buildFixtureDist(fixture, out)
        await nativeCmake(['--build', buildDir, '--target', 'canton_host', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])

        // 组装可直接运行的 Dist 应用目录：<out>/<fixture>-app/{canton-host.exe, ui.zip 或 ui/}
        const appDir = resolve(out, `${fixture}-app`)
        await rm(appDir, { recursive: true, force: true })
        await mkdir(appDir, { recursive: true })
        appExe = join(appDir, 'canton-host.exe')
        await copyFile(nativeExecutable(resolve(buildDir, 'native'), 'canton-host'), appExe)
        if (format === 'bytecode-zip' || format === 'js-zip') {
            await copyFile(format === 'bytecode-zip' ? result.bytecodeZip : result.jsZip, join(appDir, 'ui.zip'))
        } else {
            await cp(format === 'bytecode' ? result.bytecode : result.js, join(appDir, 'ui'), { recursive: true })
        }

        for (const [kind, path] of Object.entries(result)) console.log('[CantonFixtureDist]', kind, path)
        console.log('[CantonFixtureDist]', 'app', appDir)
        console.log('[CantonFixtureDist]', 'exe', appExe)
    }, [nativeTestDirectory()])

    if (!buildOnly) {
        console.log('[CantonFixtureDist]', `正在启动 Dist 应用：${appExe}`)
        const host = spawn(appExe, [], { cwd: dirname(appExe), stdio: ['ignore', 'inherit', 'inherit'] })
        const exited = new Promise<number>(done => host.once('exit', code => done(code ?? 1)))
        let stoppedBySignal = false
        const stop = () => {
            stoppedBySignal = true
            host.kill()
        }
        process.once('SIGINT', stop)
        process.once('SIGTERM', stop)
        const finalCode = await exited
        if (!stoppedBySignal && finalCode !== 0) {
            console.error('[CantonFixtureDist]', `Dist 宿主异常退出，退出码 0x${(finalCode >>> 0).toString(16).toUpperCase()}`)
            process.exitCode = 1
        }
    }
}
