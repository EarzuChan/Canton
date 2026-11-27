#include <host/HostAssembly.h>

#if !CANTON_ENABLE_DEV
#error DevAssembly.cpp 只编进 Dev 宿主
#endif

#include <core/Log.h>
#include <runtime/DiagnosticsScene.h>
#include <runtime/LiveClient.h>
#include <runtime/LiveResources.h>
#include <win32/Window.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#ifdef DrawText
#undef DrawText
#endif

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <optional>
#include <stdexcept>
#include <vector>

namespace canton::host {
    namespace {
        constexpr std::string_view DEV_TAG = "DevDebug";

        wchar_t gCrashLogPath[32768]{};

        void writeCrashLine(HANDLE file, const char* text) noexcept {
            const auto len = std::strlen(text);
            if (file != INVALID_HANDLE_VALUE && len > 0) {
                DWORD written = 0;
                (void)WriteFile(file, text, static_cast<DWORD>(len), &written, nullptr);
            }
            std::fputs(text, stderr);
        }

        LONG WINAPI devCrashFilter(EXCEPTION_POINTERS* info) {
            const auto code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul;
            const auto addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
            HANDLE file = INVALID_HANDLE_VALUE;
            if (gCrashLogPath[0] != L'\0') {
                file = CreateFileW(gCrashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            char lineBuf[1024]{};
            std::snprintf(lineBuf, sizeof(lineBuf), "\n=== Canton Dev Crash code=0x%08lX addr=%p tid=%lu ===\n", code, addr, GetCurrentThreadId());
            writeCrashLine(file, lineBuf);

            if (HMODULE dbg = LoadLibraryW(L"dbghelp.dll")) {
                using SymInitFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
                using SymOptFn = DWORD(WINAPI*)(DWORD);
                using StackWalkFn = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
                using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
                using SymGetLineFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
                const auto symInit = reinterpret_cast<SymInitFn>(GetProcAddress(dbg, "SymInitialize"));
                const auto symOpt = reinterpret_cast<SymOptFn>(GetProcAddress(dbg, "SymSetOptions"));
                const auto stackWalk = reinterpret_cast<StackWalkFn>(GetProcAddress(dbg, "StackWalk64"));
                const auto symFuncTable = reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(GetProcAddress(dbg, "SymFunctionTableAccess64"));
                const auto symModBase = reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(GetProcAddress(dbg, "SymGetModuleBase64"));
                const auto symFromAddr = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbg, "SymFromAddr"));
                const auto symGetLine = reinterpret_cast<SymGetLineFn>(GetProcAddress(dbg, "SymGetLineFromAddr64"));

                if (symInit && symOpt && stackWalk && symFuncTable && symModBase && info && info->ContextRecord) {
                    const HANDLE proc = GetCurrentProcess();
                    symOpt(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
                    symInit(proc, nullptr, TRUE);
                    CONTEXT ctx = *info->ContextRecord;
                    STACKFRAME64 frame{};
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrStack.Mode = AddrModeFlat;
#if defined(_M_AMD64)
                    constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
                    frame.AddrPC.Offset = ctx.Rip;
                    frame.AddrFrame.Offset = ctx.Rbp;
                    frame.AddrStack.Offset = ctx.Rsp;
#else
                    constexpr DWORD machine = IMAGE_FILE_MACHINE_I386;
                    frame.AddrPC.Offset = ctx.Eip;
                    frame.AddrFrame.Offset = ctx.Ebp;
                    frame.AddrStack.Offset = ctx.Esp;
#endif
                    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512]{};
                    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 511;

                    for (int i = 0; i < 48; ++i) {
                        if (!stackWalk(machine, proc, GetCurrentThread(), &frame, &ctx, nullptr, symFuncTable, symModBase, nullptr)) break;
                        if (frame.AddrPC.Offset == 0) break;
                        DWORD64 disp = 0;
                        const bool hasSym = symFromAddr && symFromAddr(proc, frame.AddrPC.Offset, &disp, sym);
                        IMAGEHLP_LINE64 line{};
                        line.SizeOfStruct = sizeof(line);
                        DWORD lineDisp = 0;
                        const bool hasLine = symGetLine && symGetLine(proc, frame.AddrPC.Offset, &lineDisp, &line);
                        if (hasSym && hasLine) {
                            std::snprintf(lineBuf, sizeof(lineBuf), "  #%02d 0x%llX %s+0x%llX (%s:%lu)\n", i, frame.AddrPC.Offset, sym->Name, disp, line.FileName, line.LineNumber);
                        } else if (hasSym) {
                            std::snprintf(lineBuf, sizeof(lineBuf), "  #%02d 0x%llX %s+0x%llX\n", i, frame.AddrPC.Offset, sym->Name, disp);
                        } else {
                            std::snprintf(lineBuf, sizeof(lineBuf), "  #%02d 0x%llX\n", i, frame.AddrPC.Offset);
                        }
                        writeCrashLine(file, lineBuf);
                    }
                }
            }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            std::fflush(stderr);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        void installDevCrashFilter(const std::optional<std::wstring>& logPath) {
            gCrashLogPath[0] = L'\0';
            if (logPath && !logPath->empty() && logPath->size() < std::size(gCrashLogPath)) {
                std::error_code ec;
                const auto parent = std::filesystem::path(*logPath).parent_path();
                if (!parent.empty()) std::filesystem::create_directories(parent, ec);
                std::wmemcpy(gCrashLogPath, logPath->c_str(), logPath->size() + 1);
            }
            SetUnhandledExceptionFilter(devCrashFilter);
        }

        std::optional<std::wstring> environment(const wchar_t* name) {
            const auto size = GetEnvironmentVariableW(name, nullptr, 0);
            if (size == 0) return std::nullopt;
            std::wstring value(size, L'\0');
            value.resize(GetEnvironmentVariableW(name, value.data(), size));
            return value.empty() ? std::nullopt : std::optional(std::move(value));
        }

        std::string narrow(const std::wstring& text) {
            if (text.empty()) return {};
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        class DevInputHandlerImpl final : public DevInputHandler {
           public:
            bool handleKeyDown(int virtualKey, core::KeyModifierState modifiers, runtime::AppSession& app, core::WindowId windowId) override {
                if (const auto* err = app.globalDiagnostics().error()) {
                    if (modifiers.ctrl && (virtualKey == 'C' || virtualKey == 'c')) {
                        win32::writeClipboardText(err->diagnosticText());
                        core::Log::i(DEV_TAG, "已复制诊断信息到剪贴板");
                        return true;
                    }
                }
                if (virtualKey == VK_F5) {
                    core::Log::i(DEV_TAG, "快捷键请求重新加载 (F5)");
                    app.requestReload();
                    return true;
                }
                if (virtualKey == VK_F6) {
                    const int phase = testToastIndex_++ % 3;
                    if (phase == 0) {
                        app.notifyToast(core::LogLevel::Info, DEV_TAG, "测试提示 · Info", "来自 F6 快捷键的诊断气泡（路由至当前主工作窗）", false);
                    } else if (phase == 1) {
                        app.notifyToast(core::LogLevel::Warn, DEV_TAG, "测试提示 · Warn", "Live 服务暂不可用时将显示此级别警告", false);
                    } else {
                        app.notifyToast(core::LogLevel::Error, DEV_TAG, "测试提示 · Error", "热更新或脚本异常时将显示此级别错误提示", false);
                    }
                    return true;
                }
                if (virtualKey == VK_F7) {
                    if (app.globalDiagnostics().hasError()) {
                        core::Log::i(DEV_TAG, "快捷键从模拟错误屏重载恢复 (F7)");
                        app.requestReload();
                    } else {
                        core::Log::i(DEV_TAG, "快捷键触发模拟错误屏 (F7)");
                        app.triggerSimulatedError(
                            "由 F7 触发的开发期模拟异常",
                            "这是一条主动注入的诊断测试错误，用于验证多窗口错误屏布局、DPI 缩放、Ctrl+C 复制与重试恢复。\n按 F5 / F7 或点击任意窗口即可重新加载恢复。");
                    }
                    return true;
                }
                if (virtualKey == VK_F8 && windowId != 0) {
                    const bool next = !app.windowFpsBadgeEnabled(windowId);
                    app.setWindowFpsBadgeEnabled(windowId, next);
                    core::Log::i(DEV_TAG, "FPS 徽标 (F8)", next ? "已开启" : "已关闭");
                    return true;
                }
                if (virtualKey == VK_F9 && windowId != 0) {
                    const bool next = !app.windowFrameTimeEnabled(windowId);
                    app.setWindowFrameTimeEnabled(windowId, next);
                    core::Log::i(DEV_TAG, "帧时间面板 (F9)", next ? "已开启" : "已关闭");
                    return true;
                }
                if (virtualKey == VK_F10 && windowId != 0) {
                    const bool next = !app.windowDirtyFlashEnabled(windowId);
                    app.setWindowDirtyFlashEnabled(windowId, next);
                    core::Log::i(DEV_TAG, "重画区闪红 (F10)", next ? "已开启" : "已关闭");
                    return true;
                }
                if (virtualKey == VK_F11 && windowId != 0) {
                    const bool next = !app.windowBoundsOverlayEnabled(windowId);
                    app.setWindowBoundsOverlayEnabled(windowId, next);
                    core::Log::i(DEV_TAG, "布局框框与命中高亮 (F11)", next ? "已开启" : "已关闭");
                    return true;
                }
                return false;
            }

            bool handleErrorClick(runtime::AppSession& app) override {
                if (app.globalDiagnostics().errorRetryAvailable()) {
                    core::Log::i(DEV_TAG, "错误屏点击重试");
                    app.requestReload();
                    return true;
                }
                return false;
            }

           private:
            int testToastIndex_ = 0;
        };
    }

    // Dev 宿主：加载规矩经 CANTON_DEV_* 环境变量注入，不占用命令行
    HostAssembly assembleHost(const std::filesystem::path& bundleRoot) {
        installDevCrashFilter(environment(L"CANTON_DEV_CRASH_LOG"));
        HostAssembly assembly;
        assembly.source.bundleRoot = bundleRoot;
        assembly.source.policy = runtime::LoadPolicy::LiveThenDist;
        if (const auto source = environment(L"CANTON_DEV_SOURCE")) {
            const auto policy = runtime::parseLoadPolicy(narrow(*source));
            if (!policy) throw std::runtime_error("CANTON_DEV_SOURCE 只接受 lfd、live 或 dist，收到的是 " + narrow(*source));
            assembly.source.policy = *policy;
        }
        assembly.source.devServerUrl = narrow(environment(L"CANTON_DEV_SERVER").value_or(L"http://127.0.0.1:9178"));
        if (const auto dist = environment(L"CANTON_DEV_DIST")) assembly.source.distOverride = *dist;
        if (const auto dataRoot = environment(L"CANTON_DEV_DATA_ROOT")) {
            assembly.dataRootOverride = *dataRoot;
        } else if (const auto prodRoot = environment(L"CANTON_DATA_ROOT")) {
            assembly.dataRootOverride = *prodRoot;
        }
        if (const auto appId = environment(L"CANTON_DEV_APP_ID")) assembly.appIdOverride = narrow(*appId);
        if (const auto single = environment(L"CANTON_DEV_SINGLE_INSTANCE")) {
            const auto v = narrow(*single);
            assembly.singleInstanceOverride = (v == "1" || v == "true" || v == "TRUE");
        }
        if (const auto file = environment(L"CANTON_DEV_READY_FILE")) {
            assembly.readyFile = *file;
            assembly.readyToken = narrow(environment(L"CANTON_DEV_READY_TOKEN").value_or(L""));
        }
        assembly.hudFactory = [] {
            runtime::DiagnosticsConfig diagnostics;
            diagnostics.badge = runtime::DiagnosticVisibility::Always;
            diagnostics.toasts = runtime::DiagnosticVisibility::Always;
            diagnostics.overlay = runtime::DiagnosticVisibility::Always;
            return runtime::createDiagnosticsHud(diagnostics);
        };
        assembly.devInput = std::make_unique<DevInputHandlerImpl>();
        assembly.detailedErrorScreen = true;
        assembly.live = [workers = runtime::createResourceWorkers()](std::function<void()> wake) {
            return runtime::makeLiveSessionFactory(std::move(wake), [workers](std::string_view devServerUrl) {
                return runtime::RuntimeLoadResources{
                    runtime::livePainterLoader(std::string(devServerUrl), {}, workers),
                    runtime::liveAssetLoader(std::string(devServerUrl), workers),
                    runtime::liveAssetExistsChecker(std::string(devServerUrl), workers),
                };
            });
        };
        return assembly;
    }
}
