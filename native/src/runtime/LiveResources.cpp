#include <runtime/LiveResources.h>

#include <runtime/HttpClient.h>
#include <runtime/UiPackage.h>

#include <stdexcept>

namespace canton::runtime {
    namespace {
        struct HttpAddress {
            std::string origin;
            std::string path;
        };

        HttpAddress parseHttpAddress(const std::string& address) {
            const auto separator = address.find("://");
            if (separator == std::string::npos) throw std::runtime_error("Live Painter 地址必须使用 HTTP 或 HTTPS");
            const auto scheme = asciiLower(address.substr(0, separator));
            if (scheme != "http" && scheme != "https") throw std::runtime_error("Live Painter 地址必须使用 HTTP 或 HTTPS");
            const auto authorityEnd = address.find_first_of("/?#", separator + 3);
            auto authority = address.substr(separator + 3, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - separator - 3);
            if (authority.empty() || authority.find_first_of("@\\ \t\r\n%") != std::string::npos) throw std::runtime_error("Live Painter 服务 origin 无效");
            authority = asciiLower(authority);
            const std::string_view defaultPort = scheme == "https" ? ":443" : ":80";
            if (authority.ends_with(defaultPort)) authority.resize(authority.size() - defaultPort.size());
            auto path = authorityEnd == std::string::npos ? "/" : address.substr(authorityEnd);
            if (path.starts_with('?')) path.insert(0, "/");
            return {scheme + "://" + authority, std::move(path)};
        }

        int hexDigit(char value) {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }

