import { existsSync, readFileSync, readdirSync, writeFileSync } from "node:fs"
import { resolve } from "node:path"
import { repoRoot } from "./common.ts"

export type CantonVersionContract = {
    frameworkVersion: string
    frameworkInternalProtocolCode: number
    cliVersion: string
    cliCompatibility: number
}

type JsonObject = Record<string, unknown>

const frameworkVersionPattern = /^\d+\.\d+\.\d+$/
const semverPattern = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/

export const versionFile = resolve(repoRoot, "canton.version.json")

export function readCantonVersionContract(): CantonVersionContract {
    const parsed = JSON.parse(readFileSync(versionFile, "utf8").replace(/^\uFEFF/, "")) as Partial<CantonVersionContract>
    const frameworkVersion = parsed.frameworkVersion
    if (typeof frameworkVersion !== "string" || !frameworkVersionPattern.test(frameworkVersion)) throw new Error(`Canton frameworkVersion must be <major>.<minor>.<patch>, got ${String(frameworkVersion)}`)
    const frameworkInternalProtocolCode = parsed.frameworkInternalProtocolCode
    if (typeof frameworkInternalProtocolCode !== "number" || !Number.isInteger(frameworkInternalProtocolCode) || frameworkInternalProtocolCode <= 0) throw new Error(`Canton frameworkInternalProtocolCode must be a positive integer, got ${String(frameworkInternalProtocolCode)}`)
    const cliVersion = parsed.cliVersion
    if (typeof cliVersion !== "string" || !semverPattern.test(cliVersion)) throw new Error(`Canton cliVersion must be semver, got ${String(cliVersion)}`)
    const cliCompatibility = parsed.cliCompatibility
    if (typeof cliCompatibility !== "number" || !Number.isInteger(cliCompatibility) || cliCompatibility <= 0) throw new Error(`Canton cliCompatibility must be a positive integer, got ${String(cliCompatibility)}`)
    return { frameworkVersion, frameworkInternalProtocolCode, cliVersion, cliCompatibility }
}

function readJson(path: string): JsonObject {
    return JSON.parse(readFileSync(path, "utf8").replace(/^\uFEFF/, "")) as JsonObject
}

function writeJson(path: string, value: JsonObject): void {
    writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`)
}

function packageJsonPaths(): string[] {
    const packagesRoot = resolve(repoRoot, "packages")
    return [
        resolve(repoRoot, "package.json"),
        ...readdirSync(packagesRoot).map((name) => resolve(packagesRoot, name, "package.json")).filter((path) => existsSync(path)),
    ]
}

function assertEqual(actual: unknown, expected: unknown, message: string): void {
    if (actual !== expected) throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`)
}

function assertFileContains(path: string, expected: string): void {
    const source = readFileSync(path, "utf8")
    if (!source.includes(expected)) throw new Error(`${path} does not contain ${expected}`)
}

