#pragma once

#include <core/InputSession.h>
#include <core/Paint.h>
#include <core/WindowTypes.h>
#include <runtime/AppSession.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace canton::host {

    // App 级开发期诊断交互分发器；仅由 DevAssembly.cpp 提供实现，Dist 宿主不链接任何相关代码与文案
    class DevInputHandler {
       public:
        virtual ~DevInputHandler() = default;
        virtual bool handleKeyDown(int virtualKey, core::KeyModifierState modifiers, runtime::AppSession& app, core::WindowId windowId) = 0;
        virtual bool handleErrorClick(runtime::AppSession& app) = 0;
    };

    // 一份宿主制品的装配：应用从哪里来、带不带 Live、诊断层与调试交互露多少。
    // Dev 宿主由 DevAssembly.cpp 按 CANTON_DEV_* 填，Dist 宿主由 DistAssembly.cpp 写死；两个文件各只编进一份制品
    struct HostAssembly {
        runtime::AppSourceConfig source;
        bool detailedErrorScreen = false;
        std::filesystem::path readyFile;
        std::string readyToken;
        std::function<std::unique_ptr<runtime::DiagnosticsHud>()> hudFactory;
        std::unique_ptr<DevInputHandler> devInput;
        // wake 可从任意线程调用，把所有者线程叫醒
        std::function<runtime::PackageRuntimeSource::LiveSessionFactory(std::function<void()> wake)> live;
    };

    // bundleRoot 是宿主 exe 所在目录。配置有误时抛 std::runtime_error
    [[nodiscard]] HostAssembly assembleHost(const std::filesystem::path& bundleRoot);

    // startupError 非空时不加载应用，直接以 0 窗兜底错误屏展示
    int runHost(HostAssembly assembly, const std::string& startupError = {});

}
