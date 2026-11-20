# 003 App 与 Window 模型、Owner 与模态

日期 2026-11-20。

## 背景

Canton 从 Arrange 的单视口模型转向 Windows 原生多窗口桌面应用，接续 [ADR 001](001-Windows用户态目标、应用入口与受管工程.md) 与 [ADR 002](002-Slot句柄与父布局数据.md)，需要明确 `<App>` 与 `<Window>` 的生命周期、Props/State/Handle 分工、原生窗口引用方式，以及贴合 Win32 语义的 `owner` 与模态规则。

## App 与 Window 的职责和生命周期

`<App>` 是进程内唯一的非布局应用根，不建立绘制表面，不创建布局节点。

**画区作用域自然分界**：框架不靠维护组件标签黑名单来区分“自绘”与“非自绘”组件，而是以是否处于有效的画区作用域（Layout / Surface Domain）自然分界。只有建立原生绘制表面和独立布局根的容器（如 `<Window>`，以及后续暗中托管无边框浮窗的 `<CustomTrayMenu>` 等）才向下提供画区作用域，其内部才可放置 `<Layout>`、`<Column>`、`<Text>` 等自绘 Arrangable；在无画区作用域处（如 `<App>` 下或原生系统菜单内）放置自绘组件会因缺少绘制所依直接报错，仅允许放置对应领域的非自绘 Shell Arrangable。

`<Window>` 是声明式窗口原语，遵循“存在即打开、移出即销毁”，对所在外层布局透明（不向祖先 `Layout` 贡献 `LayoutNode`）。每个 `<Window>` 对应独立原生 `HWND` 与独立布局根，并按窗口向下 `provide` 独立的 `Density`（适配每显示器 DPI）与 `FocusManager`。隐藏或最小化只改变窗口状态，不卸载组合树；从组合树移出才是销毁。

- **关闭请求**：宿主拦截 Win32 `WM_CLOSE`，禁止 `DefWindowProc` 绕过组合树直接 `DestroyWindow`。`WM_CLOSE` 转为 `<Window>` 必填的 `onCloseRequest` 回调，由应用修改响应式状态卸载窗口或退出应用，避免原生窗口与声明式状态脱节。
- **退出应用**：`appHandle.exitApplication()` 表示无条件进入有序退出（卸载 `<App>` 树并触发卸载钩子、按依赖逆序销毁原生窗口后退出消息循环），不重入各窗口的 `onCloseRequest`。退出前的未保存确认流程由应用业务自行编排，确认后再调用退出。
- **末窗退出**：`<App>` 的末窗自动退出策略（如 `exitOnLastWindowClosed`）默认开启，但仅在一次重排事务提交完成后、且本次会话曾经打开过至少一个窗口的前提下，活跃窗口数由 `> 0` 降为 `0` 时触发。这样既不影响启动阶段异步开窗，也不会在同帧切换窗口（如登录窗切主窗）时误退。托盘常驻或无窗驻留场景需显式关闭该策略。
- **系统注销与关机**：Win32 的 `WM_QUERYENDSESSION` 与 `WM_ENDSESSION` 归口到 `<App>` 统一处理，不向每个 `<Window>` 重复分发。默认直接放行并在确认关机时走有序卸载，不阻塞系统关机。

## Props、State 与 Handle 的分工

为避免状态、命令与作用域能力混杂，按来源和生命周期划分三层职责：

| 层次 | 归属与生命周期 | 承载内容 |
| --- | --- | --- |
| **Props** | 外部单向传入，随组合更新 | 原生不会自行改写的声明配置，如 `title`、`owner`、`modality`、`onCloseRequest`。 |
| **State** | 外部按需创建并传入，生命周期可长于单次挂载 | 需外部预设、与原生双向同步、外部观测或跨开关持久化的状态与控制入口；未传入时容器内部自建默认一份。 |
| **Handle** | 容器创建并随 Slot 向下交付，绑死本次挂载实例 | 挂载期宿主能力与作用域专属构造器，遵循 [ADR 002](002-Slot句柄与父布局数据.md) 的词法可见规则。 |

- **`<App>` 只提供 `AppHandle`**：`<App>` 位于应用根最顶层，没有需要外部预设或跨次持久化原生状态的父级，因此不设立 `AppState`，统一由 `&app-handle` 提供 `exitApplication()` 及后续按需补充的只读应用观测字段。
- **`<Window>` 同时具备 `WindowState` 与 `WindowHandle`**：
  - `WindowState`（通过可选的 `createWindowState()` 创建并传给 `:state`）承载几何与形态状态、只读运行期快照及外部控制命令（如前置激活、请求关闭）。同一 `WindowState` 同一时刻仅允许绑定一个活跃 `<Window>`，重复绑定直接报错；窗口卸载后保留快照，便于重开恢复位置和尺寸。
  - `WindowHandle`（通过 `&window-handle` 交付）只放本次挂载期能力：暴露当前窗实际使用的 `state` 引用，以及窗口作用域专属的能力构造器（如后续自绘标题栏拖拽区、系统按钮区等），不在 Handle 上重复抄写 `WindowState` 已有命令。
