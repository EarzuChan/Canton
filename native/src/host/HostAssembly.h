#pragma once

#include <runtime/SceneHost.h>

#include <filesystem>
#include <functional>
#include <string>

namespace canton::host {
    // 一份宿主制品的装配：应用从哪里来、带不带 Live、诊断层露多少。
    // Dev 宿主由 DevAssembly.cpp 按 CANTON_DEV_* 填，Dist 宿主由 DistAssembly.cpp 写死；两个文件各只编进一份制品
    struct HostAssembly {
        runtime::AppSourceConfig source;
        bool detailedErrorScreen = false;
        // 给了就在首帧成功后把令牌写进这个文件
        std::filesystem::path readyFile;
        std::string readyToken;
        std::unique_ptr<runtime::DiagnosticsHud> hud;
        // wake 可从任意线程调用，把所有者线程叫醒
        std::function<runtime::PackageRuntimeSource::LiveSessionFactory(std::function<void()> wake)> live;
    };

    // bundleRoot 是宿主 exe 所在目录。配置有误时抛 std::runtime_error
    [[nodiscard]] HostAssembly assembleHost(const std::filesystem::path& bundleRoot);

    // startupError 非空时不加载应用，直接把它显示在错误屏上
    int runHost(HostAssembly assembly, const std::string& startupError = {});
}
