// 资源获取：Live 从 DevServer 取（同源、站点根、HTTP 诊断、字节预算与总时限、热更新时间戳、请求退休），
// data: URL 与 Dist 包的根路径语义和文件边界。DevServer 由本文件里的本机 HTTP 夹具服务扮演
#include <core/Painter.h>
#include <core/Version.h>
#include <quickjs/QuickJsScriptHost.h>
#include <runtime/Digest.h>
#include <runtime/LiveResources.h>
#include <runtime/PainterResources.h>
#include <runtime/UiPackage.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using canton::core::PainterLoader;
using canton::core::PainterLoadResult;
using canton::runtime::PainterResourceLimits;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::string unhex(std::string_view text) {
        std::string result;
        for (std::size_t i = 0; i + 1 < text.size(); i += 2) result += static_cast<char>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16));
        return result;
    }

    // 3×2 的纯红与纯绿 PNG
    const std::string redPng = unhex("89504e470d0a1a0a0000000d49484452000000030000000208060000009d74661a000000114944415478da63f8cfc0f01f86199039009b7e0bf50f5f26220000000049454e44ae426082");
    const std::string greenPng = unhex("89504e470d0a1a0a0000000d49484452000000030000000208060000009d74661a0000000e4944415478da6360f88f0491390095840bf5dccfa9440000000049454e44ae426082");
    const std::string svgText = "<svg xmlns='http://www.w3.org/2000/svg' width='13' height='7'><rect width='13' height='7' fill='red'/></svg>";

    // 本机 HTTP 夹具：每个连接一个线程，按路径给出固定的响应
    class HttpFixture {
       public:
        HttpFixture() {
            WSADATA data;
            check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock 初始化失败");
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            check(listener_ != INVALID_SOCKET && bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener_, 16) == 0, "本机 HTTP 测试服务无法监听");
            int length = sizeof(address);
            getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
            origin = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
            acceptor_ = std::thread([this] { serve(); });
        }

        ~HttpFixture() {
            stopping_ = true;
            closesocket(listener_);
            acceptor_.join();
            for (auto& worker : workers_) worker.join();
            WSACleanup();
        }

        std::string origin;

        std::vector<std::string> requests() {
            const std::lock_guard guard(mutex_);
            return requests_;
        }

       private:
        static void send(SOCKET socket, std::string_view bytes) {
            while (!bytes.empty()) {
                const auto count = ::send(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
                if (count <= 0) return;
                bytes.remove_prefix(static_cast<std::size_t>(count));
            }
        }

        static void respond(SOCKET socket, const std::string& type, const std::string& body) {
            send(socket, "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
            send(socket, body);
        }

        void serve() {
            while (!stopping_) {
                const auto client = accept(listener_, nullptr, nullptr);
                if (client == INVALID_SOCKET) continue;
                const std::lock_guard guard(mutex_);
                workers_.emplace_back([this, client] {
                    handle(client);
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                });
            }
        }

        void handle(SOCKET socket) {
            std::string request;
            char byte;
            while (request.size() < 8192 && recv(socket, &byte, 1, 0) == 1) {
                request += byte;
                if (request.ends_with("\r\n\r\n")) break;
            }
            const auto begin = request.find(' ');
            const auto end = request.find(' ', begin + 1);
            if (begin == std::string::npos || end == std::string::npos) return;
            const auto path = request.substr(begin + 1, end - begin - 1);
            {
                const std::lock_guard guard(mutex_);
                requests_.push_back(path);
            }
            if (path == "/missing.png") return send(socket, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            if (path == "/redirect.png") return send(socket, "HTTP/1.1 302 Found\r\nLocation: http://example.com/image.png\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            if (path == "/factory%20%25.json") return respond(socket, "application/json", "{\"ok\":42}");
            if (path == "/large.png") {
                send(socket, "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n");
                send(socket, std::string(1024, 'x'));
                std::this_thread::sleep_for(100ms);
                return;
            }
            if (path == "/chunked.png") {
                send(socket, "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                return send(socket, "800\r\n" + std::string(2048, 'x') + "\r\n0\r\n\r\n");
            }
            if (path == "/stall.png") {
                send(socket, "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
                std::this_thread::sleep_for(350ms);
                return;
            }
            if (path.starts_with("/slow.")) std::this_thread::sleep_for(100ms);
            if (path.starts_with("/icon.svg") || path.starts_with("/slow.svg")) return respond(socket, "image/svg+xml", svgText);
            if (path == "/broken.png") return respond(socket, "image/png", "broken image");
            if (path == "/html.png") return respond(socket, "text/html", "broken image");
            if (path == "/font.ttf") return respond(socket, "font/ttf", font);
            respond(socket, "image/png", path.ends_with("?t=2") ? greenPng : redPng);
        }

       public:
        std::string font;

       private:
        SOCKET listener_ = INVALID_SOCKET;
        std::atomic<bool> stopping_{false};
        std::thread acceptor_;
        std::mutex mutex_;
        std::vector<std::thread> workers_;
        std::vector<std::string> requests_;
    };

    PainterLoadResult load(const PainterLoader& loader, const std::string& path) {
        auto result = loader(path, [] {});
        check(result.wait_for(6s) == std::future_status::ready, "资源请求未在限时内结束：" + path);
        return result.get();
    }

    void expectFailure(const PainterLoader& loader, const std::string& path, const std::string& reason) {
        const auto result = load(loader, path);
        check(!result.content && result.error.find(reason) != std::string::npos, "资源失败诊断不完整（期望含“" + reason + "”）：" + path + " → " + result.error);
    }

    std::uint32_t firstPixel(const PainterLoadResult& result) {
        const auto bitmap = std::dynamic_pointer_cast<const canton::core::BitmapContent>(result.content);
        check(bitmap && !bitmap->pixels.empty(), "没有解成位图：" + result.error);
        return bitmap->pixels.front();
    }

    void liveResources(const std::filesystem::path& sources) {
        HttpFixture server;
        std::ifstream fontFile(sources / "tests/fixtures/fonts/CantonTestSans-Regular.ttf", std::ios::binary);
        server.font.assign(std::istreambuf_iterator<char>(fontFile), {});
        for (const PainterResourceLimits limits : {PainterResourceLimits{0, 100}, PainterResourceLimits{1024, 0}, PainterResourceLimits{1024, PainterResourceLimits::MaxTimeoutMs + 1}}) {
            bool rejected = false;
            try {
                (void)canton::runtime::livePainterLoader(server.origin, limits);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            check(rejected, "资源预算或超时上限未拒绝非法设置");
        }
        const auto loader = canton::runtime::livePainterLoader(server.origin);
        const auto read = canton::runtime::liveAssetLoader(server.origin);
        check(read("factory %.json", [] {}).get() == "{\"ok\":42}", "Live 文本资产未按逻辑名读取 JSON");
        for (const auto& path : {std::string("logo.png"), std::string("/logo.png"), server.origin + "/src/assets/logo.png?import&t=1", std::string("/src/assets/logo%252e.png"), std::string("/assets/space%20%E4%B8%AD.png")}) {
            const auto loaded = load(loader, path);
            check(loaded.content && loaded.content->intrinsicSize == canton::core::Size{3, 2}, "Live PNG 未从当前 origin 解码：" + path + " → " + loaded.error);
        }
        const auto svg = load(loader, "/icon.svg");
        check(svg.content && svg.content->intrinsicSize == canton::core::Size{13, 7}, "Live SVG 没有真实尺寸：" + svg.error);
        const auto font = std::dynamic_pointer_cast<const canton::core::FontContent>(load(loader, "/font.ttf").content);
        check(font && font->info.weight == 400, "Live 字体没有解出字面");
        // 热更新给资源加时间戳：新时间戳取新内容，旧内容不被改写
        const auto before = load(loader, "/hot.png?t=1");
        const auto after = load(loader, "/hot.png?t=2");
        check(firstPixel(before) == 0xffff0000u && firstPixel(after) == 0xff00ff00u, "资源时间戳未读取新内容，或改写了旧发布内容");
        const auto requestCount = server.requests().size();
        for (const auto& path : {"../escape.png", "/a/%2e%2e/escape.png", "/a%2f..%2fescape.png", "file:///tmp/logo.png", "C:/logo.png", "C:\\logo.png", "//example.com/logo.png", "/%5c%5cexample.com/logo.png", "https://example.com/logo.png", "/bad%00.png"}) expectFailure(loader, path, "Live Painter");
        expectFailure(loader, server.origin + ".example.com/logo.png", "跨 origin");
        check(server.requests().size() == requestCount, "越界资源执行了 HTTP 请求");
        expectFailure(loader, "/missing.png", "HTTP 404");
        expectFailure(loader, "/redirect.png", "HTTP 302");
        expectFailure(loader, "/broken.png", "/broken.png");
        expectFailure(loader, "/html.png", "MIME 不支持");
        const auto bounded = canton::runtime::livePainterLoader(server.origin, {1024, 1000});
        expectFailure(bounded, "/large.png", "预算");
        expectFailure(bounded, "/chunked.png", "预算");
        const auto timed = canton::runtime::livePainterLoader(server.origin, {1024, 100});
        const auto started = std::chrono::steady_clock::now();
        expectFailure(timed, "/stall.png", "超时");
        check(std::chrono::steady_clock::now() - started < 1500ms, "阻塞响应读取没有遵守总时限");

        // 真实 HTTP 请求在途时退休或换代：旧回调不得执行
        canton::quickjs::QuickJsScriptHost host;
        host.setPainterLoader(loader);
        for (const std::string path : {"/slow.png", "/slow.svg"}) {
            const auto retired = host.executeModule("live-painter-retire.js", "const n=globalThis.__CANTON_NATIVE__;n.createNode(1,'Root');const h=n.acquirePainter('" + path + "',()=>{throw new Error('退休资源不得完成')});n.releasePainter(h)");
            check(retired.ok, retired.error);
            std::this_thread::sleep_for(250ms);
            check(host.semanticCheckpoint(16).ok && !host.hasPendingSemanticWork(), "真实 Live 请求退休后执行了旧回调");
            const auto pending = host.executeModule("live-painter-old.js", "const n=globalThis.__CANTON_NATIVE__;n.createNode(1,'Root');n.acquirePainter('" + path + "',()=>{throw new Error('旧代际不得完成')})");
            check(pending.ok, pending.error);
            check(host.executeModule("live-painter-new.js", "globalThis.__CANTON_NATIVE__.createNode(1,'Root')").ok, "Live Painter 重载失败");
            std::this_thread::sleep_for(250ms);
            check(host.semanticCheckpoint(32).ok && !host.hasPendingSemanticWork(), "真实 Live 请求跨越了运行时代际");
        }
    }

    // 写一个只有 app.js 和若干资产的目录包
    std::filesystem::path writePackage(const std::filesystem::path& root, const std::vector<std::pair<std::string, std::string>>& assets) {
        const auto write = [&](const std::string& name, const std::string& bytes) {
            const auto path = root / std::filesystem::path(std::u8string(name.begin(), name.end()));
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        const auto row = [](const std::string& name, const std::string& bytes, const char* kind) {
            return "{\"path\":\"" + name + "\",\"kind\":\"" + kind + "\",\"size\":" + std::to_string(bytes.size()) + ",\"sha256\":\"" + canton::runtime::sha256Hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}) + "\"}";
        };
        const std::string app = "export {}";
        write("app.js", app);
        std::string rows = row("app.js", app, "module");
        for (const auto& [name, bytes] : assets) {
            write(name, bytes);
            rows += "," + row(name, bytes, "asset");
        }
        write(std::string(canton::runtime::uiManifestName), "{\"format\":3,\"framework\":\"" + std::string(canton::core::PackageVersion) + "\",\"protocol\":" + std::to_string(canton::core::RuntimeVersion) + ",\"scriptFormat\":\"js\",\"entry\":\"app.js\",\"entries\":[" + rows + "]}");
        return root;
    }

    void inlineAndPackageResources() {
        std::string encoded;
        {
            // base64
            constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::uint32_t buffer = 0;
            int bits = 0;
            for (const unsigned char c : redPng) {
                buffer = (buffer << 8) | c, bits += 8;
                while (bits >= 6) encoded += alphabet[(buffer >> (bits -= 6)) & 63];
            }
            if (bits) encoded += alphabet[(buffer << (6 - bits)) & 63];
            while (encoded.size() % 4) encoded += '=';
        }
        const auto pngUrl = "data:image/png;base64," + encoded;
        const std::string svgUrl = "data:image/svg+xml,%3csvg%20xmlns='http://www.w3.org/2000/svg'%20width='13'%20height='7'%3e%3crect%20width='13'%20height='7'%20fill='%23ff0000'/%3e%3c/svg%3e";
        const auto root = std::filesystem::temp_directory_path() / ("canton-painter-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        struct Cleanup {
            std::filesystem::path path;

            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{root};

        const auto package = canton::runtime::packagePainterLoader(canton::runtime::mountUiPackage(writePackage(root, {{"logo.png", redPng}, {"logo 中文.png", redPng}, {"logo%2e.png", redPng}, {"logo%20.png", redPng}})));
        for (const auto& loader : {package, canton::runtime::livePainterLoader("http://127.0.0.1:1")}) {
            const auto bitmap = load(loader, pngUrl);
            const auto svg = load(loader, svgUrl);
            check(bitmap.content && bitmap.content->intrinsicSize == canton::core::Size{3, 2}, "内联 PNG 解码失败：" + bitmap.error);
            check(svg.content && svg.content->intrinsicSize == canton::core::Size{13, 7}, "内联 SVG 解码失败：" + svg.error);
            expectFailure(loader, "data:text/html,hello", "只支持");
            expectFailure(loader, "data:image/png;base64,!", "base64");
            expectFailure(loader, "data:image/svg+xml,%ZZ", "百分号");
            expectFailure(loader, "data:image/svg+xml,%3csvg%20xmlns='http://www.w3.org/2000/svg'%3e%3cimage%20xlink:href='file:///tmp/secret.png'/%3e%3c/svg%3e", "外部资源");
            expectFailure(loader, "data:image/svg+xml,%3c!DOCTYPE%20svg%20SYSTEM%20'file:///tmp/secret'%3e%3csvg/%3e", "DTD");
            expectFailure(loader, "data:image/svg+xml,%3csvg%20xmlns='http://www.w3.org/2000/svg'%20width='0'%20height='5'/%3e", "视口");
        }
        expectFailure(canton::runtime::livePainterLoader("http://127.0.0.1:1", {16, 1000}), pngUrl, "预算");
        for (const auto& path : {"logo.png", "/logo.png", "/logo.png?cache=1#fragment", "logo 中文.png", "/logo%20%E4%B8%AD%E6%96%87.png", "logo%2e.png", "/logo%252e.png", "logo%20.png", "/logo%2520.png"}) check(load(package, path).content != nullptr, std::string("Dist 包根路径未加载：") + path);
        for (const auto& path : {"../logo.png", "/../logo.png", "/%2e%2e/logo.png", "/%2f%2fserver/logo.png", "/C%3a/logo.png", "/bad%00.png", "/bad%ZZ.png", "C:/logo.png", "C:\\logo.png", "//server/logo.png", "\\\\server\\logo.png", "http://127.0.0.1/logo.png", "file:///tmp/logo.png", "question?.png", "/question%3F.png?cache=1"}) {
            const auto result = load(package, path);
            check(!result.content && !result.error.empty(), std::string("Dist 接受了文件路径或包外资源：") + path);
        }
    }
}

int main() {
    const char* step = "Live 资源";
    try {
        liveResources(CANTON_SOURCE_DIR);
        step = "内联资源与 Dist 包";
        inlineAndPackageResources();
        std::cout << "Live 同源、URL 边界、PNG/SVG/字体、HTTP 诊断、预算、超时、资源退休，内联资源与 Dist 包根语义通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
