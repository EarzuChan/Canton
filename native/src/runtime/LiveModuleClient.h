#pragma once

#include <quickjs/LiveModule.h>
#include <string>
#include <vector>

namespace canton::runtime {
    struct LiveModulePacket {
        quickjs::LiveModuleSnapshot snapshot;
        quickjs::HotMessage message;
        bool reload = false;
        bool unavailable = false;
        std::string error;
    };

    // 网络和快照由宿主提供，客户端实现不访问 QuickJS 上下文或 UI 场景。
    class LiveModuleClient {
       public:
        virtual ~LiveModuleClient() = default;
        virtual void start(std::string url) = 0;
        virtual void requestReload() = 0;
        virtual void send(const quickjs::HotMessage& message) = 0;
        virtual std::vector<LiveModulePacket> takePackets() = 0;
    };
}
