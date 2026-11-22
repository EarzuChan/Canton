import assert from "node:assert/strict"
import { existsSync } from "node:fs"
import { test, type TestContext } from "node:test"
import { mkdtemp, mkdir, readFile, readdir, rm, symlink, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { dirname, resolve } from "node:path"
import { defaultProjectDirectories, uiToolchainVersions } from "../src/CliMetadata.ts"
import { createInitialProjectState, type CreateProjectRequest } from "../src/feature/project/CreateProject.ts"
import { ProjectInitializer, type CopyPlan } from "../src/feature/project/ProjectInitializer.ts"
import { ProjectStateStore } from "../src/feature/project/ProjectStateStore.ts"
import { ConfigScanner } from "../src/feature/sync/ConfigScanner.ts"
import { configRegistry } from "../src/feature/config/ConfigRegistry.ts"
import { FileTransaction } from "../src/feature/transaction/FileTransaction.ts"
import type { FileSnapshot } from "../src/util/FileUtils.ts"
import { quietTerminal } from "./fixture.ts"

async function applyInitialization(initializer: ProjectInitializer, input: Parameters<ProjectInitializer["planInitialization"]>[0]) {
    return initializer.apply(await initializer.planInitialization(input))
}

async function temporaryRoot(t: TestContext): Promise<string> {
    const root = await mkdtemp(resolve(tmpdir(), "canton-initializer-"))
    t.after(() => rm(root, { recursive: true, force: true }))
    return root
}

function request(rootDir: string): CreateProjectRequest {
    return { rootDir, projectName: "StarterPlugin", projectVersion: "1.2.3", frameworkVersion: "0.0.0-m.2.2", packageManager: "npm", uiDirectory: defaultProjectDirectories.ui, nativeDirectory: defaultProjectDirectories.native, artifactsDirectory: defaultProjectDirectories.artifacts, managedItems: Object.fromEntries(configRegistry.items.map(item => [item.id, true])) }
}

async function write(path: string, content: string): Promise<void> {
    await mkdir(dirname(path), { recursive: true })
    await writeFile(path, content, "utf8")
}

test("create 的只读计划与冲突保护不覆盖源码、不留下工程或事务", async t => {
    const root = await temporaryRoot(t)
    const initializer = new ProjectInitializer(new FileTransaction())
    const plan = await initializer.planCreate(request(root))
    assert.deepEqual(await readdir(root), [])
    const source = resolve(root, "ui/src/App.cta")
    await write(source, "用户源码\n")
    await assert.rejects(initializer.apply(plan), /扫描后发生变化/)
    await assert.rejects(initializer.create(request(root)), /目标已经存在/)
    assert.equal(await readFile(source, "utf8"), "用户源码\n")
    await assert.rejects(readFile(resolve(root, "canton.project.yaml")), { code: "ENOENT" })
    await assert.rejects(readdir(resolve(root, ".canton")), { code: "ENOENT" })
})

test("确认后才建立不存在的工程根，初始化日志仍拒绝根以下工作目录链接", async t => {
    await t.test("尚不存在的多层工程根", async t => {
        const parent = await temporaryRoot(t)
        const root = resolve(parent, "new-parent/new-project")
        const initializer = new ProjectInitializer(new FileTransaction())
        const plan = await initializer.planCreate(request(root))
        assert.deepEqual(await readdir(parent), [])
        await initializer.apply(plan)
        assert.equal((await new ProjectStateStore(new FileTransaction()).load(root)).project.project.name, "StarterPlugin")
        assert.deepEqual(await readdir(resolve(root, ".canton/transactions")), [])
    })
    for (const aliased of [".canton", ".canton/transactions"]) await t.test(`链接路径：${aliased}`, async t => {
        const root = await temporaryRoot(t)
        const external = await temporaryRoot(t)
        await write(resolve(external, "keep.txt"), "外部内容")
        await mkdir(dirname(resolve(root, aliased)), { recursive: true })
        await symlink(external, resolve(root, aliased), "dir")
        const initializer = new ProjectInitializer(new FileTransaction())
        const plan = await initializer.planCreate(request(root))
        await assert.rejects(initializer.apply(plan), /符号链接|非目录/)
        assert.deepEqual(await readdir(external), ["keep.txt"])
        assert.equal(await readFile(resolve(external, "keep.txt"), "utf8"), "外部内容")
        await assert.rejects(readFile(resolve(root, "canton.project.yaml")), { code: "ENOENT" })
        await assert.rejects(readFile(resolve(root, "ui/src/App.cta")), { code: "ENOENT" })
        await assert.rejects(readFile(resolve(root, ".canton/.gitignore")), { code: "ENOENT" })
    })
})

test("已有 .gitignore 原文保留；工程根与各输出目录独立且不可复用已有配置", async t => {
    const root = await temporaryRoot(t)
    await write(resolve(root, ".gitignore"), "# 用户规则\r\nsecret/\r\n")
    await new ProjectInitializer(new FileTransaction()).create(request(root))
    const ignore = await readFile(resolve(root, ".gitignore"), "utf8")
    assert.ok(ignore.startsWith("# 用户规则\r\nsecret/\r\n"))
    assert.match(ignore, /\/canton\.local\.yaml/)
    assert.match(ignore, /\/ui\/dist\//)
    assert.match(ignore, /\/ui\/\.npmrc/)
    await assert.rejects(new ProjectInitializer(new FileTransaction()).create(request(root)), /目标已经存在/)
    const other = await temporaryRoot(t)
    await assert.rejects(new ProjectInitializer(new FileTransaction()).create({ ...request(other), nativeDirectory: "ui/native" }), /相互独立/)
    assert.deepEqual(await readdir(other), [])
})

test("初始化计划拒绝所有接入方式占用保留路径，且合法离体工程可原位接入", async t => {
    const root = await temporaryRoot(t)
    const external = await temporaryRoot(t)
    await write(resolve(external, "ui/package.json"), "{}\n")
    await write(resolve(external, "native/CMakeLists.txt"), "project(ExistingTarget)\n")
    const initializer = new ProjectInitializer(new FileTransaction())
    const paths = [".canton", ".canton/owned", ".git", ".git/objects", "node_modules", "node_modules/cache", "canton.project.yaml", "canton.project.yaml/directory", "canton.local.yaml"]
    for (const kind of ["new", "existing", "copy"] as const) for (const scope of ["ui", "native", "artifacts"] as const) for (const directory of paths) {
        const state = createInitialProjectState(request(root))
        state.project[scope].directory = directory
        const ui = kind === "copy" ? { kind, sourceDir: resolve(external, "ui") } : { kind }
        const native = kind === "copy" ? { kind, sourceDir: resolve(external, "native") } : { kind }
        await assert.rejects(initializer.planInitialization({ state, ui, native }), /CLI 保留路径重叠/)
    }
    assert.deepEqual(await readdir(root), [])
    const state = createInitialProjectState(request(root))
    state.project.ui.directory = resolve(external, "ui")
    state.project.native.directory = resolve(external, "native")
    const plan = await initializer.planInitialization({ state, ui: { kind: "existing" }, native: { kind: "existing" } })
    assert.deepEqual(plan.copies, [])
    assert.deepEqual(await readdir(root), [])
    assert.equal(await readFile(resolve(external, "ui/package.json"), "utf8"), "{}\n")
    assert.equal(await readFile(resolve(external, "native/CMakeLists.txt"), "utf8"), "project(ExistingTarget)\n")
})

test("复制目的目录在确认后出现时停止；不写配置或动已有目标", async t => {
    const parent = await temporaryRoot(t)
    const source = resolve(parent, "source")
    await write(resolve(source, "package.json"), "{}\n")
    const state = createInitialProjectState(request(resolve(parent, "project")))
    const initializer = new ProjectInitializer(new FileTransaction())
    const plan = await initializer.planInitialization({ state, ui: { kind: "copy", sourceDir: source }, native: { kind: "new" } })
    await write(resolve(state.rootDir, "ui/user.txt"), "user")
    await assert.rejects(initializer.apply(plan), /目标已经存在/)
    assert.equal(await readFile(resolve(state.rootDir, "ui/user.txt"), "utf8"), "user")
    await assert.rejects(readFile(resolve(state.rootDir, "canton.project.yaml")), { code: "ENOENT" })
})

test("排他创建复制目标拒绝执行中出现的目录，不覆盖用户文件", async t => {
    const parent = await temporaryRoot(t)
    const source = resolve(parent, "source")
    await write(resolve(source, "package.json"), "{}\n")
    const state = createInitialProjectState(request(resolve(parent, "project")))
    class ConcurrentInitializer extends ProjectInitializer {
        protected override async copyDirectory(copy: CopyPlan): Promise<void> {
            await write(resolve(copy.destination, "user.txt"), "保留用户内容")
            await super.copyDirectory(copy)
        }
    }
    await assert.rejects(applyInitialization(new ConcurrentInitializer(new FileTransaction()), { state, ui: { kind: "copy", sourceDir: source }, native: { kind: "new" } }), /EEXIST/)
    assert.equal(await readFile(resolve(state.rootDir, "ui/user.txt"), "utf8"), "保留用户内容")
    await assert.rejects(readFile(resolve(state.rootDir, "ui/package.json")), { code: "ENOENT" })
    await assert.rejects(readFile(resolve(state.rootDir, "canton.project.yaml")), { code: "ENOENT" })
    const paths = await readdir(resolve(state.rootDir, ".canton/transactions"))
    assert.equal(JSON.parse(await readFile(resolve(state.rootDir, ".canton/transactions", paths[0], "record.json"), "utf8")).status, "failed")
})

test("复制过滤保留恰名为 node_modules 的源根，仍排除其缓存及本机配置", async t => {
    const parent = await temporaryRoot(t)
    const source = resolve(parent, "node_modules")
    await write(resolve(source, "package.json"), '{"name":"root-is-project"}\n')
    await write(resolve(source, "src/keep.ts"), "export const keep = true\n")
    await write(resolve(source, "node_modules/cache.txt"), "缓存")
    await write(resolve(source, ".canton/cache.txt"), "缓存")
    await write(resolve(source, "canton.local.yaml"), "本机配置")
    const state = createInitialProjectState(request(resolve(parent, "new-parent/project")))
    await applyInitialization(new ProjectInitializer(new FileTransaction()), { state, ui: { kind: "copy", sourceDir: source }, native: { kind: "new" } })
    assert.equal(await readFile(resolve(state.rootDir, "ui/src/keep.ts"), "utf8"), "export const keep = true\n")
    for (const name of ["node_modules", ".canton", "canton.local.yaml"]) await assert.rejects(readFile(resolve(state.rootDir, "ui", name)), { code: "ENOENT" })
    assert.equal(await readFile(resolve(source, "canton.local.yaml"), "utf8"), "本机配置")
    assert.deepEqual(await readdir(resolve(state.rootDir, ".canton/transactions")), [])
})

test("成功初始化日志清理失败不把已完成工程报成失败", async t => {
    const root = await temporaryRoot(t)
    class RetainedJournalInitializer extends ProjectInitializer {
        protected override async removeJournal(): Promise<void> { throw new Error("模拟日志清理失败") }
    }
    const state = await new RetainedJournalInitializer(new FileTransaction()).create(request(root))
    assert.equal((await new ProjectStateStore(new FileTransaction()).load(root)).project.project.name, state.project.project.name)
    const paths = await readdir(resolve(root, ".canton/transactions"))
    assert.equal(paths.length, 1)
    assert.equal(JSON.parse(await readFile(resolve(root, ".canton/transactions", paths[0], "record.json"), "utf8")).status, "complete")
})

test("初始化配置写入中途取消停止后续文件，统一失败记录保留且 AbortError 穿透", async t => {
    const root = await temporaryRoot(t)
    const controller = new AbortController()
    class CancelledWriter extends FileTransaction {
        protected override async replaceFile(before: FileSnapshot, after: string, id: string): Promise<void> {
            await super.replaceFile(before, after, id)
            controller.abort()
        }
    }
    await assert.rejects(new ProjectInitializer(new CancelledWriter(controller.signal), controller.signal).create(request(root)), error => {
        assert.ok(error instanceof Error && error.name === "AbortError")
        assert.ok(error.message.includes(resolve(root, ".canton", "transactions")))
        assert.ok(error.message.includes(resolve(root, ".canton", "transactions")))
        return true
    })
    assert.equal(JSON.parse(await readFile(resolve(root, "ui/package.json"), "utf8")).name, "starterplugin")
    await assert.rejects(readFile(resolve(root, "canton.project.yaml")), { code: "ENOENT" })
    await assert.rejects(readFile(resolve(root, "native/CMakeLists.txt")), { code: "ENOENT" })
    for (const directory of ["transactions"]) {
        const paths = await readdir(resolve(root, ".canton", directory))
        assert.equal(paths.length, 1)
        const journal = JSON.parse(await readFile(resolve(root, ".canton", directory, paths[0], "record.json"), "utf8"))
        assert.equal(journal.status, "failed")
        if (directory === "transactions") assert.equal(journal.payload.files[0].written, true)
    }
})

test("初始化确认后执行前取消不创建根目录，不生成虚假的恢复记录", async t => {
    const parent = await temporaryRoot(t)
    const controller = new AbortController()
    const initializer = new ProjectInitializer(new FileTransaction(controller.signal), controller.signal)
    const plan = await initializer.planCreate(request(resolve(parent, "project")))
    controller.abort()
    await assert.rejects(initializer.apply(plan), error => error === controller.signal.reason)
    assert.deepEqual(await readdir(parent), [])
})

test("完成首个子项目复制后取消，第二项目不开始并保留准确恢复路径", async t => {
    const parent = await temporaryRoot(t)
    const uiSource = resolve(parent, "ui-source")
    const nativeSource = resolve(parent, "native-source")
    await write(resolve(uiSource, "package.json"), "{}\n")
    await write(resolve(nativeSource, "CMakeLists.txt"), "project(OriginalTarget)\n")
    const state = createInitialProjectState(request(resolve(parent, "project")))
    const controller = new AbortController()
    class CancelledCopyInitializer extends ProjectInitializer {
        copies = 0
        protected override async copyDirectory(copy: CopyPlan): Promise<void> {
            await super.copyDirectory(copy)
            this.copies++
            controller.abort()
        }
    }
    const initializer = new CancelledCopyInitializer(new FileTransaction(controller.signal), controller.signal)
    await assert.rejects(applyInitialization(initializer, { state, ui: { kind: "copy", sourceDir: uiSource }, native: { kind: "copy", sourceDir: nativeSource } }), error => error instanceof Error && error.name === "AbortError" && error.cause === controller.signal.reason && error.message.includes(resolve(state.rootDir, ".canton/transactions")))
    assert.equal(initializer.copies, 1)
    assert.equal(await readFile(resolve(state.rootDir, "ui/package.json"), "utf8"), "{}\n")
    await assert.rejects(readdir(resolve(state.rootDir, "native")), { code: "ENOENT" })
    await assert.rejects(readFile(resolve(state.rootDir, "canton.project.yaml")), { code: "ENOENT" })
    const paths = await readdir(resolve(state.rootDir, ".canton/transactions"))
    const journal = JSON.parse(await readFile(resolve(state.rootDir, ".canton/transactions", paths[0], "record.json"), "utf8"))
    assert.equal(journal.status, "failed")
    assert.deepEqual(journal.payload.copies.map((copy: { copied: boolean }) => copy.copied), [true, false])
})

test("递归复制中取消停止后续条目，保留部分内容和未完成复制记录", async t => {
    const parent = await temporaryRoot(t)
    const source = resolve(parent, "source")
    await write(resolve(source, "package.json"), "{}\n")
    await write(resolve(source, "src/a-first.ts"), "已复制内容")
    await write(resolve(source, "src/z-last.ts"), "不能继续复制")
    const state = createInitialProjectState(request(resolve(parent, "project")))
    const controller = new AbortController()
    Object.defineProperty(controller.signal, "throwIfAborted", {
        value() {
            if (existsSync(resolve(state.rootDir, "ui/src/a-first.ts"))) controller.abort()
            AbortSignal.prototype.throwIfAborted.call(this)
        }
    })
    const initializer = new ProjectInitializer(new FileTransaction(controller.signal), controller.signal)
    await assert.rejects(applyInitialization(initializer, { state, ui: { kind: "copy", sourceDir: source }, native: { kind: "new" } }), error => error instanceof Error && error.name === "AbortError" && error.cause === controller.signal.reason)
    assert.equal(await readFile(resolve(state.rootDir, "ui/src/a-first.ts"), "utf8"), "已复制内容")
    await assert.rejects(readFile(resolve(state.rootDir, "ui/src/z-last.ts")), { code: "ENOENT" })
    await assert.rejects(readFile(resolve(state.rootDir, "canton.project.yaml")), { code: "ENOENT" })
    const paths = await readdir(resolve(state.rootDir, ".canton/transactions"))
    const journal = JSON.parse(await readFile(resolve(state.rootDir, ".canton/transactions", paths[0], "record.json"), "utf8"))
    assert.equal(journal.status, "failed")
    assert.equal(journal.payload.copies[0].copied, false)
})

test("初始化复制中断保留已完成目录及精确接入记录，不伪称工程创建成功", async t => {
    const parent = await temporaryRoot(t)
    const uiSource = resolve(parent, "ui-source")
    const nativeSource = resolve(parent, "native-source")
    await write(resolve(uiSource, "package.json"), '{"name":"recoverable"}\n')
    await write(resolve(nativeSource, "CMakeLists.txt"), "project(OriginalTarget)\n")
    const state = createInitialProjectState(request(resolve(parent, "project")))
    const initializer = new ProjectInitializer(new FileTransaction())
    const plan = await initializer.planInitialization({ state, ui: { kind: "copy", sourceDir: uiSource }, native: { kind: "copy", sourceDir: nativeSource } })
    await rm(nativeSource, { recursive: true })
    await assert.rejects(initializer.apply(plan), { code: "ENOENT" })
    await assert.rejects(readFile(resolve(state.rootDir, "ui/package.json")), { code: "ENOENT" })
    await assert.rejects(readFile(resolve(state.rootDir, "canton.project.yaml")), { code: "ENOENT" })
})
