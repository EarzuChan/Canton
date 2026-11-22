import { resolve } from 'node:path'
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from './native-toolchain.ts'
import { withNativeSession, nativeSessionRun as run } from "./native-session.ts"
import { repoRoot } from "./common.ts"

await withNativeSession(async () => {
    const buildDir = nativeTestDirectory()
    const smokeExe = nativeExecutable(resolve(buildDir, 'native-tests'), 'canton_quickjs_app_smoke')
    const appBundle = resolve(buildDir, 'native-tests/fixtures/app-smoke/app.js')

    await configureNativeBuild(repoRoot, buildDir, "Debug", ["-DCANTON_BUILD_TESTS=ON", "-DCANTON_BUILD_FIXTURE_TESTS=ON"])
    await nativeCmake(["--build", buildDir, "--target", "canton_quickjs_app_smoke", "--parallel", "4"])

    await run(smokeExe, [appBundle])
    await run(smokeExe, [appBundle, '--expect-strict-modifier-ok', '{ elements: [] }'])
    for (const expression of ["{ elements: 'not-array' }", "{ elements: [{ type: 'unknownModifier', value: {} }] }"]) {
        await run(smokeExe, [appBundle, '--expect-strict-modifier-error', expression])
    }
    await run(smokeExe, [appBundle, '--expect-script-diagnostics'])
    await run(smokeExe, [appBundle, '--expect-script-diagnostics-rejection'])

    // 从 .cta 夹具经 UI 包、QuickJS 进 core 的端到端用例
    for (const [target, fixture] of [['canton_layout_boundary', 'layout'], ['canton_lazy_runtime', 'lazy'], ['canton_rearrange_runtime', 'rearrange-runtime'], ['canton_viewport_runtime', 'viewport-runtime']] as const) {
        await nativeCmake(["--build", buildDir, "--target", target, "--parallel", "4"])
        await run(nativeExecutable(resolve(buildDir, 'native-tests'), target), [resolve(buildDir, 'native-tests/fixtures', fixture)])
    }

}, [nativeTestDirectory()])
