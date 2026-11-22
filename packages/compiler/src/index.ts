// CTA 工具链的正式调用入口，不转导第三方工具或上游兼容 API
export { compileScript } from './compile.ts'
export { parse } from './cta/parse.ts'
export { invalidateTypeCache } from './script/resolveType.ts'
export { checkCtaProject, type CtaDiagnostic } from './typecheck.ts'

export type { CTAScriptCompileOptions } from './script/options.ts'
export type { CTABlock, CTADescriptor, CTAParseOptions, CTAParseResult, CTAScriptBlock, CTATemplateBlock } from './cta/parse.ts'
