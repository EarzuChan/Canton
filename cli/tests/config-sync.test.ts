import { configRegistry, createConfigRegistry } from "../src/feature/config/ConfigRegistry.ts"
import assert from "node:assert/strict"
import { test } from "node:test"
import { readFile, readdir, rm, mkdir } from "node:fs/promises"
import { join } from "node:path"
import { fixture, write, TestSyncWizard, quietTerminal } from "./fixture.ts"
import { ProjectStateStore } from "../src/feature/project/ProjectStateStore.ts"
import { FileTransaction } from "../src/feature/transaction/FileTransaction.ts"
import type { ProjectState } from "../src/feature/project/ProjectState.ts"
import { ConfigScanner } from "../src/feature/sync/ConfigScanner.ts"
import { ConfigApplier } from "../src/feature/sync/ConfigApplier.ts"
import { ConfigWriter } from "../src/feature/sync/ConfigWriter.ts"
import { SyncService } from "../src/feature/sync/SyncService.ts"
import { FrameworkRegistryClient } from "../src/feature/framework/FrameworkRegistryClient.ts"
import { FrameworkService } from "../src/feature/framework/FrameworkService.ts"
import { ConfigResolver } from "../src/feature/sync/ConfigResolver.ts"
import { cliCompatibility } from "../src/CliMetadata.ts"
import { registryRegion, registryCluster, npmrcFile, packageJsonFile, packageNameRegion, frameworkDependencyRegion } from "../src/feature/nodejs/NodeJsStuffs.ts"
import { TextFile, JsonFile } from "../src/feature/managed/ManagedFile.ts"
import { TextCluster } from "../src/feature/managed/TextCluster.ts"
import { TextRegion } from "../src/feature/managed/TextRegion.ts"
import { JsonRegion } from "../src/feature/managed/JsonRegion.ts"
import { readSnapshot, type FileSnapshot } from "../src/util/FileUtils.ts"

const store = new ProjectStateStore(new FileTransaction())
const scanner = new ConfigScanner(configRegistry)
const managedItems = configRegistry.items
const managedFiles = configRegistry.files
class TestRegistryClient extends FrameworkRegistryClient {
    override async fetchCandidateByVersion(version: string) {
        return { version, cliCompatibility, markedLatest: false, publishedAt: null }
    }
}

function run(syncWizard: TestSyncWizard, registry: FrameworkRegistryClient = new TestRegistryClient(), projectStore?: ProjectStateStore, signal = new AbortController().signal): SyncService {
    const writer = new ConfigWriter(signal)
    return new SyncService(projectStore ?? new ProjectStateStore(new FileTransaction(signal)), new FrameworkService(registry), new ConfigScanner(configRegistry), new ConfigResolver(writer, syncWizard, signal), new ConfigApplier(writer), { run: async () => ({ status: "blocked" }) }, syncWizard, signal, quietTerminal().tasks)
}

async function noJournal(root: string) { await assert.rejects(readdir(join(root, ".canton")), { code: "ENOENT" }) }

test("同源生成后全部 Idle；检查本身无副作用", async t => {
    const state = await fixture(t)
    const report = await scanner.scan(state, "Global")
    assert.equal(report.fatal.length + report.resolvable.length + report.applicable.length, 0)
    assert.equal(report.idle.filter(item => item.target.region).length, managedItems.reduce((count, item) => count + item.regions.length, 0))
    await noJournal(state.rootDir)
})

test("每轮 Resolve 只处理一项，文件创建后重扫，最终 Apply 后不复检", async t => {
    const state = await fixture(t, false)
    const ui = new TestSyncWizard()
    ui.onChoose = async () => "create"
    const result = await run(ui).run({ configOnly: true }, state.rootDir)
    assert.equal(result.status, "completed")
    assert.equal(ui.choices, 2)
    assert.equal(ui.reports.length, 3)
    assert.deepEqual(ui.reports.map(report => report.resolvable.length), [2, 1, 0])
    for (const file of managedFiles) assert.equal(await readFile(file.path(state), "utf8"), file.make(state))
    const journals = await readdir(join(state.rootDir, ".canton", "transactions"))
    assert.equal(journals.length, 0)
    assert.equal(await readFile(join(state.rootDir, ".canton", ".gitignore"), "utf8"), "*\n")
})

