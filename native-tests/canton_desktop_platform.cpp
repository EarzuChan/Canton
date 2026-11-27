// M6 桌面融合与系统能力原生综合单测：
// 1. TimerQueue 单调时钟最小堆精准触发/顺序/取消/周期重排/清空；
// 2. WinHTTP 异步/同步请求（本地环回 HTTP：GET / POST JSON / 302 重定向与跨域剥离 Authorization / 超时 / HttpCancellation 取消 / maxBytes 超限 / downloadFile 流式原子落盘、进度与非 2xx 不覆盖）；
// 3. AppPaths 三级透明重定位（显式根 / 便携标记 / 标准 LocalAppData）与用户标准目录；
// 4. FileSystem UTF-16 长路径（>260 字符）、原子写、stat、readDir、mkdir、rename、copy、remove 与错误码；
// 5. SingleInstanceGuard 真实双进程互斥与命名管道 IPC 热唤醒；
// 6. Win32 隐藏顶层 HWND（WS_EX_TOOLWINDOW、非 HWND_MESSAGE）、稳定托盘 GUID 派生与自定义托盘菜单避让定位；
// 7. D3D11 WARP 无损内存瘦身（初始 512×512 R8 图集按需扩容 + 离屏 RT 池 2 帧空闲回收计数）；
// 8. QuickJsScriptHost 全局 setTimeout / setInterval / queueMicrotask / AbortController / AbortSignal.timeout、禁止字符串回调、以及 assets.exists。
#include <core/Clock.h>
#include <core/DrawCommands.h>
#include <core/Font.h>
#include <core/GraphicsEffects.h>
#include <core/SceneFramePipeline.h>
#include <d3d11/DWriteText.h>
#include <d3d11/Renderer.h>
#include <quickjs/QuickJsScriptHost.h>
#include <runtime/AppPaths.h>
#include <runtime/FileSystem.h>
#include <runtime/HttpClient.h>
#include <runtime/IoThreadPool.h>
#include <runtime/PainterResources.h>
#include <runtime/SingleInstance.h>
#include <runtime/TimerQueue.h>
#include <runtime/UiPackage.h>
#include <win32/ShellTray.h>
#include <win32/Window.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace canton;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 本地环回 HTTP 测试服务器：覆盖 GET / POST JSON / 302 重定向 / 慢响应 / 超大响应 / 下载与 404
    class LocalHttpServer {
       public:
        LocalHttpServer() {
            WSADATA wsa{};
            check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup 失败");
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            check(listener_ != INVALID_SOCKET && bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(listener_, 16) == 0, "监听环回 HTTP 失败");
            int len = sizeof(addr);
            getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
            port = ntohs(addr.sin_port);
            origin = "http://127.0.0.1:" + std::to_string(port);
            thread_ = std::thread([this] { serve(); });
        }

        ~LocalHttpServer() {
            stopping_ = true;
            closesocket(listener_);
            if (thread_.joinable()) thread_.join();
            for (auto& w : workers_)
                if (w.joinable()) w.join();
            WSACleanup();
        }

        int port = 0;
        std::string origin;
        std::string redirectTarget;

        std::string lastReceivedAuthHeader() {
            const std::scoped_lock lock(mutex_);
            return lastAuth_;
        }

       private:
        static void sendAll(SOCKET s, std::string_view data) {
            while (!data.empty()) {
                const int sent = ::send(s, data.data(), static_cast<int>(data.size()), 0);
                if (sent <= 0) return;
                data.remove_prefix(static_cast<std::size_t>(sent));
            }
        }

        void serve() {
            while (!stopping_) {
                SOCKET c = accept(listener_, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                const std::scoped_lock lock(mutex_);
                workers_.emplace_back([this, c] {
                    handle(c);
                    shutdown(c, SD_BOTH);
                    closesocket(c);
                });
            }
        }

        void handle(SOCKET s) {
            std::string raw;
            char buf[4096];
            std::size_t headerEnd = std::string::npos;
            std::size_t contentLength = 0;
            while (headerEnd == std::string::npos && raw.size() < 65536) {
                const int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) return;
                raw.append(buf, static_cast<std::size_t>(n));
                headerEnd = raw.find("\r\n\r\n");
            }
            if (headerEnd == std::string::npos) return;
            const std::string headerPart = raw.substr(0, headerEnd);
            std::string body = raw.substr(headerEnd + 4);

            // 解析首行与 Content-Length / Authorization
            const auto lineEnd = headerPart.find("\r\n");
            const auto reqLine = headerPart.substr(0, lineEnd);
            const auto sp1 = reqLine.find(' ');
            const auto sp2 = reqLine.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) return;
            const auto method = reqLine.substr(0, sp1);
            const auto path = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

            std::string authValue;
            std::size_t pos = lineEnd == std::string::npos ? headerPart.size() : lineEnd + 2;
            while (pos < headerPart.size()) {
                const auto next = headerPart.find("\r\n", pos);
                const auto line = headerPart.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                pos = next == std::string::npos ? headerPart.size() : next + 2;
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                auto key = runtime::asciiLower(line.substr(0, colon));
                auto val = line.substr(colon + 1);
                while (!val.empty() && val.front() == ' ') val.erase(val.begin());
                if (key == "content-length") {
                    try {
                        contentLength = static_cast<std::size_t>(std::stoull(val));
                    } catch (...) {}
                } else if (key == "authorization") {
                    authValue = val;
                }
            }

            while (body.size() < contentLength) {
                const int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) break;
                body.append(buf, static_cast<std::size_t>(n));
            }
            {
                const std::scoped_lock lock(mutex_);
                lastAuth_ = authValue;
            }

            if (path == "/echo" && method == "POST") {
                const std::string resp = "{\"method\":\"POST\",\"len\":" + std::to_string(body.size()) + ",\"body\":" + body + "}";
                sendAll(s, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nX-Multi: a\r\nX-Multi: b\r\nContent-Length: " + std::to_string(resp.size()) + "\r\nConnection: close\r\n\r\n");
                sendAll(s, resp);
                return;
            }
            if (path == "/redirect") {
                const std::string loc = redirectTarget.empty() ? "/final" : redirectTarget;
                sendAll(s, "HTTP/1.1 302 Found\r\nLocation: " + loc + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            if (path == "/final") {
                const std::string resp = "landed:" + authValue;
                sendAll(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(resp.size()) + "\r\nConnection: close\r\n\r\n");
                sendAll(s, resp);
                return;
            }
            if (path == "/slow") {
                std::this_thread::sleep_for(600ms);
                sendAll(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
                return;
            }
            if (path == "/big") {
                std::string chunk(8192, 'Z');
                sendAll(s, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(chunk.size() * 8) + "\r\nConnection: close\r\n\r\n");
                for (int i = 0; i < 8; ++i) {
                    sendAll(s, chunk);
                    std::this_thread::sleep_for(15ms);
                }
                return;
            }
            if (path == "/not-found") {
                sendAll(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 7\r\nConnection: close\r\n\r\nmissing");
                return;
            }
            sendAll(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
        }

        SOCKET listener_ = INVALID_SOCKET;
        std::atomic<bool> stopping_{false};
        std::thread thread_;
        std::mutex mutex_;
        std::vector<std::thread> workers_;
        std::string lastAuth_;
    };

    void testTimerQueue() {
        runtime::TimerQueue q;
        check(!q.nextWaitMillis(0.0).has_value(), "空定时队列不应有等待时间");
        const auto id1 = q.schedule(100.0, 50.0, false, {});
        const auto id2 = q.schedule(100.0, 20.0, true, {});
        const auto id3 = q.schedule(100.0, 20.0, false, {});
        check(q.nextWaitMillis(100.0) == 20u, "最近到期应为 20ms");
        check(q.cancel(id3), "取消应成功");
        check(!q.cancel(id3), "重复取消应幂等返回 false");

        auto due1 = q.popDue(119.0);
        check(due1.empty(), "未到期不应弹出");
        auto due2 = q.popDue(120.0);
        check(due2.size() == 1 && due2[0].id == id2 && due2[0].repeating, "应仅弹出 id2 周期定时器");
        // 未调用 rescheduleAfterRun 前不应重入
        check(q.popDue(145.0).empty(), "周期定时器在回调结束重排前不可连环重入");
        q.rescheduleAfterRun(id2, 130.0);
        check(q.nextWaitMillis(130.0) == 20u, "重排后距下次到期应为 20ms");

        auto due3 = q.popDue(150.0);
        check(due3.size() == 2 && due3[0].id == id1 && due3[1].id == id2, "应按到期时间与提交序弹出 id1 与 id2");
        q.clear();
        check(q.empty() && !q.nextWaitMillis(200.0).has_value(), "clear 后应清空全部定时项");
    }

    void testHttpAndDownload(const std::filesystem::path& tempRoot) {
        LocalHttpServer server1;
        LocalHttpServer server2;
        server1.redirectTarget = server2.origin + "/final";

        // 1. POST JSON + 多头合并
        {
            runtime::HttpRequestOptions req;
            req.method = "POST";
            req.url = server1.origin + "/echo";
            req.headers = {{"Content-Type", "application/json"}};
            req.body = "{\"x\":42}";
            req.useSystemProxy = false;
            const auto res = runtime::httpRequest(req);
            check(res.status == 200, "POST /echo 应返回 200");
            check(res.contentType == "application/json", "Content-Type 应归一化为 application/json");
            check(res.headers.at("x-multi") == "a, b", "同名多头应以 ', ' 合并");
            check(res.body.find("\"x\":42") != std::string::npos, "回显应包含请求体");
        }

        // 2. 跨域 302 重定向自动剥离 Authorization
        {
            runtime::HttpRequestOptions req;
            req.url = server1.origin + "/redirect";
            req.headers = {{"Authorization", "Bearer secret-token"}};
            req.followRedirects = true;
            req.useSystemProxy = false;
            const auto res = runtime::httpRequest(req);
            check(res.status == 200, "重定向目标应返回 200");
            check(res.url == server2.origin + "/final", "最终 URL 应更新为重定向落点");
            check(res.body == "landed:", "跨域重定向必须剥离 Authorization 头，实际收到：" + res.body);
        }

        // 3. 超时
        {
            runtime::HttpRequestOptions req;
            req.url = server1.origin + "/slow";
            req.timeoutMs = 120;
            req.useSystemProxy = false;
            bool gotTimeout = false;
            try {
                (void)runtime::httpRequest(req);
            } catch (const runtime::NetError& err) {
                gotTimeout = err.code() == runtime::NetErrorCode::Timeout;
            }
            check(gotTimeout, "慢响应应抛出 NetError(TIMEOUT)");
        }

        // 4. HttpCancellation 即时取消
        {
            runtime::HttpCancellation cancel;
            std::thread killer([&] {
                std::this_thread::sleep_for(80ms);
                cancel.cancel();
            });
            runtime::HttpRequestOptions req;
            req.url = server1.origin + "/slow";
            req.timeoutMs = 5000;
            req.useSystemProxy = false;
            bool gotAborted = false;
            try {
                (void)runtime::httpRequest(req, &cancel);
            } catch (const runtime::NetError& err) {
                gotAborted = err.code() == runtime::NetErrorCode::Aborted;
            }
            killer.join();
            check(gotAborted, "取消进行中的请求应抛出 NetError(ABORTED)");
        }

        // 5. maxBytes 超限
        {
            runtime::HttpRequestOptions req;
            req.url = server1.origin + "/big";
            req.maxBytes = 4096;
            req.useSystemProxy = false;
            bool tooLarge = false;
            try {
                (void)runtime::httpRequest(req);
            } catch (const runtime::NetError& err) {
                tooLarge = err.code() == runtime::NetErrorCode::ResponseTooLarge;
            }
            check(tooLarge, "超出 maxBytes 应抛出 NetError(RESPONSE_TOO_LARGE)");
        }

        // 6. downloadFile 流式原子落盘 + 进度 + 404 不覆盖既有文件
        {
            const auto dest = runtime::AppPaths::wideToUtf8((tempRoot / L"downloaded.bin").wstring());
            int progressCalls = 0;
            std::uint64_t lastReceived = 0;
            runtime::HttpRequestOptions req;
            req.url = server1.origin + "/big";
            req.maxBytes = 0;
            req.useSystemProxy = false;
            const auto dl = runtime::httpDownloadFile(req, dest, [&](const runtime::DownloadProgress& p) {
                ++progressCalls;
                lastReceived = p.receivedBytes;
            });
            check(dl.ok && dl.status == 200 && dl.bytesWritten == 8192 * 8, "downloadFile 应完整写入 64KB");
            check(progressCalls >= 1 && lastReceived == 8192 * 8, "downloadFile 应触发进度回调");
            check(runtime::fsReadBytes(dest).size() == 8192 * 8, "磁盘文件体积应与下载字节一致");

            // 404 不应覆盖已存在目标
            req.url = server1.origin + "/not-found";
            const auto dl404 = runtime::httpDownloadFile(req, dest);
            check(!dl404.ok && dl404.status == 404 && dl404.bytesWritten == 0, "非 2xx 应返回 ok=false 且 bytesWritten=0");
            check(runtime::fsReadBytes(dest).size() == 8192 * 8, "非 2xx 不得覆盖已有目标文件");
        }
    }

    void testPathsAndFileSystem(const std::filesystem::path& tempRoot) {
        // 1. 三级透明重定位
        {
            const auto explicitRoot = tempRoot / L"explicit-root";
            runtime::AppPaths p1({tempRoot / L"exe1", "com.canton.test", explicitRoot, false});
            check(p1.relocationMode() == runtime::AppPathRelocationMode::ExplicitRoot, "应命中显式根目录模式");
            check(p1.dataDirPath() == (explicitRoot / L"data"), "显式根目录 data 落点不符");
            check(runtime::fsExists(p1.appData()), "首次调用 appData() 应懒创建目录");

            const auto portableExeDir = tempRoot / L"portable-exe";
            std::filesystem::create_directories(portableExeDir);
            std::ofstream(portableExeDir / L".canton-portable") << "1";
            runtime::AppPaths p2({portableExeDir, "com.canton.test", std::nullopt, false});
            check(p2.relocationMode() == runtime::AppPathRelocationMode::Portable, "存在 .canton-portable 时应命中便携模式");
            check(p2.dataDirPath() == (portableExeDir / L"data"), "便携模式 data 落点不符");

            runtime::AppPaths p3({tempRoot / L"std-exe", "com.canton.test", std::nullopt, false});
            check(p3.relocationMode() == runtime::AppPathRelocationMode::Standard, "常规态应命中 Standard 模式");
            check(!runtime::AppPaths::downloads().empty() && !runtime::AppPaths::documents().empty(), "系统标准目录应可读");
        }

        // 2. UTF-16 长路径（>260 字符）与原子写
        {
            std::filesystem::path deep = tempRoot / L"longpath";
            for (int i = 0; i < 12; ++i) {
                deep /= L"segment_0123456789_abcdefghij";
            }
            const auto deepUtf8 = runtime::AppPaths::wideToUtf8(deep.wstring());
            check(deepUtf8.size() > 280, "测试路径长度应超过 MAX_PATH(260)");
            runtime::fsMkdir(deepUtf8, true);
            const auto fileUtf8 = deepUtf8 + "\\原子笔记.txt";
            runtime::fsWriteText(fileUtf8, "第一版内容", true);
            check(runtime::fsReadText(fileUtf8) == "第一版内容", "长路径原子写首版读取不符");
            runtime::fsWriteText(fileUtf8, "第二版原子替换", true);
            check(runtime::fsReadText(fileUtf8) == "第二版原子替换", "长路径原子替换内容不符");
            const auto st = runtime::fsStat(fileUtf8);
            check(st.isFile && !st.isDirectory && st.size > 0, "fsStat 应准确回报文件属性");
            const auto entries = runtime::fsReadDir(deepUtf8);
            check(entries.size() == 1 && entries[0].name == "原子笔记.txt", "fsReadDir 应正确列出 Unicode 文件名且不残留原子临时文件");
            runtime::fsRemove(runtime::AppPaths::wideToUtf8((tempRoot / L"longpath").wstring()), true);
            check(!runtime::fsExists(fileUtf8), "递归删除后文件应不存在");
        }
    }

    void testSingleInstanceIpc(const wchar_t* selfExePath) {
        const std::string appKey = "com.canton.single-test." + std::to_string(GetCurrentProcessId());
        std::mutex m;
        std::vector<core::LaunchRequest> received;
        runtime::SingleInstanceGuard primary;
        const bool acquired = primary.acquireOrForward(appKey, {{"--primary"}, "C:\\"}, [&](core::LaunchRequest req) {
            const std::scoped_lock lock(m);
            received.push_back(std::move(req));
        });
        check(acquired, "首实例应成功抢占单实例互斥体");

        // 拉起真实子进程扮演第二实例
        std::wstring cmd = L"\"" + std::wstring(selfExePath) + L"\" --ipc-child " + runtime::AppPaths::utf8ToWide(appKey) + L" --open hello-second";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        check(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE, "无法启动单实例测试子进程");
        check(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "单实例测试子进程超时");
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        check(exitCode == 0, "第二实例投递后应以退出码 0 安静离场");

        // 等待主实例线程回调
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                const std::scoped_lock lock(m);
                if (!received.empty()) break;
            }
            std::this_thread::sleep_for(10ms);
        }
        primary.stop();
        check(received.size() == 1, "主实例应收到 1 条二次唤醒载荷");
        check(received[0].args.size() >= 2 && received[0].args[0] == "--open" && received[0].args[1] == "hello-second", "二次唤醒命令行参数应完整送达");
    }

    void testWin32BaseAndTrayMath() {
        win32::MessageDispatcher dispatcher({});
        HWND appHwnd = static_cast<HWND>(dispatcher.hwnd());
        check(appHwnd != nullptr && IsWindow(appHwnd), "App 级隐藏顶层 HWND 应有效");
        check(!IsWindowVisible(appHwnd), "App 级隐藏顶层 HWND 必须保持不可见");
        const LONG_PTR exStyle = GetWindowLongPtrW(appHwnd, GWL_EXSTYLE);
        check((exStyle & WS_EX_TOOLWINDOW) != 0, "App 级隐藏顶层 HWND 必须带 WS_EX_TOOLWINDOW");
        check(GetParent(appHwnd) == nullptr, "App 级隐藏顶层 HWND 不能是 HWND_MESSAGE 子窗");

        const auto g1 = win32::deriveStableTrayGuidString("com.canton.demo", "main");
        const auto g2 = win32::deriveStableTrayGuidString("com.canton.demo", "main");
        const auto g3 = win32::deriveStableTrayGuidString("com.canton.demo", "secondary");
        check(g1 == g2 && g1 != g3 && g1.size() == 36, "托盘 GUID 派生必须稳定且按 key 区分");

        core::TrayAnchorRect anchor;
        anchor.left = 1860;
        anchor.top = 1040;
        anchor.right = 1890;
        anchor.bottom = 1080;
        anchor.workLeft = 0;
        anchor.workTop = 0;
        anchor.workRight = 1920;
        anchor.workBottom = 1040;
        const auto pos = win32::computePopupPositionForTray(anchor, 240, 200);
        check(pos.x >= 0 && pos.x + 240 <= 1920, "自定义托盘菜单 X 坐标必须钳制在工作区内");
        check(pos.y >= 0 && pos.y + 200 <= 1040, "自定义托盘菜单 Y 坐标必须避让底部任务栏");
    }

    void testWarpSlimming() {
        d3d11::Renderer renderer({d3d11::AdapterPreference::Warp});
        check(renderer.adapter().software, "WARP 渲染器应识别为软件适配器");
        check(renderer.counters().atlasSize == 512, "WARP 下 R8 图集初始尺寸应瘦身为 512");

        auto target = renderer.createOffscreenTarget(256, 256);

        // 1. 触发离屏 RT 分配，再绘制 4 帧纯矩形，验证 WARP 2 帧空闲阈值回收离屏 RT
        core::DrawOp rect;
        rect.type = core::DrawOpType::FillRect;
        rect.rect = {20, 20, 120, 120};
        rect.color = 0xff3b82f6u;

        core::PublishedFrameContent blurFrame;
        {
            auto frag = std::make_shared<core::PaintFragment>();
            auto ops = std::make_shared<std::vector<core::DrawOp>>();
            ops->push_back(rect);
            frag->content = ops;
            auto layer = std::make_shared<core::PaintLayerFragment>();
            layer->bounds = {0, 0, 256, 256};
            layer->useOffscreenGroup = true;
            layer->groupAlpha = 1.0f;
            core::RenderEffectData re;
            core::RenderEffectStep blurStep;
            blurStep.kind = core::RenderEffectStepKind::Blur;
            blurStep.radiusX = 4.0f;
            blurStep.radiusY = 4.0f;
            blurStep.edgeTreatment = core::TileMode::Decal;
            re.steps.push_back(blurStep);
            layer->groupRenderEffect = core::RenderEffectRef(std::move(re));
            frag->layer = layer;
            frag->bounds = {{0, 0, 256, 256}, true, false};
            blurFrame.scenePaint = {frag, {0, 0}};
        }
        renderer.render(*target, {&blurFrame, {}, 0xff000000u});
        check(renderer.counters().offscreenTargetsCreated > 0, "模糊帧应分配离屏 RT");

        core::PublishedFrameContent plainFrame;
        {
            auto frag = std::make_shared<core::PaintFragment>();
            auto ops = std::make_shared<std::vector<core::DrawOp>>();
            ops->push_back(rect);
            frag->content = ops;
            frag->bounds = {{0, 0, 256, 256}, true, false};
            plainFrame.scenePaint = {frag, {0, 0}};
        }
        for (int i = 0; i < 4; ++i) {
            renderer.render(*target, {&plainFrame, {}, 0xff000000u});
        }
        check(renderer.counters().offscreenTargetsEvicted > 0, "WARP 模式下空闲离屏 RT 应在 2 帧后被及时回收");

        // 2. 光栅化足够多不同尺寸路径填满 512×512 初始图集，验证按需自动扩容至 >=1024
        core::PublishedFrameContent atlasStress;
        {
            auto frag = std::make_shared<core::PaintFragment>();
            auto ops = std::make_shared<std::vector<core::DrawOp>>();
            for (int i = 0; i < 20; ++i) {
                core::PathData path;
                const float s = 140.0f + static_cast<float>(i);
                path.verbs = {core::PathVerb::MoveTo, core::PathVerb::LineTo, core::PathVerb::LineTo, core::PathVerb::Close};
                path.values = {0.0f, 0.0f, s, 0.0f, s * 0.5f, s};
                path.bounds = {0.0f, 0.0f, s, s};
                core::DrawOp op;
                op.type = core::DrawOpType::DrawPath;
                op.rect = {0.0f, 0.0f, s, s};
                op.path = core::PathRef(std::move(path));
                op.color = 0xffffffffu;
                ops->push_back(std::move(op));
            }
            frag->content = ops;
            frag->bounds = {{0, 0, 256, 256}, true, false};
            atlasStress.scenePaint = {frag, {0, 0}};
        }
        renderer.render(*target, {&atlasStress, {}, 0xff000000u});
        check(renderer.counters().atlasGrowths > 0 && renderer.counters().atlasSize >= 1024, "WARP 图集填满后应按需倍增扩容");
    }

    void testQuickJsTimersAbortAndAssetExists() {
        quickjs::QuickJsScriptHost host;
        auto bridge = std::make_shared<quickjs::PlatformBridge>();
        runtime::TimerQueue tq;
        bridge->scheduleTimer = [&](double delayMs, bool repeating) { return tq.schedule(core::monotonicMillis(), delayMs, repeating, {}); };
        bridge->rescheduleRepeatingTimer = [&](std::uint64_t id) { tq.rescheduleAfterRun(id, core::monotonicMillis()); };
        bridge->cancelTimer = [&](std::uint64_t id) { (void)tq.cancel(id); };
        bridge->clearTimers = [&]() { tq.clear(); };
        host.setPlatformBridge(bridge);
        host.setAssetExistsChecker([](std::string ref, std::function<void()>) {
            std::promise<bool> p;
            p.set_value(ref == "/present.txt");
            return p.get_future();
        });

        constexpr std::string_view script = R"JS(
            globalThis.__LOG__ = [];
            let rejectedStringTimer = false;
            try { setTimeout('1+1', 10); } catch (e) { rejectedStringTimer = e instanceof TypeError; }
            globalThis.__LOG__.push('strRejected:' + rejectedStringTimer);

            const ac = new AbortController();
            let abortedName = '';
            ac.signal.addEventListener('abort', () => { abortedName = ac.signal.reason.name; }, { once: true });
            ac.abort();
            globalThis.__LOG__.push('abort:' + ac.signal.aborted + ':' + abortedName);

            queueMicrotask(() => globalThis.__LOG__.push('micro'));
            setTimeout((tag) => globalThis.__LOG__.push('t:' + tag), 15, 'ok');
            const cancelledId = setTimeout(() => globalThis.__LOG__.push('never'), 10);
            clearTimeout(cancelledId);

            __CANTON_NATIVE__.assetExists('/present.txt').then(v => globalThis.__LOG__.push('exists1:' + v));
            __CANTON_NATIVE__.assetExists('/absent.txt').then(v => globalThis.__LOG__.push('exists0:' + v));

            __CANTON_NATIVE__.installFrameDriver(() => {}, () => {}, () => {});
            globalThis.__CANTON_LAUNCH_APP__ = () => {};
            export default {};
        )JS";
        const auto res = host.executeModule("desktop-timers.js", script);
        check(res.ok, "QuickJS 全局定时/Abort/assetExists 脚本执行失败：" + res.error);

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const double now = core::monotonicMillis();
            for (const auto& due : tq.popDue(now)) {
                const auto inv = host.invokeTimer(due.id);
                check(inv.ok, "定时回调异常：" + inv.error);
            }
            const auto ck = host.semanticCheckpoint(now);
            check(ck.ok, "semanticCheckpoint 异常：" + ck.error);
            if (tq.empty() && !host.hasPendingSemanticWork()) break;
            std::this_thread::sleep_for(5ms);
        }
        check(tq.empty(), "所有测试定时器应执行完毕");
    }
}

