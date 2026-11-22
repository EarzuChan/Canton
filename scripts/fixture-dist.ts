import { createHash } from 'node:crypto'
import { copyFile, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession, nativeSessionRun as run, nativeSessionStart } from './native-session.ts'
import { repoRoot } from './common.ts'
import { stageUiPackage, verifyUiBuildInput, verifyUiPackage } from '../cli/src/feature/packing/UiPackage.ts'
import { uiManifestName } from '../cli/src/CliMetadata.ts'
import { readCantonVersionContract } from './version-contract.ts'

// 把夹具构建成 Dist 包：js 与字节码两种脚本表示，各出目录包和 ZIP 包。
// 用法：pnpm fixture:dist [夹具名，默认 showcase] [--out <目录>]
// 产物：<out>/<夹具>-js/、<夹具>-js.zip、<夹具>-bytecode/、<夹具>-bytecode.zip
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
    const outIndex = args.indexOf('--out')
    const out = resolve(outIndex >= 0 ? args[outIndex + 1]! : resolve(repoRoot, '.canton/verification/dist'))
    const fixture = args.find((argument, index) => !argument.startsWith('--') && index !== outIndex + 1) ?? 'showcase'
    await withNativeSession(async () => {
        await configureNativeBuild(repoRoot, nativeTestDirectory(), 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
        const result = await buildFixtureDist(fixture, out)
        for (const [kind, path] of Object.entries(result)) console.log('[CantonFixtureDist]', kind, path)
    }, [nativeTestDirectory()])
}
