#pragma once

#include <string>
#include <string_view>


namespace canton::runtime {
    struct DevServerEndpoint {
        bool ok = false;
        bool secure = false;
        std::string protocol;
        std::string host;
        int port = 0;
        std::string basePath = "/";
        std::string websocketPath = "/";
        std::string error;
    };

    DevServerEndpoint parseDevServerUrl(std::string_view url);

}
