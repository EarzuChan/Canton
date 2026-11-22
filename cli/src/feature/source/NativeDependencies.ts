import { readFile } from "node:fs/promises"
import { join } from "node:path"
import { z } from "zod"
import { nativeDependencyManifest } from "../../CliMetadata.ts"
import { sourceSpecSchema } from "./SourceSpec.ts"

const pinnedSourceSchema = sourceSpecSchema.required({ commit: true })
// 清单里只取源码的 miniz、lunasvg 没有在这里声明，由 scripts/native-source-cache.ts 直接读清单取用；CLI 属 M8 重造，届时一并收进来
const manifestSchema = z.object({ schema: z.literal(1), quickjs_ng: pinnedSourceSchema })
export type NativeDependencies = z.infer<typeof manifestSchema>

export async function readNativeDependencies(source: string): Promise<NativeDependencies> {
    try { return manifestSchema.parse(JSON.parse(await readFile(join(source, nativeDependencyManifest), "utf8"))) } catch (error) {
        const path = join(source, nativeDependencyManifest)
        if ((error as NodeJS.ErrnoException).code === "ENOENT") throw new Error(`Canton 原生依赖清单缺失：${path}；所选源码必须包含当前依赖清单`, { cause: error })
        throw new Error(`Canton 原生依赖清单无效：${path}`, { cause: error })
    }
}
