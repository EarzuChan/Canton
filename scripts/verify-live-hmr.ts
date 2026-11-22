import { mkdtemp, realpath, writeFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import assert from 'node:assert/strict'
import { createServer } from 'vite'
import canton from '../packages/vite-plugin/src/plugin.ts'
import { withNativeSession, nativeSessionStart } from "./native-session.ts"
import type { RunningProcess, ProcessResult } from "../cli/src/feature/platform/ProcessSpec.ts"
import { repoRoot } from "./common.ts"
import { configureNativeBuild, nativeCmake, nativeExecutable, nativeTestDirectory } from "./native-toolchain.ts"

// 对端默认用 native-tests 的 canton_live_server，先构建；也可以传一个实现同一验证协议的程序路径
const requestedExecutable = process.argv[2]
const executable = requestedExecutable ? resolve(requestedExecutable) : nativeExecutable(resolve(nativeTestDirectory(), 'native-tests'), 'canton_live_server')

await withNativeSession(async () => {
    if (!requestedExecutable) {
        await configureNativeBuild(repoRoot, nativeTestDirectory(), 'Debug', ['-DCANTON_BUILD_TESTS=ON'])
        await nativeCmake(['--build', nativeTestDirectory(), '--target', 'canton_live_server', '--parallel', process.env.CMAKE_BUILD_PARALLEL_LEVEL ?? '4'])
    }
    // 临时目录可能是 8.3 短路径（用户名带空格时），Vite 按长路径解析模块，先换成真实路径
    const directory = await realpath(await mkdtemp(join(tmpdir(), 'canton-live-hmr-')))
    const server = await createServer({ root: directory, configFile: false, plugins: [canton({ entry: 'main.ts', port: 0 })], optimizeDeps: { noDiscovery: true }, server: { fs: { allow: [directory, repoRoot] }, watch: { awaitWriteFinish: { stabilityThreshold: 50, pollInterval: 10 } } } })
    // 夹具根在临时目录、框架源码在仓库里：上面放行了仓库目录，Vite 才肯转换热更新客户端引用的框架模块
    let child: RunningProcess | undefined
    let completed: ProcessResult | undefined
    let processFailure: unknown
    let output = ''
    let errors = ''
    let cursor = 0

    async function waitFor(text: string): Promise<void> {
        const deadline = Date.now() + 20000
        while (Date.now() < deadline) {
            const found = output.indexOf(text, cursor)
            if (found >= 0) {
                cursor = found + text.length
                return
            }
            if (processFailure) throw processFailure
            if (completed) throw new Error(`native 提前退出：${output}\n${errors}`)
            await new Promise(resolve => setTimeout(resolve, 20))
        }
        throw new Error(`未等到 ${text}：${output}\n${errors}`)
    }

    const module = (value: number, invalidate = false) => `
const hot = import.meta.hot
const previous = hot.data.value ?? 0
export const value = previous + ${value}
hot.dispose(data => { data.value = value })
import.meta.hot.accept(next => { ${invalidate ? "hot.invalidate('向入口传播')" : "hot.send('test:probe', { value: next.value })"} })
`

    try {
        await writeFile(resolve(directory, 'main.ts'), `import {value} from './state.ts'\nconst lazy = await import('./lazy.ts')\nimport.meta.hot.send('test:probe', {value: value + lazy.value})\n`)
        await writeFile(resolve(directory, 'state.ts'), module(1))
        await writeFile(resolve(directory, 'lazy.ts'), 'export const value = await Promise.resolve(10)')
        await server.listen()
        const url = server.resolvedUrls!.local[0]
        child = nativeSessionStart({
            command: executable, args: [url], onOutput: (chunk, channel) => {
                if (channel === "stdout") output += chunk.toString()
                else errors += chunk.toString()
            }
        })
        void child.completion.then(result => { completed = result }, error => { processFailure = error })
        await waitFor('VALUE 11')
        await writeFile(resolve(directory, 'state.ts'), module(2))
        await waitFor('VALUE 3')
        assert.equal(output.split('LOAD').length - 1, 1, '接受更新不应重建 QuickJS context')
        await writeFile(resolve(directory, 'state.ts'), 'export const = broken')
        await waitFor('ERROR')
        await writeFile(resolve(directory, 'state.ts'), module(4, true))
        await waitFor('VALUE 7')
        await writeFile(resolve(directory, 'state.ts'), module(8))
        // 上一个版本的 accept 主动 invalidate，Vite 传播到无 accept 的入口
        await waitFor('VALUE 18')
        assert.equal(output.split('LOAD').length - 1, 2)
        await writeFile(resolve(directory, 'main.ts'), `import.meta.hot.send('test:done', {})`)
        const result = await child.completion
        if (result.exitCode !== 0) throw new Error(`原生进程退出码 ${result.exitCode}：${errors}`)
        console.log('[CantonLiveHmrVerify]', '真实 Vite → 后台快照 → QuickJS：动态 import/TLA、dispose/data、同 context accept、编译失败恢复、invalidate/full reload 全部通过')
    } finally {
        await child?.stop()
        await server.close()
        await rm(directory, { recursive: true, force: true })
    }

}, requestedExecutable ? [] : [nativeTestDirectory()])
