# 里程碑

> 实施原则：
> - 每个里程碑聚焦单一专项、按严格前后置依赖依次推进。
> - 正式示例工程到 M8 才建立，自始由 Canton CLI 托管：主示例 `demo/center`（Canton Center，启用 `native/` 的全能力 Gallery / Playground），以及极简纯 TS 工程 `demo/hello`（专门守住并验收“零 C++/CMake/MSVC 环境”的默认体验）。应用怎么摆放宿主、按什么配置启动和加载归 CLI 管，在 CLI 就绪前不为示例另开专用的启动通道。
> - M3–M7 各期用 `tests/fixtures/` 下的验收夹具验证。夹具是测试资产，不算真·Demo级“完整”工程，宿主里不得出现只为夹具存在的分支或参数。同一份夹具有两种跑法：
>   - **无头回归**：测试程序离屏渲染，做像素比对并输出截图。无头只是不开窗，显卡照常可用：基线用 WARP 出（各机器结果一致），同一批用例再在硬件显卡上复跑。M3 没有输入，主人的验收看截图和自动化结果。
>   - **开窗把玩**：仓库脚本按正式加载规矩（`CANTON_DEV_*`）拉起宿主，弹出真窗口，改 `.cta` 热更新。M3 的窗口只显示、不可交互；M4 接上输入后可上手操作，此后主人的验收以把玩为准。
> - 与验收无关的随手试验放在不进 Git 的临时目录。

## M0：设定冻结与架构蓝图：已完成

目标：完成 Canton 面向 Windows 原生桌面的首轮核心架构决策，确定实现次序与代码迁移方针，不在施工中随手发明基础语义。

范围：
- [ADR BATCH1](../adr/batch1) 定稿并完成同期一致性收口。
- 建立本路线图，并预留 `docs/ARCHITECTURE.md` 占位（待 M1 骨架落地后按主人要求正式撰写）。

验收：
- 窗口、外观、Shell、D3D11/HLSL、CMake/C ABI 扩展、路径存储、2×2 宿主矩阵与更名策略全部明确无冲突。

---

## M1：品牌更名、Native 统一与大清场：已完成

目标：完成全仓 Canton 更名、Native 目录统一与旧账清理，代码洁净优先于强行拼凑可运行/可测试，绝不造过渡假桩。施工清单见 [m1.md](m1.md)。

范围：
- **全仓更名与版本归零**：`@arrange/*` → `@canton/*`、`Arrange*` → `Canton*`、`.sfa` → `.cta`、`a-*` → `c-*`、`.aex` → `.cbc`、`arrange-ui.json` → `canton-ui.json`、`.arrange/` → `.canton/`、`ARRANGE_*` → `CANTON_*`、`canton.version.json` 归零为 `0.1.0` / 协议 `1`；保留 `Arrangable` / `Rearrange`。
- **Native 统一结构**：取消 `arrange_core` / `arrange_quickjs` / `arrange_juce` 三模块拆分，收拢为单一 `native/`（`native/include/canton/` 与 `native/src/{core,quickjs,runtime,win32,d3d11,host}/`）；彻底清除 JUCE FetchContent、Audio Interop 与 macOS 残留；原 `arrange_juce` 编排层迁入 `native/src/runtime/`，剥离 JUCE 壳（具体 WinSock/WinHTTP/WIC/ZIP 替换不强凑假桩，留待 M3 实装）。
- **清理旧修饰器**：全链路连根移除旧 CPU `M.blur` / `M.backdropBlur`；先行打掉公开 `M.align` / `M.weight` / `M.matchParentSize`（保留底层 `ParentData` 协议与 C++ 布局引擎，新 Handle 实现留到 M2）。
- **解除核心单窗暗雷**：拆掉 `FontRegistry::shared()` 全局预加载计数与 `NativePainterRegistry::global()` 进程单例，改为归属宿主实例。

