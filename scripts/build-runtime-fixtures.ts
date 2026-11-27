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
// 夹具名到入口文件与可选清单元数据；有 public 目录的夹具，其中的资源会随包输出
type FixtureEntry = string | { entry: string; appId?: string; singleInstance?: boolean; portable?: boolean }
const fixtures: Record<string, FixtureEntry> = {
    'app-smoke': 'src/main.ts',
    showcase: 'main.ts',
    layout: 'main.ts',
    lazy: 'main.ts',
    'rearrange-runtime': 'main.ts',
    'viewport-runtime': 'main.ts',
    'window-interaction': 'main.ts',
    'render-windows': 'main.ts',
    'shader-effects': 'main.ts',
    'desktop-shell': { entry: 'main.ts', appId: 'com.canton.desktop.shell', singleInstance: true },
}
const requested = process.argv.slice(3)
for (const fixture of requested) if (!(fixture in fixtures)) throw new Error(`未知的运行时夹具：${fixture}`)
for (const fixture of requested.length ? requested : Object.keys(fixtures)) {
    const config = fixtures[fixture]!
    const entryFile = typeof config === 'string' ? config : config.entry
    const meta = typeof config === 'string' ? {} : { appId: config.appId, singleInstance: config.singleInstance, portable: config.portable }
    await build({
        root: workspace,
        configFile: false,
        publicDir: resolve(workspace, 'tests/fixtures', fixture, 'public'),
        plugins: [canton({ entry: resolve(workspace, 'tests/fixtures', fixture, entryFile), ...meta }) as unknown as Plugin],
        build: { outDir: resolve(output, fixture), emptyOutDir: true },
    })
    // 字节码构建的中间产物是以 .cbc 命名的 ESM
    const source = await readFile(resolve(output, fixture, process.env.CANTON_UI_BUILD_SCRIPT_FORMAT === 'bytecode' ? 'app.cbc' : 'app.js'), 'utf8')
    if (CANTON_MACRO_PATTERN.test(source)) throw new Error(`测试 UI 产物残留未替换的编译宏：${fixture}`)
}
