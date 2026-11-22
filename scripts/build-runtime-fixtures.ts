import { mkdir, readFile, rm } from 'node:fs/promises'
import { resolve } from 'node:path'
import { build, type Plugin } from 'vite'
import canton from '../packages/vite-plugin/src/plugin.ts'
import { CANTON_MACRO_PATTERN } from '../packages/vite-plugin/src/constraints.ts'
import { prepareRuntimeFixtureWorkspace } from '../tests/helpers/ui-fixture.ts'

if (!process.argv[2]) throw new Error('必须指定测试夹具输出目录')
const output = resolve(process.argv[2])
const workspace = resolve(output, 'workspace')
await rm(workspace, { recursive: true, force: true })
await mkdir(workspace, { recursive: true })
await prepareRuntimeFixtureWorkspace(workspace)
const fixtures = ['app-smoke']
const requested = process.argv.slice(3)
for (const fixture of requested) if (!fixtures.includes(fixture)) throw new Error(`未知的运行时夹具：${fixture}`)
for (const fixture of requested.length ? requested : fixtures) {
    await build({
        root: workspace,
        configFile: false,
        plugins: [canton({ entry: resolve(workspace, 'tests/fixtures', fixture, 'src/main.ts') }) as unknown as Plugin],
        build: { outDir: resolve(output, fixture), emptyOutDir: true },
    })
    const source = await readFile(resolve(output, fixture, 'app.js'), 'utf8')
    if (CANTON_MACRO_PATTERN.test(source)) throw new Error(`测试 UI 产物残留未替换的编译宏：${fixture}`)
}
