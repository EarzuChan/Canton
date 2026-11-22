import assert from "node:assert/strict"
import { readFile, readdir, rm } from "node:fs/promises"
import { resolve } from "node:path"
import { test, type TestContext } from "node:test"
import type { CmakeService } from "../src/feature/cmake/CmakeService.ts"
import type { FrameworkService } from "../src/feature/framework/FrameworkService.ts"
import type { NodeJsService } from "../src/feature/nodejs/NodeJsService.ts"
import type { ToolchainService } from "../src/feature/platform/ToolchainService.ts"
import type { BuildFlavor, LocalDefinition, ProjectState } from "../src/feature/project/ProjectState.ts"
import { ProjectStateStore } from "../src/feature/project/ProjectStateStore.ts"
import { FileTransaction } from "../src/feature/transaction/FileTransaction.ts"
import { SetupService } from "../src/feature/sync/SetupService.ts"
import type { SetupScope } from "../src/feature/sync/SetupScanReport.ts"
import type { SetupInteraction } from "../src/feature/sync/SetupInteraction.ts"
import { fixture, quietTerminal, testTask, write } from "./fixture.ts"

const local: LocalDefinition = { node: { path: "/tools/node", version: "26.5.0" }, packageManager: { path: "/tools/npm", version: "11.0.0" } }
async function preparedState(t: TestContext): Promise<ProjectState> {
    const state = await fixture(t, false)
    state.local = structuredClone(local)
    await new ProjectStateStore(new FileTransaction()).save(state)
    return state
}

async function setupHarness(t: TestContext) {
    const state = await preparedState(t)
    const store = new ProjectStateStore(new FileTransaction())
    const applied: string[] = []
    const tools = { inspect: async (input: ProjectState) => ({ local: input.local!, issues: [] }) } as unknown as ToolchainService
    const node = { inspect: async () => ({ ready: false, reason: "依赖未安装" }), install: async () => { applied.push("ui") } } as unknown as NodeJsService
    const cmake = { inspect: async () => ({ ready: false }), configure: async (_state: ProjectState, flavor: BuildFlavor) => { applied.push(flavor) } } as unknown as CmakeService
    const framework = { assertCompatible: async () => { } } as unknown as FrameworkService
    const messages: string[] = []
    const failures: string[] = []
    const wizard: SetupInteraction = {
        report: () => { },
        acceptTools: async () => { throw new Error("测试不应进入交互") },
        editTools: async () => { throw new Error("测试不应进入交互") },
        message: message => { messages.push(message) },
        failure: message => { failures.push(message) },
    }
    const setup = new SetupService(store, tools, node, cmake, framework, wizard, quietTerminal().tasks, new AbortController().signal)
    return { state, store, tools, node, cmake, framework, setup, wizard, applied, messages, failures }
}

test("SETUP scan 与 --scan 仅报告动作，不保存 local、不安装或配置", async t => {
    const input = await setupHarness(t)
    const proposed = { ...local, node: { path: "/new/node", version: "26.6.0" } }
    input.tools.inspect = async () => ({ local: proposed, issues: [] })
    const before = await readdir(input.state.rootDir, { recursive: true })
    const projectText = await readFile(resolve(input.state.rootDir, "canton.project.yaml"), "utf8")
    const localText = await readFile(resolve(input.state.rootDir, "canton.local.yaml"), "utf8")
    const report = await input.setup.scan(input.state.rootDir, "Global")
    assert.deepEqual(report.applicable.map(task => task.kind), ["install-ui", "configure-native", "configure-native"])
    assert.equal(report.resolvable[0].key, "local-tools")
    assert.equal((await input.setup.run(input.state.rootDir, "Global", true)).status, "blocked")
    assert.deepEqual(input.applied, [])
    assert.deepEqual(await readdir(input.state.rootDir, { recursive: true }), before)
    assert.equal(await readFile(resolve(input.state.rootDir, "canton.project.yaml"), "utf8"), projectText)
    assert.equal(await readFile(resolve(input.state.rootDir, "canton.local.yaml"), "utf8"), localText)
})

