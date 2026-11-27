#include <runtime/SingleInstance.h>

#include <runtime/AppPaths.h>
#include <runtime/Digest.h>
#include <runtime/Json.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstring>
#include <span>

namespace canton::runtime {
    namespace {
        std::string escapeJsonString(std::string_view text) {
            std::string out;
            out.reserve(text.size() + 8);
            out.push_back('"');
            for (const unsigned char c : text) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buf[8]{};
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }
            out.push_back('"');
            return out;
        }

        std::string serializeLaunchRequest(const core::LaunchRequest& req) {
            std::string out = "{\"workingDirectory\":";
            out += escapeJsonString(req.workingDirectory);
            out += ",\"args\":[";
            for (std::size_t i = 0; i < req.args.size(); ++i) {
                if (i > 0) out.push_back(',');
                out += escapeJsonString(req.args[i]);
            }
            out += "]}";
            return out;
        }

        std::optional<core::LaunchRequest> deserializeLaunchRequest(std::string_view jsonText) {
            try {
                const auto parsed = parseJson(jsonText);
                if (!std::holds_alternative<JsonValue::Object>(parsed.value)) return std::nullopt;
                core::LaunchRequest req;
                if (const auto* cwd = jsonString(jsonMember(parsed, "workingDirectory"))) {
                    req.workingDirectory = *cwd;
                }
                if (const auto* args = jsonArray(jsonMember(parsed, "args"))) {
                    for (const auto& item : *args) {
                        if (const auto* s = jsonString(&item)) req.args.push_back(*s);
                    }
                }
                return req;
            } catch (...) {
                return std::nullopt;
            }
        }

        bool forwardToPrimary(const std::wstring& pipeName, const core::LaunchRequest& request) {
            const auto payload = serializeLaunchRequest(request);
            const std::uint32_t payloadLen = static_cast<std::uint32_t>(payload.size());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            while (std::chrono::steady_clock::now() < deadline) {
                HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe == INVALID_HANDLE_VALUE) {
                    const DWORD err = GetLastError();
                    if (err == ERROR_PIPE_BUSY) {
                        WaitNamedPipeW(pipeName.c_str(), 200);
                        continue;
                    }
                    Sleep(30);
                    continue;
                }
                DWORD written = 0;
                bool ok = WriteFile(pipe, &payloadLen, sizeof(payloadLen), &written, nullptr) && written == sizeof(payloadLen);
                if (ok && payloadLen > 0) {
                    ok = WriteFile(pipe, payload.data(), payloadLen, &written, nullptr) && written == payloadLen;
                }
                FlushFileBuffers(pipe);
                DWORD primaryPid = 0;
                DWORD read = 0;
                if (ok && ReadFile(pipe, &primaryPid, sizeof(primaryPid), &read, nullptr) && read == sizeof(primaryPid) && primaryPid != 0) {
                    (void)AllowSetForegroundWindow(primaryPid);
                }
                CloseHandle(pipe);
                return ok;
            }
            return false;
        }
    }

    SingleInstanceGuard::~SingleInstanceGuard() {
        stop();
    }

    bool SingleInstanceGuard::acquireOrForward(
        const std::string& appKey,
        const core::LaunchRequest& request,
        std::function<void(core::LaunchRequest)> onSecondInstance) {
        stop();
        const auto hash = sha256Hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(appKey.data()), appKey.size())).substr(0, 24);
        const std::wstring mutexName = L"Local\\CantonSingleInstance-" + AppPaths::utf8ToWide(hash);
        pipeName_ = L"\\\\.\\pipe\\canton-single-" + AppPaths::utf8ToWide(hash);

        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!mutex) return true;
        const DWORD lastErr = GetLastError();
        if (lastErr == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex);
            (void)forwardToPrimary(pipeName_, request);
            return false;
        }

        mutexHandle_ = mutex;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        callback_ = std::move(onSecondInstance);
        primary_ = true;
        stopping_.store(false, std::memory_order_release);
        listenerThread_ = std::thread([this] { listenLoop(); });
        return true;
    }

    void SingleInstanceGuard::stop() {
        stopping_.store(true, std::memory_order_release);
        if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
        if (listenerThread_.joinable()) listenerThread_.join();
        if (stopEvent_) {
            CloseHandle(static_cast<HANDLE>(stopEvent_));
            stopEvent_ = nullptr;
        }
        if (mutexHandle_) {
            CloseHandle(static_cast<HANDLE>(mutexHandle_));
            mutexHandle_ = nullptr;
        }
        primary_ = false;
        callback_ = {};
    }

    void SingleInstanceGuard::listenLoop() {
        while (!stopping_.load(std::memory_order_acquire)) {
            HANDLE pipe = CreateNamedPipeW(
                pipeName_.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4096,
                65536,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                Sleep(50);
                continue;
            }

            HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED ov{};
            ov.hEvent = connectEvent;
            BOOL connected = ConnectNamedPipe(pipe, &ov);
            if (!connected) {
                const DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waits[2] = {connectEvent, static_cast<HANDLE>(stopEvent_)};
                    const DWORD waitRes = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (waitRes == WAIT_OBJECT_0) {
                        DWORD transferred = 0;
                        connected = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
                    } else {
                        CancelIoEx(pipe, &ov);
                        CloseHandle(connectEvent);
                        CloseHandle(pipe);
                        break;
                    }
                } else if (err == ERROR_PIPE_CONNECTED) {
                    connected = TRUE;
                }
            }
            CloseHandle(connectEvent);

            if (connected && !stopping_.load(std::memory_order_acquire)) {
                auto readExact = [&](void* dst, DWORD total) -> bool {
                    DWORD got = 0;
                    auto* out = static_cast<char*>(dst);
                    while (got < total) {
                        OVERLAPPED rov{};
                        rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                        DWORD chunk = 0;
                        BOOL ok = ReadFile(pipe, out + got, total - got, &chunk, &rov);
                        if (!ok && GetLastError() == ERROR_IO_PENDING) {
                            HANDLE waits[2] = {rov.hEvent, static_cast<HANDLE>(stopEvent_)};
                            if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) {
                                ok = GetOverlappedResult(pipe, &rov, &chunk, FALSE);
                            } else {
                                CancelIoEx(pipe, &rov);
                                ok = FALSE;
                            }
                        }
                        CloseHandle(rov.hEvent);
                        if (!ok || chunk == 0) return false;
                        got += chunk;
                    }
                    return true;
                };

                std::uint32_t payloadLen = 0;
                if (readExact(&payloadLen, sizeof(payloadLen)) && payloadLen <= 1024 * 1024) {
                    std::string payload(payloadLen, '\0');
                    if (payloadLen == 0 || readExact(payload.data(), payloadLen)) {
                        const DWORD pid = GetCurrentProcessId();
                        OVERLAPPED wov{};
                        wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                        DWORD written = 0;
                        if (!WriteFile(pipe, &pid, sizeof(pid), &written, &wov) && GetLastError() == ERROR_IO_PENDING) {
                            WaitForSingleObject(wov.hEvent, 1000);
                        }
                        CloseHandle(wov.hEvent);
                        FlushFileBuffers(pipe);
                        if (auto req = deserializeLaunchRequest(payload); req && callback_) {
                            callback_(std::move(*req));
                        }
                    }
                }
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

}
