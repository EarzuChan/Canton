import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { resolve } from "node:path"
import { npmSubprocessEnv, repoRoot, run } from "./common.ts"
import { readCantonVersionContract } from "./version-contract.ts"
import { CANTON_MACRO_PATTERN } from '../packages/vite-plugin/src/constraints.ts'

const contract = readCantonVersionContract()
const consumerDir = resolve(repoRoot, "build/npm-package-consumer")
const frameworkTarball = resolve(repoRoot, `artifacts/npm/canton-framework-${contract.frameworkVersion}.tgz`)

async function runNpm(args: readonly string[]): Promise<void> {
    await run("cmd.exe", ["/d", "/c", "npm.cmd", ...args], { cwd: consumerDir, env: npmSubprocessEnv() })
}

// 验证当前源码生成的发布包，不复用旧产物
await run(process.execPath, ["--import", "tsx", "scripts/pack-npm-package.ts", "--framework-only"])
if (!existsSync(frameworkTarball)) throw new Error(`缺少 Framework 发布包： ${frameworkTarball}`)

rmSync(consumerDir, { recursive: true, force: true })
mkdirSync(resolve(consumerDir, "src"), { recursive: true })
cpSync(resolve(repoRoot, "tests/fixtures/app-smoke/src"), resolve(consumerDir, "src"), { recursive: true })
cpSync(resolve(repoRoot, "tests/fixtures/package-consumer/vite.config.ts"), resolve(consumerDir, "vite.config.ts"))
cpSync(resolve(repoRoot, "tests/fixtures/package-consumer/check-entries.ts"), resolve(consumerDir, "src/check-entries.ts"))
const consumerEntry = resolve(consumerDir, "src/main.ts")
writeFileSync(consumerEntry, `import './check-entries.ts'\n${readFileSync(consumerEntry, "utf8")}`)
writeFileSync(resolve(consumerDir, "tsconfig.json"), JSON.stringify({ compilerOptions: { target: "ES2022", module: "ESNext", moduleResolution: "Bundler", allowImportingTsExtensions: true, noEmit: true, skipLibCheck: true, strict: true, lib: ["ES2022", "DOM"] }, include: ["src/**/*.ts", "src/**/*.cta"] }, null, 4))
writeFileSync(resolve(consumerDir, "check.mts"), [
    "import { checkCtaProject } from '@canton/framework/vite'",
    "const diagnostics = checkCtaProject('tsconfig.json', ['src'])",
    "if (diagnostics.length) throw new Error(JSON.stringify(diagnostics, null, 4))",
    "console.log('[CantonPackageConsumer]', '发布包 CTA 参数、内容与脚本类型检查通过')",
].join("\n"))
writeFileSync(resolve(consumerDir, "package.json"), `${JSON.stringify({
    name: "canton-npm-package-consumer",
    private: true,
    type: "module",
    scripts: {
        build: "vite build --configLoader runner",
    },
    dependencies: {
        "@canton/framework": `file:../../artifacts/npm/canton-framework-${contract.frameworkVersion}.tgz`,
    },
}, null, 2)}\n`)

await runNpm(["install"])
await run(process.execPath, ["--import", import.meta.resolve("tsx"), "check.mts"], { cwd: consumerDir, env: npmSubprocessEnv() })
await runNpm(["run", "build"])

const appBundle = resolve(consumerDir, "dist/app.js")
if (!existsSync(appBundle)) throw new Error(`发布包消费者未生成产物： ${appBundle}`)
const source = readFileSync(appBundle, "utf8")
const macro = source.match(CANTON_MACRO_PATTERN)
if (macro) throw new Error(`发布包产物残留未替换的编译宏： ${macro[0]}`)

console.log("[CantonPackageVerify]", `Framework ${contract.frameworkVersion} 发布包安装、分层入口、CTA 类型检查与 Vite 构建通过`)
