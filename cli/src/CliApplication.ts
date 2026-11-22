import { cliDescription, cliName, cliVersion } from "./CliMetadata.ts"

import type { CommandInterface } from "./business/CommandInterface.ts"
import { ApplicationCommand } from "./business/ApplicationCommand.ts"
import { registerMaintenanceCommands } from "./business/command/Maintenance.ts"

import { CmakeService } from "./feature/cmake/CmakeService.ts"
import type { ConfigInteraction } from "./feature/sync/ConfigInteraction.ts"
import { Executor } from "./feature/platform/Executor.ts"
import { createPlatformService } from "./feature/platform/CreatePlatformService.ts"
import { GitSource } from "./feature/source/GitSource.ts"
import { FileTransaction } from "./feature/transaction/FileTransaction.ts"
import { OperationHost } from "./feature/operation/OperationHost.ts"
import { Operations } from "./feature/operation/Operations.ts"
import { ProjectOperations } from "./feature/operation/ProjectOperations.ts"
import type { ProjectInteraction } from "./feature/project/ProjectInteraction.ts"
import { ProjectStateStore } from "./feature/project/ProjectStateStore.ts"
import { ProjectRegistry } from "./feature/user/ProjectRegistry.ts"
import { UserDirectories } from "./feature/user/UserDirectories.ts"
import type { SetupInteraction } from "./feature/sync/SetupInteraction.ts"
import { StorageInventory } from "./feature/storage/StorageInventory.ts"
import { StorageMaintenance } from "./feature/storage/StorageMaintenance.ts"
import { ProjectCleaner } from "./feature/storage/ProjectCleaner.ts"

export interface CliInteractions {
    readonly project: ProjectInteraction
    readonly config: ConfigInteraction
    readonly setup: SetupInteraction
}

export interface CliApplicationOptions {
    readonly signal: AbortSignal
    readonly interactions: CliInteractions | "terminal"
    // 持久消息与实时任务的输出终端，由入口创建
    readonly terminal: CommandInterface
    readonly userDirectories?: UserDirectories
}

export function createCliApplication(options: CliApplicationOptions): ApplicationCommand {
    const { terminal } = options
    const directories = options.userDirectories ?? new UserDirectories()

    const host = new OperationHost(directories)
    const signal = AbortSignal.any([options.signal, host.failureSignal])
    const operations = new Operations(host, signal, message => terminal.reporter.info(message))
    const executor = new Executor(signal, host)
    const platform = createPlatformService(executor)

    const fileWriter = new FileTransaction(signal)
    const store = new ProjectStateStore(fileWriter)

    const cmake = new CmakeService(executor, platform, directories, undefined, signal)
    const projectRegistry = new ProjectRegistry(directories, signal)
    const lifetime = { kind: "directory" as const, path: directories.dependencySources, mode: "read" as const }
    const projectOperations = new ProjectOperations(operations, store, cmake, projectRegistry, lifetime)

    const cli = new ApplicationCommand(host).exitOverride()/*这是AppCmd自己的Exit覆写*/.name(cliName).description(cliDescription).version(cliVersion)
    cli.configureOutput({ writeOut: text => terminal.reporter.info(text), writeErr: text => terminal.reporter.error(text) })

    const storageGit = new GitSource(executor)
    const inventory = new StorageInventory(directories, projectRegistry, storageGit, signal, store, cmake)
    registerMaintenanceCommands(cli, { terminal, store, projects: projectOperations, operations, directories, registry: projectRegistry, inventory, cleaner: new ProjectCleaner(cmake, signal), maintenance: new StorageMaintenance(directories, inventory, storageGit, signal, projectRegistry), lifetime, signal, interactive: options.interactions === "terminal" })

    cli.exitOverride() // 确保子命令都覆写退出
    return cli
}
