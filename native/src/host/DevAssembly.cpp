#include <host/HostAssembly.h>

#if !CANTON_ENABLE_DEV
#error DevAssembly.cpp 只编进 Dev 宿主
#endif

#include <runtime/LiveClient.h>
#include <runtime/LiveResources.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>
#include <stdexcept>

namespace canton::host {
    namespace {
        std::optional<std::wstring> environment(const wchar_t* name) {
            const auto size = GetEnvironmentVariableW(name, nullptr, 0);
            if (size == 0) return std::nullopt;
            std::wstring value(size, L'\0');
            value.resize(GetEnvironmentVariableW(name, value.data(), size));
            return value.empty() ? std::nullopt : std::optional(std::move(value));
        }

        std::string narrow(const std::wstring& text) {
            if (text.empty()) return {};
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }
    }

    // Dev 宿主：加载规矩经 CANTON_DEV_* 环境变量注入，不占用命令行
    HostAssembly assembleHost(const std::filesystem::path& bundleRoot) {
        HostAssembly assembly;
        assembly.source.bundleRoot = bundleRoot;
        assembly.source.policy = runtime::LoadPolicy::LiveThenDist;
        if (const auto source = environment(L"CANTON_DEV_SOURCE")) {
            const auto policy = runtime::parseLoadPolicy(narrow(*source));
            if (!policy) throw std::runtime_error("CANTON_DEV_SOURCE 只接受 lfd、live 或 dist，收到的是 " + narrow(*source));
            assembly.source.policy = *policy;
        }
        assembly.source.devServerUrl = narrow(environment(L"CANTON_DEV_SERVER").value_or(L"http://127.0.0.1:9178"));
        if (const auto dist = environment(L"CANTON_DEV_DIST")) assembly.source.distOverride = *dist;
        if (const auto file = environment(L"CANTON_DEV_READY_FILE")) {
            assembly.readyFile = *file;
            assembly.readyToken = narrow(environment(L"CANTON_DEV_READY_TOKEN").value_or(L""));
        }
        assembly.diagnostics.badge = runtime::DiagnosticVisibility::Always;
        assembly.diagnostics.toasts = runtime::DiagnosticVisibility::Always;
        assembly.diagnostics.overlay = runtime::DiagnosticVisibility::Always;
        assembly.detailedErrorScreen = true;
        assembly.liveClient = [](std::function<void()> wake) -> runtime::PackageRuntimeSource::LiveClientFactory {
            return [wake = std::move(wake)] {
                return runtime::createLiveModuleClient(wake);
            };
        };
        assembly.liveResources = [workers = runtime::createResourceWorkers()](std::string_view devServerUrl) {
            return runtime::RuntimeLoadResources{runtime::livePainterLoader(std::string(devServerUrl), {}, workers), runtime::liveAssetLoader(std::string(devServerUrl), workers)};
        };
        return assembly;
    }
}
