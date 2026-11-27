#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <string>
#include <string_view>

namespace canton::core {
    enum class UiEntryKind { Module, Asset };
    enum class UiScriptFormat { Js, Bytecode };

    class UiPackage {
       public:
        virtual ~UiPackage() = default;
        virtual UiScriptFormat scriptFormat() const noexcept = 0;
        virtual std::string_view entry() const noexcept = 0;
        virtual std::string read(std::string_view path, UiEntryKind kind, std::size_t limit) const = 0;
        virtual bool exists(std::string_view path, UiEntryKind kind) const noexcept {
            try {
                (void)read(path, kind, 1);
                return true;
            } catch (...) {
                return false;
            }
        }
        virtual std::string_view appId() const noexcept { return {}; }
        virtual bool singleInstance() const noexcept { return false; }
        virtual bool portable() const noexcept { return false; }
    };

    inline constexpr std::size_t UiEntryLimit = 64 * 1024 * 1024;
    using AssetLoader = std::function<std::future<std::string>(std::string, std::function<void()>)>;
    using AssetExistsChecker = std::function<std::future<bool>(std::string, std::function<void()>)>;
}
