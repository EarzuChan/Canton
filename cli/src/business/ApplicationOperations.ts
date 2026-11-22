import { dirname } from "node:path"
import type { ProjectInitializer, InitializationPlan } from "../feature/project/ProjectInitializer.ts"
import type { SyncService, SyncRunOptions } from "../feature/sync/SyncService.ts"
import type { Operations } from "../feature/operation/Operations.ts"
import type { ProjectOperations, ProjectAccess } from "../feature/operation/ProjectOperations.ts"
import type { Resource } from "../feature/operation/Resource.ts"
import type { ProjectRegistry } from "../feature/user/ProjectRegistry.ts"

// 命令边界一次声明完整资源集合；领域服务内的并行任务继承占用，不各自追加锁
export class ApplicationOperations {
    constructor(private readonly operations: Operations, private readonly projects: ProjectOperations, private readonly registry?: ProjectRegistry) { }

    initialize(initializer: ProjectInitializer, plan: InitializationPlan) {
        const resources: Resource[] = [{ kind: "initialization", path: plan.state.rootDir, mode: "write" }]
        for (const file of plan.files) resources.push({ kind: "directory", path: dirname(file.before.path), mode: "write" })
        for (const copy of plan.copies) resources.push({ kind: "directory", path: copy.source, mode: "read" }, { kind: "directory", path: copy.destination, mode: "write" })
        return this.operations.run(resources, "创建或接入工程", async () => {
            await this.registry?.register(plan.state.rootDir)
            return initializer.apply(plan)
        })
    }

    sync(service: SyncService, options: SyncRunOptions, root: string) {
        const mode = options.scanOnly ? "read" : "write"
        const access: ProjectAccess = { configuration: mode, ui: options.nativeOnly ? undefined : mode, nativeSource: options.uiOnly ? undefined : options.setupOnly ? "read" : mode, native: options.configOnly || options.uiOnly ? undefined : mode, observe: options.scanOnly }
        return this.projects.run(root, access, "canton sync", () => service.run(options, root))
    }
}
