# 009 宿主加载规矩、Native 统一结构与扩展交付

日期 2026-11-20。

## 背景

承接 [ADR 001](001-Windows用户态目标、应用入口与受管工程.md)–[ADR 008](008-应用路径、只读资产与文件存储边界.md)，在明确应用入口归于 TS 侧、宿主生命周期由框架统一接管后，本篇确定四项落地与迁移决策：2×2 预编译宿主矩阵、Live / Dist 加载规矩、`native/` 代码库统一结构与扩展 DLL 安全边界，以及全仓脱离 Arrange 品牌后的统一命名与版本归零方针。

## 2×2 预编译宿主矩阵（同源条件编译）

框架采用单一宿主源码，通过两个正交编译宏 **`CANTON_ENABLE_DEV`**（`0 | 1`）与 **`CANTON_ENABLE_NATIVE_EXT`**（`0 | 1`）编译产出四份 `/MT` 预编译宿主，另配一份共用的字节码编译工具 `canton_ui_bytecode.exe`：

| 组合 | 预编译制品 | 定位 |
| --- | --- | --- |
| `DEV=1, EXT=0` | `canton-host-dev.exe` | 纯 TS 工程 `canton dev`（debug 风味） |
| `DEV=0, EXT=0` | `canton-host.exe` | 纯 TS 工程 `release` / `pack` 成品（`mini` 基础版） |
| `DEV=1, EXT=1` | `canton-host-ext-dev.exe` | 启用 `native/` 的 `canton dev`（debug 风味） |
| `DEV=0, EXT=1` | `canton-host-ext.exe` | 启用 `native/` 的 `release` / `pack` 成品（`ext` 可扩展版） |

- **最小攻击面与按需拉取**：纯 TS 工程仅缓存并使用 `mini` 组（`EXT=0`），成品二进制层面彻底不存在外部 DLL 加载入口、C ABI 分发表与原生服务桥接代码；启用 `native/` 后，CLI 按需拉取 `ext` 组（`EXT=1`）与轻量 `canton-sdk` 头文件包。
- **两条编译铁律**：
  1. 两个开关只允许出现在构建接线（控制是否编入 Live / Native 扩展源文件）以及 `host/`、`quickjs/` 的顶层装配入口，**严禁将 `#ifdef` 分支渗透进 `core/`、`win32/`、`d3d11/`**。
  2. 四份宿主与唯一的 `canton_ui_bytecode.exe` 必须链接同一套固定编译参数的 `qjs` 静态库（沿用统一 `/O2` 策略），保证共享同一个 `QuickJsBuildIdentity`，编出的 `.cbc` 字节码包在四份宿主间完全通用。

## 宿主加载规矩（风味默认 × 强配策略 × 编译期底线）

彻底废除 Arrange 时期由用户在 C++ 中调用 `useLive()` / `useDist()` 的配置方式。加载行为划分为三种策略：

- **`live-then-dist`（简称 `lfd`）**：优先连接 Live DevServer；连不上且存在随包 Dist 时回退至 Dist，并**弹出显式诊断提示**（避免开发者误以为运行在 Live）；若无 Dist 则进入错误屏并支持重试。
- **`only-live`（简称 `live`）**：仅连接 Live DevServer；连不上直接进入错误屏并支持重试，绝不静默回退旧 Dist。
- **`only-dist`（简称 `dist`）**：不启动也不连接 DevServer，仅读取随包 Dist。

策略受编译期物理边界与开发风味双重约束：

| 场景 | 使用的宿主 | 允许的策略 | 默认策略 |
| --- | --- | --- | --- |
| `canton dev`（默认 `--flavor debug`） | Dev 宿主（`DEV=1`） | `lfd` / `live` / `dist` | **`live-then-dist`（`lfd`）** |
| `canton dev --flavor release` | Dist 宿主（`DEV=0`） | **仅 `dist`**（强配 `live`/`lfd` 时 CLI 直接报错拒绝） | **`only-dist`（`dist`）** |
| `canton pack` 正式成品 | Dist 宿主（`DEV=0`） | **编译期锁死 `dist`** | **`only-dist`（`dist`）** |

