#include <host/HostAssembly.h>

#if CANTON_ENABLE_DEV
#error DistAssembly.cpp 只编进 Dist 宿主
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace canton::host {
    // Dist 宿主：只读随包 Dist，不认任何开发期开关，也不链接 Live、HMR、DiagnosticsHud 和 DiagnosticsOverlay
    HostAssembly assembleHost(const std::filesystem::path& bundleRoot) {
        HostAssembly assembly;
        assembly.source.policy = runtime::LoadPolicy::OnlyDist;
        assembly.source.bundleRoot = bundleRoot;
        const auto size = GetEnvironmentVariableW(L"CANTON_DATA_ROOT", nullptr, 0);
        if (size > 0) {
            std::wstring value(size, L'\0');
            value.resize(GetEnvironmentVariableW(L"CANTON_DATA_ROOT", value.data(), size));
            if (!value.empty()) assembly.dataRootOverride = std::filesystem::path(value);
        }
        return assembly;
    }
}
