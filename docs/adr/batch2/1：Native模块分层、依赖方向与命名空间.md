# 001 Native 模块分层、依赖方向与命名空间

日期 2026-11-21。

## 背景

[batch1 ADR 009](../batch1/9：宿主加载规矩、Native统一结构与扩展交付.md) 把 `native/` 收拢为单一工程并划分了子目录，但没有约定子目录之间谁能依赖谁、命名空间怎么对应，以及日志、时钟这类各层都要用的工具放在哪。M1 清理时暴露出几处问题：`Log` 放在 `quickjs/`，单调时钟放在 `runtime/`，`win32/` 为了取时间反过来引用 `runtime/`；Arrange 时期“QuickJS 可选”留下的 `CANTON_WITH_QUICKJS_NG` 条件编译散落各处；`runtime/` 的一部分类型挂在顶层 `canton::`。

## 决定

- **依赖方向**：`core ← quickjs ← runtime ← host`；`win32`、`d3d11` 只依赖 `core`。`host` 是唯一的装配点，`win32`、`d3d11` 通过回调或 `core` 里的接口接到 `runtime`，三者之间不直接引用。不允许反向或绕道 include。
- **命名空间与目录一一对应**：`canton::core`、`canton::quickjs`、`canton::runtime`、`canton::win32`、`canton::d3d11`、`canton::host`。顶层 `canton::` 只留给 `native/include/canton/` 的公开 SDK（见 [batch1 ADR 007](../batch1/7：CMake构建、宿主交付与Native增强接口.md)），C ABI 符号用 `canton_` 前缀，内部类型不出现在 SDK 头文件里。
- **各层共用的工具放 `core`**：日志（`Log`、`LogLevel`）和单调时钟在 `core`，其它各层都用这一份。要调用系统 API 的部分不进 `core`，例如控制台着色由 `win32` 提供、`host` 启动时打开，`core` 默认输出无色日志。
- **QuickJS-NG 必选**：它是 Canton 唯一的脚本引擎，`quickjs/`、`runtime/` 不保留“没有 QuickJS”的条件编译分支。

## 为什么不选别的

- **让 `win32`、`d3d11` 依赖 `runtime` 或 `quickjs`**：平台层会被脚本层一起拖进来，单测和替换都变重，`runtime` 与平台层之间也容易形成循环依赖。
- **`Log` 留在 `quickjs`**：按上面的依赖方向，`win32`、`d3d11` 就用不了它，只能各写一套。
- **顶层 `canton::` 混放内部类型**：会和 SDK 的 `canton::HostBuilder`、`canton::DrawCanvas` 等公开名字撞车，SDK 头文件也难以保证不泄露内部实现。

## 代价

- `host` 要承担全部装配胶水；放进 `core` 的接口要克制，避免 `core` 变成什么都往里放的公共层。
- 日志着色这类平台增强必须由 `host` 显式打开，忘了装配时只会退化成无色输出，不会报错。
