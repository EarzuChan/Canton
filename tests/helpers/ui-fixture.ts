import { cp, mkdir, symlink, writeFile } from 'node:fs/promises'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const repository = fileURLToPath(new URL('../..', import.meta.url))

async function prepareWorkspace(root: string): Promise<void> {
    await mkdir(root, { recursive: true })
    await mkdir(resolve(root, 'node_modules/@canton'), { recursive: true })
    await symlink(resolve(repository, 'packages/framework'), resolve(root, 'node_modules/@canton/framework'), process.platform === 'win32' ? 'junction' : 'dir')
    await writeFile(resolve(root, 'package.json'), JSON.stringify({ private: true, type: 'module' }))
}

export async function prepareUiFixture(root: string): Promise<void> {
    await prepareWorkspace(root)
    await cp(resolve(repository, 'tests/fixtures/app-smoke/src'), resolve(root, 'src'), { recursive: true })
}

export async function prepareRuntimeFixtureWorkspace(root: string): Promise<void> {
    await prepareWorkspace(root)
    await cp(resolve(repository, 'tests/fixtures'), resolve(root, 'tests/fixtures'), { recursive: true })
    await writeFile(resolve(root, 'tsconfig.json'), JSON.stringify({ extends: resolve(repository, 'tsconfig.json'), include: ['tests/fixtures/**/*.ts'] }))
}
