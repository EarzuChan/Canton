#pragma once

#include "Geometry.h"

#include <cstdint>
#include <optional>
#include <string>

namespace canton::core {
    using WindowId = std::uint32_t;

    enum class WindowPlacement : std::uint8_t {
        Normal,
        Minimized,
        Maximized,
        Fullscreen,
    };

    enum class WindowPositionPreset : std::uint8_t {
        PlatformDefault,
        CenterScreen,
        CenterOwner,
        Coordinates,
    };

    enum class WindowChrome : std::uint8_t {
        System,
        CustomTitleBar,
        Frameless,
    };

    enum class WindowRole : std::uint8_t {
        Standard,
        Utility,
    };

    enum class WindowModality : std::uint8_t {
        None,
        Owner,
        Application,
    };

    enum class WindowBackdrop : std::uint8_t {
        None,
        Mica,
        MicaAlt,
        Acrylic,
    };

    enum class WindowCornerPreference : std::uint8_t {
        Default,
        Round,
        RoundSmall,
        Square,
    };

    enum class CaptionButtonKind : std::uint8_t {
        Minimize,
        Maximize,
        Close,
    };

    enum class WindowCommandKind : std::uint8_t {
        Activate,
        RequestClose,
    };

    enum class WindowHitResult : std::uint8_t {
        Client,
        ClientInteractive,
        Draggable,
        MinimizeButton,
        MaximizeButton,
        CloseButton,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    struct WindowPositionSpec {
        WindowPositionPreset preset = WindowPositionPreset::PlatformDefault;
        int x = 0;
        int y = 0;
        bool operator==(const WindowPositionSpec&) const = default;
    };

    struct AppConfigSpec {
        bool exitOnLastWindowClosed = true;
        std::string appId;
        std::string icon;
        bool operator==(const AppConfigSpec&) const = default;
    };

    struct WindowConfigSpec {
        std::optional<WindowId> ownerWindowId;
        std::string title;
        std::string icon;
        bool visible = true;
        Size sizeDp{960.0f, 640.0f};
        std::optional<Size> minSizeDp;
        std::optional<Size> maxSizeDp;
        WindowPositionSpec position;
        WindowPlacement placement = WindowPlacement::Normal;
        WindowModality modality = WindowModality::None;
        WindowChrome chrome = WindowChrome::System;
        WindowRole role = WindowRole::Standard;
        bool resizable = true;
        bool minimizable = true;
        bool maximizable = true;
        bool closable = true;
        bool alwaysOnTop = false;
        std::optional<bool> showInTaskbar;
        bool transparent = false;
        WindowBackdrop backdrop = WindowBackdrop::None;
        WindowCornerPreference cornerPreference = WindowCornerPreference::Default;
        bool operator==(const WindowConfigSpec&) const = default;
    };

    struct WindowGeometryEvent {
        int widthPx = 0;
        int heightPx = 0;
        int x = 0;
        int y = 0;
        bool hasPosition = false;
        WindowPlacement placement = WindowPlacement::Normal;
    };

    struct WindowCapabilities {
        bool resizable = true;
        bool minimizable = true;
        bool maximizable = true;
        bool closable = true;
        WindowPlacement placement = WindowPlacement::Normal;
        WindowChrome chrome = WindowChrome::System;
        float resizeBorderPx = 6.0f;
    };
}