验收：
- 仓库内无遗留 `arrange` 品牌、JUCE、Audio Interop 与 macOS 引用；目录与模块归位清晰干净。
- 原本完全独立的纯逻辑层（`packages/` TS 侧、`native/src/core/`、`native/src/quickjs/`）作为更名防手滑安全网保持类型检查/编译通过；涉及后续窗口、渲染、网络或新 CLI 的缺口不强求本期可编译或可运行。
- 验收通过后，按主人要求撰写第一版反映真实新骨架的 `docs/ARCHITECTURE.md`。

---

## M2：`.cta` 槽位句柄与布局正规化（ADR B1：2）：已完成

目标：在点亮新窗口前先锁定终态 `.cta` DSL 与布局 Handle 体系，确保自 M3 起所有新界面与 Demo 全部以终态语法编写、零返工。施工清单见 [m2.md](m2.md)。

范围：
- **编译器与类型检查**：新增 `&handle` 词法/解析与词法作用域遮蔽（含同名绑定、别名与解构）、砍掉 `a-slot:*`/`c-slot:*` 仅保留 `#name`、放开 `<Slot>` 具名传参、新增 `defineSlots<...>()` 与 `.cta` 离线类型检查推导和诊断。
- **运行时与布局组件**：`Content` 支持参数传递与多出口独立作用域；`Box`、`Row`、`Column`、`FlowRow`、`FlowColumn` 升级产出对应布局 Handle（接回底层 `ParentData`）；Lazy 四件套与 `Crossfade` 由传定义改为槽位传参；恢复并扩写依赖布局 Handle 的用例。
- **布局能力补齐**：按对齐线对齐（`alignBy` / `alignByBaseline`）、Flow 交叉轴填充（`fillMaxRowHeight` / `fillMaxColumnWidth`）、Lazy 项按视口填充（`fillParentMaxSize` 等）。
- **HMR 兼容**：保状态 HMR 与槽位参数更新路径适配。

验收：
- 零参/单参/多参、默认槽与具名槽、嵌套同名 Handle 别名区分通过自动化测试。
- 跨组件转交与错误父级布局诊断分两段验证：TS 侧验 Handle 片段经参数转交后不变，C++ core 验错误父级报错；从 `.cta` 贯通到布局结果的用例移交 M3。
- 现有测试与内置布局迁移完成，旧公开布局 Modifier 不可用。

---

## M3：D3D11 + DirectWrite 基础渲染与首屏闭环（ADR B1：6 基础）：已完成

目标：建成纯 D3D11 2D 渲染管线与 DirectWrite 文字字形图集，配合最小 Win32 承载窗打通从 `.cta` 编辑到原生像素上屏与 Live HMR 的完整可视化闭环。施工清单见 [m3.md](m3.md)。

范围：
- **`native/src/d3d11/` 基础渲染**：纯 D3D11 2D 批渲染（矩形、分角圆角、椭圆、边框、渐变、内外阴影、图片/九宫格/像素缓冲、任意矢量 `Path` 覆盖率/三角化与裁剪、Canvas 指令流回放）；DXGI Flip Model + DirectComposition 呈现；预乘 Alpha sRGB（`B8G8R8A8_UNORM`）。
- **文本引擎**：DirectWrite 排版接入 `TextLayoutService` 与 GPU 字形图集（Atlas），支持字体加载与预加载屏障。
- **稳健性**：Feature Level 11_0 + WARP 透明回退与 `DEVICE_REMOVED`/`DEVICE_RESET` 自动重建。
- **宿主条件编译与运行时实装**：补齐 `native/src/runtime/` 的 Windows 原生工具替换（`UiPackage` ZIP/JSON/`BCrypt`、Live WinSock/WinHTTP 客户端、WIC/SVG 图像解码）；跑通 `CANTON_ENABLE_DEV` 条件编译、`CANTON_DEV_*` 加载规矩（`lfd`/`live`/`dist`）与最小 Win32 承载窗 + Vite Live HMR。本期窗口只显示、可缩放和关闭，不接输入；密度固定 1 倍，DPI 整体留给 M4。
- **夹具的两种跑法**：无头回归的测试程序直接链接渲染库，离屏渲染、读回像素做比对并存成截图，不经宿主参数，WARP 出基线、硬件显卡复跑；开窗的启动脚本（起 Vite、注入 `CANTON_DEV_*`、拉起宿主）。
- **承接 M2**：从 `.cta` 夹具经 QuickJS 进 core 场景树，断言布局结果与错误父级报错的贯通用例；首屏点亮后补 Lazy 项的 `animateItem`（位移动画、淡入淡出），网格项的 Handle 随之交付。