test("SETUP 的 Framework/UI Fatal 阻止全范围所有 APPLY 和 Resolve", async t => {
    for (const fatal of ["framework", "ui"] as const) await t.test(`阻塞来源：${fatal}`, async t => {
        const input = await setupHarness(t)
        input.tools.inspect = async () => ({ local: { ...local, node: { path: "/changed/node" } }, issues: [] })
        if (fatal === "framework") input.framework.assertCompatible = async () => { throw new Error("Framework 不兼容") }
        else input.node.inspect = async () => ({ ready: false, fatal: true, reason: "UI manifest 损坏" })
        const result = await input.setup.run(input.state.rootDir, "Global")
        assert.equal(result.status, "blocked")
        assert.equal(result.report?.fatal[0].key, fatal)
        assert.ok(result.report!.applicable.length > 0)
        assert.deepEqual(input.applied, [])
        assert.deepEqual((await input.store.load(input.state.rootDir)).local, local)
        await assert.rejects(readdir(resolve(input.state.rootDir, ".canton")), { code: "ENOENT" })
    })
})

test("SETUP 任务被取消时向命令入口传递取消，而不是报告为失败", async t => {
    const input = await setupHarness(t)
    const cancellation = new DOMException("操作已取消", "AbortError")
    input.node.install = async () => { throw cancellation }
    await assert.rejects(input.setup.run(input.state.rootDir, "Global"), error => error === cancellation)
    assert.deepEqual(input.failures, [])
})

test("SETUP 并行执行：UI 安装失败时各 flavor 的 configure 仍跑完，最后一起报告失败", async t => {
    const input = await setupHarness(t)
    let release: () => void = () => { }
    const gate = new Promise<void>(resolve => { release = resolve })
    const started: string[] = []
    input.node.install = async () => {
        started.push("ui")
        if (started.length === 3) release()
        await gate
        throw new Error("网络中断")
    }
    input.cmake.configure = (async (_state: ProjectState, flavor: BuildFlavor) => {
        started.push(flavor)
        // 三个任务都已开始才放行，证明是并行而不是依次执行
        if (started.length === 3) release()
        await gate
        input.applied.push(flavor)
    }) as unknown as CmakeService["configure"]
    const result = await input.setup.run(input.state.rootDir, "Global")
    assert.equal(result.status, "failed")
    assert.deepEqual([...started].sort(), ["debug", "release", "ui"])
    assert.deepEqual([...input.applied].sort(), ["debug", "release"])
    assert.equal(input.failures.length, 1)
    assert.match(input.failures[0]!, /安装 UI 依赖：网络中断/)
})

test("SETUP 每次 Resolve 后重载状态，安装使用确认保存后的 local", async t => {
    const input = await setupHarness(t)
    const proposed: LocalDefinition = { ...local, node: { path: "/confirmed/node", version: "26.6.0" } }
    let loads = 0
    const deepLoad = input.store.deepLoad.bind(input.store)
    input.store.deepLoad = async root => {
        loads++
        return deepLoad(root)
    }
    input.tools.inspect = async () => ({ local: proposed, issues: [] })
    let confirmations = 0
    input.wizard.acceptTools = async candidate => {
        confirmations++
        assert.deepEqual(candidate, proposed)
        assert.deepEqual(input.applied, [])
        return true
    }
    input.node.install = async state => {
        assert.ok(loads >= 2)
        assert.deepEqual(state.local, proposed)
        input.applied.push("ui")
    }
    const result = await input.setup.run(input.state.rootDir, "Global")
    assert.equal(result.status, "completed")
    assert.equal(confirmations, 1)
    assert.deepEqual([...input.applied].sort(), ["debug", "release", "ui"])
    assert.deepEqual((await input.store.load(input.state.rootDir)).local, proposed)
})

test("SETUP Resolve 确认期间 YAML 改动阻止保存，APPLY 间改动阻止后续工具", async t => {
    await t.test("确认前并发修改", async t => {
        const input = await setupHarness(t)
        input.tools.inspect = async () => ({ local: { ...local, node: { path: "/changed/node" } }, issues: [] })
        input.wizard.acceptTools = async () => {
            await write(resolve(input.state.rootDir, "canton.project.yaml"), "# 用户的新内容\n")
            return true
        }
        assert.equal((await input.setup.run(input.state.rootDir, "Global")).status, "failed")
        assert.deepEqual(input.applied, [])
        assert.equal(await readFile(resolve(input.state.rootDir, "canton.project.yaml"), "utf8"), "# 用户的新内容\n")
        assert.deepEqual((await input.store.deepLoad(input.state.rootDir)).state, null)
    })
    await t.test("执行期间并发修改", async t => {
        const input = await setupHarness(t)
        input.node.install = async () => {
            input.applied.push("ui")
            await write(resolve(input.state.rootDir, "canton.local.yaml"), "# 用户切换工具链\n")
        }
        assert.equal((await input.setup.run(input.state.rootDir, "Global")).status, "failed")
        assert.ok(input.applied.includes("ui"))
        assert.equal(input.messages.includes("SETUP 完成"), false)
    })
})