- **强配优先级（`debug` 风味下）**：命令行 `--source <live|dist|lfd>` > `canton.local.yaml`（`dev.sourcePolicy`，个人偏好不进 Git）> 风味默认。`canton.project.yaml` 不控制开发期加载策略。当 `debug` 风味配置为 `dist` 时，CLI 自动跳过启动 Vite DevServer。
- **Dev 通道不污染 `argv`**：Live 地址、加载策略、首帧就绪文件/令牌以及 [ADR 008](008-应用路径、只读资产与文件存储边界.md) 的开发期路径重定位，统一经内部环境变量 `CANTON_DEV_*` 注入 Dev 宿主；Dist 宿主在编译期彻底剥离 LiveClient、WebSocket、HMR、诊断浮层与所有 `CANTON_DEV_*` 解析逻辑。进程命令行 `argv` 完整留给 [ADR 005](005-WindowsShell集成、托盘与启动唤醒.md) 的 `LaunchRequest`。

## Native 统一结构与扩展 DLL 安全边界

- **取消三层子工程拆分，统一 `native/` 目录**：Canton 锁定 Windows + D3D11 + DirectWrite + QuickJS-NG，不再保留 Arrange 的 `arrange_core` / `arrange_quickjs` / `arrange_juce` 三工程拆分（以及全部 JUCE、Audio Interop 与 macOS 残留）。统一收拢为单一 `native/` 工程，内部按职责划分子目录：
  - `include/canton/`：仅存放面向 `native/` 扩展用户的 `canton-sdk` 公开头文件（`canton_abi.h` 与 Header-only C++20 封装）；
  - `src/core/`：布局、事务场景树、输入状态机、保留绘制指令（保持零 Win32 / D3D11 依赖，确保纯逻辑无头单测轻量高速）；
  - `src/quickjs/`：QuickJS-NG 宿主、协议桥接与 `canton_ui_bytecode` 工具；
  - `src/runtime/`：原寄居于 `arrange_juce` 的宿主编排、包加载、Live 客户端与诊断层扶正归位；
  - `src/win32/`：Win32 窗口、外观、TSF/IMM、Shell/托盘/SMTC、路径与文件实现；
  - `src/d3d11/`：D3D11 2D 管线、DirectWrite、HLSL/`D3DReflect`、离屏层与 `DrawCanvas` 实现；
  - `src/host/`：`WinMain` 入口、加载规矩与扩展装配。
- **扩展 DLL 防劫持与版本握手**：承接 [ADR 007](007-CMake构建、宿主交付与Native增强接口.md) 的 C ABI 扩展模式，`ext` 宿主严禁盲扫或自动加载同目录未知 DLL。扩展模块必须在打包清单（`canton-ui.json`）中显式声明相对路径与 SHA-256；Dist 模式下加载前强制校验哈希并使用 `LOAD_LIBRARY_SEARCH_APPLICATION_DIR` 限制搜索范围；初始化入口校验 `CANTON_NATIVE_ABI_VERSION` 与函数表 `struct_size`。

## 品牌更名、文件后缀与版本归零

借首轮迁移一次性剥离 Arrange 品牌印记，避免新老混杂：

- **统一品牌与标识符**：`@arrange/*` → `@canton/*`、`Arrange*` → `Canton*`、`Canton::*`、`.arrange/` → `.canton/`、`ARRANGE_*` → `CANTON_*`、`canton.version.json`、`canton.project.yaml`、`canton.local.yaml`、`canton-ui.json`。
- **DSL 后缀与指令**：单文件后缀定为 **`.cta`**（CanTon Arrangables），模板指令由 `a-*` 改为 **`c-*`**（`c-if`、`c-else-if`、`c-else`、`c-for`、`c-bind`；`:`、`#slot`、`&handle` 不变）；保留 **`Arrangable`** 与 **`Rearrange`** 作为核心概念命名（不视为旧品牌）。
- **字节码后缀**：QuickJS 预编译模块后缀定为 **`.cbc`**（Canton Bytecode），取代原 `.aex`。
- **版本号与协议号归零**：Framework 与 CLI 统一从 **`0.1.0`** 起步，内部协议号归零为 **`1`**，彻底甩掉历史包袱。

## 代价与后续边界

- 每次框架发版需同步构建并校验四份预编译宿主、`canton_ui_bytecode.exe` 与 `canton-sdk` 头文件包，并与 `@canton/framework` 版本号严格锁定。
- C ABI 与 Header-only C++20 SDK 需长期遵守二进制兼容纪律；后续演进按 [路线图](../proj/ROADMAP.md) 逐期落地。
