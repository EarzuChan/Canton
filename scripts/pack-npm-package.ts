import { cpSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs"
import { basename, resolve } from "node:path"
import { npmSubprocessEnv, repoRoot, run } from "./common.ts"
import { assertCantonVersionContract, readCantonVersionContract } from "./version-contract.ts"

const artifactsDir = resolve(repoRoot, "artifacts/npm")
const stagingRoot = resolve(artifactsDir, "staging")
const frameworkSourceDir = resolve(repoRoot, "packages/framework")
const cliSourceDir = resolve(repoRoot, "cli")
const frameworkBundleDirs = ['vite-plugin', 'reactivity', 'compiler', 'shared'].map(name => resolve(repoRoot, 'packages', name))

function readJson(path: string): Record<string, unknown> {
    return JSON.parse(readFileSync(path, "utf8")) as Record<string, unknown>
}

function writeJson(path: string, value: Record<string, unknown>): void {
    writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`)
}

async function runNpm(args: readonly string[]): Promise<void> {
    await run("cmd.exe", ["/d", "/c", "npm.cmd", ...args], { env: npmSubprocessEnv() })
}

function copyIfExists(source: string, target: string): void {
    if (!existsSync(source)) return
    cpSync(source, target, {
        recursive: true,
        filter: (path) => !path.includes(`${resolve(source, "node_modules")}`),
    })
}

function copyPackageSource(sourceDir: string, targetDir: string): void {
    mkdirSync(targetDir, { recursive: true })
    const items = ["src", "README.md", "LICENSE", "UPSTREAM.md"]
    for (const item of items) copyIfExists(resolve(sourceDir, item), resolve(targetDir, item))
    writeJson(resolve(targetDir, "package.json"), readJson(resolve(sourceDir, "package.json")))
}

function packageStageName(sourceDir: string): string {
    const manifest = readJson(resolve(sourceDir, "package.json"))
    const rawName = String(manifest.name ?? basename(sourceDir))
    return rawName.replace(/^@/, "").replaceAll("/", "-")
}

function packageScopeName(sourceDir: string): string {
    return basename(sourceDir).replace(/^canton-/, "")
}

function assertStagedTreeClean(stageDir: string): void {
    const badPatterns = [/[/\\]__tests__[/\\]/, /[/\\]__benchmarks__[/\\]/, /[/\\]dist[/\\]/]
    const stack = [stageDir]
    while (stack.length) {
        const current = stack.pop()!
        for (const item of readdirSync(current)) {
            const path = resolve(current, item)
            const relative = path.slice(stageDir.length)
            if (badPatterns.some((pattern) => pattern.test(relative))) {
                throw new Error(`staged npm package contains non-publish source: ${path}`)
            }
            const stat = statSync(path)
            if (stat.isDirectory()) stack.push(path)
        }
    }
}

assertCantonVersionContract()
const contract = readCantonVersionContract()

rmSync(stagingRoot, { recursive: true, force: true })
mkdirSync(artifactsDir, { recursive: true })
mkdirSync(stagingRoot, { recursive: true })

const stageDir = resolve(stagingRoot, packageStageName(frameworkSourceDir))
copyPackageSource(frameworkSourceDir, stageDir)

const cantonScopeDir = resolve(stageDir, "node_modules/@canton")
mkdirSync(cantonScopeDir, { recursive: true })
for (const bundleDir of frameworkBundleDirs) {
    copyPackageSource(bundleDir, resolve(cantonScopeDir, packageScopeName(bundleDir)))
}

assertStagedTreeClean(stageDir)
await runNpm(["pack", stageDir, "--pack-destination", artifactsDir])

console.log('[CantonPackagePack]', `packed @canton/framework ${contract.frameworkVersion} into ${artifactsDir}`)

if (!process.argv.includes('--framework-only')) {
    await runNpm(["pack", cliSourceDir, "--pack-destination", artifactsDir])
    const cliManifest = readJson(resolve(cliSourceDir, "package.json"))
    console.log('[CantonPackagePack]', `packed ${cliManifest.name} ${cliManifest.version} into ${artifactsDir}`)
}
