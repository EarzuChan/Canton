import assert from "node:assert/strict"
import test from "node:test"
import { dirname, join } from "node:path"
import { rm } from "node:fs/promises"
import { ApplicationOperations } from "../src/business/ApplicationOperations.ts"
import { ProjectOperations, type ProjectAccess } from "../src/feature/operation/ProjectOperations.ts"
import { OperationHost } from "../src/feature/operation/OperationHost.ts"
import { Operations } from "../src/feature/operation/Operations.ts"
import { ProjectStateStore } from "../src/feature/project/ProjectStateStore.ts"
import { FileTransaction } from "../src/feature/transaction/FileTransaction.ts"
import { CmakeService } from "../src/feature/cmake/CmakeService.ts"
import { Executor } from "../src/feature/platform/Executor.ts"
import { createPlatformService } from "../src/feature/platform/CreatePlatformService.ts"
import { UserDirectories } from "../src/feature/user/UserDirectories.ts"
import { ResourcePlanChanged } from "../src/util/ResourcePlanChanged.ts"
import { fixture } from "./fixture.ts"
import { ProjectInitializer } from "../src/feature/project/ProjectInitializer.ts"

test("两个 create 计划竞争同一目录，仅一个应用成功，后者重检而不覆盖", async t => {
    const state = await fixture(t, false)
    const root = join(state.rootDir, "new-project")
    const user = new UserDirectories(`${state.rootDir}-user`)
    const firstHost = new OperationHost(user)
    const secondHost = new OperationHost(user)
    t.after(async () => {
        await firstHost.close()
        await secondHost.close()
        await rm(user.root, { recursive: true, force: true })
    })
    const initializer = new ProjectInitializer(new FileTransaction())
    const request = { rootDir: root, projectName: "ConcurrentPlugin", projectVersion: "1.0.0", frameworkVersion: "0.0.0-m.3.0", packageManager: "npm" as const, uiDirectory: "ui", nativeDirectory: "native", artifactsDirectory: "artifacts", managedItems: {} }
    const firstPlan = await initializer.planCreate(request)
    const secondPlan = await initializer.planCreate(request)
    const unusedProjects = {} as ProjectOperations
    const first = new ApplicationOperations(new Operations(firstHost, new AbortController().signal, () => { }), unusedProjects)
    const second = new ApplicationOperations(new Operations(secondHost, new AbortController().signal, () => { }), unusedProjects)
    const results = await Promise.allSettled([first.initialize(initializer, firstPlan), second.initialize(initializer, secondPlan)])
    assert.equal(results.filter(result => result.status === "fulfilled").length, 1)
    const failure = results.find(result => result.status === "rejected") as PromiseRejectedResult
    assert.match(String(failure.reason), /发生变化|已存在/)
    assert.equal((await new ProjectStateStore(new FileTransaction()).load(root)).project.project.name, "ConcurrentPlugin")
})

test("取得资源后重读配置，SETUP 改变资源位置时释放整组并重新规划", async t => {
    const state = await fixture(t, false)
    state.local = { native: { generator: "Ninja", architecture: "arm64" } }
    const store = new ProjectStateStore(new FileTransaction())
    await store.save(state)
    const user = new UserDirectories(`${state.rootDir}-user`)
    const host = new OperationHost(user)
    t.after(async () => {
        await host.close()
        await rm(user.root, { recursive: true, force: true })
    })
    const operations = new Operations(host, new AbortController().signal, () => { })
    const executor = new Executor()
    const cmake = new CmakeService(executor, createPlatformService(executor), user)
    const projects = new ProjectOperations(operations, store, cmake)
    let runs = 0
    await projects.run(state.rootDir, { configuration: "write", native: "write", nativeSource: "read" }, "SETUP", async () => {
        runs++
        const current = await store.load(state.rootDir)
        if (runs === 1) {
            current.local!.native!.buildRoot = join(state.rootDir, "alternate-build-root")
            await store.save(current)
            await assert.rejects(projects.assertSetup(current, "Native"), ResourcePlanChanged)
            throw new ResourcePlanChanged()
        }
        await projects.assertSetup(current, "Native")
        assert.equal(dirname(cmake.buildDirectory(current, "debug")).startsWith(current.local!.native!.buildRoot!), true)
    })
    assert.equal(runs, 2)
})
