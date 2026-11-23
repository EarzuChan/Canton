export { CANTON_DEFINES, CANTON_DEFINE_KEYS, CANTON_MACRO_PATTERN, CANTON_VERSION, PUBLIC_PLUGIN_NAME } from "./constraints.ts"
export { createModuleSnapshot, MODULE_SNAPSHOT_PATH } from './module-snapshot.ts'

export { canton, default } from "./plugin.ts"
export { compileHlslToDxbc, formatHlslDiagnostics } from "./hlslCompiler.ts"
export type { HlslCompileOptions, HlslCompileResult } from "./hlslCompiler.ts"
export { isHlslModule, transformHlslModule, inlineLocalHlslIncludes } from "./hlslTransform.ts"

export type { CantonVitePlugin, CantonVitePluginOptions } from "./types.ts"

