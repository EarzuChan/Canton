#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace canton::core {

    // 图形运行环境与系统偏好快照（驱动 GraphicsEnvironmentKey）
    struct GraphicsEnvironmentInfo {
        std::string adapterName = "Default Hardware Adapter";
        bool isSoftwareRenderer = false;
        bool isRemoteSession = false;
        bool systemTransparency = true;
        bool systemAnimations = true;
        bool operator==(const GraphicsEnvironmentInfo&) const = default;
    };

    // 进程冷启动 / 单实例热唤醒统一载荷
    struct LaunchRequest {
        std::vector<std::string> args;
        std::string workingDirectory;
        bool operator==(const LaunchRequest&) const = default;
    };

    // 任务栏进度态（映射 ITaskbarList3::SetProgressState）
    enum class TaskbarProgressState : std::uint8_t {
        None,
        Indeterminate,
        Normal,
        Paused,
        Error,
    };

    struct TaskbarProgressSpec {
        TaskbarProgressState state = TaskbarProgressState::None;
        double value = 0.0;  // [0, 1]
        bool operator==(const TaskbarProgressSpec&) const = default;
    };

    struct TaskbarBadgeSpec {
        std::string icon;
        std::string label;
        bool operator==(const TaskbarBadgeSpec&) const = default;
    };

    struct TaskbarActionSpec {
        std::string id;
        std::string icon;
        std::string tooltip;
        bool enabled = true;
        bool dismissOnClick = false;
        bool operator==(const TaskbarActionSpec&) const = default;
    };

    // 系统托盘声明快照
    struct TrayIconSpec {
        std::string id;
        std::string guid;
        std::string icon;
        std::string tooltip;
        bool visible = true;
        bool operator==(const TrayIconSpec&) const = default;
    };

    enum class TrayMenuItemKind : std::uint8_t {
        Item,
        Checkbox,
        Radio,
        Separator,
        SubMenu,
    };

    struct TrayMenuItemSpec {
        TrayMenuItemKind kind = TrayMenuItemKind::Item;
        std::uint32_t commandId = 0;
        std::string label;
        bool enabled = true;
        bool checked = false;
        std::vector<TrayMenuItemSpec> children;
        bool operator==(const TrayMenuItemSpec&) const = default;
    };

    enum class TrayEventKind : std::uint8_t {
        Click,
        DoubleClick,
        ContextMenu,
    };

    struct TrayAnchorRect {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        int cursorX = 0;
        int cursorY = 0;
        int workLeft = 0;
        int workTop = 0;
        int workRight = 1920;
        int workBottom = 1080;
        double dpiScale = 1.0;
    };

    struct TrayEvent {
        std::string trayId;
        TrayEventKind kind = TrayEventKind::Click;
        TrayAnchorRect anchor;
    };

    // 系统全局媒体控制（SMTC）
    enum class MediaPlaybackStatus : std::uint8_t {
        Closed,
        Stopped,
        Playing,
        Paused,
    };

    enum class MediaButtonKind : std::uint8_t {
        Play,
        Pause,
        Stop,
        Next,
        Previous,
    };

    struct MediaSessionSpec {
        bool active = false;
        MediaPlaybackStatus playbackStatus = MediaPlaybackStatus::Closed;
        std::string title;
        std::string artist;
        std::string album;
        std::string thumbnail;
        bool canPlay = false;
        bool canPause = false;
        bool canStop = false;
        bool canNext = false;
        bool canPrevious = false;
        bool operator==(const MediaSessionSpec&) const = default;
    };

    // 任务栏 Jump List（ICustomDestinationList）
    struct JumpListTaskSpec {
        std::string title;
        std::vector<std::string> args;
        std::string icon;
        std::string description;
        bool operator==(const JumpListTaskSpec&) const = default;
    };

    struct JumpListCategorySpec {
        std::string title;
        std::vector<JumpListTaskSpec> items;
        bool operator==(const JumpListCategorySpec&) const = default;
    };

}
