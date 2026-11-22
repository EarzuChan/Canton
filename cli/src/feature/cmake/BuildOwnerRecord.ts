import { createHash } from "node:crypto"
import { isAbsolute, resolve } from "node:path"
import { z } from "zod"

export const buildOwnerRecordSchema = z.object({ rootDir: z.string().refine(isAbsolute, "工程归属路径必须为绝对路径") })
export function nativeBuildRootIdentity(root: string): string { return resolve(root).toLowerCase() }
export function nativeBuildRootKey(root: string): string { return createHash("sha256").update(nativeBuildRootIdentity(root)).digest("hex").slice(0, 8) }
