#pragma once

#include "IpcProtocol.h"
#include "IpcTransport.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace ammod::output_device {

// AppleMusic.exe (UI) writes and AMPLibraryAgent.exe (hook) reads. The section
// lives only while one of them holds it, so the selection resets with the app.
class SelectedEndpointStore final {
public:
    SelectedEndpointStore() {
        const auto prefix = L"Local\\AMExclusive.OutputDevice.SelectedEndpoint." + ammod::ipc::CurrentUserSid();
        mutex_ = CreateMutexW(nullptr, FALSE, (prefix + L".Lock").c_str());
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Shared), prefix.c_str());
        if (mapping_) {
            view_ = static_cast<Shared*>(
                MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
        }
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
        if (!IsValid() || !Lock()) return false;
        wcsncpy_s(view_->endpointId, std::wstring(endpoint).c_str(), _TRUNCATE);
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

    HANDLE mutex_{};
    HANDLE mapping_{};
    Shared* view_{};
};

} // namespace ammod::output_device