export function syncCantonVersionContract(): void {
    const contract = readCantonVersionContract()
    for (const path of packageJsonPaths()) {
        const manifest = readJson(path)
        manifest.version = contract.frameworkVersion
        for (const field of ["dependencies", "devDependencies", "peerDependencies"] as const) {
            const dependencies = manifest[field]
            if (!dependencies || typeof dependencies !== "object" || Array.isArray(dependencies)) continue
            const dependencyRecord = dependencies as Record<string, unknown>
            for (const name of Object.keys(dependencyRecord)) if (name.startsWith("@canton/")) dependencyRecord[name] = contract.frameworkVersion
        }
        writeJson(path, manifest)
    }

    const frameworkManifestPath = resolve(repoRoot, "packages/framework/package.json")
    const frameworkManifest = readJson(frameworkManifestPath)
    const canton = frameworkManifest.canton
    if (!canton || typeof canton !== "object" || Array.isArray(canton)) frameworkManifest.canton = {};
    (frameworkManifest.canton as Record<string, unknown>).cliCompatibility = contract.cliCompatibility
    writeJson(frameworkManifestPath, frameworkManifest)

    const cliManifestPath = resolve(repoRoot, "cli/package.json")
    const cliManifest = readJson(cliManifestPath)
    cliManifest.version = contract.cliVersion
    cliManifest.compatibility = contract.cliCompatibility
    writeJson(cliManifestPath, cliManifest)

    const runtimeVersionPath = resolve(repoRoot, "packages/shared/src/version.ts")
    writeFileSync(runtimeVersionPath, [
        `export const CANTON_PACKAGE_VERSION = ${JSON.stringify(contract.frameworkVersion)}`,
        `export const CANTON_PROTOCOL_VERSION = ${contract.frameworkInternalProtocolCode}`,
        "",
    ].join("\n"))

    const nativeHeaderPath = resolve(repoRoot, "native/src/core/Version.h")
    writeFileSync(nativeHeaderPath, [
        "#pragma once",
        "",
        "namespace canton::core {",
        `    inline constexpr unsigned RuntimeVersion = ${contract.frameworkInternalProtocolCode}u;`,
        "    inline constexpr unsigned ProtocolVersion = RuntimeVersion;",
        `    inline constexpr const char* PackageVersion = ${JSON.stringify(contract.frameworkVersion)};`,
        "    const char* version() noexcept;",
        "}",
        "",
    ].join("\n"))

    const nativeSourcePath = resolve(repoRoot, "native/src/core/Version.cpp")
    writeFileSync(nativeSourcePath, [
        "#include <core/Version.h>",
        "",
        "namespace canton::core {",
        "    const char* version() noexcept {",
        "        return PackageVersion;",
        "    }",
        "}",
        "",
    ].join("\n"))
}

export function assertCantonVersionContract(): void {
    const contract = readCantonVersionContract()
    for (const path of packageJsonPaths()) {
        const manifest = readJson(path)
        assertEqual(manifest.version, contract.frameworkVersion, `${path} version diverges from canton.version.json`)
        for (const field of ["dependencies", "devDependencies", "peerDependencies"] as const) {
            const dependencies = manifest[field]
            if (!dependencies || typeof dependencies !== "object" || Array.isArray(dependencies)) continue
            for (const [name, spec] of Object.entries(dependencies as Record<string, unknown>)) {
                if (name.startsWith("@canton/")) {
                    assertEqual(spec, contract.frameworkVersion, `${path} ${field}.${name} diverges from canton.version.json`)
                }
            }
        }
    }
    assertFileContains(resolve(repoRoot, "packages/shared/src/version.ts"), `CANTON_PACKAGE_VERSION = ${JSON.stringify(contract.frameworkVersion)}`)
    assertFileContains(resolve(repoRoot, "packages/shared/src/version.ts"), `CANTON_PROTOCOL_VERSION = ${contract.frameworkInternalProtocolCode}`)
    assertFileContains(resolve(repoRoot, "native/src/core/Version.h"), `RuntimeVersion = ${contract.frameworkInternalProtocolCode}u`)
    assertFileContains(resolve(repoRoot, "native/src/core/Version.h"), `PackageVersion = ${JSON.stringify(contract.frameworkVersion)}`)

    const cliManifest = readJson(resolve(repoRoot, "cli/package.json"))
    assertEqual(cliManifest.version, contract.cliVersion, "cli/package.json version diverges from canton.version.json")
    assertEqual(cliManifest.compatibility, contract.cliCompatibility, "cli/package.json compatibility diverges from canton.version.json")

    const frameworkManifest = readJson(resolve(repoRoot, "packages/framework/package.json"))
    const canton = frameworkManifest.canton
    if (!canton || typeof canton !== "object" || Array.isArray(canton)) throw new Error("packages/framework/package.json canton metadata missing")
    assertEqual((canton as Record<string, unknown>).cliCompatibility, contract.cliCompatibility, "framework cliCompatibility diverges from canton.version.json")
}
