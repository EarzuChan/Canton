import assert from 'node:assert/strict'
import { once } from 'node:events'
import { createServer, type IncomingMessage } from 'node:http'
import { mkdtemp, readdir, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test, { type TestContext } from 'node:test'
import { cliCompatibility } from '../src/CliMetadata.ts'
import { FrameworkRegistryClient } from '../src/feature/framework/FrameworkRegistryClient.ts'
import { FrameworkService } from '../src/feature/framework/FrameworkService.ts'
import { GitSource } from '../src/feature/source/GitSource.ts'
import { SourceCache } from '../src/feature/source/SourceCache.ts'
import { UserDirectories } from '../src/feature/user/UserDirectories.ts'
import { OperationHost } from '../src/feature/operation/OperationHost.ts'
import { Operations } from '../src/feature/operation/Operations.ts'
import { Executor } from '../src/feature/platform/Executor.ts'
import { isAbortError } from '../src/feature/platform/ProcessSpec.ts'
import { write, testTask } from './fixture.ts'
import { assertFrameworkDevReadiness } from '../src/feature/framework/FrameworkSourceMetadata.ts'

const version = '1.2.3'
const dependency = { repository: 'https://example.invalid/dependency.git', ref: 'v1', commit: 'a'.repeat(40) }
const dependencies = { schema: 1, quickjs_ng: dependency }

async function fixture(t: TestContext) {
    const root = await mkdtemp(join(tmpdir(), 'canton-admission-'))
    const user = new UserDirectories(join(root, 'user'))
    const signal = new AbortController()
    const host = new OperationHost(user)
    const executor = new Executor(signal.signal, host)
    const operations = new Operations(host, signal.signal, () => { })
    const git = new GitSource(executor)
    const cache = new SourceCache(user, git, signal.signal, operations)
    const repository = join(root, 'source repository')
    const run = async (...args: string[]) => {
        const result = await executor.run({ command: 'git', args, cwd: repository })
        assert.equal(result.exitCode, 0, result.stderr)
        return result.stdout.trim()
    }
    await write(join(repository, 'CMakeLists.txt'), 'cmake_minimum_required(VERSION 3.24)\nproject(Fixture NONE)\n')
    await run('init', '--quiet')
    await run('config', 'user.email', 'fixture@example.invalid')
    await run('config', 'user.name', 'Fixture')
    const tag = async (selected = version, metadata: unknown = { frameworkVersion: selected, cliCompatibility, frameworkInternalProtocolCode: 1 }, manifest: unknown = dependencies, annotated = false) => {
        await write(join(repository, 'canton.version.json'), JSON.stringify(metadata))
        await write(join(repository, 'cmake', 'NativeDependencies.json'), JSON.stringify(manifest))
        await run('add', '.')
        await run('commit', '--quiet', '--allow-empty', '-m', selected)
        if (annotated) await run('tag', '-a', `v${selected}`, '-m', selected)
        else await run('tag', `v${selected}`)
        return run('rev-parse', 'HEAD')
    }
    let handle = (_request: IncomingMessage): { status?: number, body: unknown } => ({ body: { version, canton: { cliCompatibility } } })
    const requests: string[] = []
    const server = createServer((request, response) => {
        requests.push(decodeURIComponent(request.url!))
        const result = handle(request)
        response.writeHead(result.status ?? 200, { 'Content-Type': 'application/json' })
        response.end(JSON.stringify(result.body))
    })
    server.listen(0, '127.0.0.1')
    await once(server, 'listening')
    const address = server.address()
    assert.ok(address && typeof address !== 'string')
    const registryUrl = `http://127.0.0.1:${address.port}`
    const service = new FrameworkService(new FrameworkRegistryClient(signal.signal), { git, cache, operations, directory: user.dependencySources })
    t.after(async () => {
        signal.abort()
        server.closeAllConnections()
        await new Promise<void>((resolve, reject) => server.close(error => error ? reject(error) : resolve()))
        await host.close()
        await rm(root, { recursive: true, force: true })
    })
    return { root, user, service, signal, executor, git, cache, tag, run, repository, registryUrl, requests, handle: (next: typeof handle) => { handle = next }, input: { registryUrl, repositoryUrl: repository } }
}

test('registry 确切版本与 Git 注解 tag 同时验证，复用源码缓存且不安装其它依赖', async t => {
    const input = await fixture(t)
    const commit = await input.tag(version, undefined, undefined, true)
    const selected = await input.service.selectVersion(version, input.input, testTask())
    assert.deepEqual(selected, { version, cliCompatibility, registryUrl: input.registryUrl, repositoryUrl: input.repository, commit })
    assert.ok(Object.isFrozen(selected))
    assert.equal(await input.git.resolveTag(input.repository, `v${version}`), commit)
    assert.equal((await readdir(join(input.user.dependencySources, 'entries'))).length, 1)
    assert.deepEqual(await input.service.selectVersion(version, input.input, testTask()), selected)
    assert.deepEqual(input.requests, [`/@canton/framework/${version}`, `/@canton/framework/${version}`])
    await assert.rejects(readdir(join(input.root, 'node_modules')), { code: 'ENOENT' })
})

test('Native dev 只接受声明当前就绪回执协议的 Framework 源码', async t => {
    const input = await fixture(t)
    await write(join(input.repository, 'canton.version.json'), JSON.stringify({ frameworkVersion: version, cliCompatibility, frameworkInternalProtocolCode: 6, nativeDevReadinessProtocolCode: 1 }))
    await write(join(input.repository, 'cmake', 'NativeDependencies.json'), JSON.stringify(dependencies))
    await assertFrameworkDevReadiness(input.repository, version)
    await write(join(input.repository, 'canton.version.json'), JSON.stringify({ frameworkVersion: version, cliCompatibility, frameworkInternalProtocolCode: 6 }))
    await assert.rejects(assertFrameworkDevReadiness(input.repository, version), /不支持 Native 开发就绪握手/)
})

test('不存在、不兼容或无法验证的 registry 版本在接触源码前拒绝', async t => {
    const input = await fixture(t)
    for (const response of [{ status: 404, body: {} }, { body: { version } }, { body: { version, canton: { cliCompatibility: cliCompatibility + 1 } } }, { body: { version: '9.9.9', canton: { cliCompatibility } } }]) {
        input.handle(() => response)
        await assert.rejects(input.service.selectVersion(version, input.input, testTask()), /404|cliCompatibility|CLI 兼容契约|版本与请求不符/)
    }
    await assert.rejects(readdir(input.user.dependencySources), { code: 'ENOENT' })
    input.signal.abort()
    await assert.rejects(input.service.selectVersion(version, input.input, testTask()), isAbortError)
})

test('缺 Git tag 和错误源码版本、兼容码、内部协议、依赖清单均拒绝且不发布缓存', async t => {
    const input = await fixture(t)
    await input.tag('1.2.0')
    await assert.rejects(input.service.selectVersion(version, input.input, testTask()), /没有 tag v1.2.3/)
    const cases = [
        { selected: '1.2.1', metadata: { frameworkVersion: '9.9.9', cliCompatibility, frameworkInternalProtocolCode: 1 }, manifest: dependencies, error: /源码声明.*不符/ },
        { selected: '1.2.2', metadata: { frameworkVersion: '1.2.2', frameworkInternalProtocolCode: 1 }, manifest: dependencies, error: /cliCompatibility/ },
        { selected: '1.2.3', metadata: { frameworkVersion: '1.2.3', cliCompatibility: cliCompatibility + 1, frameworkInternalProtocolCode: 1 }, manifest: dependencies, error: /CLI 兼容契约/ },
        { selected: '1.2.4', metadata: { frameworkVersion: '1.2.4', cliCompatibility }, manifest: dependencies, error: /frameworkInternalProtocolCode/ },
        { selected: '1.2.5', metadata: { frameworkVersion: '1.2.5', cliCompatibility, frameworkInternalProtocolCode: 1 }, manifest: {}, error: /依赖清单无效/ },
    ]
    for (const item of cases) {
        await input.tag(item.selected, item.metadata, item.manifest)
        input.handle(() => ({ body: { version: item.selected, canton: { cliCompatibility } } }))
        await assert.rejects(input.service.selectVersion(item.selected, input.input, testTask()), item.error)
        assert.equal(await input.cache.find({ repository: input.repository, ref: `v${item.selected}` }), undefined)
    }
    for (const directory of ['entries', 'requests', 'tmp']) assert.deepEqual(await readdir(join(input.user.dependencySources, directory)), [])
})

test('远端 tag 移动后拒绝旧缓存，保留原源码快照', async t => {
    const input = await fixture(t)
    const old = await input.tag()
    await input.service.selectVersion(version, input.input, testTask())
    await write(join(input.repository, 'changed.txt'), 'changed')
    await input.run('add', '.')
    await input.run('commit', '--quiet', '-m', 'changed')
    await input.run('tag', '-f', `v${version}`)
    await assert.rejects(input.service.selectVersion(version, input.input, testTask()), /commit 与远端 tag 不一致/)
    assert.equal((await input.cache.find({ repository: input.repository, ref: `v${version}` }))!.commit, old)
})
