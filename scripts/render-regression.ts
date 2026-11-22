import { resolve } from 'node:path'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession, nativeSessionRun as run } from './native-session.ts'
import { repoRoot } from './common.ts'

// 无头回归：构建回归程序和夹具，离屏渲染后与 native-tests/baselines 里的基线图比对。
// 基线只在带 --update 时写入；其余参数原样交给回归程序（如 --only <用例>）
const update = process.argv.includes('--update')
const passthrough = process.argv.slice(2).filter(argument => argument !== '--update' && argument !== '--clean')

await withNativeSession(async () => {
    const buildDir = nativeTestDirectory()
    const fixtures = resolve(buildDir, 'native-tests/fixtures')
    await configureNativeBuild(repoRoot, buildDir, 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
    await nativeCmake(['--build', buildDir, '--target', 'canton_render_regression', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])
    await run(process.execPath, ['--import', 'tsx', resolve(repoRoot, 'scripts/build-runtime-fixtures.ts'), fixtures, 'showcase'])
    await run(nativeExecutable(resolve(buildDir, 'native-tests'), 'canton_render_regression'), [
        '--baselines', resolve(repoRoot, 'native-tests/baselines'),
        '--output', resolve(repoRoot, '.canton/verification/render'),
        '--fixtures', fixtures,
        ...(update ? ['--update'] : []),
        ...passthrough,
    ])
}, [nativeTestDirectory()])
