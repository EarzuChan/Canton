#include <win32/MediaSession.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <systemmediatransportcontrolsinterop.h>
#include <windows.media.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <atomic>
#include <string_view>

namespace canton::win32 {
    namespace {
        using Microsoft::WRL::Callback;
        using Microsoft::WRL::ComPtr;
        namespace WinMedia = ABI::Windows::Media;
        namespace WinStorage = ABI::Windows::Storage;
        namespace WinStreams = ABI::Windows::Storage::Streams;
        namespace WinFoundation = ABI::Windows::Foundation;

        using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
        using WindowsCreateStringFn = HRESULT(WINAPI*)(PCNZWCH, UINT32, HSTRING*);
        using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);

        struct WinRtApi {
            HMODULE combase = nullptr;
            RoGetActivationFactoryFn getActivationFactory = nullptr;
            WindowsCreateStringFn createString = nullptr;
            WindowsDeleteStringFn deleteString = nullptr;

            WinRtApi() {
                combase = LoadLibraryW(L"combase.dll");
                if (!combase) return;
                getActivationFactory = reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
                createString = reinterpret_cast<WindowsCreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
                deleteString = reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
            }

            bool ready() const noexcept {
                return getActivationFactory && createString && deleteString;
            }
        };

        const WinRtApi& winrtApi() {
            static const WinRtApi api;
            return api;
        }

        class ScopedHString {
           public:
            explicit ScopedHString(std::wstring_view text) {
                const auto& api = winrtApi();
                if (api.ready()) {
                    (void)api.createString(text.data(), static_cast<UINT32>(text.size()), &value_);
                }
            }
            ~ScopedHString() {
                if (value_ && winrtApi().deleteString) winrtApi().deleteString(value_);
            }
            ScopedHString(const ScopedHString&) = delete;
            ScopedHString& operator=(const ScopedHString&) = delete;
            HSTRING get() const noexcept { return value_; }

           private:
            HSTRING value_ = nullptr;
        };

        std::wstring utf8ToWide(std::string_view text) {
            if (text.empty()) return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (needed <= 0) return {};
            std::wstring out(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
            return out;
        }

        WinMedia::MediaPlaybackStatus toWinRtStatus(core::MediaPlaybackStatus status) noexcept {
            switch (status) {
                case core::MediaPlaybackStatus::Closed: return WinMedia::MediaPlaybackStatus_Closed;
                case core::MediaPlaybackStatus::Stopped: return WinMedia::MediaPlaybackStatus_Stopped;
                case core::MediaPlaybackStatus::Playing: return WinMedia::MediaPlaybackStatus_Playing;
                case core::MediaPlaybackStatus::Paused: return WinMedia::MediaPlaybackStatus_Paused;
            }
            return WinMedia::MediaPlaybackStatus_Closed;
        }

        ComPtr<WinStreams::IRandomAccessStreamReference> createThumbnailFromPath(const std::string& diskPath) {
            if (diskPath.empty()) return {};
            const auto& api = winrtApi();
            if (!api.ready()) return {};

            const auto widePath = utf8ToWide(diskPath);
            if (widePath.empty()) return {};

            // 使用 Uri("file:///...") 经 RandomAccessStreamReference::CreateFromUri 创建封面引用（纯同步无异步文件句柄等待）
            std::wstring uriStr = L"file:///";
            for (wchar_t ch : widePath) {
                if (ch == L'\\')
                    uriStr.push_back(L'/');
                else if (ch == L' ')
                    uriStr += L"%20";
                else
                    uriStr.push_back(ch);
            }

            ScopedHString uriClassName(L"Windows.Foundation.Uri");
            ComPtr<WinFoundation::IUriRuntimeClassFactory> uriFactory;
            if (FAILED(api.getActivationFactory(uriClassName.get(), IID_PPV_ARGS(uriFactory.GetAddressOf())))) return {};
            ScopedHString uriHstr(uriStr);
            ComPtr<WinFoundation::IUriRuntimeClass> uri;
            if (FAILED(uriFactory->CreateUri(uriHstr.get(), uri.GetAddressOf())) || !uri) return {};

            ScopedHString streamRefClassName(L"Windows.Storage.Streams.RandomAccessStreamReference");
            ComPtr<WinStreams::IRandomAccessStreamReferenceStatics> streamStatics;
            if (FAILED(api.getActivationFactory(streamRefClassName.get(), IID_PPV_ARGS(streamStatics.GetAddressOf())))) return {};
            ComPtr<WinStreams::IRandomAccessStreamReference> streamRef;
            if (FAILED(streamStatics->CreateFromUri(uri.Get(), streamRef.GetAddressOf()))) return {};
            return streamRef;
        }
    }

    struct MediaSessionController::Impl {
        HWND appHwnd = nullptr;
        ButtonCallback onButton;
        ComPtr<WinMedia::ISystemMediaTransportControls> smtc;
        EventRegistrationToken buttonToken{};
        bool tokenRegistered = false;
        std::string lastThumbnailPath;

