# ADR B4-04：窗口任务栏高亮提醒（`FlashWindowEx`）

- **状态**：采纳
- **批次**：Batch 4（M6 桌面融合补充）
- **关联**：[ADR B1-03](../batch1/3：App与Window模型、Owner与模态.md)、[ADR B1-05](../batch1/5：Windows_Shell集成与系统能力.md)

---

## 背景

在 Windows 桌面即时通讯、下载器与后台协作类应用中，除任务栏进度条（`ITaskbarList3`）与覆盖角标（`SetOverlayIcon`）外，最常见的非侵入式用户提醒是「任务栏图标闪烁并停留在橙/黄色高亮态，用户点击激活该窗口后高亮自动消失」（Win32 `FlashWindowEx`）。

这类提醒是**瞬时命令语义**（由某次事件触发一次提醒，且可由系统在窗口获焦时自动终止），并非持久化声明属性，因此不应作为 `<Window>` 的声明式 prop，而应归属窗口控制命令面。

---

## 决策

1. **命令落点**：在 `WindowState`（同时经 `windowHandle.state` 访问）新增命令方法：
   ```ts
   export type WindowAttentionMode = 'informational' | 'critical' | 'stop'

   export interface WindowState {
       // ...现有属性与 activate() / requestClose()
       requestAttention(mode?: WindowAttentionMode): void
   }
   ```
   - 省略 `mode` 时默认 `'informational'`。
   - 若目标窗口尚未挂载或已关闭，静默忽略，与 `activate()` / `requestClose()` 语义保持一致。

2. **Win32 原生映射（`FlashWindowEx`）**：
   - `'informational'`：`FLASHW_TRAY | FLASHW_TIMERNOFG`，`uCount = 3`、`dwTimeout = 0`（使用系统默认光标闪烁节拍）。当窗口不在前台时，任务栏图标闪烁 3 次并保持橙/黄色高亮，用户点击激活窗口后由 Windows 自动熄灭；若窗口当前已在前台，Windows 自动不闪烁。
   - `'critical'`：`FLASHW_ALL | FLASHW_TIMERNOFG`，`uCount = 0`、`dwTimeout = 0`。标题栏与任务栏持续闪烁，直至窗口获得前台焦点或显式调用 `requestAttention('stop')`。
   - `'stop'`：`FLASHW_STOP`，立即停止闪烁并清除任务栏高亮态。

3. **通道复用**：沿用既有 `windowCommand` 事务外命令队列（扩展 `core::WindowCommandKind`），不新增独立桥接通道，不破坏 `win32 → core` 分层约束。

---

## 后果

- 应用无需依赖系统通知弹窗即可提供原生 Windows 任务栏来消息高亮体验，且点击激活自动消除由操作系统原生接管，零状态同步负担。
- 若后续 M7 需要向 Native 扩展开放同类能力，可直接复用底层 `win32::Window::requestAttention`。