        std::string percentDecode(std::string_view value) {
            std::string result;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] != '%')
                    result += value[index];
                else {
                    if (index + 2 >= value.size() || hexDigit(value[index + 1]) < 0 || hexDigit(value[index + 2]) < 0) throw std::runtime_error("Live Painter 资源含有无效的百分号编码");
                    result += static_cast<char>((hexDigit(value[index + 1]) << 4) | hexDigit(value[index + 2]));
                    index += 2;
                }
            }
            return result;
        }

        // 把资源引用换成 DevServer 上的完整地址；越出同源或站点根的一律拒绝，不发请求
        std::string liveResourceAddress(const HttpAddress& server, const std::string& resource) {
            if (resource.size() > 8192) throw std::runtime_error("Live Painter 资源地址过长");
            if (resource.empty() || resource.starts_with("//") || resource.find_first_of("\\#\r\n\t") != std::string::npos) throw std::runtime_error("Live Painter 资源地址无效：" + resource);
            std::string path;
            if (resource.find("://") != std::string::npos) {
                const auto absolute = parseHttpAddress(resource);
                if (absolute.origin != server.origin) throw std::runtime_error("Live Painter 禁止跨 origin 资源：" + resource);
                path = absolute.path;
            } else {
                if (resource.find(':') != std::string::npos) throw std::runtime_error("Live Painter 禁止文件路径或非 HTTP 资源：" + resource);
                path = resource.starts_with('/') ? resource : "/" + resource;
            }
            const auto decoded = percentDecode(path.substr(0, path.find('?')));
            if (decoded.find_first_of("\\:") != std::string::npos || decoded.starts_with("//")) throw std::runtime_error("Live Painter 禁止文件路径：" + resource);
            for (const auto c : decoded)
                if (static_cast<unsigned char>(c) < 32 || c == 127) throw std::runtime_error("Live Painter 资源路径含有控制字符");
            for (std::size_t start = 0; start <= decoded.size();) {
                const auto end = decoded.find('/', start);
                if (decoded.substr(start, end == std::string::npos ? std::string::npos : end - start) == "..") throw std::runtime_error("Live Painter 资源路径禁止越界：" + resource);
                if (end == std::string::npos) break;
                start = end + 1;
            }
            std::string escaped;
            constexpr std::string_view digits = "0123456789ABCDEF";
            for (const auto value : path) {
                const auto byte = static_cast<unsigned char>(value);
                if (byte >= 128 || byte == ' ') {
                    escaped += '%';
                    escaped += digits[byte >> 4];
                    escaped += digits[byte & 15];
                } else
                    escaped += value;
            }
            return server.origin + escaped;
        }

        ResourcePayload httpPayload(const std::string& address, const PainterResourceLimits& limits, bool painter) {
            HttpResponse response;
            try {
                response = httpGet(address, {limits.maxBytes, limits.timeoutMs});
            } catch (const std::exception& error) {
                throw std::runtime_error("Live 资产读取失败：" + address + "：" + error.what());
            }
            const auto fail = [&](const std::string& reason) {
                return std::runtime_error("Live 资产读取失败：" + address + "：" + reason);
            };
            if (response.status != 200) throw fail("HTTP " + std::to_string(response.status));
            const auto& mime = response.contentType;
            if (painter && !mime.empty() && !mime.starts_with("image/") && !isFontMime(mime) && mime != "application/octet-stream") throw fail("HTTP 200，资源 MIME 不支持：" + mime);
            ResourcePayload result;
            result.source = address + "（HTTP 200）";
            const auto path = asciiLower(address.substr(0, address.find('?')));
            result.svg = mime == "image/svg+xml" || path.ends_with(".svg");
            result.font = isFontMime(mime) || isFontPath(path);
            result.bytes = std::move(response.body);
            return result;
        }
    }

    core::PainterLoader livePainterLoader(std::string devServerUrl, PainterResourceLimits limits, std::shared_ptr<ResourceWorkers> workers) {
        if (limits.maxBytes == 0 || limits.maxBytes > PainterResourceLimits::MaxResourceBytes || limits.timeoutMs <= 0 || limits.timeoutMs > PainterResourceLimits::MaxTimeoutMs) throw std::invalid_argument("Live Painter 资源限制无效");
        const auto server = parseHttpAddress(devServerUrl);
        return asynchronousPainterLoader(std::move(workers), [server, limits](const std::string& resource) {
            if (resource.starts_with("data:")) return dataUrlPayload(resource, limits);
            return httpPayload(liveResourceAddress(server, resource), limits, true);
        });
    }

    core::AssetLoader liveAssetLoader(std::string devServerUrl, std::shared_ptr<ResourceWorkers> workers) {
        const auto server = parseHttpAddress(devServerUrl);
        return asynchronousAssetLoader(std::move(workers), [server](const std::string& reference) {
            const auto name = packageAssetName(reference);
            std::string path = "/";
            constexpr std::string_view hex = "0123456789ABCDEF";
            for (const unsigned char byte : name) {
                if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || std::string_view("-._~/").find(static_cast<char>(byte)) != std::string_view::npos)
                    path += static_cast<char>(byte);
                else {
                    path += '%';
                    path += hex[byte >> 4];
                    path += hex[byte & 15];
                }
            }
            return httpPayload(liveResourceAddress(server, path), {}, false).bytes;
        });
    }

    core::AssetExistsChecker liveAssetExistsChecker(std::string devServerUrl, std::shared_ptr<ResourceWorkers> workers) {
        const auto server = parseHttpAddress(devServerUrl);
        return asynchronousAssetExistsChecker(std::move(workers), [server](const std::string& reference) {
            const auto name = packageAssetName(reference);
            std::string path = "/";
            constexpr std::string_view hex = "0123456789ABCDEF";
            for (const unsigned char byte : name) {
                if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || std::string_view("-._~/").find(static_cast<char>(byte)) != std::string_view::npos)
                    path += static_cast<char>(byte);
                else {
                    path += '%';
                    path += hex[byte >> 4];
                    path += hex[byte & 15];
                }
            }
            const auto address = liveResourceAddress(server, path);
            try {
                HttpRequestOptions req;
                req.method = "HEAD";
                req.url = address;
                req.headers = {{"Cache-Control", "no-cache"}};
                req.timeoutMs = PainterResourceLimits::MaxTimeoutMs;
                req.maxBytes = 1024;
                req.followRedirects = false;
                req.useSystemProxy = false;
                const auto resp = httpRequest(req);
                if (resp.status == 200) return true;
                if (resp.status == 405) {
                    const auto getResp = httpGet(address, {PainterResourceLimits::MaxResourceBytes, PainterResourceLimits::MaxTimeoutMs});
                    return getResp.status == 200;
                }
                return false;
            } catch (...) {
                return false;
            }
        });
    }
}