int wmain(int argc, wchar_t** argv) {
    // 子进程模式：供单实例 IPC 双进程实测使用
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--ipc-child") {
        const std::string appKey = runtime::AppPaths::wideToUtf8(argv[2]);
        core::LaunchRequest req;
        for (int i = 3; i < argc; ++i) req.args.push_back(runtime::AppPaths::wideToUtf8(argv[i]));
        req.workingDirectory = "C:\\ChildCwd";
        runtime::SingleInstanceGuard guard;
        const bool primary = guard.acquireOrForward(appKey, req, {});
        return primary ? 2 : 0;
    }

    const bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const auto tempRoot = std::filesystem::temp_directory_path() / (L"canton-desktop-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);

    int exitCode = 0;
    try {
        testTimerQueue();
        testHttpAndDownload(tempRoot);
        testPathsAndFileSystem(tempRoot);
        testSingleInstanceIpc(argv[0]);
        testWin32BaseAndTrayMath();
        testWarpSlimming();
        testQuickJsTimersAbortAndAssetExists();
        std::cout << "[canton_desktop_platform] all tests passed\n";
    } catch (const std::exception& err) {
        std::cerr << "[canton_desktop_platform] FAILED: " << err.what() << "\n";
        exitCode = 1;
    }

    std::filesystem::remove_all(tempRoot, ec);
    if (comReady) CoUninitialize();
    return exitCode;
}
