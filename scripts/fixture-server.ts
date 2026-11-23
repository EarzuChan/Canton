import { existsSync, readFileSync } from 'node:fs'
import { resolve, sep } from 'node:path'
import { createServer, type Plugin, type ViteDevServer } from 'vite'
import canton from '../packages/vite-plugin/src/plugin.ts'
import { repoRoot } from './common.ts'

// 给 tests/fixtures 下的夹具起一个 Vite DevServer（未监听，调用方自己 listen）。port 为 0 时由系统挑端口
export async function createFixtureServer(fixture: string, port = 0): Promise<ViteDevServer> {
    const fixtureRoot = resolve(repoRoot, 'tests/fixtures', fixture)
    const entry = ['main.ts', 'src/main.ts'].map(path => resolve(fixtureRoot, path)).find(path => existsSync(path))
    if (!entry) throw new Error(`找不到夹具入口：${fixtureRoot}`)
    // 夹具直接放在仓库里，不是一个装了依赖的工程：@canton/framework 与私有旁包 @canton/kyant-glass 按各自导出表解析到源码
    const frameworkRoot = resolve(repoRoot, 'packages/framework')
    const frameworkExports = JSON.parse(readFileSync(resolve(frameworkRoot, 'package.json'), 'utf8')).exports as Record<string, { import?: string }>
    const kyantGlassRoot = resolve(repoRoot, 'packages/kyant-glass')
    const kyantGlassExports = JSON.parse(readFileSync(resolve(kyantGlassRoot, 'package.json'), 'utf8')).exports as Record<string, { import?: string }>
    const workspacePackages: Plugin = {
        name: 'canton-fixture-workspace-packages',
        enforce: 'pre',
        resolveId(id) {
            if (id === '@canton/framework' || id.startsWith('@canton/framework/')) {
                const target = frameworkExports['.' + id.slice('@canton/framework'.length)]?.import
                return target ? resolve(frameworkRoot, target).split(sep).join('/') : null
            }
            if (id === '@canton/kyant-glass' || id.startsWith('@canton/kyant-glass/')) {
                const target = kyantGlassExports['.' + id.slice('@canton/kyant-glass'.length)]?.import
                return target ? resolve(kyantGlassRoot, target).split(sep).join('/') : null
            }
            return null
        },
    }
    return createServer({
        root: fixtureRoot,
        configFile: false,
        publicDir: resolve(fixtureRoot, 'public'),
        plugins: [workspacePackages, canton({ entry, port, strictPort: port !== 0 }) as unknown as Plugin],
        optimizeDeps: { noDiscovery: true },
        server: { fs: { allow: [repoRoot] } },
    })
}
