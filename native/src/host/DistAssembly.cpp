#include <host/HostAssembly.h>

#if CANTON_ENABLE_DEV
#error DistAssembly.cpp 只编进 Dist 宿主
#endif

namespace canton::host {
    // Dist 宿主：只读随包 Dist，不认任何开发期开关，也不链接 Live、HMR、DiagnosticsHud 和 DiagnosticsOverlay
    HostAssembly assembleHost(const std::filesystem::path& bundleRoot) {
        HostAssembly assembly;
        assembly.source.policy = runtime::LoadPolicy::OnlyDist;
        assembly.source.bundleRoot = bundleRoot;
        return assembly;
    }
}
