import { dirname, join, resolve } from "node:path"
import { mkdir, realpath } from "node:fs/promises"
import { Executor } from "../cli/src/feature/platform/Executor.ts"
import { createPlatformService } from "../cli/src/feature/platform/CreatePlatformService.ts"
import { locateTool } from "../cli/src/feature/platform/ToolLocator.ts"
import { windowsObjectPathMax } from "../cli/src/CliMetadata.ts"
import { repoRoot } from "./common.ts"
import { NativeBuildTree } from "../cli/src/feature/cmake/NativeBuildTree.ts"
import { cachedNativeDefinitions } from "./native-source-cache.ts"
import { BuildReferences } from "../cli/src/feature/source/BuildReferences.ts"
import { claimNativeVerification, nativeSessionRun, currentNativeSession } from "./native-session.ts"

let discovery: ReturnType<typeof discover> | undefined

async function discover() {
    const executor = new Executor()
    const platform = createPlatformService(executor)
    const found = await platform.discoverNative({
        ...(process.env.CMAKE_EXE ? { cmake: { path: process.env.CMAKE_EXE } } : {}),
        ...(process.env.NINJA_EXE ? { ninja: { path: process.env.NINJA_EXE } } : {}),
        native: { generator: "Ninja", architecture: process.arch === "arm64" ? "arm64" : "x64", ...(process.env.VSDEVCMD ? { developerCommand: process.env.VSDEVCMD } : {}) },
    })
    if (found.issues.length) throw new Error(`本机原生工具链未就绪：${found.issues.map(issue => issue.message).join("；")}`)
    const env: NodeJS.ProcessEnv = { ...process.env, ...await platform.nativeEnvironment(found.local), VSLANG: "1033" }
    // PATH 的大小写来自系统，不保留两份 Windows 环境变量
    for (const key of Object.keys(env)) if (key !== "PATH" && key.toUpperCase() === "PATH") {
        env.PATH = env[key]
        delete env[key]
    }
    const cmake = found.local.cmake?.path
    const ninja = found.local.ninja?.path
    const ctest = cmake ? await locateTool(join(dirname(cmake), "ctest.exe"), env) ?? await locateTool("ctest", env) : undefined
    if (!cmake || !ninja || !ctest) throw new Error("本机原生验收需要 CMake、CTest 和 Ninja")
    return { local: found.local, platform: platform.name, cmake, ninja, ctest, env }
}

export function nativeToolchain() { return discovery ??= discover() }

export function nativeExecutable(directory: string, name: string): string { return resolve(directory, `${name}.exe`) }

export function nativeTestDirectory(flavor: "Debug" | "Release" = "Debug"): string {
    const configuration = `${process.arch === "arm64" ? "a" : "x"}${flavor === "Debug" ? "d" : "r"}`
    return resolve(repoRoot, process.env.CANTON_NATIVE_TEST_BUILD_DIR ?? `build/test-win-${configuration}`)
}

export async function nativeCmake(args: readonly string[]): Promise<void> {
    const tools = await nativeToolchain()
    await nativeSessionRun(tools.cmake, args, tools.env)
}

export async function configureNativeBuild(source: string, build: string, flavor: "Debug" | "Release", definitions: readonly string[] = []): Promise<void> {
    const tools = await nativeToolchain()
    const compiler = (await realpath(tools.local.nativeCompiler!.path)).replaceAll("\\", "/")
    await claimNativeVerification(build)
    const args = ["-S", resolve(source), "-B", resolve(build), "-G", "Ninja", `-DCMAKE_MAKE_PROGRAM=${tools.ninja}`, `-DCMAKE_BUILD_TYPE=${flavor}`, `-DCMAKE_CXX_COMPILER=${compiler}`, `-DCMAKE_C_COMPILER=${compiler}`, `-DCMAKE_OBJECT_PATH_MAX=${windowsObjectPathMax}`]
    const requested: Record<string, string> = {}
    for (const key of ["CANTON_QUICKJS_NG_SOURCE_DIR", "FETCHCONTENT_SOURCE_DIR_QUICKJS_NG"]) if (process.env[key]) requested[key] = process.env[key]
    for (const definition of definitions) {
        const match = definition.match(/^-D([^=]+)=(.*)$/s)
        if (!match) throw new Error(`不支持的原生验收定义：${definition}`)
        requested[match[1]!] = match[2]!
    }
    const cached = await cachedNativeDefinitions(requested)
    args.push(...Object.entries(cached).map(([key, value]) => `-D${key}=${value}`))
    const complete = await new NativeBuildTree(currentNativeSession().signal).prepare(source, build, [args, tools.local, { WindowsSdkDir: tools.env.WindowsSdkDir }], process.argv.includes("--clean"))
    const completeReferences = await new BuildReferences(resolve(build)).begin(cached, repoRoot)
    await nativeCmake(args)
    await completeReferences()
    await complete()
}

export async function configureSmokeBuild(build: string, flavor: "Debug" | "Release" = "Debug"): Promise<void> {
    await configureNativeBuild(repoRoot, build, flavor, ["-DCANTON_BUILD_TESTS=ON"])
}

export async function nativeCtest(directory: string, flavor: "Debug" | "Release"): Promise<void> {
    const tools = await nativeToolchain()
    const evidence = resolve(repoRoot, ".canton/verification", `${tools.platform}-${tools.local.native!.architecture}`)
    await mkdir(evidence, { recursive: true })
    // 运行序列包含窗口和全局输入；保持串行，避免本机系统资源互相污染
    await nativeSessionRun(tools.ctest, ["--test-dir", directory, "-C", flavor, "--output-on-failure", "--no-tests=error", "--output-junit", resolve(evidence, `native-${flavor.toLowerCase()}.xml`)], tools.env)
    console.log("[CantonNativeTest]", `${tools.platform}/${tools.local.native!.architecture} ${flavor} 本机 CTest 完成`)
}