- **不公开深层 Handle 注入**：`AppHandle` 与 `WindowHandle` 不进入公开 `provide/inject`，防止任意深层子组件隐式越权并保持 [ADR 002](002-Slot句柄与父布局数据.md) 的词法边界。窗口向子树公开的 `provide/inject` 仅保留环境与只读服务（如 `Density`、`FocusManager` 及后续按需补充的只读窗口信息）。
- **原生窗口引用不暴露 ID**：不向用户公开裸 `HWND` 或数字/字符串 `WindowId`。TS 调用 `native/` 原生服务时直接传递 `WindowHandle` 或 `WindowState` 对象，由框架桥接层经私有 `WeakMap` 解出进程内单调不复用的私有 `WindowId`，C++ 宿主再经活跃表安全反查 `HWND`；窗口销毁即摘除，避免 Win32 句柄复用导致野句柄操作。

## Owner 与模态规则

模型明确切开两层关系：**组合树决定生命周期**（子窗随所在作用域卸载而销毁），**Win32 `owner` 决定窗口系统从属关系**（Z-order 浮于上方、联动最小化、任务栏归组及窗口模态目标）。

- **Owner 继承与边界**：写在 `<App>` 下（无祖先 `<Window>`）的窗口无 `owner`；嵌套在其他 `<Window>` 子树内的 `<Window>` 通过框架私有上下文自动以最近祖先 `<Window>` 为 `owner`。这种树形嵌套天然无环，且与 Win32 销毁 Owner 时同步清理 Owned 窗口的语义一致。嵌套窗口允许显式指定 `owner="none"`，在保持组合树生命周期的同时断开 Win32 Owned 从属关系（如允许被主窗遮挡或独立占任务栏格的分离预览窗）。
- **暂不做跨分支与动态换 Owner**：运行期修改顶层窗口 `GWLP_HWNDPARENT` 在 Win32 上副作用多，跨分支平铺引用也会引入环检测与孤儿窗恢复复杂度。首阶段不支持跨分支及动态切换 `owner`；`WindowState` 已为未来按需扩展预留接口。同一事务销毁父子窗口时，原生层必须按拓扑逆序（先 Owned 后 Owner）销毁，防止 OS 提前强杀 Owned 产生野句柄。
- **统一的模态原语**：底层不拆 `<Window>` 与 `<Dialog>` 两套窗口原语，在 `<Window>` 上正交提供 `modality`（`none` / `owner` / `application`，默认 `none`）；高层对话框封装留给上层组件库。
- **非阻塞 Win32 模态实现**：禁止使用 Win32 `DialogBox` 嵌套模态消息循环（会挂起 QuickJS 调用栈并破坏重排事务）。宿主统一通过 `EnableWindow(hwnd, FALSE)` 搭配每窗模态锁计数实现非阻塞模态，自然支持多层嵌套模态。
- **锁定范围与防焦点跌落**：
  - `modality="owner"` 必须存在有效 `owner`（无 `owner` 时直接报错，不静默降级），锁定范围为该 `owner` 及其下挂的其他非模态 owned 子孙窗（排除当前模态窗自身及其子孙），防止主窗被锁期间仍可通过其工具窗修改状态；
  - `modality="application"` 锁定本应用内所有其他顶层窗口，有无 `owner` 均合法；
  - 卸载模态窗时原生层必须严格按顺序执行：**先释放模态锁（锁计数归零时 `EnableWindow(TRUE)`）-> 恢复激活目标窗口 -> 最后 `DestroyWindow` 模态窗**，防止前台焦点在窗口销毁瞬间跌落到其他进程窗口。

## 代价与后续边界

- 首阶段放弃跨分支指定 `owner`，多文档共享单一浮动工具窗暂时不能自动跟随活跃文档切换 `owner`，需靠独立顶层/置顶窗口策略处理。
- 窗口样式与边框、自绘标题栏拖拽区与 Win11 系统三键协作、`WindowState` 几何/形态具体字段、`<TrayIcon>` 与单实例协议，以及 `native/` 服务桥接与阻塞型系统对话框协作，留待后续 ADR 定义。