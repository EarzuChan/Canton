import { transformWithOxc } from "vite"
import { compileCantonCta, isCtaModule, isCtaQueryModule } from "./ctaTransform.ts"
import type { CantonTransformPlugin, CantonTransformPluginOptions, TransformThis } from "./types.ts"

export function createCantonTransformPlugin(options: CantonTransformPluginOptions): CantonTransformPlugin {
    return {
        name: options.name,
        enforce: "pre",
        load(id: string): string | null {
            if (!isCtaQueryModule(id)) return null
            return ""
        },
        transform(this: TransformThis, code: string, id: string) {
            if (!isCtaModule(id)) return null
            if (isCtaQueryModule(id)) return ""

            const { code: transformed, map, dependencies } = compileCantonCta(code, id, options.hot())
            for (const dependency of dependencies) this.addWatchFile?.(dependency)

            return transformWithOxc(transformed, id, {
                lang: "ts",
                target: "es2022",
                sourcemap: true,
            }, map).then(result => ({ code: result.code, map: result.map }))
        },
    }
}