        bool ensureInitialized() {
            if (smtc) return true;
            if (!appHwnd) return false;
            const auto& api = winrtApi();
            if (!api.ready()) return false;

            ScopedHString className(L"Windows.Media.SystemMediaTransportControls");
            ComPtr<ISystemMediaTransportControlsInterop> interop;
            if (FAILED(api.getActivationFactory(className.get(), IID_PPV_ARGS(interop.GetAddressOf()))) || !interop) {
                return false;
            }
            if (FAILED(interop->GetForWindow(appHwnd, IID_PPV_ARGS(smtc.GetAddressOf()))) || !smtc) {
                smtc.Reset();
                return false;
            }

            using Handler = WinFoundation::ITypedEventHandler<WinMedia::SystemMediaTransportControls*, WinMedia::SystemMediaTransportControlsButtonPressedEventArgs*>;
            auto cb = Callback<Handler>([this](WinMedia::ISystemMediaTransportControls*, WinMedia::ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
                if (!args || !onButton) return S_OK;
                WinMedia::SystemMediaTransportControlsButton button{};
                if (FAILED(args->get_Button(&button))) return S_OK;
                switch (button) {
                    case WinMedia::SystemMediaTransportControlsButton_Play:
                        onButton(core::MediaButtonKind::Play);
                        break;
                    case WinMedia::SystemMediaTransportControlsButton_Pause:
                        onButton(core::MediaButtonKind::Pause);
                        break;
                    case WinMedia::SystemMediaTransportControlsButton_Stop:
                        onButton(core::MediaButtonKind::Stop);
                        break;
                    case WinMedia::SystemMediaTransportControlsButton_Next:
                        onButton(core::MediaButtonKind::Next);
                        break;
                    case WinMedia::SystemMediaTransportControlsButton_Previous:
                        onButton(core::MediaButtonKind::Previous);
                        break;
                    default:
                        break;
                }
                return S_OK;
            });
            if (cb && SUCCEEDED(smtc->add_ButtonPressed(cb.Get(), &buttonToken))) {
                tokenRegistered = true;
            }
            return true;
        }
    };

    MediaSessionController::MediaSessionController(void* appHwnd, ButtonCallback onButton) : impl_(std::make_unique<Impl>()) {
        impl_->appHwnd = static_cast<HWND>(appHwnd);
        impl_->onButton = std::move(onButton);
    }

    MediaSessionController::~MediaSessionController() {
        deactivate();
        if (impl_ && impl_->smtc && impl_->tokenRegistered) {
            (void)impl_->smtc->remove_ButtonPressed(impl_->buttonToken);
            impl_->tokenRegistered = false;
        }
    }

    bool MediaSessionController::available() const noexcept {
        return impl_ && impl_->appHwnd != nullptr && winrtApi().ready();
    }

    void MediaSessionController::update(const core::MediaSessionSpec& spec, const std::string& thumbnailDiskPath) {
        if (!spec.active) {
            deactivate();
            return;
        }
        if (!impl_->ensureInitialized()) return;

        (void)impl_->smtc->put_IsEnabled(TRUE);
        (void)impl_->smtc->put_PlaybackStatus(toWinRtStatus(spec.playbackStatus));
        (void)impl_->smtc->put_IsPlayEnabled(spec.canPlay ? TRUE : FALSE);
        (void)impl_->smtc->put_IsPauseEnabled(spec.canPause ? TRUE : FALSE);
        (void)impl_->smtc->put_IsStopEnabled(spec.canStop ? TRUE : FALSE);
        (void)impl_->smtc->put_IsNextEnabled(spec.canNext ? TRUE : FALSE);
        (void)impl_->smtc->put_IsPreviousEnabled(spec.canPrevious ? TRUE : FALSE);

        ComPtr<WinMedia::ISystemMediaTransportControlsDisplayUpdater> updater;
        if (SUCCEEDED(impl_->smtc->get_DisplayUpdater(updater.GetAddressOf())) && updater) {
            (void)updater->put_Type(WinMedia::MediaPlaybackType_Music);
            ComPtr<WinMedia::IMusicDisplayProperties> music;
            if (SUCCEEDED(updater->get_MusicProperties(music.GetAddressOf())) && music) {
                ScopedHString title(utf8ToWide(spec.title));
                ScopedHString artist(utf8ToWide(spec.artist));
                (void)music->put_Title(title.get());
                (void)music->put_Artist(artist.get());
                ComPtr<WinMedia::IMusicDisplayProperties2> music2;
                if (SUCCEEDED(music.As(&music2)) && music2) {
                    ScopedHString album(utf8ToWide(spec.album));
                    (void)music2->put_AlbumTitle(album.get());
                }
            }
            if (thumbnailDiskPath != impl_->lastThumbnailPath) {
                impl_->lastThumbnailPath = thumbnailDiskPath;
                if (thumbnailDiskPath.empty()) {
                    (void)updater->put_Thumbnail(nullptr);
                } else if (auto thumb = createThumbnailFromPath(thumbnailDiskPath)) {
                    (void)updater->put_Thumbnail(thumb.Get());
                }
            }
            (void)updater->Update();
        }
    }

    void MediaSessionController::deactivate() {
        if (!impl_ || !impl_->smtc) return;
        impl_->lastThumbnailPath.clear();
        ComPtr<WinMedia::ISystemMediaTransportControlsDisplayUpdater> updater;
        if (SUCCEEDED(impl_->smtc->get_DisplayUpdater(updater.GetAddressOf())) && updater) {
            (void)updater->ClearAll();
            (void)updater->Update();
        }
        (void)impl_->smtc->put_PlaybackStatus(WinMedia::MediaPlaybackStatus_Closed);
        (void)impl_->smtc->put_IsEnabled(FALSE);
    }

}
