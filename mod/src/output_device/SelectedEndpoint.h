#pragma once

#include "IpcProtocol.h"
#include "IpcTransport.h"
#include "AsioDevices.h"
#include <filesystem>

#include <windows.h>

#include <string>
#include <string_view>

namespace ammod::output_device {

// AppleMusic.exe (UI) writes and AMPLibraryAgent.exe (hook) reads. The section
// shares the live selection. An ASIO preference is restored from the installed INI.
class SelectedEndpointStore final {
public:
    SelectedEndpointStore() {
        const auto prefix = L"Local\\AMExclusive.OutputDevice.SelectedEndpoint." + ammod::ipc::CurrentUserSid();
        mutex_ = CreateMutexW(nullptr, FALSE, (prefix + L".Lock").c_str());
        if (!mutex_ || !Lock()) return;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Shared), prefix.c_str());
        const bool created = mapping_ && GetLastError() != ERROR_ALREADY_EXISTS;
        if (mapping_) {
            view_ = static_cast<Shared*>(
                MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
        }
        if (created && view_ && !iniPath_.empty()) {
            wchar_t id[48]{};
            GetPrivateProfileStringW(L"mod", L"asio_driver", L"", id, static_cast<DWORD>(std::size(id)),
                                     iniPath_.c_str());
            GUID guid{};
            if (wcsnlen_s(id, std::size(id)) == 38 && id[0] == L'{' &&
                SUCCEEDED(CLSIDFromString(id, &guid))) {
                wcscpy_s(view_->endpointId, L"asio:");
                wcscat_s(view_->endpointId, id);
            }
        }
        ReleaseMutex(mutex_);
    }

    ~SelectedEndpointStore() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
    }

    SelectedEndpointStore(const SelectedEndpointStore&) = delete;
    SelectedEndpointStore& operator=(const SelectedEndpointStore&) = delete;

    bool IsValid() const noexcept { return view_ && mutex_; }

    std::wstring Read() const {
        if (!IsValid() || !Lock()) return {};
        std::wstring endpoint(view_->endpointId,
                              wcsnlen_s(view_->endpointId, std::size(view_->endpointId)));
        ReleaseMutex(mutex_);
        return endpoint;
    }

    bool Write(std::wstring_view endpoint) {
        if (endpoint.size() >= ipc::kEndpointIdChars) return false;
        const std::wstring value(endpoint);
        if (!IsValid() || !Lock()) return false;
        const bool save = IsAsioRoute(endpoint) || IsAsioRoute(view_->endpointId);
        wcscpy_s(view_->endpointId, value.c_str());
        if (save && !iniPath_.empty()) {
            const auto* clsid = IsAsioRoute(endpoint) ? value.c_str() + kAsioPrefix.size() : L"";
            WritePrivateProfileStringW(L"mod", L"asio_driver", clsid, iniPath_.c_str());
        }
        ReleaseMutex(mutex_);
        return true;
    }

private:
    struct Shared {
        wchar_t endpointId[ammod::ipc::kEndpointIdChars];
    };

    bool Lock() const {
        const DWORD wait = WaitForSingleObject(mutex_, 1000);
        return wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }

    static std::filesystem::path IniPath() {
        static const wchar_t anchor = 0;
        HMODULE module{};
        wchar_t path[MAX_PATH]{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, &anchor, &module)) return {};
        const auto n = GetModuleFileNameW(module, path, MAX_PATH);
        return n && n < MAX_PATH ? std::filesystem::path(path).parent_path() / L"am-exclusive.ini"
                                 : std::filesystem::path{};
    }
    const std::filesystem::path iniPath_ = IniPath();
    HANDLE mutex_{};
    HANDLE mapping_{};
    Shared* view_{};
};

} // namespace ammod::output_device
