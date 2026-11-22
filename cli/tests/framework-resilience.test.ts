import assert from "node:assert/strict"
import { createServer as createHttpServer } from "node:http"
import { createServer as createTcpServer } from "node:net"
import { join } from "node:path"
import { test } from "node:test"
import { cliCompatibility, frameworkPackageName } from "../src/CliMetadata.ts"
import { configRegistry } from "../src/feature/config/ConfigRegistry.ts"
import { RegistryUnreachableError } from "../src/feature/framework/FrameworkErrors.ts"
import { FrameworkRegistryClient } from "../src/feature/framework/FrameworkRegistryClient.ts"
import { FrameworkService } from "../src/feature/framework/FrameworkService.ts"
import { ProjectStateStore } from "../src/feature/project/ProjectStateStore.ts"
import { ConfigApplier } from "../src/feature/sync/ConfigApplier.ts"
import { ConfigResolver } from "../src/feature/sync/ConfigResolver.ts"
import { ConfigScanner } from "../src/feature/sync/ConfigScanner.ts"
import { ConfigWriter } from "../src/feature/sync/ConfigWriter.ts"
import { SyncService } from "../src/feature/sync/SyncService.ts"
import { FileTransaction } from "../src/feature/transaction/FileTransaction.ts"
import { fixture, TestSyncWizard, write, quietTerminal } from "./fixture.ts"

// 一个刚关闭的本机端口：连接它必然被拒绝（ECONNREFUSED），不依赖外部网络
async function closedPort(): Promise<number> {
    const server = createTcpServer()
    await new Promise<void>(resolve => server.listen(0, "127.0.0.1", resolve))
    const port = (server.address() as { port: number }).port
    await new Promise<void>(resolve => server.close(() => resolve()))
    return port
}

test("连不上 registry 归为 RegistryUnreachableError，带上地址与真实原因而不是 fetch failed", async () => {
    const registry = `http://127.0.0.1:${await closedPort()}`
    const client = new FrameworkRegistryClient()
    for (const call of [() => client.fetchCandidateByVersion("0.0.0-m.3.0", registry), () => client.fetchCandidates(5, registry)]) {
        await assert.rejects(call(), error => {
            assert.ok(error instanceof RegistryUnreachableError)
            assert.equal(error.registry, registry)
            assert.match(error.message, new RegExp(registry.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")))
            assert.match(error.message, /ECONNREFUSED/)
            assert.doesNotMatch(error.message, /^fetch failed/)
            return true
        })
    }
})

test("registry 可达但给出否定答案（HTTP 404）不算连不上", async t => {
    const server = createHttpServer((_request, response) => { response.writeHead(404).end("{}") })
    await new Promise<void>(resolve => server.listen(0, "127.0.0.1", resolve))
    t.after(() => new Promise<void>(resolve => server.close(() => resolve())))
    const registry = `http://127.0.0.1:${(server.address() as { port: number }).port}`
    await assert.rejects(new FrameworkRegistryClient().fetchCandidateByVersion("9.9.9", registry), error => {
        assert.ok(!(error instanceof RegistryUnreachableError))
        assert.match((error as Error).message, /HTTP 404/)
        return true
    })
})

test("用户取消仍以 AbortError 传出，不被当成连不上", async () => {
    const controller = new AbortController()
    controller.abort()
    await assert.rejects(new FrameworkRegistryClient(controller.signal).fetchCandidateByVersion("0.0.0-m.3.0", `http://127.0.0.1:${await closedPort()}`), error => (error as Error).name === "AbortError")
})

class UnreachableRegistry extends FrameworkRegistryClient {
    calls = 0
    override async fetchCandidateByVersion(): Promise<never> {
        this.calls++
        throw new RegistryUnreachableError("http://registry.example.invalid", "ECONNREFUSED")
    }
}

class IncompatibleRegistry extends FrameworkRegistryClient {
    override async fetchCandidateByVersion(version: string) {
        return { version, cliCompatibility: cliCompatibility + 1, markedLatest: false, publishedAt: null }
    }
}

function runSync(registry: FrameworkRegistryClient, wizard: TestSyncWizard): SyncService {
    const signal = new AbortController().signal
    const writer = new ConfigWriter(signal)
    return new SyncService(new ProjectStateStore(new FileTransaction(signal)), new FrameworkService(registry), new ConfigScanner(configRegistry), new ConfigResolver(writer, wizard, signal), new ConfigApplier(writer), { run: async () => ({ status: "blocked" }) }, wizard, signal, quietTerminal().tasks)
}

test("sync：连不上 registry 且本机没有已安装版本 → Fatal framework-unverifiable，说明地址、原因和缺少的离线依据", async t => {
    const state = await fixture(t)
    const registry = new UnreachableRegistry()
    const wizard = new TestSyncWizard()
    const result = await runSync(registry, wizard).run({ configOnly: true }, state.rootDir)
    assert.equal(result.status, "blocked")
    const fatal = wizard.reports[0].fatal.find(item => item.cause === "framework-unverifiable")
    assert.ok(fatal, JSON.stringify(wizard.reports[0].fatal))
    assert.match(fatal.message, /registry\.example\.invalid/)
    assert.match(fatal.message, /ECONNREFUSED/)
    assert.match(fatal.message, /没有可用于离线验证/)
    assert.equal(wizard.reports[0].fatal.some(item => item.cause === "framework-incompatible"), false)
})

test("sync：registry 明确给出不兼容 → 仍是 framework-incompatible", async t => {
    const state = await fixture(t)
    const wizard = new TestSyncWizard()
    await runSync(new IncompatibleRegistry(), wizard).run({ configOnly: true }, state.rootDir)
    assert.equal(wizard.reports[0].fatal.some(item => item.cause === "framework-incompatible"), true)
    assert.equal(wizard.reports[0].fatal.some(item => item.cause === "framework-unverifiable"), false)
})

test("sync：本机已安装匹配版本时优先使用，registry 不可达也不阻塞，且根本不联网", async t => {
    const state = await fixture(t)
    await write(join(state.rootDir, state.project.ui.directory, "node_modules", frameworkPackageName, "package.json"), JSON.stringify({ version: state.project.framework.version, canton: { cliCompatibility } }))
    const registry = new UnreachableRegistry()
    const wizard = new TestSyncWizard()
    const result = await runSync(registry, wizard).run({ configOnly: true }, state.rootDir)
    assert.equal(result.status, "completed")
    assert.equal(wizard.reports[0].fatal.length, 0)
    assert.equal(registry.calls, 0)
})
