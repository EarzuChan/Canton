#pragma once

#include <core/GraphicsEffects.h>

#include <string_view>

namespace canton::d3d11 {
    // 基于系统 d3dcompiler_47.dll 的运行期 HLSL 编译器与 DXBC 反射器（按需懒加载）
    class D3DShaderCompiler final : public core::ShaderCompiler {
       public:
        static std::string_view standardHeader() noexcept;

        [[nodiscard]] core::ShaderCompileOutput compileHlsl(std::string_view source, std::string_view entryPoint, std::string_view debugName) const override;
        [[nodiscard]] core::ShaderReflection reflectDxbc(std::span<const std::uint8_t> dxbc, std::string_view debugName) const override;
    };
}
