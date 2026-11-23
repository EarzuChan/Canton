#include <d3d11/ShaderCompiler.h>

#include <d3d11/Com.h>

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace canton::d3d11 {
    namespace {
        constexpr std::string_view kCantonHlslHeader = R"hlsl(#ifndef CANTON_HLSL_INCLUDED
#define CANTON_HLSL_INCLUDED

struct CantonInput {
    float4 position : SV_Position;
    float2 fragCoord : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

cbuffer Canton : register(b12) {
    float2 cantonResolution;
    float2 cantonInvResolution;
};

SamplerState cantonLinearClamp  : register(s12);
SamplerState cantonLinearWrap   : register(s13);
SamplerState cantonLinearMirror : register(s14);
SamplerState cantonLinearDecal  : register(s15);
SamplerState cantonPointClamp   : register(s11);

#endif
)hlsl";

        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        using D3DReflectFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);

        struct CompilerLibrary {
            HMODULE module = nullptr;
            D3DCompileFn compile = nullptr;
            D3DReflectFn reflect = nullptr;

            CompilerLibrary() {
                module = LoadLibraryW(L"d3dcompiler_47.dll");
                if (!module) return;
                compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile"));
                reflect = reinterpret_cast<D3DReflectFn>(GetProcAddress(module, "D3DReflect"));
            }
        };

        const CompilerLibrary& compilerLibrary() {
            static const CompilerLibrary lib;
            if (!lib.module || !lib.compile || !lib.reflect) {
                throw std::runtime_error("无法加载系统 d3dcompiler_47.dll，请确认 Windows 图形运行环境完整");
            }
            return lib;
        }

        class CantonIncludeHandler final : public ID3DInclude {
           public:
            HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID* ppData, UINT* pBytes) override {
                if (!pFileName || !ppData || !pBytes) return E_INVALIDARG;
                std::string_view name(pFileName);
                while (!name.empty() && (name.front() == '/' || name.front() == '\\')) name.remove_prefix(1);
                if (name == "canton.hlsl") {
                    *ppData = kCantonHlslHeader.data();
                    *pBytes = static_cast<UINT>(kCantonHlslHeader.size());
                    return S_OK;
                }
                return E_FAIL;
            }

            HRESULT __stdcall Close(LPCVOID) override {
                return S_OK;
            }
        };

        bool isBuiltinSamplerName(std::string_view name) noexcept {
            return name == "cantonPointClamp" || name == "cantonLinearClamp" || name == "cantonLinearWrap" || name == "cantonLinearMirror" || name == "cantonLinearDecal";
        }

        core::ShaderScalarType toScalarType(D3D_SHADER_VARIABLE_TYPE type) {
            switch (type) {
                case D3D_SVT_FLOAT:
                    return core::ShaderScalarType::Float;
                case D3D_SVT_INT:
                    return core::ShaderScalarType::Int;
                case D3D_SVT_UINT:
                    return core::ShaderScalarType::UInt;
                case D3D_SVT_BOOL:
                    return core::ShaderScalarType::Bool;
                default:
                    throw std::runtime_error("着色器常量缓冲包含暂不支持的标量类型");
            }
        }
    }

    std::string_view D3DShaderCompiler::standardHeader() noexcept {
        return kCantonHlslHeader;
    }

    core::ShaderCompileOutput D3DShaderCompiler::compileHlsl(std::string_view source, std::string_view entryPoint, std::string_view debugName) const {
        if (source.empty()) throw std::invalid_argument("HLSL 源码不能为空");
        const auto& lib = compilerLibrary();
        const std::string entry = entryPoint.empty() ? std::string("main") : std::string(entryPoint);
        const std::string name = debugName.empty() ? std::string("inline.hlsl") : std::string(debugName);

        CantonIncludeHandler includeHandler;
        ComPtr<ID3DBlob> codeBlob;
        ComPtr<ID3DBlob> errorBlob;
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
        const HRESULT hr = lib.compile(source.data(), source.size(), name.c_str(), nullptr, &includeHandler, entry.c_str(), "ps_5_0", flags, 0, codeBlob.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr) || !codeBlob) {
            std::string diagnostic;
            if (errorBlob && errorBlob->GetBufferPointer() && errorBlob->GetBufferSize() > 0) {
                diagnostic.assign(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
                while (!diagnostic.empty() && (diagnostic.back() == '\0' || diagnostic.back() == '\r' || diagnostic.back() == '\n')) diagnostic.pop_back();
            }
            if (diagnostic.empty()) {
                diagnostic = "D3DCompile 返回错误码 0x" + std::to_string(static_cast<unsigned long>(hr));
            }
            throw std::runtime_error("HLSL 编译失败（" + name + "）：\n" + diagnostic);
        }

        core::ShaderCompileOutput output;
        output.dxbc.resize(codeBlob->GetBufferSize());
        std::memcpy(output.dxbc.data(), codeBlob->GetBufferPointer(), output.dxbc.size());
        output.reflection = reflectDxbc(output.dxbc, name);
        return output;
    }

    core::ShaderReflection D3DShaderCompiler::reflectDxbc(std::span<const std::uint8_t> dxbc, std::string_view debugName) const {
        if (dxbc.size() < 4 || dxbc[0] != 'D' || dxbc[1] != 'X' || dxbc[2] != 'B' || dxbc[3] != 'C') {
            throw std::invalid_argument("DXBC 字节码格式无效：" + std::string(debugName));
        }
        const auto& lib = compilerLibrary();
        ComPtr<ID3D11ShaderReflection> reflector;
        const HRESULT hr = lib.reflect(dxbc.data(), dxbc.size(), IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflector.GetAddressOf()));
        if (FAILED(hr) || !reflector) {
            throw std::runtime_error("D3DReflect 解析着色器失败：" + std::string(debugName));
        }

        D3D11_SHADER_DESC shaderDesc{};
        if (FAILED(reflector->GetDesc(&shaderDesc))) {
            throw std::runtime_error("无法读取着色器反射描述：" + std::string(debugName));
        }

        core::ShaderReflection reflection;
        for (UINT i = 0; i < shaderDesc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bindDesc{};
            if (FAILED(reflector->GetResourceBindingDesc(i, &bindDesc))) continue;
            const std::string resName = bindDesc.Name ? bindDesc.Name : "";

            if (bindDesc.Type == D3D_SIT_CBUFFER) {
                if (bindDesc.BindPoint >= 12) {
                    if (resName != "Canton") {
                        throw std::runtime_error("用户着色器常量缓冲不能占用保留槽位 b12..b13：" + resName);
                    }
                    continue;
                }
                ID3D11ShaderReflectionConstantBuffer* cb = reflector->GetConstantBufferByName(bindDesc.Name);
                if (!cb) continue;
                D3D11_SHADER_BUFFER_DESC cbDesc{};
                if (FAILED(cb->GetDesc(&cbDesc))) continue;
                const std::uint32_t alignedSize = (cbDesc.Size + 15u) & ~15u;
                reflection.buffers.push_back({bindDesc.BindPoint, alignedSize});

                for (UINT v = 0; v < cbDesc.Variables; ++v) {
                    ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                    if (!var) continue;
                    D3D11_SHADER_VARIABLE_DESC varDesc{};
                    if (FAILED(var->GetDesc(&varDesc))) continue;
                    ID3D11ShaderReflectionType* type = var->GetType();
                    if (!type) continue;
                    D3D11_SHADER_TYPE_DESC typeDesc{};
                    if (FAILED(type->GetDesc(&typeDesc))) continue;
                    if (typeDesc.Class != D3D_SVC_SCALAR && typeDesc.Class != D3D_SVC_VECTOR && typeDesc.Class != D3D_SVC_MATRIX_ROWS && typeDesc.Class != D3D_SVC_MATRIX_COLUMNS) {
                        continue;
                    }
                    core::ShaderUniformField field;
                    field.name = varDesc.Name ? varDesc.Name : "";
                    field.bufferSlot = bindDesc.BindPoint;
                    field.startOffset = varDesc.StartOffset;
                    field.size = varDesc.Size;
                    field.scalarType = toScalarType(typeDesc.Type);
                    field.rows = std::max<std::uint32_t>(1u, typeDesc.Rows);
                    field.columns = std::max<std::uint32_t>(1u, typeDesc.Columns);
                    field.elements = typeDesc.Elements;
                    field.rowMajor = (typeDesc.Class == D3D_SVC_MATRIX_ROWS);
                    reflection.uniforms.push_back(std::move(field));
                }
            } else if (bindDesc.Type == D3D_SIT_TEXTURE) {
                if (bindDesc.BindPoint >= 12) {
                    throw std::runtime_error("用户着色器纹理不能占用保留槽位 t12..t15：" + resName);
                }
                reflection.textures.push_back({resName, bindDesc.BindPoint});
            } else if (bindDesc.Type == D3D_SIT_SAMPLER) {
                if (isBuiltinSamplerName(resName)) continue;
                if (bindDesc.BindPoint >= 12) {
                    throw std::runtime_error("用户着色器采样器不能占用保留槽位 s12..s15：" + resName);
                }
                reflection.samplers.push_back({resName, bindDesc.BindPoint});
            }
        }

        std::sort(reflection.buffers.begin(), reflection.buffers.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
        return reflection;
    }
}
