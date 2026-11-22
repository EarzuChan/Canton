#include <runtime/DevServerClient.h>

#include <charconv>

namespace canton::runtime {
    namespace {
        std::string trimCopy(std::string_view value) {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
            return std::string(value);
        }

    }

    DevServerEndpoint parseDevServerUrl(std::string_view url) {
        DevServerEndpoint endpoint;
        auto input = trimCopy(url);
        if (input.empty()) {
            endpoint.error = "Canton dev server URL is empty.";
            return endpoint;
        }

        const auto schemeSep = input.find("://");
        if (schemeSep == std::string::npos) {
            endpoint.error = "Canton dev server URL must include http://, https://, ws:// or wss://.";
            return endpoint;
        }

        endpoint.protocol = input.substr(0, schemeSep);
        endpoint.secure = endpoint.protocol == "https" || endpoint.protocol == "wss";
        if (endpoint.protocol != "http" && endpoint.protocol != "https" && endpoint.protocol != "ws" && endpoint.protocol != "wss") {
            endpoint.error = "Canton dev server URL has unsupported protocol: " + endpoint.protocol;
            return endpoint;
        }

        auto rest = input.substr(schemeSep + 3);
        const auto slash = rest.find('/');
        const auto hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
        endpoint.basePath = slash == std::string::npos ? "/" : rest.substr(slash);
        if (endpoint.basePath.empty()) endpoint.basePath = "/";
        if (endpoint.basePath.front() != '/') endpoint.basePath.insert(endpoint.basePath.begin(), '/');
        endpoint.websocketPath = endpoint.basePath;
        const auto query = endpoint.websocketPath.find('?');
        if (query != std::string::npos) endpoint.websocketPath.resize(query);
        if (endpoint.websocketPath.empty()) endpoint.websocketPath = "/";

        const auto colon = hostPort.rfind(':');
        if (colon == std::string::npos) {
            endpoint.host = hostPort;
            endpoint.port = endpoint.secure ? 443 : 80;
        } else {
            endpoint.host = hostPort.substr(0, colon);
            const auto portText = std::string_view(hostPort).substr(colon + 1);
            auto parsed = 0;
            const auto [ptr, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
            if (ec != std::errc{} || ptr != portText.data() + portText.size() || parsed <= 0 || parsed > 65535) {
                endpoint.error = "Canton dev server URL has invalid port: " + std::string(portText);
                return endpoint;
            }
            endpoint.port = parsed;
        }

        if (endpoint.host.empty()) {
            endpoint.error = "Canton dev server URL host is empty.";
            return endpoint;
        }

        endpoint.ok = true;
        return endpoint;
    }

}
