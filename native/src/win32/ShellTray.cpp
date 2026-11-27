#include <win32/ShellTray.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace canton::win32 {
    namespace {
        std::wstring utf8ToWide(std::string_view text) {
            if (text.empty()) return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (needed <= 0) return {};
            std::wstring out(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
            return out;
        }

        // 轻量 SHA-256（仅用于确定性派生托盘 NIF_GUID，避免 win32 反向依赖 runtime）
        std::array<std::uint8_t, 32> sha256Bytes(std::string_view data) {
            constexpr std::uint32_t k[64] = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
            };
            const auto rotr = [](std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32u - n)); };
            std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

            std::vector<std::uint8_t> msg(data.begin(), data.end());
            const std::uint64_t bitLen = static_cast<std::uint64_t>(msg.size()) * 8ull;
            msg.push_back(0x80u);
            while ((msg.size() % 64) != 56) msg.push_back(0);
            for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>((bitLen >> (i * 8)) & 0xffu));

            for (std::size_t off = 0; off < msg.size(); off += 64) {
                std::uint32_t w[64]{};
                for (int i = 0; i < 16; ++i) {
                    w[i] = (static_cast<std::uint32_t>(msg[off + i * 4]) << 24) |
                           (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 16) |
                           (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 8) |
                           static_cast<std::uint32_t>(msg[off + i * 4 + 3]);
                }
                for (int i = 16; i < 64; ++i) {
                    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (int i = 0; i < 64; ++i) {
                    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                    const std::uint32_t ch = (e & f) ^ ((~e) & g);
                    const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
                    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t t2 = S0 + maj;
                    hh = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
                h[5] += f;
                h[6] += g;
                h[7] += hh;
            }
            std::array<std::uint8_t, 32> digest{};
            for (int i = 0; i < 8; ++i) {
                digest[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xffu);
                digest[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xffu);
                digest[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xffu);
                digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xffu);
            }
            return digest;
        }

        GUID deriveStableTrayGuid(std::string_view appId, std::string_view trayKey) {
            std::string material = "canton.tray.v1:";
            material.append(appId.empty() ? "CantonApp" : appId);
            material.push_back(':');
            material.append(trayKey.empty() ? "default" : trayKey);
            auto digest = sha256Bytes(material);
            // 按 RFC 4122 设定版本 5 与变体位
            digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0fu) | 0x50u);
            digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3fu) | 0x80u);
            GUID guid{};
            guid.Data1 = (static_cast<unsigned long>(digest[0]) << 24) |
                         (static_cast<unsigned long>(digest[1]) << 16) |
                         (static_cast<unsigned long>(digest[2]) << 8) |
                         static_cast<unsigned long>(digest[3]);
            guid.Data2 = static_cast<unsigned short>((static_cast<unsigned int>(digest[4]) << 8) | digest[5]);
            guid.Data3 = static_cast<unsigned short>((static_cast<unsigned int>(digest[6]) << 8) | digest[7]);
            std::memcpy(guid.Data4, digest.data() + 8, 8);
            return guid;
        }

        std::optional<GUID> parseGuidString(std::string_view text) {
            if (text.empty()) return std::nullopt;
            std::wstring wide = utf8ToWide(text);
            if (!wide.empty() && wide.front() != L'{') {
                wide = L"{" + wide + L"}";
            }
            GUID guid{};
            if (SUCCEEDED(CLSIDFromString(wide.c_str(), &guid))) return guid;
            return std::nullopt;
        }

        HMENU buildWin32Menu(const std::vector<core::TrayMenuItemSpec>& items) {
            HMENU menu = CreatePopupMenu();
            if (!menu) return nullptr;
            for (const auto& item : items) {
                switch (item.kind) {
                    case core::TrayMenuItemKind::Separator:
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                        break;
                    case core::TrayMenuItemKind::SubMenu: {
                        HMENU sub = buildWin32Menu(item.children);
                        if (sub) {
                            const auto label = utf8ToWide(item.label);
                            UINT flags = MF_POPUP | MF_STRING;
                            if (!item.enabled) flags |= MF_GRAYED;
                            AppendMenuW(menu, flags, reinterpret_cast<UINT_PTR>(sub), label.c_str());
                        }
                        break;
                    }
                    case core::TrayMenuItemKind::Item:
                    case core::TrayMenuItemKind::Checkbox:
                    case core::TrayMenuItemKind::Radio: {
                        const auto label = utf8ToWide(item.label);
                        MENUITEMINFOW info{};
                        info.cbSize = sizeof(info);
                        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_STRING;
                        info.wID = item.commandId;
                        info.fType = MFT_STRING | (item.kind == core::TrayMenuItemKind::Radio ? MFT_RADIOCHECK : 0u);
                        info.fState = (item.enabled ? MFS_ENABLED : MFS_DISABLED) | (item.checked ? MFS_CHECKED : MFS_UNCHECKED);
                        info.dwTypeData = const_cast<LPWSTR>(label.c_str());
                        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
                        break;
                    }
                }
            }
            return menu;
        }
    }

    std::string deriveStableTrayGuidString(std::string_view appId, std::string_view trayKey) {
        const auto g = deriveStableTrayGuid(appId, trayKey);
        char buf[64]{};
        std::snprintf(
            buf,
            sizeof(buf),
            "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            g.Data1,
            g.Data2,
            g.Data3,
            g.Data4[0],
            g.Data4[1],
            g.Data4[2],
            g.Data4[3],
            g.Data4[4],
            g.Data4[5],
            g.Data4[6],
            g.Data4[7]);
        return buf;
    }

    PopupPositionPx computePopupPositionForTray(const core::TrayAnchorRect& anchor, int popupWidthPx, int popupHeightPx) noexcept {
        const int w = std::max(1, popupWidthPx);
        const int h = std::max(1, popupHeightPx);
        const int anchorCenterX = (anchor.right > anchor.left) ? (anchor.left + anchor.right) / 2 : anchor.cursorX;
        const int anchorTop = (anchor.bottom > anchor.top) ? anchor.top : anchor.cursorY;
        const int anchorBottom = (anchor.bottom > anchor.top) ? anchor.bottom : anchor.cursorY;

        constexpr int margin = 6;
        int x = anchorCenterX - w / 2;
        int y = anchorTop - h - margin;

        // 如果上方空间不足（如任务栏在屏幕顶部），改放锚点下方
        if (y < anchor.workTop) {
            y = anchorBottom + margin;
        }
        // 严格钳制在当前显示器工作区内（自动避让任务栏）
        const int maxX = std::max(anchor.workLeft, anchor.workRight - w - margin);
        const int maxY = std::max(anchor.workTop, anchor.workBottom - h - margin);
        x = std::clamp(x, anchor.workLeft + margin, maxX);
        y = std::clamp(y, anchor.workTop + margin, maxY);
        return {x, y};
    }

    struct ShellTrayManager::Impl {
        struct Entry {
            std::string trayId;
            std::uint32_t uId = 0;
            GUID guid{};
            core::TrayIconSpec spec;
            HICON icon = nullptr;
            bool added = false;
        };

        HWND appHwnd = nullptr;
        EventCallback onEvent;
        std::string appId;
        std::uint32_t nextUid = 1;
        std::unordered_map<std::string, Entry> entries;
        std::unordered_map<std::uint32_t, std::string> uidToTrayId;
        HICON fallbackIcon = nullptr;

        HICON ensureFallbackIcon() {
            if (!fallbackIcon) fallbackIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            return fallbackIcon;
        }

        NOTIFYICONDATAW makeNid(const Entry& entry, UINT flags) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = appHwnd;
            nid.uID = entry.uId;
            nid.uFlags = flags | NIF_GUID;
            nid.guidItem = entry.guid;
            nid.uCallbackMessage = MessageDispatcher::kTrayCallbackMessage;
            nid.hIcon = entry.icon ? entry.icon : ensureFallbackIcon();
            if (flags & NIF_TIP) {
                const auto wideTip = utf8ToWide(entry.spec.tooltip);
                wcsncpy_s(nid.szTip, wideTip.c_str(), _TRUNCATE);
            }
            return nid;
        }

        void applyEntry(Entry& entry) {
            if (!appHwnd) return;
            if (!entry.spec.visible) {
                if (entry.added) {
                    NOTIFYICONDATAW nid = makeNid(entry, 0);
                    Shell_NotifyIconW(NIM_DELETE, &nid);
                    entry.added = false;
                }
                return;
            }
            if (!entry.added) {
                NOTIFYICONDATAW nid = makeNid(entry, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
                if (Shell_NotifyIconW(NIM_ADD, &nid)) {
                    nid.uVersion = NOTIFYICON_VERSION_4;
                    Shell_NotifyIconW(NIM_SETVERSION, &nid);
                    entry.added = true;
                }
            } else {
                NOTIFYICONDATAW nid = makeNid(entry, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
                if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
                    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
                        nid.uVersion = NOTIFYICON_VERSION_4;
                        Shell_NotifyIconW(NIM_SETVERSION, &nid);
                        entry.added = true;
                    }
                }
            }
        }
    };

    ShellTrayManager::ShellTrayManager(void* appHwnd, EventCallback onEvent) : impl_(std::make_unique<Impl>()) {
        impl_->appHwnd = static_cast<HWND>(appHwnd);
        impl_->onEvent = std::move(onEvent);
    }

    ShellTrayManager::~ShellTrayManager() {
        clearAll();
    }

    void ShellTrayManager::setAppId(std::string appId) {
        impl_->appId = std::move(appId);
    }

    void ShellTrayManager::upsertTray(const core::TrayIconSpec& spec, void* iconHandle) {
        auto it = impl_->entries.find(spec.id);
        const GUID resolvedGuid = parseGuidString(spec.guid).value_or(deriveStableTrayGuid(impl_->appId, spec.id));
        if (it == impl_->entries.end()) {
            Impl::Entry entry;
            entry.trayId = spec.id;
            entry.uId = impl_->nextUid++;
            entry.guid = resolvedGuid;
            entry.spec = spec;
            entry.icon = static_cast<HICON>(iconHandle);
            auto [inserted, _] = impl_->entries.emplace(spec.id, std::move(entry));
            impl_->uidToTrayId[inserted->second.uId] = spec.id;
            impl_->applyEntry(inserted->second);
            return;
        }
        auto& entry = it->second;
        if (std::memcmp(&entry.guid, &resolvedGuid, sizeof(GUID)) != 0) {
            if (entry.added) {
                NOTIFYICONDATAW nid = impl_->makeNid(entry, 0);
                Shell_NotifyIconW(NIM_DELETE, &nid);
                entry.added = false;
            }
            entry.guid = resolvedGuid;
        }
        entry.spec = spec;
        entry.icon = static_cast<HICON>(iconHandle);
        impl_->applyEntry(entry);
    }

    void ShellTrayManager::removeTray(const std::string& trayId) {
        const auto it = impl_->entries.find(trayId);
        if (it == impl_->entries.end()) return;
        if (it->second.added && impl_->appHwnd) {
            NOTIFYICONDATAW nid = impl_->makeNid(it->second, 0);
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        impl_->uidToTrayId.erase(it->second.uId);
        impl_->entries.erase(it);
    }

    void ShellTrayManager::clearAll() {
        if (!impl_) return;
        for (auto& [_, entry] : impl_->entries) {
            if (entry.added && impl_->appHwnd) {
                NOTIFYICONDATAW nid = impl_->makeNid(entry, 0);
                Shell_NotifyIconW(NIM_DELETE, &nid);
                entry.added = false;
            }
        }
        impl_->entries.clear();
        impl_->uidToTrayId.clear();
    }

    void ShellTrayManager::onTaskbarCreated() {
        for (auto& [_, entry] : impl_->entries) {
            entry.added = false;
            impl_->applyEntry(entry);
        }
    }

    core::TrayAnchorRect ShellTrayManager::queryAnchor(const std::string& trayId, int fallbackX, int fallbackY) const {
        core::TrayAnchorRect out;
        POINT cursor{fallbackX, fallbackY};
        if (cursor.x == 0 && cursor.y == 0) GetCursorPos(&cursor);
        out.cursorX = cursor.x;
        out.cursorY = cursor.y;

        if (const auto it = impl_->entries.find(trayId); it != impl_->entries.end() && impl_->appHwnd) {
            NOTIFYICONIDENTIFIER id{};
            id.cbSize = sizeof(id);
            id.hWnd = impl_->appHwnd;
            id.uID = it->second.uId;
            id.guidItem = it->second.guid;
            RECT rect{};
            if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect)) && rect.right > rect.left && rect.bottom > rect.top) {
                out.left = rect.left;
                out.top = rect.top;
                out.right = rect.right;
                out.bottom = rect.bottom;
            }
        }

        POINT probe = (out.right > out.left) ? POINT{(out.left + out.right) / 2, (out.top + out.bottom) / 2} : cursor;
        HMONITOR mon = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            out.workLeft = mi.rcWork.left;
            out.workTop = mi.rcWork.top;
            out.workRight = mi.rcWork.right;
            out.workBottom = mi.rcWork.bottom;
        }
        const UINT sysDpi = GetDpiForSystem();
        out.dpiScale = sysDpi > 0 ? static_cast<double>(sysDpi) / 96.0 : 1.0;
        return out;
    }

    void ShellTrayManager::handleTrayMessage(std::uint32_t iconId, std::uint32_t mouseMsg, int cursorX, int cursorY) {
        const auto uidIt = impl_->uidToTrayId.find(iconId);
        if (uidIt == impl_->uidToTrayId.end()) return;
        const std::string trayId = uidIt->second;

        std::optional<core::TrayEventKind> kind;
        switch (mouseMsg) {
            case WM_LBUTTONUP:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                kind = core::TrayEventKind::Click;
                break;
            case WM_LBUTTONDBLCLK:
                kind = core::TrayEventKind::DoubleClick;
                break;
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                kind = core::TrayEventKind::ContextMenu;
                break;
            default:
                return;
        }

        if (kind && impl_->onEvent) {
            // 右键或点击托盘图标瞬间系统赋予当前进程前台切换权限：
            // 立即将隐藏顶层 App 窗置前，确保随后异步弹出的 <CustomTrayMenu> 窗口一经创建即可真正获焦，
            // 从而让用户不先点弹窗也能通过点击外部区域立刻触发失焦关闭。
            if (impl_->appHwnd) SetForegroundWindow(impl_->appHwnd);
            core::TrayEvent ev;
            ev.trayId = trayId;
            ev.kind = *kind;
            ev.anchor = queryAnchor(trayId, cursorX, cursorY);
            impl_->onEvent(ev);
        }
    }

    std::uint32_t ShellTrayManager::showSystemMenu(const std::vector<core::TrayMenuItemSpec>& items, int screenX, int screenY) {
        if (!impl_->appHwnd || items.empty()) return 0;
        HMENU menu = buildWin32Menu(items);
        if (!menu) return 0;

        // 标准 Win32 托盘菜单防僵尸惯用法：先置前，再弹窗，返回后投递 WM_NULL
        SetForegroundWindow(impl_->appHwnd);
        const UINT cmd = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screenX,
            screenY,
            impl_->appHwnd,
            nullptr);
        PostMessageW(impl_->appHwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        return static_cast<std::uint32_t>(cmd);
    }

}
