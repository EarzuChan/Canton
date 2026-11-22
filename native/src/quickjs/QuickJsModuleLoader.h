#pragma once

#include <memory>
#include <core/UiPackage.h>

extern "C" {
#include <quickjs.h>
}

namespace canton::quickjs {
    class QuickJsModuleLoader {
       public:
        void install(std::shared_ptr<const core::UiPackage> package) {
            package_ = std::move(package);
        }

        void clear() noexcept {
            package_.reset();
        }

        static char* normalize(JSContext* context, const char* moduleBaseName, const char* moduleName, void* opaque);
        static JSModuleDef* load(JSContext* context, const char* moduleName, void* opaque);

       private:
        std::shared_ptr<const core::UiPackage> package_;
    };
}

