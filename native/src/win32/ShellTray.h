#pragma once

#include <core/PlatformTypes.h>
#include <win32/Window.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace canton::win32 {

    // 解析或按 AUMID + key 稳定派生托盘 GUID（RFC 4122 UUIDv5 兼容格式，保留 Win11 托盘显隐偏好）
    [[nodiscard]] std::string deriveStableTrayGuidString(std::string_view appId, std::string_view trayKey);

    // 根据托盘锚点与所在屏工作区，计算无边框自定义托盘弹窗的屏幕坐标（避让任务栏且不越出工作区）
    struct PopupPositionPx {
        int x = 0;
        int y = 0;
    };
    [[nodiscard]] PopupPositionPx computePopupPositionForTray(const core::TrayAnchorRect& anchor, int popupWidthPx, int popupHeightPx) noexcept;

    // 系统托盘图标与原生 HMENU 管理器（绑定 App 级隐藏顶层 HWND，支持 TaskbarCreated 自动重建与零显存原生菜单）
    class ShellTrayManager final {
       public:
        using EventCallback = std::function<void(const core::TrayEvent&)>;

        explicit ShellTrayManager(void* appHwnd, EventCallback onEvent = {});
        ~ShellTrayManager();
        ShellTrayManager(const ShellTrayManager&) = delete;
        ShellTrayManager& operator=(const ShellTrayManager&) = delete;

        void setAppId(std::string appId);

        // 新增或更新托盘图标；iconHandle 可为空（自动回退应用默认图标）
        void upsertTray(const core::TrayIconSpec& spec, void* iconHandle);
        void removeTray(const std::string& trayId);
        void clearAll();

        // 响应 Explorer 重启后的 TaskbarCreated 广播
        void onTaskbarCreated();

        // 处理 MessageDispatcher 转发的托盘回调消息
        void handleTrayMessage(std::uint32_t iconId, std::uint32_t mouseMsg, int cursorX, int cursorY);

        // 查询托盘图标当前屏幕锚点与工作区
        [[nodiscard]] core::TrayAnchorRect queryAnchor(const std::string& trayId, int fallbackX, int fallbackY) const;

        // 弹出纯 Win32 原生 HMENU（零显存；自动 SetForegroundWindow + WM_NULL 防僵尸菜单），返回被选中的 commandId（0 表示未选择）
        [[nodiscard]] std::uint32_t showSystemMenu(const std::vector<core::TrayMenuItemSpec>& items, int screenX, int screenY);

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