验收夹具：
- **首屏点亮**：夹具跑起基础布局、文本与字体、图片、滚动/Lazy、动画、Canvas 各场景，支持改 `.cta` 热更新、错误屏与恢复。

验收：
- 在真实 Win11 桌面与无头回归环境（WARP 与硬件显卡）中稳定渲染上述图元与文字，改 `.cta` 可秒级热更，设备重置可自动恢复。

---

## M4：App / Window 多窗口、原生外观与输入（ADR B1：1 / B1：3 / B1：4）：已完成

目标：完成运行时从“1 VM : 1 视口”到“1 VM : N 原生表面”的根本扭转，落地 Windows 11 级多窗口、自绘标题栏、DPI 与原生输入体验。施工清单与实测笔记见 [m4.md](m4.md)。

范围：
- **多窗口与入口模型**：落地 `appBuilder(App).build()`（全仓清零 `createApp`）、`<App>` 与 `<Window>` 生命周期、画区作用域自然分界、每窗独立 `NativeScene`/`SceneFramePipeline`/`Density`/`FocusManager`、`WindowState` 与 `WindowHandle`、私有 `WeakMap` → `WindowId` → `HWND` 映射、Win32 `owner` 拓扑逆序销毁、非阻塞模态（`EnableWindow` + 模态锁计数 + 防焦点跌落顺序）与 0 窗错误屏兜底。
- **原生窗口外观与标题栏**：`chrome`（`system` / `customTitleBar` / `frameless`）、能力开关与 `role`、Win11 `backdrop`（Mica / Mica Alt / Acrylic，不支持环境静默回退深色底）与 `cornerPreference`、Per-Monitor V2 DPI 与 `WM_DPICHANGED` 驱动窗口级 `Density`、`&windowHandle.draggableArea / nonDraggableArea / captionButton` 5 级 `WM_NCHITTEST` 与非客户区三键防复古按钮闪现托管、`AUMID` 与进程/窗口图标。
- **原生输入**：鼠标多击与捕获、滚轮平滑与边界接力、键盘与焦点导航、光标形状、`Input` 词级操作/多击选词/`readOnly`/剪贴板接入宿主窗口；纯 Win32 IMM32 中文输入法与显式 `compositionRange` 组合模型接入 `InputTextSession`（按焦点动态启停 `HIMC`）；接通 DirectManipulation 精密触控板滚轮（`WindowsPrecisionWheelSource`）。
- **M3 移交与 Dist 物理剥离**：Dev 窗口级调试交互（`F5` 重载、`F8` 帧率徽标、`F9` / `Shift+F9` `DiagnosticsOverlay`、错误屏点击重试与 `Ctrl+C` 复制诊断）经 `DevAssembly` 注入并在 Dist 宿主编译/链接期物理剥离；恢复并扩充 `canton_focus_input`、`canton_scroll_input`、`canton_frame_submission`、`canton_lazy_runtime` 与 `canton_window_runtime`。

验收夹具（兼作临时 Demo，正式工程化的 Demo 仍得等到后续）：
- `window-interaction`（「窗口与交互」夹具）：**自绘标题栏 + Mica 背景** 的桌面工作台，覆盖多窗共享响应式状态、同帧切窗不误退、二级嵌套模态锁、置顶工具窗、无框圆角浮窗、跨 DPI 缩放与中文 IMM32 试验场。

验收：
- 多窗口创建/销毁/同帧切换、`onCloseRequest` 拦截、嵌套模态、Win11 Snap Layout 悬停最大化键、鼠标/滚轮/触控板/键盘导航、跨屏 DPI 切换与中文 IME 实机及全套自动化检查通过。
- 移交后续里程碑事项已分别归入 M5（着色器与离屏组透明度）、M6（托盘/任务栏扩展/单实例/文件存储）、M7（`EXT=1` 宿主与 `CallContext::lookupHwnd` 对接私有窗口表）与 M8（静态打包 PE 图标与默认 AUMID）。

