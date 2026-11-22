import type { PackageManagerName, ProjectState } from "./ProjectState.ts"
import { defaultProjectIconPath } from "../../CliMetadata.ts"


export interface CreateProjectRequest {
    readonly rootDir: string
    readonly projectName: string
    readonly displayName?: string
    readonly iconSource?: string
    readonly projectVersion: string
    readonly frameworkVersion: string
    readonly frameworkNodeRegistryUrl?: string
    readonly packageManager: PackageManagerName
    readonly uiDirectory: string
    readonly nativeDirectory: string
    readonly artifactsDirectory: string
    readonly managedItems: Record<string, boolean>
}

// 将向导返回的请求包装为项目状态
export function createInitialProjectState(request: CreateProjectRequest): ProjectState {
    return {
        rootDir: request.rootDir,
        project: {
            project: {
                name: request.projectName,
                displayName: request.displayName ?? request.projectName,
                ...(request.iconSource !== undefined ? { icon: defaultProjectIconPath } : {}),
                version: request.projectVersion,
            },
            framework: {
                version: request.frameworkVersion,
                ...(request.frameworkNodeRegistryUrl !== undefined ? { nodeRegistryUrl: request.frameworkNodeRegistryUrl } : {}),
            },
            ui: {
                directory: request.uiDirectory,
                packageManager: request.packageManager,
            },
            native: {
                directory: request.nativeDirectory,
                target: request.projectName,
            },
            artifacts: {
                directory: request.artifactsDirectory,
                includeVersionDirectory: true,
            },
            "managed-items": Object.entries(request.managedItems).filter(([, managed]) => managed).map(([key]) => key),
        },
        local: null, // 初次创建没有本机配置，由 sync 阶段生成
    }
}
