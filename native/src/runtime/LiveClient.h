#pragma once

#include <runtime/LiveModuleClient.h>
#include <runtime/PackageRuntimeSource.h>

#include <functional>
#include <memory>

namespace canton::runtime {
    // 真实的 Live 客户端：模块快照走 HTTP，热更新走 Vite 的 WebSocket，都在后台线程。
    // 有新包可取时调用 wake（在后台线程上，调用方转投到自己的线程）。
    // 连不上或中途断开时交出一个 unavailable 的包，随后每秒重连；连上后自动取新快照
    [[nodiscard]] std::unique_ptr<LiveModuleClient> createLiveModuleClient(std::function<void()> wake = {});

    [[nodiscard]] PackageRuntimeSource::LiveSessionFactory makeLiveSessionFactory(std::function<void()> wake, RuntimePackageLoader::LiveResources resources);
}
