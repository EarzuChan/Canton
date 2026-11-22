#include <runtime/LiveClient.h>

#include <core/InputIntent.h>
#include <core/Log.h>
#include <runtime/CantonRuntime.h>
#include <runtime/DevServerClient.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/HttpClient.h>
#include <runtime/Json.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace canton::runtime {
    namespace {
        constexpr std::string_view TAG = "LiveModuleClient";

        std::wstring widen(std::string_view text) {
            if (text.empty()) return {};
            const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        // Vite 热更新通道：同步的 WinHTTP WebSocket。接收在自己的线程，发送可来自别的线程
        class HotSocket {
           public:
            ~HotSocket() {
                close();
            }

            bool connect(const DevServerEndpoint& endpoint) {
                close();
                auto* session = WinHttpOpen(L"Canton", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                if (!session) return false;
                WinHttpSetTimeouts(session, 2000, 2000, 2000, 0);
                auto* connection = WinHttpConnect(session, widen(endpoint.host).c_str(), static_cast<INTERNET_PORT>(endpoint.port), 0);
                auto* request = connection ? WinHttpOpenRequest(connection, L"GET", widen(endpoint.websocketPath).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
                HINTERNET socket = nullptr;
                // 不带 Origin 头：Vite 把它当作非浏览器客户端，不要求连接令牌
                if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) && WinHttpSendRequest(request, L"Sec-WebSocket-Protocol: vite-hmr\r\n", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) socket = WinHttpWebSocketCompleteUpgrade(request, 0);
                if (request) WinHttpCloseHandle(request);
                const std::scoped_lock lock(mutex_);
                if (!socket || closing_) {
                    if (socket) WinHttpCloseHandle(socket);
                    if (connection) WinHttpCloseHandle(connection);
                    WinHttpCloseHandle(session);
                    return false;
                }
                session_ = session;
                connection_ = connection;
                socket_ = socket;
                return true;
            }

            // 收一条完整的文本消息；连接断开或被关闭时返回空
            std::optional<std::string> receive() {
                std::string message;
                std::array<char, 16384> buffer{};
                while (true) {
                    HINTERNET socket;
                    {
                        const std::scoped_lock lock(mutex_);
                        socket = socket_;
                    }
                    if (!socket) return std::nullopt;
                    DWORD read = 0;
                    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                    if (WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return std::nullopt;
                    message.append(buffer.data(), read);
                    if (message.size() > 64 * 1024 * 1024) return std::nullopt;
                    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return message;
                }
            }

            void send(const std::string& text) {
                const std::scoped_lock lock(mutex_);
                if (socket_) WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
            }

            // 关掉句柄，阻塞中的接收随即返回
            void close(bool forever = false) {
                const std::scoped_lock lock(mutex_);
                closing_ = closing_ || forever;
                for (auto* handle : {&socket_, &connection_, &session_})
                    if (*handle) WinHttpCloseHandle(std::exchange(*handle, nullptr));
            }

           private:
            std::mutex mutex_;
            HINTERNET session_ = nullptr;
            HINTERNET connection_ = nullptr;
            HINTERNET socket_ = nullptr;
            bool closing_ = false;
        };
    }

    namespace {
        class LiveClient final : public LiveModuleClient {
           public:
            explicit LiveClient(std::function<void()> wake) : wake_(std::move(wake)) {}

            ~LiveClient() override {
                {
                    const std::scoped_lock lock(mutex_);
                    stopping_ = true;
                }
                http_.cancel();
                socket_.close(true);
                changed_.notify_all();
                if (socketThread_.joinable()) socketThread_.join();
                if (fetchThread_.joinable()) fetchThread_.join();
            }

            void start(std::string url) override {
                url_ = std::move(url);
                endpoint_ = parseDevServerUrl(url_);
                core::Log::i(TAG, "Live 客户端已启动", url_);
                if (!endpoint_.ok) {
                    LiveModulePacket packet;
                    packet.unavailable = true;
                    packet.reload = true;
                    packet.error = endpoint_.error;
                    push(std::move(packet));
                    return;
                }
                fetchThread_ = std::thread([this] { fetchLoop(); });
                // 首个快照由服务端的 connected 消息触发；连不上时只报一次不可用，随后静默重连
                socketThread_ = std::thread([this] { socketLoop(); });
            }

            void requestReload() override {
                {
                    const std::scoped_lock lock(mutex_);
                    if (reloadPending_ || stopping_) return;
                    reloadPending_ = true;
                    quickjs::HotMessage message;
                    message.type = "full-reload";
                    requests_.push_back(std::move(message));
                }
                core::Log::i(TAG, "已请求 Live 模块快照");
                changed_.notify_all();
            }

            void send(const quickjs::HotMessage& message) override {
                if (message.event == "canton:import") {
                    // 动态 import：先把目标模块的快照取回来，再让脚本侧继续
                    const auto* id = jsonNumber(jsonMember(message.data, "id"));
                    const auto* url = jsonString(jsonMember(message.data, "url"));
                    if (!id || !url) {
                        LiveModulePacket packet;
                        packet.error = "动态 ESM 请求必须提供数值 id 与字符串 url";
                        push(std::move(packet));
                        return;
                    }
                    auto request = message;
                    request.type = "import";
                    {
                        const std::scoped_lock lock(mutex_);
                        requests_.push_back(std::move(request));
                    }
                    changed_.notify_all();
                    return;
                }
                socket_.send(writeJson({JsonValue::Object{{"type", {message.type}}, {"event", {message.event}}, {"data", message.data}}}));
            }

            std::vector<LiveModulePacket> takePackets() override {
                const std::scoped_lock lock(mutex_);
                return std::exchange(packets_, {});
            }

           private:
            void push(LiveModulePacket packet) {
                {
                    const std::scoped_lock lock(mutex_);
                    if (stopping_) return;
                    packets_.push_back(std::move(packet));
                }
                if (wake_) wake_();
            }

            bool stopping() {
                const std::scoped_lock lock(mutex_);
                return stopping_;
            }

            void reportUnavailable(std::string reason) {
                core::Log::w(TAG, reason, url_);
                LiveModulePacket packet;
                packet.unavailable = true;
                packet.reload = true;
                packet.error = std::move(reason);
                push(std::move(packet));
            }

            void socketLoop() {
                bool reported = false;
                while (!stopping()) {
                    if (socket_.connect(endpoint_)) {
                        reported = false;
                        core::Log::i(TAG, "已连上 DevServer 的热更新通道", url_);
                        while (const auto message = socket_.receive()) receive(*message);
                        socket_.close();
                        if (stopping()) return;
                        reportUnavailable("与 Live DevServer 的连接已断开");
                        reported = true;
                    } else if (!std::exchange(reported, true))
                        reportUnavailable("连不上 Live DevServer");
                    std::unique_lock lock(mutex_);
                    changed_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
                }
            }

            void receive(const std::string& text) {
                JsonValue value;
                try {
                    value = parseJson(text);
                } catch (const std::exception&) {
                    return;
                }
                const auto* type = jsonString(jsonMember(value, "type"));
                if (!type) return;
                quickjs::HotMessage message;
                message.type = *type;
                if (message.type == "update" || message.type == "full-reload" || message.type == "prune") core::Log::i(TAG, "已收到 Vite 更新事件", message.type);
                if (message.type == "connected") {
                    // 每次连上（含断线重连）都取一份新快照，离线期间的改动不会漏
                    requestReload();
                    return;
                }
                if (message.type == "error") {
                    LiveModulePacket packet;
                    const auto* error = jsonMember(value, "err");
                    const auto* reason = error ? jsonString(jsonMember(*error, "message")) : nullptr;
                    packet.error = reason ? *reason : "DevServer 报告了错误";
                    push(std::move(packet));
                    return;
                }
                if (message.type != "update" && message.type != "prune" && message.type != "full-reload" && message.type != "custom") return;
                if (const auto* updates = jsonArray(jsonMember(value, "updates")))
                    for (const auto& update : *updates) {
                        const auto text = [&](const char* name) {
                            const auto* found = jsonString(jsonMember(update, name));
                            return found ? *found : std::string();
                        };
                        if (text("type") != "js-update") continue;
                        const auto* timestamp = jsonNumber(jsonMember(update, "timestamp"));
                        message.updates.push_back({"js-update", text("path"), text("acceptedPath"), timestamp ? *timestamp : 0.0, jsonBoolean(jsonMember(update, "explicitImportRequired")), text("firstInvalidatedBy")});
                    }
                if (const auto* paths = jsonArray(jsonMember(value, "paths")))
                    for (const auto& path : *paths)
                        if (const auto* item = jsonString(&path)) message.paths.push_back(*item);
                if (const auto* event = jsonString(jsonMember(value, "event"))) message.event = *event;
                if (const auto* data = jsonMember(value, "data")) message.data = *data;
                if (message.type == "full-reload") {
                    requestReload();
                    return;
                }
                {
                    const std::scoped_lock lock(mutex_);
                    requests_.push_back(std::move(message));
                }
                changed_.notify_all();
            }

            void fetchLoop() {
                while (true) {
                    quickjs::HotMessage request;
                    {
                        std::unique_lock lock(mutex_);
                        changed_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
                        if (stopping_) return;
                        request = std::move(requests_.front());
                        requests_.pop_front();
                    }
                    auto packet = fetch(std::move(request));
                    if (packet.message.type == "import") {
                        packet.message.type = "custom";
                        packet.message.event = "canton:import-ready";
                        if (!packet.error.empty()) {
                            std::get<JsonValue::Object>(packet.message.data.value)["error"] = {std::exchange(packet.error, {})};
                            packet.unavailable = false;
                        } else
                            core::Log::i(TAG, "已返回动态模块快照", packet.snapshot.modules.size(), "个模块");
                    }
                    if (packet.reload) {
                        const std::scoped_lock lock(mutex_);
                        reloadPending_ = false;
                    }
                    push(std::move(packet));
                }
            }

            LiveModulePacket fetch(quickjs::HotMessage message) {
                LiveModulePacket packet;
                packet.reload = message.type == "full-reload";
                packet.message = std::move(message);
                const auto dynamicImport = packet.message.type == "import";
                if (!packet.reload && packet.message.type != "update" && !dynamicImport) return packet;
                std::string address = std::string(endpoint_.secure ? "https://" : "http://") + endpoint_.host + ":" + std::to_string(endpoint_.port) + "/@canton/modules";
                const auto addRoot = [&](const std::string& path) {
                    address += (address.find('?') == std::string::npos ? "?url=" : "&url=") + encodeQueryValue(path);
                };
                for (const auto& update : packet.message.updates) addRoot(update.acceptedPath + (update.acceptedPath.find('?') == std::string::npos ? "?" : "&") + "t=" + std::to_string(static_cast<std::uint64_t>(update.timestamp)) + (update.explicitImportRequired ? "&import" : ""));
                if (dynamicImport) addRoot(*jsonString(jsonMember(packet.message.data, "url")));
                HttpResponse response;
                try {
                    response = httpGet(address, {64 * 1024 * 1024, 30000}, &http_);
                } catch (const std::exception& error) {
                    packet.unavailable = true;
                    packet.error = std::string("Live server 不可用（") + error.what() + "）：" + address;
                    core::Log::e(TAG, "Live 快照请求失败", packet.error);
                    return packet;
                }
                if (response.status != 200) {
                    packet.error = "Live ESM 请求失败（HTTP " + std::to_string(response.status) + "）：" + response.body;
                    core::Log::e(TAG, "Live 快照响应异常", response.status, response.body);
                    return packet;
                }
                try {
                    const auto snapshot = parseJson(response.body);
                    const auto* modules = jsonArray(jsonMember(snapshot, "modules"));
                    const auto* entry = jsonString(jsonMember(snapshot, "entry"));
                    if (!modules || !entry) throw std::runtime_error("Live ESM 快照格式无效");
                    packet.snapshot.entry = *entry;
                    for (const auto& module : *modules) {
                        const auto* url = jsonString(jsonMember(module, "url"));
                        const auto* source = jsonString(jsonMember(module, "source"));
                        if (!url || !source) throw std::runtime_error("Live ESM 模块格式无效");
                        const auto* map = jsonMember(module, "map");
                        packet.snapshot.modules.push_back({*url, *source, map ? writeJson(*map) : std::string("null")});
                    }
                } catch (const std::exception& error) {
                    packet.snapshot = {};
                    packet.error = error.what();
                    core::Log::e(TAG, "Live 模块快照解析失败", packet.error);
                    return packet;
                }
                if (packet.reload) core::Log::i(TAG, "已收到 Live 模块快照", packet.snapshot.modules.size(), "个模块");
                return packet;
            }

            std::function<void()> wake_;
            std::string url_;
            DevServerEndpoint endpoint_;
            HotSocket socket_;
            HttpCancellation http_;
            std::mutex mutex_;
            std::condition_variable changed_;
            std::deque<quickjs::HotMessage> requests_;
            std::vector<LiveModulePacket> packets_;
            std::thread socketThread_;
            std::thread fetchThread_;
            bool reloadPending_ = false;
            bool stopping_ = false;
        };
    }

    std::unique_ptr<LiveModuleClient> createLiveModuleClient(std::function<void()> wake) {
        return std::make_unique<LiveClient>(std::move(wake));
    }

    namespace {
        constexpr std::string_view SESSION_TAG = "SceneHost";

        RuntimePackageLoadResult loadLiveSnapshot(const RuntimePackageLoader& loader, const RuntimePackageLoader::LiveResources& liveResources, const std::string& devServerUrl, const quickjs::LiveModuleSnapshot& snapshot, const std::string& error) {
            RuntimePackageLoadResult result;
            if (devServerUrl.empty()) {
                result.error = makeErrorScreenModel(ErrorSource::AppPackage, "Canton 开发服务器地址无效");
                result.toast = RuntimeLoadToast{core::LogLevel::Error, "Live 源无效", result.error->summary, true};
                return result;
            }
            if (!error.empty()) {
                result.error = makeErrorScreenModel(ErrorSource::ScriptLoad, error, {}, devServerUrl);
                result.toast = RuntimeLoadToast{core::LogLevel::Error, "Live 模块加载失败", error, true};
                return result;
            }
            std::unique_ptr<canton::quickjs::QuickJsScriptHost> scriptHost;
            try {
                scriptHost = loader.createHost(liveResources ? liveResources(devServerUrl) : RuntimeLoadResources{});
            } catch (const std::exception& failure) {
                result.error = makeErrorScreenModel(ErrorSource::AppPackage, failure.what(), {}, devServerUrl);
                result.toast = RuntimeLoadToast{core::LogLevel::Error, "Live 资源来源无效", failure.what(), true};
                return result;
            }
            const auto executed = scriptHost->executeLiveModules(snapshot);
            if (!executed.ok) {
                result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, executed.error, {}, devServerUrl);
                result.toast = RuntimeLoadToast{core::LogLevel::Error, "Live 运行时异常", executed.error, true};
                return result;
            }
            if (!scriptHost->hasMountedApp()) {
                result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, "入口模块未启动应用：需要默认导出 appBuilder(Root).build() 的应用定义", {}, devServerUrl);
                result.toast = RuntimeLoadToast{core::LogLevel::Error, "Live 入口无效", result.error->summary, true};
                return result;
            }
            result.initialTransaction = scriptHost->takePendingTransaction();
            result.scriptHost = std::move(scriptHost);
            result.ok = true;
            result.toast = RuntimeLoadToast{core::LogLevel::Info, "Live App 已启动", devServerUrl, true};
            return result;
        }

        class LiveSessionImpl final : public LiveSession {
           public:
            LiveSessionImpl(std::unique_ptr<LiveModuleClient> client, RuntimePackageLoader::LiveResources resources) : client_(std::move(client)), resources_(std::move(resources)) {}

            void start(const std::string& devServerUrl) override {
                unavailable_ = false;
                hotUpdateFailed_ = false;
                client_->start(devServerUrl);
            }

            void requestReload() override {
                client_->requestReload();
            }

            bool unavailable() const noexcept override {
                return unavailable_;
            }

            void pump(PackageRuntimeSource& source, CantonRuntime& runtime, DiagnosticsState& diagnostics, const std::function<void(PackageLoadOutcome)>& apply) override {
                for (auto& packet : client_->takePackets()) {
                    if (packet.reload) {
                        hotUpdateFailed_ = false;
                        apply(completeLoad(source, std::move(packet)));
                        continue;
                    }
                    const auto live = source.activeSource() == PackageSource::Live;
                    if (packet.message.type == "update" && (!live || (diagnostics.hasError() && !hotUpdateFailed_))) {
                        hotUpdateFailed_ = false;
                        apply(source.reload());
                        continue;
                    }
                    if (packet.error.empty() && live) {
                        auto* host = runtime.scriptHost();
                        const auto result = host ? host->applyHotUpdate(packet.snapshot, packet.message) : quickjs::CallbackInvokeResult{false, "没有 live script host"};
                        runtime.notifyWorkAvailable();
                        if (!result.ok)
                            packet.error = result.error;
                        else if (hotUpdateFailed_ && packet.message.type == "update") {
                            hotUpdateFailed_ = false;
                            diagnostics.clearError();
                            (void)diagnostics.notify(core::LogLevel::Info, SESSION_TAG, "热更新已恢复", source.config().devServerUrl);
                            runtime.enqueueIntent(core::InputIntent::diagnostics());
                        }
                    }
                    if (!packet.error.empty()) {
                        diagnostics.setError(makeErrorScreenModel(ErrorSource::Hmr, packet.error, "改好后保存，画面会自动恢复", source.config().devServerUrl));
                        (void)diagnostics.notify(core::LogLevel::Error, SESSION_TAG, "Live 热更新失败", packet.error);
                        hotUpdateFailed_ = true;
                        runtime.enqueueIntent(core::InputIntent::diagnostics());
                    }
                }
                if (auto* host = runtime.scriptHost())
                    for (const auto& message : host->takeHotMessages()) client_->send(message);
            }

           private:
            PackageLoadOutcome completeLoad(PackageRuntimeSource& source, LiveModulePacket packet) {
                const auto& config = source.config();
                if (packet.unavailable) {
                    unavailable_ = true;
                    if (config.policy == LoadPolicy::LiveThenDist && !source.resolver().locateDist(config).missing) {
                        auto outcome = source.loadDistPackage();
                        outcome.intentKind = PackageLoadOutcome::IntentKind::Reload;
                        outcome.toasts.insert(outcome.toasts.begin(), RuntimeLoadToast{core::LogLevel::Warn, "Live 不可用，已回退到 Dist", packet.error + "；DevServer 恢复后自动切回 Live", false});
                        return outcome;
                    }
                    PackageLoadOutcome outcome;
                    outcome.intentKind = PackageLoadOutcome::IntentKind::Reload;
                    source.setActiveSource(PackageSource::None);
                    const auto detail = config.policy == LoadPolicy::LiveThenDist ? "也没有可回退的 Dist 包。启动 DevServer 后会自动连接" : "启动 DevServer 后会自动连接";
                    outcome.error = makeErrorScreenModel(ErrorSource::AppPackage, "连不上 Live DevServer：" + config.devServerUrl, packet.error + "\n" + detail, {}, true);
                    outcome.toasts.push_back({core::LogLevel::Error, "Live 不可用", packet.error, true});
                    return outcome;
                }
                auto outcome = PackageRuntimeSource::fromLoadResult(loadLiveSnapshot(source.loader(), resources_, config.devServerUrl, packet.snapshot, packet.error), PackageSource::Live);
                outcome.intentKind = PackageLoadOutcome::IntentKind::HmrReload;
                unavailable_ = false;
                source.setActiveSource(outcome.loaded ? PackageSource::Live : PackageSource::None);
                return outcome;
            }

            std::unique_ptr<LiveModuleClient> client_;
            RuntimePackageLoader::LiveResources resources_;
            bool unavailable_ = false;
            bool hotUpdateFailed_ = false;
        };
    }

    PackageRuntimeSource::LiveSessionFactory makeLiveSessionFactory(std::function<void()> wake, RuntimePackageLoader::LiveResources resources) {
        return [wake = std::move(wake), resources = std::move(resources)] {
            return std::make_unique<LiveSessionImpl>(createLiveModuleClient(wake), resources);
        };
    }
}
