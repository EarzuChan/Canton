# 004 Dev 宿主的环境变量与随包位置

日期 2026-11-22。

## 背景

[batch1 ADR 009](../batch1/9：宿主加载规矩、Native统一结构与扩展交付.md) 定了 `lfd`、`live`、`dist` 三种加载策略和 Dev/Dist 两份宿主，没有定开发期怎么把策略和地址交给宿主、随包的 UI 放在哪。M3 的宿主只做显示，没有按键和设置界面，入口仍是 `createApp(App).mount()`。

## 决定

- **Dev 宿主只读环境变量**：`CANTON_DEV_SOURCE`（策略，缺省 `lfd`）、`CANTON_DEV_SERVER`（DevServer 地址，缺省 `http://127.0.0.1:9178`）、`CANTON_DEV_DIST`（开发期覆写 Dist 位置，目录或 ZIP）、`CANTON_DEV_READY_FILE` 与 `CANTON_DEV_READY_TOKEN`（首帧成功后写就绪令牌，给自动化用）。
- **Dist 宿主写死 `dist`**：二进制里没有 `CANTON_DEV_*` 的解析、Live 客户端和诊断浮层。由 `pnpm verify:host` 在产物里查字样确认。
- **随包位置**：宿主 exe 同目录下的 `ui/`（目录包）或 `ui.zip`（ZIP 包），两者并存时报错，不猜用哪个。
- **Live 不可用**：`lfd` 有 Dist 就加载 Dist 并弹不合并的警告；没有 Dist 与 `live` 一样进错误屏。客户端在后台每秒重连，连上后取新快照切回 Live。
- **装配点**：策略、地址、随包目录、覆写合成 `runtime::AppSourceConfig`，由 `host/` 的 Dev/Dist 两个装配文件交给 `PackageRuntimeSource`；Live 客户端经工厂注入，Dist 宿主不给工厂，也就不链接 Live 库（`canton_runtime_live`）。

## 为什么不选别的

- **命令行参数**：开发脚本、IDE 启动配置和自动化都更容易注入环境变量；参数会进进程列表和快捷方式，Dist 宿主也得留解析代码才能拒绝它们。
- **配置文件**：开发期要频繁切策略和地址，多一个文件就多一处要同步；M4 起有 `appBuilder` 之后再看是否需要工程级配置。
- **目录包与 ZIP 并存时取其一**：两份内容可能不同，静默选一个会让人以为改动没生效。

## 代价

- 环境变量对子进程可见，开发期从 Dev 宿主再拉起的进程会继承它们。
- 就绪文件只表示首帧成功，不表示后续帧健康。

【主人备注：以后把LFD改为LTD（Live then Dist）？】