---

## M5：HLSL 着色器、离屏合成与视觉进阶（ADR B1：6 高阶 / B3：1）：已完成

目标：释放纯 D3D11 架构的完整 GPU 表现力，交付标准 HLSL 像素着色器与 Compose 粒度离屏合成原语。施工清单与实测笔记见 [m5.md](m5.md)。

范围：
- **`RuntimeEffect` & `RuntimeShader`**：100% 标准 HLSL SM 5.0 像素着色器 + 共享 `<canton.hlsl>` 虚拟头 + `D3DReflect` 自动解析 `cbuffer`（含跨 16 字节边界、数组、矩阵、预乘颜色）与纹理/采样器槽；Live 态 `d3dcompiler_47.dll` 编译与 Vite `.hlsl` 自接受热替换（保留 uniform、报错保留上一版并弹 Toast）；Dist 态构建期由 `koffi` 直调 `d3dcompiler_47.dll` 预编译为 `assets/<name>-<hash>.dxbc` 并登记进 `canton-ui.json`，动态源码留系统 `d3dcompiler_47.dll` 兜底；设备丢失后自动从缓存 DXBC 重建。
- **双落点与离屏原语**：`shaderBrush`（覆及矩形/圆角/椭圆/边框/线段/矢量 `Path`）、D3D11 按桶离屏 RT 池、`RenderEffect`（两 Pass 可分离高斯 `blur` 含四种 `TileMode`、`offset`、`colorFilter`、`shader`、`chain`）、全量 29 种 `BlendMode`（硬件 15 种 + 拷出背景 `Load` 着色器 14 种）、`CompositingStrategy`、`M.graphicsLayer` 完备化、组透明度（升级 `M.alpha` 与 Lazy `animateItem` 淡入淡出）、`RenderLayer`（`createRenderLayer` / `recordLayer` 捕获 C++ 保留子片段树 / `drawLayer` 与直接/间接环引用检测）、`DrawScope` / `DrawCacheScope` 补齐与 `M.onGloballyPositioned` + `LayoutCoordinates` 同帧绘前零滞后派发。
- **私有旁包 `@canton/kyant-glass`**：严格仅消费 `@canton/framework` 公开出口（由 `package-layers.test.ts` 锁死），完整落地液态玻璃圆角 SDF 折射、七段光谱色散、方向/环境高光、内外阴影与 `exportedBackdrop` 双层玻璃级联。

验收夹具：
- `shader-effects`（「着色器与视觉特效」夹具）：涵盖液态玻璃实景与双层 `exportedBackdrop` 级联、`.hlsl` 生成型 `shaderBrush` 试验场、多级 `RenderEffect.chain` / `BlendMode` / `RenderLayer` 实时倒影与组透明度对比。

验收：
- WARP 与硬件显卡（RTX 5070 Ti）32/32 无头像素回归、全套 TS/Native/QuickJS/宿主/Live 验证通过；Dist 四形态包全部含预编译 `.dxbc` 且冷启首帧 296–346ms；真窗口 `.hlsl` 热替换、语法错误保留旧版与恢复在 76–83ms 内无重载生效。
- 移交后续里程碑：M6（Windows Shell 集成与 `@canton/framework/fs`）、M7（`EXT=1` 扩展宿主、`canton-sdk`、`drawNative` 与外部 `ID3D11Texture2D` 零拷贝 `Painter`）、M8（CLI 重造、`demo/center` 迁入本期场景与 `demo/hello`）。

---

## M6：Windows Shell 集成、文件存储、网络/定时与图形感知（ADR B1：5 / B1：8 / B4：1–3）

目标：打通桌面应用与 Windows Shell 深度融合、规范克制的只读资产与真实磁盘路径存储体系，同时补齐 TS 侧定时器与网络基础能力，以及面向低配/无驱环境的图形能力感知与渐进降级。施工清单见 [m6.md](m6.md)。

