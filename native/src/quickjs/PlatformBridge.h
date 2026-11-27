#pragma once

#include <core/PlatformTypes.h>
#include <core/WindowTypes.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canton::quickjs {

    struct FsBridgeResult {
        bool ok = true;
        std::string errorCode;
        std::string errorPath;
        std::string errorMessage;
        std::string bytes;
        bool boolValue = false;
        bool isFile = false;
        bool isDirectory = false;
        std::uint64_t size = 0;
        double modifiedTimeMs = 0.0;
        double createdTimeMs = 0.0;
        struct DirEntry {
            std::string name;
            bool isFile = false;
            bool isDirectory = false;
        };
        std::vector<DirEntry> entries;
    };

    struct NetBridgeRequest {
        std::string method = "GET";
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        int timeoutMs = 15000;
        std::size_t maxBytes = 32 * 1024 * 1024;
        bool followRedirects = true;
        int maxRedirects = 10;
        bool useSystemProxy = true;
    };

    struct NetBridgeResponse {
        bool ok = true;
        std::string errorCode;
        std::string errorUrl;
        std::string errorMessage;
        int status = 0;
        std::string finalUrl;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string contentType;
        std::string body;
        bool downloadOk = false;
        std::string destPath;
        std::uint64_t bytesWritten = 0;
    };

    struct NetBridgeProgress {
        std::uint64_t receivedBytes = 0;
        std::optional<std::uint64_t> totalBytes;
    };

    // 由 runtime/AppSession 注入到 QuickJsScriptHost 的平台能力表（保持 quickjs 不反向依赖 runtime/win32）
    struct PlatformBridge {
        // 定时队列（返回正整数 ID）
        std::function<std::uint64_t(double delayMs, bool repeating)> scheduleTimer;
        std::function<void(std::uint64_t id)> rescheduleRepeatingTimer;
        std::function<void(std::uint64_t id)> cancelTimer;
        std::function<void()> clearTimers;

        // 图形环境与启动载荷
        std::function<core::GraphicsEnvironmentInfo()> getGraphicsEnvironment;
        std::function<core::LaunchRequest()> getLaunchRequest;

        // 路径与异步文件系统
        std::function<std::string(std::string_view kind)> getKnownPath;
        std::function<std::future<FsBridgeResult>(std::string op, std::string path, std::string path2, std::string bytes, bool flag, std::function<void()> wake)> fsAsync;

        // 异步 WinHTTP 网络与流式下载
        std::function<std::future<NetBridgeResponse>(std::uint64_t reqId, NetBridgeRequest req, std::function<void()> wake)> netRequestAsync;
        std::function<std::future<NetBridgeResponse>(
            std::uint64_t reqId,
            NetBridgeRequest req,
            std::string destPath,
            std::function<void(NetBridgeProgress)> onProgress,
            std::function<void()> wake)> netDownloadAsync;
        std::function<void(std::uint64_t reqId)> netCancel;

        // Windows Shell：托盘、双轨菜单、任务栏增强、SMTC、Jump List
        std::function<void(const core::TrayIconSpec&)> upsertTray;
        std::function<void(const std::string& trayId)> removeTray;
        std::function<std::uint32_t(const std::vector<core::TrayMenuItemSpec>& items, int screenX, int screenY)> showSystemTrayMenu;
        std::function<core::TrayAnchorRect(const std::string& trayId)> queryTrayAnchor;
        std::function<void(core::WindowId, const core::TaskbarProgressSpec&)> setWindowTaskbarProgress;
        std::function<void(core::WindowId, const std::optional<core::TaskbarBadgeSpec>&)> setWindowTaskbarBadge;
        std::function<void(core::WindowId, const std::vector<core::TaskbarActionSpec>&)> setWindowTaskbarActions;
        std::function<void(const core::MediaSessionSpec&)> updateMediaSession;
        std::function<void(const std::vector<core::JumpListCategorySpec>&)> setJumpList;
        std::function<void()> clearJumpList;
    };

}
