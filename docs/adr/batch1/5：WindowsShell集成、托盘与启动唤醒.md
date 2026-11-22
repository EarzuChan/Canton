# 005 Windows Shell 集成、托盘与启动唤醒

日期 2026-11-20。

## 背景

承接 [ADR 001](1：Windows用户态目标、应用入口与受管工程.md)、[ADR 003](3：App与Window模型、Owner与模态.md) 与 [ADR 004](4：原生窗口外观、自绘标题栏与任务栏身份.md)，本篇确定系统托盘、窗口任务栏增强、系统全局媒体控制（SMTC）、统一启动与单实例唤醒、Jump List，以及安装期文件关联与 URL 协议的架构边界。

## 原生底座：App 级隐藏顶层 `HWND`

原生宿主在 `<App>` 存活期间固定维护一个永不显示、带 `WS_EX_TOOLWINDOW` 的隐藏顶层 `HWND`（不能使用 `HWND_MESSAGE`；不进任务栏与 Alt-Tab、不占 GPU 显存），统一承接三类与具体业务窗口无关的系统能力：

1. `<TrayIcon>` 消息及 `TaskbarCreated` 广播（`explorer.exe` 重启后自动重建托盘图标）；
2. SMTC 系统媒体会话（Win32 `ISystemMediaTransportControlsInterop::GetForWindow` 强制要求真实顶层 `HWND`，挂在此处可保证所有业务窗口关闭或最小化到托盘后仍响应媒体键与锁屏卡片）；
3. 无可见业务窗口常驻时的 `WM_QUERYENDSESSION` / `WM_ENDSESSION` 关机与注销通知。

## 系统托盘与双轨菜单

`<TrayIcon>` 为 `<App>` 下的非自绘声明式组件，存在即注册，移出即 `NIM_DELETE` 清理。为保留 Win11 托盘区显隐偏好，宿主默认基于 `app.id`（AUMID）与托盘键派生稳定 `NIF_GUID`（允许显式覆写）。

托盘右键菜单采取**双轨声明式设计**（同一 `<TrayIcon>` 内最多二选一，混写或与自管 `onContextMenu` 冲突时直接报错）：

- **`<SystemTrayMenu>`（非自绘原生菜单）**：映射 Win32 `HMENU` 与 `TrackPopupMenuEx`，宿主自动处理 `SetForegroundWindow` + `PostMessage(WM_NULL)` 防僵尸菜单；纯托盘常驻时保持零 GPU 显存占用。内部使用 `<MenuItem>`、`<MenuCheckbox>`、`<MenuRadio>`、`<MenuSeparator>`、`<SubMenu>`（可供未来 `<SystemContextMenu>` 复用）。
- **`<CustomTrayMenu>`（自绘豪华面板）**：内部暗中托管无边框工具弹窗，依 [ADR 003](3：App与Window模型、Owner与模态.md) 的画区作用域规则自然允许放置自绘 Arrangable；宿主自动按托盘锚点矩形避让任务栏定位、处理前台激活与失焦收起，并通过 `&menu-handle` 提供 `dismiss()`。

## 任务栏增强与系统媒体控制

- **窗口级任务栏增强（归属 `<Window>`）**：任务栏进度（`taskbarProgress`）、右下角徽标（`taskbarBadge`）与悬浮预览底部的缩略图工具栏按钮（`taskbarActions`）作为 `<Window>` 的可选声明属性，底层走 `ITaskbarList3`，随状态更新、随窗口卸载清空，并由宿主监听 `TaskbarButtonCreated` 在首次上任务栏或 Explorer 重启后自动重放。缩略图按钮受 Win32 限制单窗最多 7 项（超限报错），宿主内部按需预占槽位并映射显示/隐藏，屏蔽 `ThumbBarAddButtons` 无法动态增删个数的底层限制。
- **系统全局媒体控制（归属 `<App>`）**：SMTC 作为可选导入的非自绘声明式组件 `<MediaSession>` 挂在 `<App>` 下，存在即激活系统媒体会话，移出即关闭清空。因单进程仅对应唯一系统媒体会话，同一时刻仅允许挂载一个活跃 `<MediaSession>`，同时挂载多个直接运行时报错。

## 启动入参、单实例唤醒、Jump List 与安装期声明

- **统一启动载荷与单实例协议**：命令行带参启动、双击已关联文件、打开自定义 URL 协议与点击 Jump List 条目，到达进程时统一封装为不可变只读的 `LaunchRequest { args, workingDirectory }`（不做魔法参数猜解）。
  - `app/src/main.ts` 保持纯装配，顶层不执行原生副作用。
  - **冷启动**：宿主在挂载根组件前，通过只读环境注入 **`LaunchRequestKey`** 提供本次进程的 `LaunchRequest`（无隐式“读一次清空”副作用），根组件 `setup` 阶段同步可读，首帧即可建立目标窗口，避免先闪空窗。
  - **单实例热唤醒**：在 `canton.project.yaml` 中通过 `app.singleInstance` 配置（默认 `false` 允许多开）。开启后，C++ 宿主在启动 QuickJS 之前按 AUMID 做互斥检测；若主实例已在运行，第二进程通过本地 IPC 投递 `LaunchRequest` 并调用 `AllowSetForegroundWindow` 移交前台激活权后立即安静退出。主实例由 `<App :on-second-instance>` 接收（若主实例尚未完成 `<App>` 挂载则由宿主暂存队列并在挂载后补发；未开启单实例时该回调无害忽略）。
- **分发与主窗心智归属用户**：框架不设特权 `MainWindow`，也不内置全局路由。收到冷/热 `LaunchRequest` 后如何开新窗、通过 `WindowState.activate()` 激活已有窗或切换窗口内页面，由用户业务状态自行决定。
- **Jump List**：按 AUMID 持久化于 Windows Shell，不塞入 `AppHandle`，作为可选模块导出的运行时函数（`setJumpList` / `clearJumpList`，应用挂载前误调报错），便于在业务 Store 或 `watch` 中动态刷新，点击后统一回到 `LaunchRequest` 管线。
- **安装期 Shell 注册**：文件关联（“打开方式”与双击打开）和自定义 URL 协议在 `canton.project.yaml` 中声明，由打包/安装器在安装与卸载时成对注册与清理，不在运行期偷偷改写系统注册表。

## 明确本篇暂不设计

- **资源管理器右键菜单暂不做**：普通用户态安装注册仅能进入 Win11 经典二级菜单，新版一级右键菜单需包身份（MSIX / Sparse Package）与 COM `IExplorerCommand` 扩展，暂不纳入框架范围。
- **注册表与应用数据存储本篇没设计**：框架核心不提供通用注册表读写；应用配置、缓存与用户数据目录留待后续存储与路径专题 ADR 定义。
- **不阻塞首阶段主线**：本篇所涉 Shell 增强模块均可按需独立演进，不阻塞基础窗口与 D3D11 渲染骨架落地。