范围：
- **Windows Shell 集成**：App 级隐藏顶层 `HWND`（`WS_EX_TOOLWINDOW`）、`<TrayIcon>`（稳定 `NIF_GUID` 与 `TaskbarCreated` 重建）、双轨 `<SystemTrayMenu>`（原生 `HMENU`）与 `<CustomTrayMenu>`（自绘浮窗 + `&menuHandle.dismiss()`）、`<Window>` `ITaskbarList3` 进度/角标/7 槽缩略图按钮、单活跃 `<MediaSession>`（SMTC）、`LaunchRequestKey` 冷启动与单实例 IPC 热唤醒（`<App :onSecondInstance>`）、Jump List、关机拦截。
- **资产与文件存储**：TS `assets.exists` 补齐与宿主只读资产直读接口；`@canton/framework/fs`（`SHGetKnownFolderPath` 用户标准目录、`dataDir`/`tempDir` 透明重定位、后台线程池异步文件 I/O 与可选 `atomic` 原子写）。
- **图形能力感知与 WARP 无损瘦身（ADR B4：1）**：专注 D3D11 单后端不自建软渲染；`<App>` 顶层通过 `GraphicsEnvironmentKey` 响应式 `provide` 渲染器与系统桌面偏好（适配设备重建与系统设置变更，支持子树覆写）；底层管线保持完整能力不静默硬砍；`@canton/kyant-glass` 感知 `GraphicsEnvironmentKey` 在 `reduced` 档主动降级；`d3d11::Renderer` 在软件渲染态下无损收紧空闲离屏 RT 池回收与图集按需扩容。
- **定时器与取消原语（ADR B4：2，TS 侧）**：宿主事件循环原生定时队列（HMR 重载自动清空），向 `globalThis` 挂载 `setTimeout`/`clearTimeout`/`setInterval`/`clearInterval`/`queueMicrotask` 与轻量 `AbortController`/`AbortSignal`；`@canton/framework` 导出作用域感知的 `useTimeout`/`useInterval` 与非阻塞 `delay(ms, signal?)`。
- **网络 API（ADR B4：3，TS 侧）**：`@canton/framework/net`，基于 Windows 原生 WinHTTP 与后台 I/O 线程池提供 `request`/`http` 快捷方法（含 `query`/`json`/超时/大小上限/代理/重定向/`AbortSignal` 取消与同步读取响应体）、大文件流式直落盘 `downloadFile`（临时文件原子替换 + 节流进度）与结构化 `NetError`。

验收夹具：
- 「桌面融合与系统能力」夹具（托盘常驻与双轨菜单切换、任务栏进度与缩略图按钮、模拟播放器 SMTC 联动、单实例唤醒展示、本地笔记原子持久化与开发期重定位 / 绿色便携模式、本地环回 HTTP 请求/下载/取消与定时轮询、`GraphicsEnvironmentKey` 实时展示与液态玻璃感知降级验证）。

验收：
- 关窗托盘驻留零显存、Explorer 重启托盘与任务栏状态自动恢复、单实例唤醒正确移交前台、标准目录改盘符后仍精准定位、透明重定位与原子写可靠。
- 定时器在组件卸载与 HMR 重载后无残留泄漏，HTTP 请求/下载支持超时与取消且不阻塞渲染帧；WARP 无头像素回归基线不受影响，`@canton/kyant-glass` 在低配/覆写 `reduced` 档下正确降级。

---

## M7：Native 扩展 SDK 与 C ABI 挂载体系（ADR B1：7 / B1：9 / B4：2–3）

目标：建立低耦合、免重编宿主的 `native/` 原生扩展体系，交付 C ABI 底座、Header-only C++20 SDK 与安全加载机制。

