import assert from "node:assert/strict"
import { cp, mkdir, mkdtemp, rm } from "node:fs/promises"
import { tmpdir } from "node:os"
import { isAbsolute, join, relative, resolve, sep } from "node:path"
import type { TestContext } from "node:test"
import { Executor } from "../src/feature/platform/Executor.ts"
import { createPlatformService } from "../src/feature/platform/CreatePlatformService.ts"
import type { LocalDefinition, ProjectState } from "../src/feature/project/ProjectState.ts"
import { fixture, testTask, write } from "./fixture.ts"

export const hostArchitecture = process.arch === "arm64" ? "arm64" : "x64"

let discovery: Promise<{ local: LocalDefinition, env: Record<string, string> }> | undefined

// 本平台缺少工具链就是未完成验证；不能通过跳过伪装成验收成功
export function nativeTools(): Promise<{ local: LocalDefinition, env: Record<string, string> }> {
    return discovery ??= (async () => {
        const executor = new Executor()
        const platform = createPlatformService(executor)
        const found = await platform.discoverNative({})
        assert.deepEqual(found.issues, [], "本机原生工具链不可用，请准备 MSVC、CMake 和 Ninja")
        const env = { ...await platform.nativeEnvironment(found.local), VSLANG: "1033" }
        for (const key of ["cmake", "ninja"] as const) {
            assert.ok(found.local[key]?.path, `未发现 ${key}`)
            const result = await executor.run({ command: found.local[key]!.path, args: ["--version"], env })
            assert.equal(result.exitCode, 0, result.stdout + result.stderr)
        }
        return { local: found.local, env }
    })()
}

export async function nativeState(t: Pick<TestContext, "after">): Promise<ProjectState> {
    const state = await fixture(t, false)
    const { local } = await nativeTools()
    state.local = structuredClone(local)
    const root = await mkdtemp(join(tmpdir(), "a"))
    t.after(() => rm(root, { recursive: true, force: true }))
    state.local.native!.buildRoot = root
    return state
}

export async function cmakeRun(args: readonly string[], cwd?: string, env?: Record<string, string>) {
    const tools = await nativeTools()
    return new Executor().run({ command: tools.local.cmake!.path, args, cwd, env: { ...tools.env, ...env } })
}

export async function configureNative(source: string, build: string, definitions: readonly string[] = []) {
    const { local } = await nativeTools()
    return cmakeRun(["-S", source, "-B", build, "-G", "Ninja", `-DCMAKE_MAKE_PROGRAM=${local.ninja!.path}`, "-DCMAKE_BUILD_TYPE=Release", ...definitions])
}
