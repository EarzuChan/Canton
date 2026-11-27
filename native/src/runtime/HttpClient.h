#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canton::runtime {

    enum class NetErrorCode : std::uint8_t {
        InvalidUrl,
        Network,
        Timeout,
        Aborted,
        ResponseTooLarge,
        Io,
    };

    [[nodiscard]] const char* netErrorCodeName(NetErrorCode code) noexcept;

    class NetError final : public std::runtime_error {
       public:
        NetError(NetErrorCode code, std::string url, std::string message)
            : std::runtime_error(std::move(message)), code_(code), url_(std::move(url)) {}

        [[nodiscard]] NetErrorCode code() const noexcept { return code_; }
        [[nodiscard]] const std::string& url() const noexcept { return url_; }

       private:
        NetErrorCode code_;
        std::string url_;
    };

    struct HttpResponse {
        int status = 0;
        // 最终 URL（重定向后）
        std::string url;
        // 小写 key；同名多头以 ", " 合并
        std::map<std::string, std::string> headers;
        // 小写、不含参数的 Content-Type
        std::string contentType;
        // 响应头声明的长度，没有声明时为 -1
        long long declaredLength = -1;
        std::string body;
    };

    struct HttpLimits {
        std::size_t maxBytes = 64 * 1024 * 1024;
        // 覆盖连接、发送与读取的总时限；0 表示不限
        int timeoutMs = 5000;
    };

    struct HttpRequestOptions {
        std::string method = "GET";
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        // 总时限毫秒；0 表示不限
        int timeoutMs = 15000;
        // 响应体字节上限；0 表示不限
        std::size_t maxBytes = 32 * 1024 * 1024;
        bool followRedirects = true;
        int maxRedirects = 10;
        bool useSystemProxy = true;
    };

    struct DownloadProgress {
        std::uint64_t receivedBytes = 0;
        std::optional<std::uint64_t> totalBytes;
    };

    struct DownloadResult {
        int status = 0;
        bool ok = false;
        std::string url;
        std::string destPath;
        std::uint64_t bytesWritten = 0;
    };

    // 取消句柄：另一个线程调用 cancel 后，进行中的请求立刻失败，之后发起的请求也直接失败
    class HttpCancellation {
       public:
        HttpCancellation();
        ~HttpCancellation();
        void cancel() noexcept;
        [[nodiscard]] bool isCancelled() const noexcept;

        struct State;

        [[nodiscard]] const std::shared_ptr<State>& state() const noexcept {
            return state_;
        }

       private:
        std::shared_ptr<State> state_;
    };

    // 同步 GET（Live/DevServer 内部使用），不跟随重定向、不走系统代理
    [[nodiscard]] HttpResponse httpGet(const std::string& url, const HttpLimits& limits = {}, const HttpCancellation* cancellation = nullptr);

    // 完整同步 HTTP 请求（供后台 IoThreadPool 与 M7 Native SDK 使用），失败抛 NetError
    [[nodiscard]] HttpResponse httpRequest(const HttpRequestOptions& options, const HttpCancellation* cancellation = nullptr);

    // 完整流式直落盘下载（不经 JS 内存；写同目录临时文件 + 节流进度 + 2xx 原子替换），失败抛 NetError
    [[nodiscard]] DownloadResult httpDownloadFile(
        const HttpRequestOptions& options,
        const std::string& destPath,
        const std::function<void(const DownloadProgress&)>& onProgress = {},
        const HttpCancellation* cancellation = nullptr);

    // 查询参数值的百分号编码
    [[nodiscard]] std::string encodeQueryValue(std::string_view value);

}