test("范围过滤仅扫描选中分支，不改变 ManagedItem 开关", async t => {
    const state = await fixture(t)
    await write(packageJsonFile.path(state), "bad json")
    const input = structuredClone(state)
    const report = await scanner.scan(state, "Native")
    assert.equal(report.fatal.length, 0)
    assert.ok(report.idle.every(entry => entry.target.file.scope === "Native"))
    assert.deepEqual(state, input)
})

test("已有内容补 Wrapper 后保留到重扫，再按配置 Apply", async t => {
    const state = await fixture(t)
    const path = npmrcFile.path(state)
    await write(path, registryCluster.wrapper.make("@canton:registry=old\n"))
    const ui = new TestSyncWizard()
    ui.onChoose = async () => "wrap"
    ui.onEdit = async () => {
        await write(path, registryCluster.wrapper.make(registryRegion.wrapper.make("@canton:registry=old\n")))
        return true
    }
    const result = await run(ui).run({ configOnly: true }, state.rootDir)
    assert.equal(result.status, "completed")
    assert.equal(ui.reports.length, 2)
    assert.equal(ui.reports[1].applicable.length, 1)
    assert.equal(await readFile(path, "utf8"), npmrcFile.make(state))
})

test("Marker 建立 Region，保留父级自定义内容并删除一次性 Marker", async t => {
    const state = await fixture(t)
    const path = npmrcFile.path(state)
    await write(path, registryCluster.wrapper.make("# 自定义\n"))
    const ui = new TestSyncWizard()
    ui.onChoose = async () => "marker"
    ui.onEdit = async () => {
        await write(path, registryCluster.wrapper.make(`# 自定义\n${registryRegion.wrapper.marker}\n`))
        return true
    }
    assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "completed")
    assert.equal(await readFile(path, "utf8"), registryCluster.wrapper.make(`# 自定义\n${registryRegion.make(state)}`))
    assert.equal(ui.reports.length, 2)
})

test("交互时 YAML 改变，重载开关，不依据旧 Marker 写入", async t => {
    const state = await fixture(t)
    const path = npmrcFile.path(state)
    await write(path, registryCluster.wrapper.make(""))
    const ui = new TestSyncWizard()
    ui.onChoose = async () => "marker"
    ui.onEdit = async () => {
        state.project["managed-items"] = []
        await store.save(state)
        await write(path, registryCluster.wrapper.make(`${registryRegion.wrapper.marker}\n`))
        return true
    }
    assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "completed")
    assert.equal((await readFile(path, "utf8")).includes(registryRegion.wrapper.marker), true)
    await noJournal(state.rootDir)
})

test("JSON 缺字段确认补入，类型损坏只允许手改；取消无写入", async t => {
    const state = await fixture(t)
    const path = packageJsonFile.path(state)
    await write(path, JSON.stringify({ name: "testplugin", dependencies: 123, other: "keep" }))
    const ui = new TestSyncWizard()
    ui.onChoose = async (_issue, choices) => {
        assert.deepEqual(choices, ["edit", "abort"])
        return "abort"
    }
    assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "aborted")
    await noJournal(state.rootDir)
    await write(path, JSON.stringify({ name: "testplugin", other: "keep" }))
    const confirm = new TestSyncWizard()
    confirm.onChoose = async () => "create"
    assert.equal((await run(confirm).run({ configOnly: true }, state.rootDir)).status, "completed")
    assert.deepEqual(JSON.parse(await readFile(path, "utf8")), { name: "testplugin", other: "keep", dependencies: { "@canton/framework": state.project.framework.version } })
})

