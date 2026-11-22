import type { ProjectState } from "../project/ProjectState.ts"

// Framework 获取来源：共享配置声明工程的正式来源，本机配置可覆盖为本机来源（本地 registry）
// 本机覆盖只影响 CLI 驱动的依赖获取，不改写共享配置

export function frameworkRegistryUrl(state: ProjectState): string | null {
    return state.local?.framework?.nodeRegistryUrl ?? state.project.framework.nodeRegistryUrl ?? null
}
