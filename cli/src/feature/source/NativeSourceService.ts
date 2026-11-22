import type { ProjectState } from "../project/ProjectState.ts"
import type { TaskOutput } from "../operation/Work.ts"
import { SourceCache } from "./SourceCache.ts"
import { readNativeDependencies } from "./NativeDependencies.ts"
import { resolve } from "node:path"
import { assertFrameworkDevReadiness, assertFrameworkSource } from '../framework/FrameworkSourceMetadata.ts'

export class NativeSourceService {
    constructor(private readonly cache: SourceCache) { }

    async assertDevReadiness(state: ProjectState): Promise<void> {
        const override = state.local?.native?.cmakeDefinitions?.FETCHCONTENT_SOURCE_DIR_CANTON
        if (!override) throw new Error("无法核验 Native 开发就绪协议：请设置 FETCHCONTENT_SOURCE_DIR_CANTON")
        await assertFrameworkDevReadiness(resolve(state.rootDir, override), state.project.framework.version)
    }

    async prepare(state: ProjectState, task: TaskOutput): Promise<Readonly<Record<string, string>>> {
        const definitions = state.local?.native?.cmakeDefinitions ?? {}
        const root = definitions.FETCHCONTENT_SOURCE_DIR_CANTON
        if (!root) return {}
        await assertFrameworkSource(resolve(state.rootDir, root), state.project.framework.version)
        const dependencies = await readNativeDependencies(resolve(state.rootDir, root))
        const result: Record<string, string> = {}
        if (definitions.CANTON_QUICKJS_NG_SOURCE_DIR === undefined && definitions.FETCHCONTENT_SOURCE_DIR_QUICKJS_NG === undefined) {
            const declared = dependencies.quickjs_ng
            const repository = definitions.CANTON_QUICKJS_NG_GIT_REPOSITORY ?? declared.repository
            const ref = definitions.CANTON_QUICKJS_NG_GIT_TAG ?? declared.ref
            const spec = repository === declared.repository && ref === declared.ref ? declared : { repository, ref }
            result.FETCHCONTENT_SOURCE_DIR_QUICKJS_NG = (await this.cache.ensure(spec, 'QuickJS-NG', task)).directory
        }
        return result
    }
}