test("同一 ManagedItem 跨文本和 JSON，一支缺失不阻止另一支扫描", async t => {
    const state = await fixture(t, false)
    state.project["managed-items"] = ["both"]
    const text = new class extends TextRegion {
        readonly id = "text"
        readonly managedItemId = "both"
        protected override makeInner(): string { return "expected\n" }
    }()
    const cluster = new class extends TextCluster {
        readonly id = "cluster"
        readonly regions = [text]
        protected override makeInner(state: ProjectState): string { return text.make(state) }
    }()
    const textFile = new class extends TextFile {
        readonly id = "text"
        readonly scope = "Native"
        readonly clusters = [cluster]
        override path(state: ProjectState): string { return join(state.rootDir, "text.txt") }
        override make(state: ProjectState): string { return cluster.make(state) }
    }()
    const json = new class extends JsonRegion {
        readonly id = "json"
        readonly managedItemId = "both"
        protected readonly path = ["value"]
        protected override makeValue(): undefined { return undefined }
    }()
    const jsonFile = new class extends JsonFile {
        readonly id = "json"
        readonly scope = "UI"
        readonly regions = [json]
        override path(state: ProjectState): string { return join(state.rootDir, "data.json") }
        protected override makeContent() { return {} }
    }()
    await write(jsonFile.path(state), '{"value":null,"other":7}')
    const custom = new ConfigScanner(createConfigRegistry([textFile, jsonFile], [{ id: "both", label: "两项共同托管" }]))
    const report = await custom.scan(state, "Global")
    assert.equal(report.resolvable.length, 1)
    assert.equal(report.applicable.length, 1)
    await assert.rejects(new ConfigApplier(new ConfigWriter()).apply(state.rootDir, report), /阻塞/)
    await write(textFile.path(state), textFile.make(state))
    await new ConfigApplier(new ConfigWriter()).apply(state.rootDir, await custom.scan(state, "Global"))
    assert.deepEqual(JSON.parse(await readFile(jsonFile.path(state), "utf8")), { other: 7 })
})

test("兼容性失败为 Fatal；SETUP 的失败向上传递", async t => {
    const state = await fixture(t)
    const ui = new TestSyncWizard()
    class FailingRegistryClient extends TestRegistryClient {
        override async fetchCandidateByVersion(): Promise<never> { throw new Error("不兼容") }
    }
    const service = run(ui, new FailingRegistryClient())
    assert.equal((await service.run({ configOnly: true }, state.rootDir)).status, "blocked")
    assert.equal(ui.reports[0].fatal[0].cause, "framework-incompatible")
    assert.equal((await run(new TestSyncWizard()).run({}, state.rootDir)).status, "blocked")
    await assert.rejects(run(new TestSyncWizard()).run({ configOnly: true, setupOnly: true }, state.rootDir), /互斥/)
})

test("Marker 新建 Cluster，子 Region 重扫后才出现", async t => {
    const state = await fixture(t)
    const path = npmrcFile.path(state)
    await write(path, "# 外部文本\n")
    const ui = new TestSyncWizard()
    ui.onChoose = async () => "marker"
    ui.onEdit = async () => {
        await write(path, `# 外部文本\n${registryCluster.wrapper.marker}\n`)
        return true
    }
    assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "completed")
    assert.equal(ui.reports[0].idle.some(entry => entry.target.region === registryRegion), false)
    assert.equal(ui.reports[1].idle.some(entry => entry.target.region === registryRegion), true)
    assert.equal(await readFile(path, "utf8"), `# 外部文本\n${registryCluster.make(state)}`)
})

test("重复 Marker 和错误父级内的 Marker 均不写入", async t => {
    for (const mode of ["duplicate", "nested"] as const) {
        const state = await fixture(t)
        const path = npmrcFile.path(state)
        await write(path, registryCluster.wrapper.make(""))
        const ui = new TestSyncWizard()
        ui.onChoose = async () => ui.choices === 1 ? "marker" : "abort"
        const marker = `${registryRegion.wrapper.marker}\n`
        const content = registryCluster.wrapper.make(mode === "duplicate" ? marker + marker : `# canton:begin region:custom\n${marker}# canton:end region:custom\n`)
        ui.onEdit = async () => {
            await write(path, content)
            return true
        }
        assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "aborted")
        assert.equal(await readFile(path, "utf8"), content)
        assert.ok(ui.messages.length)
        await noJournal(state.rootDir)
    }
})