范围：
- **2×2 宿主扩展端就位**：完善 `CANTON_ENABLE_NATIVE_EXT=1` 的 `ext` 宿主与 C ABI 导出函数表（`canton_abi.h`），保证 `mini` 版（`EXT=0`）物理不含扩展加载路径且 4 份宿主共享同一 `QuickJsBuildIdentity`。
- **Header-only C++20 SDK（`native/include/canton/`）**：`canton_register_native(canton::HostBuilder&)`、`onInit`/`onShutdown`、`.sync` / `.async` / **STA 线程**原生服务、`CallContext::lookupHwnd`、`EventEmitter`、泛化 `DataSource`、`canton::DrawCanvas` 2D 封装、外部 `ID3D11Texture2D` 零拷贝 `Painter`、自定义 `MeasurePolicy`（回调形式的 `alignBy` 随它一起定）、同步 `ctx.assets()`、`ctx.paths()`、`canton::fs`，以及与 M6 对称的原生侧 `ctx.timers()`（RAII 定时原语）与 `ctx.net()`（复用底层 WinHTTP 请求与下载能力）。
- **M3 移交**：`drawNative` 的后端实现（M3 的渲染器遇到它不绘制，记诊断日志）；`EXT=1` 的两份宿主。
- **防劫持安全加载**：清单声明扩展 DLL 相对路径与 SHA-256，Dist 加载前校验哈希并限定 `LOAD_LIBRARY_SEARCH_APPLICATION_DIR`，握手校验 ABI 版本与结构体尺寸。

验收夹具：
- 「Native 扩展」夹具，自带一个只用 `canton-sdk` 头文件编译的扩展 DLL（实时高频 `DrawCanvas` 波形、外部 `ID3D11Texture2D` 渲染纹理叠加 Canton 圆角/阴影/`RenderEffect`、后台异步与 STA 原生服务、原生重型文件处理）。

验收：
- 扩展 DLL 仅凭 `canton-sdk` 头文件在秒级编译完成；测试 `/MD` 第三方库接入与 `/MT` 宿主共存无 `LNK2038`；篡改或未声明 DLL 在 Dist 下被严格拒绝。

---

## M8：Canton CLI 与双模预构建交付体系（ADR B1：7 & B1：9）

目标：重造 Windows-only Canton CLI，落地零 C++ 环境默认体验与按需启用 `native/` 扩展的完整工程生命周期管理。

范围：
- **项目模型与按需环境**：新 `canton.project.yaml` / `canton.local.yaml` 与受管区（去音频插件、去 macOS、去 `adopt`）；纯 TS 工程零 VS/MSVC/CMake/Git 门槛，启用 `native/` 后由 `canton sync` 按需校验工具链并同步精简 `native/CMakeLists.txt`。
- **M3 移交**：`cli/src/feature/source/NativeDependencies.ts` 的清单类型收进只取源码的 `miniz`、`lunasvg`（目前由 `scripts/native-source-cache.ts` 直接读清单）。
- **预构建制品分发与缓存**：按框架版本同步缓存 2×2 预编译宿主、`canton_ui_bytecode.exe` 与 `canton-sdk`，校验 SHA-256、协议号与 `QuickJsBuildIdentity`；无 `native/` 时直接用预构建工具编译 `.cbc` 字节码。
- **`dev` / `build` / `pack` 闭环**：
  - `dev` 落实 `--flavor debug|release` × `--source lfd|live|dist` 规矩、注入 `CANTON_DEV_*` 与重定位目录，纯 TS 零编译秒启，有 `native/` 时仅快编用户扩展 DLL；
  - `pack` 产出统一 Windows 应用分发目录：复制预编译宿主改名 `<AppName>.exe`、直接修补 PE 注入 `.ico`/VersionInfo/DPI 清单、按需打包扩展 DLL 与 App-Local CRT DLL、写入扩展哈希清单与可选安装期文件关联/协议元数据。

Demo 建立与总验收：
- 用 Canton CLI 新建 **`demo/center`**（启用 `native/`）与 **`demo/hello`**（极简纯 TS），自始由 CLI 托管。`demo/center` 的各页取自 M3–M7 验收夹具里的场景（首屏各项、窗口与模态、着色器与视觉特效、桌面融合与存储、Native 扩展）；仍用于自动回归的夹具留在 `tests/fixtures/`。
- 端到端验收：在一台无 MSVC/CMake 环境下用 `demo/hello` 跑通 `create / sync / dev / build / pack`；在开发机用 `demo/center` 跑通纯 TS 与 `native/` 扩展双态交付与离线运行。

---

## 后续方向（M9+，按需排期）

