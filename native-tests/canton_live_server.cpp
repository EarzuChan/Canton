#include <quickjs/QuickJsScriptHost.h>
#include <runtime/LiveClient.h>

#include <chrono>
#include <iostream>
#include <thread>

// scripts/verify-live-hmr.ts 的原生对端：不开窗，连真实的 Vite DevServer，
// 把首载（LOAD）、探针值（VALUE n）和失败（ERROR）打到标准输出，供脚本按次序断言
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto client = canton::runtime::createLiveModuleClient();
    client->start(argv[1]);
    std::unique_ptr<canton::quickjs::QuickJsScriptHost> host;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& packet : client->takePackets()) {
            if (!packet.error.empty()) {
                std::cout << "ERROR " << packet.error << std::endl;
                continue;
            }
            if (packet.reload) {
                host = std::make_unique<canton::quickjs::QuickJsScriptHost>();
                const auto result = host->executeLiveModules(packet.snapshot);
                if (!result.ok) {
                    std::cerr << result.error << std::endl;
                    return 1;
                }
                std::cout << "LOAD" << std::endl;
            } else if (host) {
                const auto result = host->applyHotUpdate(packet.snapshot, packet.message);
                if (!result.ok) {
                    std::cerr << result.error << std::endl;
                    return 1;
                }
            }
        }
        if (host) {
            for (const auto& message : host->takeHotMessages()) {
                if (message.event == "test:probe") {
                    const auto& data = std::get<canton::quickjs::HotValue::Object>(message.data.value);
                    std::cout << "VALUE " << std::get<double>(data.at("value").value) << std::endl;
                } else if (message.event == "test:done")
                    return 0;
                else
                    client->send(message);
            }
            for (const auto& action : host->takeDiagnosticActions())
                if (action.kind == canton::quickjs::QuickJsDiagnosticActionKind::RequestReload) client->requestReload();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr << "live integration timed out" << std::endl;
    return 1;
}
