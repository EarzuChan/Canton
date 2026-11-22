import { lstat } from "node:fs/promises"
import { dirname, join, resolve } from "node:path"
import type { CmakeService } from "../cmake/CmakeService.ts"
import type { BuildFlavor, ProjectState } from "../project/ProjectState.ts"
import { nativeDirectory, safeUiOutputDirectory, uiDirectory, workDirectory } from "../project/ProjectPaths.ts"
import { assertPlainDirectoryPath } from "../../util/PlainDirectoryPath.ts"
import { containsPath } from "../transaction/TreeState.ts"
import { TransactionStore } from "../transaction/TransactionStore.ts"
import { DeleteTransaction } from "../transaction/DeleteTransaction.ts"
import { nativeBuildRecordName } from "../building/BuildReceipt.ts"
import { defaultNativeGenerator } from "../../CliMetadata.ts"
import { buildOwnerRecordSchema } from "../cmake/BuildOwnerRecord.ts"
import { readCurrentRecord } from "../../util/RecordInspection.ts"

export interface CleanOptions { readonly ui: boolean, readonly native: boolean, readonly flavor?: BuildFlavor }
export interface CleanEntry { readonly root: string, readonly path: string, readonly reason: string }

export class ProjectCleaner {
    constructor(private readonly cmake: Pick<CmakeService, "buildDirectory" | "buildAnchor">, private readonly signal: AbortSignal) { }

    async plan(state: ProjectState, options: CleanOptions): Promise<readonly CleanEntry[]> {
        const transactions = await new TransactionStore(state.rootDir).list()
        if (transactions.some(entry => entry.error || entry.record?.status !== "complete")) throw new Error("工程存在未完成或损坏事务，请先运行 canton recover")
        const entries: CleanEntry[] = []
        if (options.ui) {
            const path = await safeUiOutputDirectory(state)
            const protectedPaths = [state.rootDir, nativeDirectory(state), workDirectory(state), resolve(state.rootDir, state.project.artifacts.directory)]
            if (protectedPaths.some(protectedPath => containsPath(path, protectedPath) || containsPath(protectedPath, path) && protectedPath !== state.rootDir)) throw new Error("UI 输出与保护目录重叠")
            entries.push({ root: uiDirectory(state), path, reason: "UI 构建输出" }, { root: state.rootDir, path: join(workDirectory(state), "built-ui.json"), reason: "UI 构建凭据" })
        }
        if (options.native) {
            const prepared = state.local?.native ? state : { ...state, local: { ...state.local, native: { generator: defaultNativeGenerator, architecture: process.arch === "arm64" ? "arm64" as const : "x64" as const } } }
            for (const flavor of options.flavor ? [options.flavor] : ["debug", "release"] as const) {
                const path = this.cmake.buildDirectory(prepared, flavor)
                const root = this.cmake.buildAnchor(prepared)
                const ownerPath = join(dirname(path), "owner.json")
                const info = await lstat(path).catch(error => {
                    if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined
                    throw error
                })
                if (info) {
                    await assertPlainDirectoryPath(root, path)
                    const owner = await readCurrentRecord(ownerPath, buildOwnerRecordSchema)
                    const normalize = (value: string) => resolve(value).toLowerCase()
                    if (normalize(owner.rootDir) !== normalize(state.rootDir)) throw new Error(`构建归属不符：${path}`)
                    entries.push({ root, path, reason: `原生 ${flavor} 构建树` })
                }
                entries.push({ root: state.rootDir, path: join(workDirectory(state), `built-${nativeBuildRecordName(prepared, flavor, "win32")}.json`), reason: `原生 ${flavor} 构建凭据` })
            }
        }
        const existing: CleanEntry[] = []
        for (const entry of entries) {
            const info = await lstat(entry.path).catch(error => {
                if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined
                throw error
            })
            if (info) {
                if (info.isSymbolicLink()) throw new Error(`清理目标不能为链接：${entry.path}`)
                existing.push(entry)
            }
        }
        const unique = [...new Map(existing.map(entry => [entry.path, entry])).values()]
        return unique.filter(entry => !unique.some(other => other !== entry && containsPath(other.path, entry.path)))
    }

    async execute(state: ProjectState, entries: readonly CleanEntry[]): Promise<void> {
        const transaction = new DeleteTransaction(new TransactionStore(state.rootDir), this.signal)
        await transaction.apply(await transaction.prepare(entries))
    }
}