- **桌面交互补齐**：系统原生文件对话框（`IFileOpenDialog` / `IFileSaveDialog`）、剪贴板与富文本/图片读写、文件和内容的拖入脱出（`DragAndDrop`）。
- **Inspector / DevTools 与 Profiler**：基于多窗口能力构建独立窗口 Inspector（节点树、布局边界、脏区原因、着色器/离屏层检视）。
- **稳定化与压测**：长稳 HMR 重载、QuickJS 内存回收、大树布局与 Lazy 压测、文档补齐。
- **按脏区局部呈现**（M3 每帧整幅重绘）；SVG 里的 `<text>` 接 Canton 的绘字管线（M3 的 lunasvg 不出字）；`animateItem` 的命中区域跟随动画。
- **编辑器支持**：VSCode 等主流编辑器的 `.cta` 语法高亮与语言服务插件。

---

# 远期想法暂存中心（非正常里程碑内容，属于未来再说、仅供参考）

## `.cta` 内层级作用域脚本，以及模板内、作用域下直写代码的写法支持

- **并列设想**（目标相同：在单 `.cta` 内获得类似 Compose 嵌套 lambda 块的局部作用域，而不必为几行逻辑强拆 `.cta` 文件）：
  1. **层级作用域脚本**：除根 `<script>` 外，声明与模板某槽位绑定的 `<script scope="sub1">`（默认槽写 `c-scope="sub1"`，具名槽写 `<Template #name c-scope="sub1">`）；
  2. **模板内作用域下直写代码**：在模板对应位置直接内联一次性局部 setup 块（无需起 `scope` 名、无需在文件上下跳转对照）。
- **核心前提与红线**：两种设想都必须坚持**挂载期只执行一次**（编译降级为带外层词法闭包捕获的内联私有子 Arrangable），以与根 `<script>`“实例不死不重跑、天然无需 `remember`”的心智保持统一。**绝不引入随重排反复执行的模板语句**（否则将被迫引入 `remember` 与位置槽位表约束，与 `<script>` 心智直接冲突）。
- **价值**：
  - 在局部作用域内直接非空访问当前槽位的 `&handle`（如 `&appHandle`、`&windowHandle`、`&columnHandle`）及 `c-for` 迭代变量，无需改坏 [ADR 002](../adr/batch1/2：Slot句柄与父布局数据.md) 做带 `null` 风险的模板 `ref` 反向赋值；
  - 在 `c-if` / `c-for` 分支内声明随分支生灭的局部 `ref` / `watch` / 副作用，不必全提升到根 `<script>`。
- **代价与待定取舍**：
  - **类型检查与 IDE 成本高**：`.cta` 类型检查需按模板嵌套树重组嵌套闭包、注入槽位参数和循环变量类型并维护精确 SourceMap；主流编辑器对单文件多 `<script scope="...">` 或模板内直写 TS 块默认高亮与语言服务支持弱；模板内直写代码还需额外处理 TS 泛型 `<T>` / 比较符与标签词法冲突。
  - **语法与运行边界**：局部脚本禁止顶层 `import`；多层嵌套的词法父子以模板树为准；需处理 Live HMR 下按局部作用域身份的状态保留。
- **当前取舍**：纯属 `.cta` 编译器层增强，与现有运行时、窗口和 Shell 契约正交。暂不纳入首阶段，现阶段通过拆分子 Arrangable 或模板传参解决，待真实业务写感积累后再评估单开 ADR 推进。

## 非 Windows 环境开发与编译

- 考虑以后让非 Win 环境能开发、编译，但调试和使用产物仍需要 Win 环境。当前只支持在 Windows 上开发，产物只面向 Windows。
- **已知约束**：`.cbc` 字节码靠 `QuickJsBuildIdentity`（`qjs` 产物哈希）与宿主绑定，非 Win 编出的字节码工具身份必然不同；需要改成按源码与编译参数计算的身份，或改由宿主自己编译字节码。
- **可行形态**：非 Win 机器只跑 CLI 与 Vite DevServer，Win 虚拟机或真机上的 Dev 宿主经 `CANTON_DEV_*` 注入的 Live 地址远端连入热更新；`native/` 扩展的交叉编译不作承诺。