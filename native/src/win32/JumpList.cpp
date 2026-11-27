#include <win32/JumpList.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <stdexcept>

namespace canton::win32 {
    namespace {
        using Microsoft::WRL::ComPtr;

        std::wstring utf8ToWide(std::string_view text) {
            if (text.empty()) return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (needed <= 0) return {};
            std::wstring out(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
            return out;
        }

        std::wstring currentExePath() {
            std::wstring buf(32768, L'\0');
            const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            buf.resize(len);
            return buf;
        }

        std::wstring quoteWin32Arg(const std::wstring& arg) {
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
            std::wstring out = L"\"";
            for (std::size_t i = 0; i < arg.size(); ++i) {
                std::size_t backslashes = 0;
                while (i < arg.size() && arg[i] == L'\\') {
                    ++backslashes;
                    ++i;
                }
                if (i == arg.size()) {
                    out.append(backslashes * 2, L'\\');
                    break;
                }
                if (arg[i] == L'"') {
                    out.append(backslashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                } else {
                    out.append(backslashes, L'\\');
                    out.push_back(arg[i]);
                }
            }
            out.push_back(L'"');
            return out;
        }

        std::wstring joinArgs(const std::vector<std::string>& args) {
            std::wstring out;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0) out.push_back(L' ');
                out += quoteWin32Arg(utf8ToWide(args[i]));
            }
            return out;
        }

        ComPtr<IShellLinkW> createJumpListLink(const std::wstring& exePath, const core::JumpListTaskSpec& task) {
            ComPtr<IShellLinkW> link;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf())))) return {};
            (void)link->SetPath(exePath.c_str());
            const auto args = joinArgs(task.args);
            if (!args.empty()) (void)link->SetArguments(args.c_str());
            if (!task.description.empty()) {
                const auto desc = utf8ToWide(task.description);
                (void)link->SetDescription(desc.c_str());
            }
            if (!task.icon.empty()) {
                const auto iconPath = utf8ToWide(task.icon);
                (void)link->SetIconLocation(iconPath.c_str(), 0);
            } else {
                (void)link->SetIconLocation(exePath.c_str(), 0);
            }
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(link.As(&store)) && store) {
                const auto title = utf8ToWide(task.title);
                PROPVARIANT pv{};
                pv.vt = VT_LPWSTR;
                pv.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc((title.size() + 1) * sizeof(wchar_t)));
                if (pv.pwszVal) {
                    std::memcpy(pv.pwszVal, title.c_str(), (title.size() + 1) * sizeof(wchar_t));
                    (void)store->SetValue(PKEY_Title, pv);
                    (void)store->Commit();
                    PropVariantClear(&pv);
                }
            }
            return link;
        }
    }

    void setJumpList(std::string_view appId, const std::vector<core::JumpListCategorySpec>& categories) {
        ComPtr<ICustomDestinationList> list;
        if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(list.GetAddressOf()))) || !list) {
            throw std::runtime_error("无法创建系统 ICustomDestinationList");
        }
        if (!appId.empty()) {
            const auto wideAppId = utf8ToWide(appId);
            if (FAILED(list->SetAppID(wideAppId.c_str()))) {
                throw std::runtime_error("设置 Jump List AUMID 失败");
            }
        }
        UINT maxSlots = 0;
        ComPtr<IObjectArray> removed;
        if (FAILED(list->BeginList(&maxSlots, IID_PPV_ARGS(removed.GetAddressOf())))) {
            throw std::runtime_error("初始化 Jump List 事务失败");
        }

        const auto exePath = currentExePath();
        for (const auto& category : categories) {
            if (category.items.empty()) continue;
            ComPtr<IObjectCollection> collection;
            if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(collection.GetAddressOf()))) || !collection) {
                (void)list->AbortList();
                throw std::runtime_error("无法创建 Jump List 条目集合");
            }
            for (const auto& task : category.items) {
                if (auto link = createJumpListLink(exePath, task)) {
                    (void)collection->AddObject(link.Get());
                }
            }
            ComPtr<IObjectArray> array;
            if (FAILED(collection.As(&array))) {
                (void)list->AbortList();
                throw std::runtime_error("Jump List 条目集合转换失败");
            }
            if (category.title.empty() || category.title == "Tasks") {
                if (FAILED(list->AddUserTasks(array.Get()))) {
                    (void)list->AbortList();
                    throw std::runtime_error("写入 Jump List 任务列表失败");
                }
            } else {
                const auto wideCat = utf8ToWide(category.title);
                if (FAILED(list->AppendCategory(wideCat.c_str(), array.Get()))) {
                    (void)list->AbortList();
                    throw std::runtime_error("写入 Jump List 自定义分类失败：" + category.title);
                }
            }
        }

        if (FAILED(list->CommitList())) {
            throw std::runtime_error("提交 Jump List 失败");
        }
    }

    void clearJumpList(std::string_view appId) {
        ComPtr<ICustomDestinationList> list;
        if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(list.GetAddressOf()))) || !list) {
            return;
        }
        const auto wideAppId = utf8ToWide(appId);
        (void)list->DeleteList(wideAppId.empty() ? nullptr : wideAppId.c_str());
    }

}