test("交互修复损坏 Wrapper 后全量重扫；未修好仍为同一 Issue", async t => {
    const state = await fixture(t)
    const path = npmrcFile.path(state)
    await write(path, registryCluster.wrapper.make(`${registryRegion.wrapper.begin}\n`))
    const ui = new TestSyncWizard()
    ui.onChoose = async (issue, choices) => {
        assert.equal(issue.cause, "damaged")
        assert.deepEqual(choices, ["edit", "abort"])
        return "edit"
    }
    ui.onEdit = async () => {
        if (ui.choices === 2) await write(path, npmrcFile.make(state))
        return true
    }
    assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "completed")
    assert.equal(ui.reports.length, 3)
    assert.equal(ui.choices, 2)
    await noJournal(state.rootDir)
})

test("文件创建确认期间出现同名文件，拒绝覆盖", async t => {
    const state = await fixture(t)
    const path = npmrcFile.path(state)
    await rm(path)
    const ui = new TestSyncWizard()
    ui.onChoose = async () => {
        await write(path, "用户新建\n")
        return "create"
    }
    assert.equal((await run(ui).run({ configOnly: true }, state.rootDir)).status, "failed")
    assert.equal(await readFile(path, "utf8"), "用户新建\n")
    await noJournal(state.rootDir)
})

test("两套同步服务并发执行，各自使用自己的存储、registry 与交互", async t => {
    const first = await fixture(t)
    const second = await fixture(t)
    first.project.framework.nodeRegistryUrl = "https://first.invalid"
    second.project.framework.nodeRegistryUrl = "https://second.invalid"
    await store.save(first)
    await store.save(second)
    await rm(packageJsonFile.path(first))
    await rm(packageJsonFile.path(second))

    class RecordingStore extends ProjectStateStore {
        readonly roots: string[] = []
        override async deepLoad(rootDir: string) {
            this.roots.push(rootDir)
            return super.deepLoad(rootDir)
        }
    }
    class RecordingRegistry extends TestRegistryClient {
        readonly urls: (string | undefined)[] = []
        override async fetchCandidateByVersion(version: string, registryUrl?: string) {
            this.urls.push(registryUrl)
            return super.fetchCandidateByVersion(version)
        }
    }
    const firstStore = new RecordingStore(new FileTransaction())
    const secondStore = new RecordingStore(new FileTransaction())
    const firstRegistry = new RecordingRegistry()
    const secondRegistry = new RecordingRegistry()
    const firstWizard = new TestSyncWizard()
    const secondWizard = new TestSyncWizard()
    firstWizard.onChoose = async () => "create"
    secondWizard.onChoose = async () => "abort"
    const firstService = run(firstWizard, firstRegistry, firstStore)
    const secondService = run(secondWizard, secondRegistry, secondStore)

    const results = await Promise.all([firstService.run({ configOnly: true }, first.rootDir), secondService.run({ configOnly: true }, second.rootDir)])
    assert.deepEqual(results.map(result => result.status), ["completed", "aborted"])
    assert.deepEqual(firstStore.roots, [first.rootDir, first.rootDir])
    assert.deepEqual(secondStore.roots, [second.rootDir])
    assert.deepEqual(firstRegistry.urls, ["https://first.invalid", "https://first.invalid"])
    assert.deepEqual(secondRegistry.urls, ["https://second.invalid"])
    assert.equal(firstWizard.choices, 1)
    assert.equal(secondWizard.choices, 1)
    assert.equal(firstWizard.reports.length, 2)
    assert.equal(secondWizard.reports.length, 1)
    assert.equal(await readFile(packageJsonFile.path(first), "utf8"), packageJsonFile.make(first))
    await assert.rejects(readFile(packageJsonFile.path(second)), { code: "ENOENT" })
    await noJournal(second.rootDir)
})

test("Resolve 选择返回之前取消，不创建文件或继续 Apply", async t => {
    const state = await fixture(t)
    await rm(packageJsonFile.path(state))
    const controller = new AbortController()
    const ui = new TestSyncWizard()
    ui.onChoose = async () => {
        controller.abort()
        return "create"
    }
    await assert.rejects(run(ui, new TestRegistryClient(), undefined, controller.signal).run({ configOnly: true }, state.rootDir), error => error === controller.signal.reason)
    assert.equal(ui.choices, 1)
    assert.deepEqual(ui.messages, [])
    assert.deepEqual(ui.failures, [])
    await assert.rejects(readFile(packageJsonFile.path(state)), { code: "ENOENT" })
    await noJournal(state.rootDir)
})
