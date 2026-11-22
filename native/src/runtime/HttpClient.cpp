#include <runtime/HttpClient.h>

#include <runtime/PainterResources.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>

#include <array>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace canton::runtime {
    struct HttpCancellation::State {
        std::mutex mutex;
        HINTERNET request = nullptr;
        bool cancelled = false;
    };

    HttpCancellation::HttpCancellation() : state_(std::make_shared<State>()) {}

    HttpCancellation::~HttpCancellation() = default;

    void HttpCancellation::cancel() noexcept {
        const std::scoped_lock lock(state_->mutex);
        state_->cancelled = true;
        // 关掉句柄让阻塞中的调用立刻返回；句柄本身仍由发起请求的线程释放
        if (state_->request) WinHttpCloseHandle(std::exchange(state_->request, nullptr));
    }

    namespace {
        struct Handle {
            HINTERNET value = nullptr;

            ~Handle() {
                if (value) WinHttpCloseHandle(value);
            }
        };

        std::wstring widen(std::string_view text) {
            if (text.empty()) return {};
            const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        std::string narrow(std::wstring_view text) {
            if (text.empty()) return {};
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }
    }

    std::string encodeQueryValue(std::string_view value) {
        constexpr std::string_view hex = "0123456789ABCDEF";
        std::string result;
        for (const unsigned char byte : value) {
            if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~')
                result += static_cast<char>(byte);
            else {
                result += '%';
                result += hex[byte >> 4];
                result += hex[byte & 15];
            }
        }
        return result;
    }

    HttpResponse httpGet(const std::string& url, const HttpLimits& limits, const HttpCancellation* cancellation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs);
        const auto remaining = [&] {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        };
        const auto wideUrl = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        std::array<wchar_t, 256> host{};
        std::wstring path(wideUrl.size() + 1, L'\0'), extra(wideUrl.size() + 1, L'\0');
        parts.lpszHostName = host.data();
        parts.dwHostNameLength = static_cast<DWORD>(host.size());
        parts.lpszUrlPath = path.data();
        parts.dwUrlPathLength = static_cast<DWORD>(path.size());
        parts.lpszExtraInfo = extra.data();
        parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
        if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) throw std::runtime_error("地址无效");
        path.resize(parts.dwUrlPathLength);
        extra.resize(parts.dwExtraInfoLength);

        Handle session{WinHttpOpen(L"Canton", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value) throw std::runtime_error("无法初始化 HTTP");
        Handle connection{WinHttpConnect(session.value, host.data(), parts.nPort, 0)};
        if (!connection.value) throw std::runtime_error("连接失败");
        // 路径已由调用方编码，原样发出
        Handle request{WinHttpOpenRequest(connection.value, L"GET", (path + extra).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY | (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0))};
        if (!request.value) throw std::runtime_error("连接失败");
        const auto state = cancellation ? cancellation->state() : nullptr;
        if (state) {
            const std::scoped_lock lock(state->mutex);
            if (state->cancelled) throw std::runtime_error("请求已取消");
            state->request = request.value;
        }

        // 取消方可能已经替我们关掉了句柄
        struct Unregister {
            std::shared_ptr<HttpCancellation::State> state;
            Handle& request;

            ~Unregister() {
                if (!state) return;
                const std::scoped_lock lock(state->mutex);
                if (state->request == request.value)
                    state->request = nullptr;
                else
                    request.value = nullptr;
            }
        } unregister{state, request};

        const auto fail = [&](const char* reason) -> std::runtime_error {
            if (state) {
                const std::scoped_lock lock(state->mutex);
                if (state->cancelled) return std::runtime_error("请求已取消");
            }
            return std::runtime_error(remaining() <= 0 || GetLastError() == ERROR_WINHTTP_TIMEOUT ? "请求超时" : reason);
        };
        const auto arm = [&] {
            const auto left = std::max(remaining(), 1);
            WinHttpSetTimeouts(request.value, left, left, left, left);
        };
        DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        arm();
        if (!WinHttpSendRequest(request.value, L"Cache-Control: no-cache\r\n", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) throw fail("连接失败");
        arm();
        if (!WinHttpReceiveResponse(request.value, nullptr)) throw fail("连接失败");

        HttpResponse response;
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) throw fail("响应头无效");
        response.status = static_cast<int>(status);
        const auto prefix = "HTTP " + std::to_string(response.status) + "，";
        std::array<wchar_t, 256> buffer{};
        size = sizeof(buffer);
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
            auto type = asciiLower(narrow(std::wstring_view(buffer.data(), size / sizeof(wchar_t))));
            type = type.substr(0, type.find(';'));
            while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.pop_back();
            response.contentType = type;
        }
        size = sizeof(buffer);
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) response.declaredLength = _wtoi64(buffer.data());
        if (response.declaredLength > static_cast<long long>(limits.maxBytes)) throw std::runtime_error(prefix + "响应超出资源字节预算");
        std::array<char, 8192> chunk{};
        while (true) {
            arm();
            DWORD read = 0;
            if (!WinHttpReadData(request.value, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) throw std::runtime_error(prefix + fail("响应读取失败").what());
            if (remaining() <= 0) throw std::runtime_error(prefix + "请求超时");
            if (read == 0) break;
            if (response.body.size() + read > limits.maxBytes) throw std::runtime_error(prefix + "响应超出资源字节预算");
            response.body.append(chunk.data(), read);
        }
        if (response.declaredLength >= 0 && response.body.size() != static_cast<std::size_t>(response.declaredLength)) throw std::runtime_error(prefix + "响应内容不完整");
        return response;
    }
}
