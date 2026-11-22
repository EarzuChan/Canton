import pkg from "../package.json" with {type: "json"}

export const cliName = "canton"
export const cliVersion = pkg.version
export const cliCompatibility = pkg.compatibility
export const uiManifestName = 'canton-ui.json'
export const uiPackageFormat = 3
export const uiBuildStageEnvironment = 'CANTON_UI_BUILD_STAGE'
export const uiBuildScriptFormatEnvironment = 'CANTON_UI_BUILD_SCRIPT_FORMAT'
export const defaultUiPackageFormat = 'zip'
export const defaultUiScriptFormat = 'bytecode'
export const uiBytecodeCompilerTarget = 'canton_ui_bytecode'
export const quickJsIdentityDirectory = 'canton-quickjs-identity'
export const cliDescription = pkg.description
export const supportedCliPlatforms = ["win32"] as const
export const frameworkRegistryTimeoutMs = 15000
export const frameworkVerificationConcurrency = 2

export const defaultFetchUrl = "https://github.com/EarzuChan/Canton.git"
export const defaultNodeRegistryUrl = "https://registry.npmjs.org"
export const frameworkPackageName = "@canton/framework"
export const frameworkVersionFileName = "canton.version.json"
export const minimumNodeMajor = 24
export const minimumCmakeVersion = "3.24.0"
export const defaultDevServerUrl = "http://127.0.0.1:9178"
export const defaultUiOutputDirectory = "dist"
export const localWorkDirectory = ".canton"
export const uiToolchainVersions = { vite: pkg.dependencies.vite, tsx: pkg.devDependencies.tsx, typescript: pkg.devDependencies.typescript, nodeTypes: pkg.devDependencies["@types/node"] } as const
export const defaultProjectDirectories = { ui: "ui", native: "native", artifacts: "artifacts" } as const
export const defaultProjectIconPath = "assets/icon.png"
export const defaultNativeGenerator = "Ninja"
export const processStopGraceMs = 2000
export const processReapPollMs = 20
// 主进程退出后内核对 Job 成员的收尾有短暂延迟，先等其自然排空再判定残留
export const processExitDrainMs = 150
// 兜底扫描要冷启动 PowerShell 并枚举全机进程，量级与优雅停止等待不同
export const processTreeScanTimeoutMs = 30000
export const toolProbeTimeoutMs = 15000
export const frameworkSourceProbeTimeoutMs = 30000
export const devReadinessTimeoutMs = 60000
// Windows 的 cl、link、rc 不声明长路径支持（即使系统已启用 LongPathsEnabled），完整路径超过 259 字符就无法打开文件，因此 Windows 构建树放在短的哈希目录下
export const windowsMaxPathLength = 259
export const windowsBuildPathTailBudget = 180
export const windowsExternalSourceTailBudget = 200
export const buildOwnerFileName = "owner.json"
export const cmakeQueryClientName = "canton"
export const cmakePreparationFileName = "preparation.json"
export const cmakeDefinitionsFileName = "definitions.json"
export const reservedCmakeDefinitions = ["CMAKE_BUILD_TYPE", "CMAKE_GENERATOR", "CMAKE_GENERATOR_PLATFORM", "CMAKE_MAKE_PROGRAM", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"] as const
export const userDirectoryName = ".canton"
export const userDirectoryEnvironment = "CANTON_HOME"
export const projectRegistrySchema = 2
export const transactionSchema = 1
export const sourceReferencesFileName = "source-references.json"
export const sourceReferencesSchema = 1
export const nativeVerificationFileName = "canton-native-verification.json"
export const nativeVerificationSchema = 1
export const nativeDevReadinessProtocolCode = 1 // CHECK：在 Sync 等中不再读取校验实装的框架依赖
export const sourceCacheSchema = 2
export const sourceLocalTimeoutMs = 600000
export const nativeDependencyManifest = "cmake/NativeDependencies.json"
export const windowsObjectPathMax = 200
export const operationPollMs = 100
export const operationWaitMs = 600000
export const operationProtocolVersion = 1
export const uiPreparationFileName = "ui-preparation.json"
export const defaultDevReadinessPath = "/@canton/modules"
