import { readFile } from "node:fs/promises"
import { join } from "node:path"
import { readNativeDependencies } from "../cli/src/feature/source/NativeDependencies.ts"
import { sourceSpecSchema } from "../cli/src/feature/source/SourceSpec.ts"
import { nativeDependencyManifest } from "../cli/src/CliMetadata.ts"
import { SourceCache } from "../cli/src/feature/source/SourceCache.ts"
import { GitSource } from "../cli/src/feature/source/GitSource.ts"
import { UserDirectories } from "../cli/src/feature/user/UserDirectories.ts"
import { Executor } from "../cli/src/feature/platform/Executor.ts"
import { LineDecoder } from "../cli/src/util/LineDecoder.ts"
import type { TaskOutput } from "../cli/src/feature/operation/Work.ts"
import { repoRoot } from "./common.ts"
import { currentNativeSession } from "./native-session.ts"

export const sourceOnlyDependencies = ["miniz", "lunasvg"] as const

export async function cachedNativeDefinitions(definitions: Readonly<Record<string, string>>): Promise<Readonly<Record<string, string>>> {
    const user = new UserDirectories()
    const { host, operations, signal } = currentNativeSession()
    const executor = new Executor(signal, host)
    const cache = new SourceCache(user, new GitSource(executor), signal, operations)
    const log = (line: string) => console.log("[CantonNativeSource]", line)
    const task: TaskOutput = {
        title: "验收依赖源码", line: log, waiting: text => { if (text) log(text) }, stage: text => { if (text) log(text) }, progress: () => { }, process: spec => {
            const decoders = { stdout: new LineDecoder(log), stderr: new LineDecoder(log) }
            return {
                ...spec, onOutput: (bytes, stream) => decoders[stream].push(bytes), onOutputEnd: () => {
                    decoders.stdout.flush()
                    decoders.stderr.flush()
                }
            }
        }
    }
    return operations.run([{ kind: "directory", path: user.dependencySources, mode: "read" }], "准备验收依赖源码", async () => {
        const manifest = await readNativeDependencies(repoRoot)
        const result: Record<string, string> = { ...definitions }
        if (!result.FETCHCONTENT_SOURCE_DIR_QUICKJS_NG && !result.CANTON_QUICKJS_NG_SOURCE_DIR) {
            result.FETCHCONTENT_SOURCE_DIR_QUICKJS_NG = (await cache.ensure(manifest.quickjs_ng, "QuickJS-NG", task)).directory
        }
        // 只取源码的依赖：清单里的键名与 FetchContent 名一致
        const pinned = sourceSpecSchema.required({ commit: true })
        const declared = JSON.parse(await readFile(join(repoRoot, nativeDependencyManifest), "utf8")) as Record<string, unknown>
        for (const name of sourceOnlyDependencies) {
            const key = `FETCHCONTENT_SOURCE_DIR_${name.toUpperCase()}`
            if (!result[key]) result[key] = (await cache.ensure(pinned.parse(declared[name]), name, task)).directory
        }
        return result
    })
}
