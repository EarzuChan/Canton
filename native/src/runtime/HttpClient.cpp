#include <runtime/HttpClient.h>

#include <runtime/AppPaths.h>
#include <runtime/FileSystem.h>
#include <runtime/PainterResources.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace canton::runtime {

    const char* netErrorCodeName(NetErrorCode code) noexcept {
        switch (code) {
            case NetErrorCode::InvalidUrl: return "INVALID_URL";
            case NetErrorCode::Network: return "NETWORK";
            case NetErrorCode::Timeout: return "TIMEOUT";
            case NetErrorCode::Aborted: return "ABORTED";
            case NetErrorCode::ResponseTooLarge: return "RESPONSE_TOO_LARGE";
            case NetErrorCode::Io: return "IO";
        }
        return "NETWORK";
    }

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

    bool HttpCancellation::isCancelled() const noexcept {
        const std::scoped_lock lock(state_->mutex);
        return state_->cancelled;
    }

    namespace {
        struct Handle {
            HINTERNET value = nullptr;
            ~Handle() {
                if (value) WinHttpCloseHandle(value);
            }
        };

        struct FileHandle {
            HANDLE h = INVALID_HANDLE_VALUE;
            ~FileHandle() {
                if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
            }
        };

        std::wstring widen(std::string_view text) {
            if (text.empty()) return {};
            const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (size <= 0) return {};
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        std::string narrow(std::wstring_view text) {
            if (text.empty()) return {};
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (size <= 0) return {};
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        struct ParsedUrl {
            INTERNET_SCHEME scheme = INTERNET_SCHEME_HTTP;
            std::wstring host;
            INTERNET_PORT port = 80;
            std::wstring pathAndQuery;
            std::string originKey;
        };

        std::optional<ParsedUrl> crackHttpUrl(const std::string& url) {
            const auto wideUrl = widen(url);
            if (wideUrl.empty()) return std::nullopt;
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            std::wstring host(wideUrl.size() + 1, L'\0');
            std::wstring path(wideUrl.size() + 1, L'\0');
            std::wstring extra(wideUrl.size() + 1, L'\0');
            parts.lpszHostName = host.data();
            parts.dwHostNameLength = static_cast<DWORD>(host.size());
            parts.lpszUrlPath = path.data();
            parts.dwUrlPathLength = static_cast<DWORD>(path.size());
            parts.lpszExtraInfo = extra.data();
            parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
            if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) return std::nullopt;
            if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
            host.resize(parts.dwHostNameLength);
            path.resize(parts.dwUrlPathLength);
            extra.resize(parts.dwExtraInfoLength);
            if (host.empty()) return std::nullopt;
            ParsedUrl out;
            out.scheme = parts.nScheme;
            out.host = std::move(host);
            out.port = parts.nPort;
            out.pathAndQuery = (path.empty() ? L"/" : path) + extra;
            out.originKey = (out.scheme == INTERNET_SCHEME_HTTPS ? "https://" : "http://") + asciiLower(narrow(out.host)) + ":" + std::to_string(out.port);
            return out;
        }

        std::string resolveRedirectUrl(const ParsedUrl& base, std::string_view location) {
            // 去除首尾空白
            while (!location.empty() && (location.front() == ' ' || location.front() == '\t')) location.remove_prefix(1);
            while (!location.empty() && (location.back() == ' ' || location.back() == '\t' || location.back() == '\r' || location.back() == '\n')) location.remove_suffix(1);
            if (location.empty()) return {};
            if (location.find("://") != std::string_view::npos) return std::string(location);
            const std::string scheme = base.scheme == INTERNET_SCHEME_HTTPS ? "https:" : "http:";
            if (location.starts_with("//")) return scheme + std::string(location);
            const bool defaultPort = (base.scheme == INTERNET_SCHEME_HTTP && base.port == 80) || (base.scheme == INTERNET_SCHEME_HTTPS && base.port == 443);
            std::string hostPort = narrow(base.host);
            if (!defaultPort) hostPort += ":" + std::to_string(base.port);
            if (location.starts_with('/')) {
                return scheme + "//" + hostPort + std::string(location);
            }
            // 相对路径
            std::string basePath = narrow(base.pathAndQuery);
            const auto qpos = basePath.find('?');
            if (qpos != std::string::npos) basePath.resize(qpos);
            const auto slash = basePath.rfind('/');
            if (slash != std::string::npos)
                basePath.resize(slash + 1);
            else
                basePath = "/";
            return scheme + "//" + hostPort + basePath + std::string(location);
        }

        std::map<std::string, std::string> parseRawHeaders(HINTERNET request) {
            std::map<std::string, std::string> headers;
            DWORD size = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
            if (size == 0) return headers;
            std::wstring raw(size / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
                return headers;
            }
            const auto utf8 = narrow(std::wstring_view(raw.data(), size / sizeof(wchar_t)));
            std::size_t pos = 0;
            bool firstLine = true;
            while (pos < utf8.size()) {
                const auto eol = utf8.find("\r\n", pos);
                const std::string_view line = (eol == std::string::npos) ? std::string_view(utf8).substr(pos) : std::string_view(utf8).substr(pos, eol - pos);
                pos = (eol == std::string::npos) ? utf8.size() : eol + 2;
                if (firstLine) {
                    firstLine = false;
                    continue;
                }
                if (line.empty()) continue;
                const auto colon = line.find(':');
                if (colon == std::string_view::npos) continue;
                auto key = asciiLower(line.substr(0, colon));
                auto val = line.substr(colon + 1);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
                if (key.empty()) continue;
                if (auto it = headers.find(key); it != headers.end()) {
                    it->second += ", ";
                    it->second.append(val);
                } else {
                    headers.emplace(std::move(key), std::string(val));
                }
            }
            return headers;
        }

        bool isRedirectStatus(int status) noexcept {
            return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        }

        struct CoreHttpResult {
            int status = 0;
            std::string finalUrl;
            std::map<std::string, std::string> headers;
            std::string contentType;
            long long declaredLength = -1;
            std::string body;
            std::uint64_t bytesWritten = 0;
        };

        CoreHttpResult executeHttpCore(
            const HttpRequestOptions& options,
            const HttpCancellation* cancellation,
            const std::string* downloadDestPath,
            const std::function<void(const DownloadProgress&)>& onProgress) {
            const auto state = cancellation ? cancellation->state() : nullptr;
            if (state) {
                const std::scoped_lock lock(state->mutex);
                if (state->cancelled) throw NetError(NetErrorCode::Aborted, options.url, "请求已取消");
            }

            std::string currentUrl = options.url;
            std::string currentMethod = options.method.empty() ? "GET" : options.method;
            std::vector<std::pair<std::string, std::string>> currentHeaders = options.headers;
            std::string currentBody = options.body;

            const bool hasTimeout = options.timeoutMs > 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hasTimeout ? options.timeoutMs : 0);
            const auto remainingMs = [&]() -> int {
                if (!hasTimeout) return 0;
                return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            };

            const DWORD accessType = options.useSystemProxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
            Handle session{WinHttpOpen(L"Canton", accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
            if (!session.value && options.useSystemProxy) {
                session.value = WinHttpOpen(L"Canton", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            }
            if (!session.value) throw NetError(NetErrorCode::Network, currentUrl, "无法初始化 WinHTTP 会话");

            int redirects = 0;
            while (true) {
                const auto parsed = crackHttpUrl(currentUrl);
                if (!parsed) throw NetError(NetErrorCode::InvalidUrl, currentUrl, "无效的 HTTP/HTTPS URL：" + currentUrl);

                Handle connection{WinHttpConnect(session.value, parsed->host.c_str(), parsed->port, 0)};
                if (!connection.value) throw NetError(NetErrorCode::Network, currentUrl, "无法建立连接");

                const auto wideMethod = widen(currentMethod);
                const DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY | (parsed->scheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
                Handle request{WinHttpOpenRequest(connection.value, wideMethod.c_str(), parsed->pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
                if (!request.value) throw NetError(NetErrorCode::Network, currentUrl, "无法创建 HTTP 请求");

                if (state) {
                    const std::scoped_lock lock(state->mutex);
                    if (state->cancelled) throw NetError(NetErrorCode::Aborted, currentUrl, "请求已取消");
                    state->request = request.value;
                }

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

                const auto makeNetError = [&](const char* reason) -> NetError {
                    if (state) {
                        const std::scoped_lock lock(state->mutex);
                        if (state->cancelled) return NetError(NetErrorCode::Aborted, currentUrl, "请求已取消");
                    }
                    const DWORD err = GetLastError();
                    if ((hasTimeout && remainingMs() <= 0) || err == ERROR_WINHTTP_TIMEOUT) {
                        return NetError(NetErrorCode::Timeout, currentUrl, "请求超时");
                    }
                    return NetError(NetErrorCode::Network, currentUrl, std::string(reason) + "（WinHTTP 错误 " + std::to_string(err) + "）");
                };

                const auto armTimeouts = [&] {
                    if (!hasTimeout) {
                        WinHttpSetTimeouts(request.value, 0, 0, 0, 0);
                        return;
                    }
                    const int left = remainingMs();
                    if (left <= 0) throw NetError(NetErrorCode::Timeout, currentUrl, "请求超时");
                    WinHttpSetTimeouts(request.value, left, left, left, left);
                };

                // 手动处理重定向以便精确控制上限、方法变换与跨域剥离敏感头
                DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
                WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

                std::wstring headerBlock;
                for (const auto& [k, v] : currentHeaders) {
                    headerBlock += widen(k);
                    headerBlock += L": ";
                    headerBlock += widen(v);
                    headerBlock += L"\r\n";
                }

                armTimeouts();
                const DWORD bodySize = static_cast<DWORD>(currentBody.size());
                if (!WinHttpSendRequest(
                        request.value,
                        headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                        headerBlock.empty() ? 0 : static_cast<DWORD>(headerBlock.size()),
                        bodySize > 0 ? const_cast<char*>(currentBody.data()) : WINHTTP_NO_REQUEST_DATA,
                        bodySize,
                        bodySize,
                        0)) {
                    throw makeNetError("发送 HTTP 请求失败");
                }

                armTimeouts();
                if (!WinHttpReceiveResponse(request.value, nullptr)) {
                    throw makeNetError("接收 HTTP 响应失败");
                }

                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
                    throw makeNetError("读取响应状态码失败");
                }

                auto headers = parseRawHeaders(request.value);
                if (options.followRedirects && isRedirectStatus(static_cast<int>(status))) {
                    const auto locIt = headers.find("location");
                    if (locIt != headers.end() && !locIt->second.empty()) {
                        if (redirects >= options.maxRedirects) {
                            throw NetError(NetErrorCode::Network, currentUrl, "重定向次数超过上限（" + std::to_string(options.maxRedirects) + "）");
                        }
                        const auto nextUrl = resolveRedirectUrl(*parsed, locIt->second);
                        const auto nextParsed = crackHttpUrl(nextUrl);
                        if (!nextParsed) throw NetError(NetErrorCode::InvalidUrl, nextUrl, "重定向目标 URL 无效：" + nextUrl);

                        // 跨域重定向自动剥离 Authorization 与 Cookie
                        if (nextParsed->originKey != parsed->originKey) {
                            std::erase_if(currentHeaders, [](const auto& pair) {
                                const auto lower = asciiLower(pair.first);
                                return lower == "authorization" || lower == "cookie";
                            });
                        }
                        // 301/302/303 重定向改为 GET 并清空请求体
                        if (status == 303 || ((status == 301 || status == 302) && currentMethod == "POST")) {
                            currentMethod = "GET";
                            currentBody.clear();
                            std::erase_if(currentHeaders, [](const auto& pair) {
                                const auto lower = asciiLower(pair.first);
                                return lower == "content-length" || lower == "content-type";
                            });
                        }
                        currentUrl = nextUrl;
                        ++redirects;
                        continue;
                    }
                }

                CoreHttpResult result;
                result.status = static_cast<int>(status);
                result.finalUrl = currentUrl;
                result.headers = std::move(headers);
                if (const auto ct = result.headers.find("content-type"); ct != result.headers.end()) {
                    auto type = asciiLower(ct->second);
                    type = type.substr(0, type.find(';'));
                    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.pop_back();
                    result.contentType = std::move(type);
                }
                if (const auto cl = result.headers.find("content-length"); cl != result.headers.end()) {
                    try {
                        result.declaredLength = std::stoll(cl->second);
                    } catch (...) {
                        result.declaredLength = -1;
                    }
                }

                if (options.maxBytes > 0 && result.declaredLength > static_cast<long long>(options.maxBytes)) {
                    throw NetError(NetErrorCode::ResponseTooLarge, currentUrl, "响应体超出 maxBytes 上限");
                }

                const bool isHead = asciiLower(currentMethod) == "head";
                if (downloadDestPath) {
                    const bool ok = result.status >= 200 && result.status < 300;
                    if (!ok) {
                        // 非 2xx 不写入目标，不覆盖原文件
                        result.bytesWritten = 0;
                        return result;
                    }
                    std::wstring targetLong;
                    try {
                        targetLong = AppPaths::toWin32LongPath(std::string_view(*downloadDestPath));
                    } catch (const std::exception& error) {
                        throw NetError(NetErrorCode::Io, currentUrl, error.what());
                    }
                    const std::wstring tempLong = fsMakeSiblingTempPath(targetLong);
                    FileHandle outFile{CreateFileW(tempLong.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                    if (outFile.h == INVALID_HANDLE_VALUE) {
                        throw NetError(NetErrorCode::Io, currentUrl, "无法创建下载临时文件：" + *downloadDestPath);
                    }

                    const auto cleanupTemp = [&] {
                        if (outFile.h != INVALID_HANDLE_VALUE) {
                            CloseHandle(outFile.h);
                            outFile.h = INVALID_HANDLE_VALUE;
                        }
                        DeleteFileW(tempLong.c_str());
                    };

                    const std::optional<std::uint64_t> totalBytes = result.declaredLength >= 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(result.declaredLength)) : std::nullopt;
                    auto lastProgressEmit = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
                    std::uint64_t received = 0;
                    std::array<char, 16384> chunk{};
                    try {
                        while (!isHead) {
                            armTimeouts();
                            DWORD read = 0;
                            if (!WinHttpReadData(request.value, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                                throw makeNetError("下载流读取失败");
                            }
                            if (hasTimeout && remainingMs() <= 0) throw NetError(NetErrorCode::Timeout, currentUrl, "下载超时");
                            if (read == 0) break;
                            received += read;
                            if (options.maxBytes > 0 && received > options.maxBytes) {
                                throw NetError(NetErrorCode::ResponseTooLarge, currentUrl, "下载体积超出 maxBytes 上限");
                            }
                            DWORD written = 0;
                            if (!WriteFile(outFile.h, chunk.data(), read, &written, nullptr) || written != read) {
                                throw NetError(NetErrorCode::Io, currentUrl, "写入下载临时文件失败：" + *downloadDestPath);
                            }
                            if (onProgress) {
                                const auto now = std::chrono::steady_clock::now();
                                if (now - lastProgressEmit >= std::chrono::milliseconds(60)) {
                                    lastProgressEmit = now;
                                    onProgress(DownloadProgress{received, totalBytes});
                                }
                            }
                        }
                        if (result.declaredLength >= 0 && received != static_cast<std::uint64_t>(result.declaredLength)) {
                            throw NetError(NetErrorCode::Network, currentUrl, "下载内容不完整");
                        }
                        if (!FlushFileBuffers(outFile.h)) {
                            throw NetError(NetErrorCode::Io, currentUrl, "下载文件刷盘失败：" + *downloadDestPath);
                        }
                        CloseHandle(outFile.h);
                        outFile.h = INVALID_HANDLE_VALUE;
                        try {
                            fsAtomicReplaceFile(tempLong, targetLong, *downloadDestPath);
                        } catch (const std::exception& error) {
                            throw NetError(NetErrorCode::Io, currentUrl, error.what());
                        }
                        if (onProgress) onProgress(DownloadProgress{received, totalBytes});
                        result.bytesWritten = received;
                        return result;
                    } catch (...) {
                        cleanupTemp();
                        throw;
                    }
                }

                // 内存响应模式
                if (!isHead) {
                    std::array<char, 8192> chunk{};
                    while (true) {
                        armTimeouts();
                        DWORD read = 0;
                        if (!WinHttpReadData(request.value, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                            throw makeNetError("响应读取失败");
                        }
                        if (hasTimeout && remainingMs() <= 0) throw NetError(NetErrorCode::Timeout, currentUrl, "请求超时");
                        if (read == 0) break;
                        if (options.maxBytes > 0 && result.body.size() + read > options.maxBytes) {
                            throw NetError(NetErrorCode::ResponseTooLarge, currentUrl, "响应超出资源字节预算");
                        }
                        result.body.append(chunk.data(), read);
                    }
                    if (result.declaredLength >= 0 && result.body.size() != static_cast<std::size_t>(result.declaredLength)) {
                        throw NetError(NetErrorCode::Network, currentUrl, "响应内容不完整");
                    }
                }
                return result;
            }
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
        HttpRequestOptions opts;
        opts.method = "GET";
        opts.url = url;
        opts.headers = {{"Cache-Control", "no-cache"}};
        opts.timeoutMs = limits.timeoutMs;
        opts.maxBytes = limits.maxBytes;
        opts.followRedirects = false;
        opts.useSystemProxy = false;
        try {
            return httpRequest(opts, cancellation);
        } catch (const NetError& err) {
            if (err.code() == NetErrorCode::InvalidUrl) throw std::runtime_error("地址无效");
            if (err.code() == NetErrorCode::Aborted) throw std::runtime_error("请求已取消");
            if (err.code() == NetErrorCode::Timeout) throw std::runtime_error("请求超时");
            if (err.code() == NetErrorCode::ResponseTooLarge) throw std::runtime_error("响应超出资源字节预算");
            throw std::runtime_error(err.what());
        }
    }

    HttpResponse httpRequest(const HttpRequestOptions& options, const HttpCancellation* cancellation) {
        auto core = executeHttpCore(options, cancellation, nullptr, {});
        HttpResponse response;
        response.status = core.status;
        response.url = std::move(core.finalUrl);
        response.headers = std::move(core.headers);
        response.contentType = std::move(core.contentType);
        response.declaredLength = core.declaredLength;
        response.body = std::move(core.body);
        return response;
    }

    DownloadResult httpDownloadFile(
        const HttpRequestOptions& options,
        const std::string& destPath,
        const std::function<void(const DownloadProgress&)>& onProgress,
        const HttpCancellation* cancellation) {
        auto core = executeHttpCore(options, cancellation, &destPath, onProgress);
        DownloadResult out;
        out.status = core.status;
        out.ok = core.status >= 200 && core.status < 300;
        out.url = std::move(core.finalUrl);
        out.destPath = destPath;
        out.bytesWritten = core.bytesWritten;
        return out;
    }

}
