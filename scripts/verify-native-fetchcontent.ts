import { existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { resolve } from "node:path"
import { repoRoot, run } from "./common.ts"
import { configureNativeBuild, nativeCmake, nativeExecutable } from "./native-toolchain.ts"
import { readCantonVersionContract } from "./version-contract.ts"
import { withNativeSession, nativeSessionRun } from "./native-session.ts"

await withNativeSession(async () => {


    const contract = readCantonVersionContract()
    const consumerRoot = resolve(repoRoot, "build/native-fetchcontent-consumer")
    const buildDir = resolve(consumerRoot, "build")
    const cantonSource = repoRoot.replaceAll("\\", "/")
    const gitRepository = process.env.CANTON_FETCHCONTENT_GIT_REPOSITORY?.trim()
    const gitTag = process.env.CANTON_FETCHCONTENT_GIT_TAG?.trim()

    if (Boolean(gitRepository) !== Boolean(gitTag)) {
        throw new Error("CANTON_FETCHCONTENT_GIT_REPOSITORY and CANTON_FETCHCONTENT_GIT_TAG must be provided together")
    }

    function cantonFetchContentDeclaration(): string {
        if (gitRepository && gitTag) {
            return [
                "FetchContent_Declare(canton",
                `  GIT_REPOSITORY [[${gitRepository}]]`,
                `  GIT_TAG [[${gitTag}]]`,
                "  GIT_SHALLOW TRUE",
                ")",
            ].join("\n")
        }
        return `FetchContent_Declare(canton SOURCE_DIR [[${cantonSource}]])`
    }

    if (existsSync(consumerRoot)) rmSync(consumerRoot, { recursive: true, force: true })
    mkdirSync(consumerRoot, { recursive: true })

    writeFileSync(resolve(consumerRoot, "CMakeLists.txt"), `
cmake_minimum_required(VERSION 3.24)
project(CantonFetchContentConsumer LANGUAGES C CXX)
include(FetchContent)
${cantonFetchContentDeclaration()}
FetchContent_MakeAvailable(canton)
add_executable(canton_fetchcontent_consumer main.cpp)
target_link_libraries(canton_fetchcontent_consumer PRIVATE Canton::core)
`.trimStart())

    writeFileSync(resolve(consumerRoot, "main.cpp"), `
#include <core/Version.h>
#include <stdexcept>
#include <string>

int main() {
    static_assert(canton::core::RuntimeVersion == ${contract.frameworkInternalProtocolCode}u);
    try {
        const std::string version = canton::core::version();
        if (version.empty()) throw std::runtime_error("Canton 版本不能为空");
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
`.trimStart())

    await configureNativeBuild(consumerRoot, buildDir, "Debug")
    await nativeCmake(["--build", buildDir])
    await nativeSessionRun(nativeExecutable(buildDir, "canton_fetchcontent_consumer"))

    const sourceLabel = gitRepository && gitTag ? `${gitRepository}#${gitTag}` : cantonSource
    console.log('[CantonNativeVerify]', `verified native FetchContent consumer for Canton ${contract.frameworkVersion} from ${sourceLabel}`)

}, [resolve(repoRoot, "build/native-fetchcontent-consumer/build")])
