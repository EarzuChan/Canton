#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace canton::runtime {
    struct HttpResponse {
        int status = 0;
        // 小写、不含参数的 Content-Type
        std::string contentType;
        // 响应头声明的长度，没有声明时为 -1
        long long declaredLength = -1;
        std::string body;
    };

    struct HttpLimits {
        std::size_t maxBytes = 64 * 1024 * 1024;
        // 覆盖连接、发送与读取的总时限
        int timeoutMs = 5000;
    };

    // 取消句柄：另一个线程调用 cancel 后，进行中的请求立刻失败，之后发起的请求也直接失败
    class HttpCancellation {
       public:
        HttpCancellation();
        ~HttpCancellation();
        void cancel() noexcept;

        struct State;

        [[nodiscard]] const std::shared_ptr<State>& state() const noexcept {
            return state_;
        }

       private:
        std::shared_ptr<State> state_;
    };

    // 同步 GET，不跟随重定向、不走系统代理。失败抛 std::runtime_error，原因是“连接失败”“请求超时”等短语
    [[nodiscard]] HttpResponse httpGet(const std::string& url, const HttpLimits& limits = {}, const HttpCancellation* cancellation = nullptr);

    // 查询参数值的百分号编码
    [[nodiscard]] std::string encodeQueryValue(std::string_view value);
}
