import { resolve } from 'node:path'
import { mkdtemp, realpath, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { checkCtaProject } from '../packages/compiler/src/typecheck.ts'
import { prepareRuntimeFixtureWorkspace } from '../tests/helpers/ui-fixture.ts'

const roots = process.argv.slice(2)
const workspace = roots.length ? undefined : await realpath(await mkdtemp(resolve(tmpdir(), 'canton-cta-typecheck-')))
let diagnostics
try {
    if (workspace) await prepareRuntimeFixtureWorkspace(workspace)
    diagnostics = checkCtaProject(resolve(workspace ?? '.', 'tsconfig.json'), roots.length ? roots : ['tests/fixtures'].map(path => resolve(workspace!, path)))
} finally {
    if (workspace) await rm(workspace, { recursive: true, force: true })
}
for (const diagnostic of diagnostics) console.error('[CantonCtaTypecheck]', `${diagnostic.file}:${diagnostic.line}:${diagnostic.column} ${diagnostic.code}：${diagnostic.message}`)
if (diagnostics.length) process.exitCode = 1
else console.log('[CantonCtaTypecheck]', 'CTA 脚本与模板类型检查通过')
