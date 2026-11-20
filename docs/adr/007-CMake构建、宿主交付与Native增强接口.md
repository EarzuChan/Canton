# 007 CMake 构建、宿主交付与 Native 增强接口

日期 2026-11-20。

## 背景

承接 [ADR 001](001-Windows用户态目标、应用入口与受管工程.md)、[ADR 003](003-App与Window模型、Owner与模态.md)、[ADR 004](004-原生窗口外观、自绘标题栏与任务栏身份.md) 与 [ADR 006](006-D3D11渲染管线、HLSL着色器与离屏层.md)，本篇确定框架与工程的构建系统、两种宿主模式的交付与 CRT 策略，以及 `native/` 作为原生增强层（而非用户手写整套宿主）时的 C++ 扩展接口边界。

## 构建系统：统一采用 CMake

框架本体与启用 `native/` 增强的工程统一采用 **CMake（C++20 + MSVC `/utf-8`）**。

- 直接复用并收整现有 CLI 中已沉淀的受管 CMake 接线、源码缓存与 `sync` 迁移机制，降低改造成本，避免环境配置与框架升级失控。
- 贴合 Windows C++ 生态，便于用户在 `native/` 中经由 `FetchContent`、`vcpkg` 或 `find_package` 接入第三方原生库。

## 宿主交付与 CRT 运行库策略

应用生命周期、窗口树与渲染主循环始终由框架宿主与 TS 侧共同主导。**无论工程是否启用 `native/`，均统一使用框架官方预编译的 `/MT` Windows 宿主**（不再要求用户本地拉取并重编框架引擎与 QuickJS 源码），两者共用同一套 `app/src/main.ts` 入口契约与 PE 打包链路，且均仅依赖 Windows 系统自带 DLL（`d3d11`、`dxgi`、`dcomp`、`dwrite`、`d3dcompiler_47` 等）：

- **纯 TS 工程（默认）**：
  - 使用官方预编译的基础版（`mini`）宿主，随框架版本由 CLI 按需拉取缓存。Dev / Live 直连 Vite 热更新，开发者本机无需安装 VS、MSVC 或 CMake。
  - Dist / Pack 将预编译宿主复制改名为 `<AppName>.exe`，随包交付 QuickJS 字节码（`.cbc`）与静态资源（图片、字体、预编译 DXBC），并按 [ADR 004](004-原生窗口外观、自绘标题栏与任务栏身份.md) 注入 PE 图标、版本元数据与 Per-Monitor V2 DPI 清单。
  - 宿主静态链接 **`/MT`** CRT，单 `.exe` 干净交付，终端用户免装 VC++ Redist。
- **启用 `native/` 原生扩展的工程**：
  - 使用官方预编译的可扩展版（`ext`）宿主（本身仍为 **`/MT`**）；宿主 2×2 预编译矩阵（`debug/release × mini/ext`）与加载规矩见 [ADR 009](009-宿主加载规矩、Native统一结构与扩展交付.md)。
  - 用户不手写 `WinMain`、消息循环、QuickJS 或 D3D11 初始化，也无需编译框架本体；CLI 按需拉取轻量 `canton-sdk` 头文件包并受管维护精简的 `native/CMakeLists.txt`，仅将用户自己的原生代码快速编译为独立扩展 DLL，由宿主经纯 C ABI 边界安全挂载。从纯 TS 切换到启用 `native/` 时，`app/` 下入口代码无需改动。
  - **CRT 彻底解耦**：因宿主与扩展 DLL 之间严格走 C ABI（绝不跨 DLL 传 STL 对象或跨堆释放），宿主永远保持 `/MT`，而用户的扩展 DLL 可自由选用 `/MT` 或 **`/MD`**（接入外部预编译 `/MD` C++ DLL / `.lib` 或 vcpkg 库时不再与宿主发生 `LNK2038` 运行库冲突）。当扩展 DLL 选用 `/MD` 或依赖第三方 DLL 时，CLI 在 Dist 打包时按需执行 **App-Local 部署**（随产物携带对应 `vcruntime`/`msvcp` 及依赖 DLL，体积约 1MB），使终端用户同样免装 VC++ Redist。

## `native/` 原生增强接口

底层经由带版本号的纯 C ABI 导出入口与函数表通信（兼顾 ABI 稳定、CRT 隔离以及未来接入 Rust/Zig 等其他原生语言的可能）；面向 C++ 用户，由 `canton-sdk` 的 **Header-only C++20 封装层**提供统一入口 `void canton_register_native(canton::HostBuilder& host)`（自动展开为 C 导出符号，彻底取代 Arrange 时期的全局单例与 `void*` 类型擦除），并按职责划分为业务能力与 UI 高性能增强两类（同时提供 `onInit` / `onShutdown` 钩子供管理第三方 SDK 生命周期）：

- **业务原生服务（DLL、IPC、外部库与系统对话框）**：
  - 按服务名注册 `.sync(...)`（微秒级非阻塞，UI/JS 线程直接返回）与 `.async(...)`（默认推荐：宿主派发至后台工作线程或独立 **STA 线程**以承载 `IFileOpenDialog` 等 COM 阻塞对话框，完成后切回 UI 线程结算 TS `Promise`，绝不阻塞渲染与消息循环）。
  - 窗口互操作承接 [ADR 003](003-App与Window模型、Owner与模态.md)：TS 直接传入 `WindowHandle` 或 `WindowState`，C++ 经 `CallContext::lookupHwnd(...)` 安全反查活跃 `HWND`。
  - 原生推送分双轨：离散事件经线程安全 `EventEmitter` 投递回 UI 线程；高频连续数值流沿用 `DataSource`（每帧原地写 `Float32Array` 推进版本，零响应式与 GC 分配）。
- **UI 高性能增强（原生绘制、外部 GPU 纹理与原生布局）**：
  - **强类型 C++ 2D 绘制 `canton::DrawCanvas`（主力）**：由 TS 侧 `DrawScope.drawNative(name, { params })` 调用，API 风格与 `DrawScope` 一致，底层经函数表直连框架 D3D11 批渲染器。高频波形、图表、自定义控件等原生绘制无需触碰底层 D3D11 状态机。
  - **外部 GPU 纹理零拷贝桥接**：3D 引擎视口或硬件视频解码（如 D3D11VA）可将渲染结果 `ID3D11Texture2D`（COM 本身即 Windows 标准跨 DLL ABI）包装为 Canton `Painter`（GPU 零拷贝纹理源）交付 TS 组合，直接享有布局、圆角裁剪、阴影与 [ADR 006](006-D3D11渲染管线、HLSL着色器与离屏层.md) 的 `RenderEffect`，避免中途打断主渲染批（必要时宿主暴露只读 D3D11 设备访问器）。
  - **原生测量与布局**：通过 `host.measurePolicy(name, schema, fn)` 注册强类型 C++ 策略，在 `LayoutTree` 测量阶段直接完成测量与放置，全程不回跳 QuickJS。

## 代价与后续边界

- 框架需同步构建并分发预编译 Windows 宿主二进制与 `canton-sdk` 头文件包，并长期遵守 C ABI 演进纪律（结构体带尺寸/版本、不跨边界裸传 STL）；启用 `native/` 扩展的工程本地仍需准备 MSVC 与 CMake 工具链，但仅编译用户自身扩展 DLL。
- 应用路径与存储边界见 [ADR 008](008-应用路径、只读资产与文件存储边界.md)；2×2 宿主矩阵、加载规矩与扩展 DLL 防劫持校验见 [ADR 009](009-宿主加载规矩、Native统一结构与扩展交付.md)。
