#pragma once

#include "AsioDevices.h"
#include "IpcProtocol.h"
#include "IpcTransport.h"

#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ammod::output_device {

struct OutputSelection {
    std::wstring endpoint;
    bool exclusive{};

    bool operator==(const OutputSelection&) const = default;
};

class SelectedEndpointStore final {
public:
    SelectedEndpointStore() : SelectedEndpointStore(IniPath(),
        L"Local\\AMExclusive.OutputDevice.Selection.2." + ipc::CurrentUserSid()) {}

    SelectedEndpointStore(std::filesystem::path iniPath, const std::wstring& name)
        : iniPath_(std::move(iniPath)) {
        mutex_ = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        const Guard guard(mutex_);
        if (!guard || iniPath_.empty()) return;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Shared), name.c_str());
        const bool created = mapping_ && GetLastError() != ERROR_ALREADY_EXISTS;
        if (mapping_) {
            view_ = static_cast<Shared*>(
                MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
        }
        if (view_ && (created || guard.abandoned())) RestoreLocked();
    }

    ~SelectedEndpointStore() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
    }

    SelectedEndpointStore(const SelectedEndpointStore&) = delete;
    SelectedEndpointStore& operator=(const SelectedEndpointStore&) = delete;
    bool IsValid() const noexcept { return view_ != nullptr; }

    std::optional<OutputSelection> Read() const {
        const Guard guard(mutex_);
        if (!guard || !view_) return std::nullopt;
        if (guard.abandoned()) RestoreLocked();
        return OutputSelection{view_->endpointId, view_->exclusive};
    }

    bool Write(const OutputSelection& selection) {
        const Guard guard(mutex_);
        if (!guard || !view_) return false;
        if (guard.abandoned()) RestoreLocked();
        return SaveLocked(selection);
    }

    bool SetExclusive(bool exclusive) {
        const Guard guard(mutex_);
        if (!guard || !view_) return false;
        if (guard.abandoned()) RestoreLocked();
        return SaveLocked({view_->endpointId, exclusive});
    }

private:
    struct Shared {
        wchar_t endpointId[ipc::kEndpointIdChars];
        bool exclusive;
    };

    class Guard {
    public:
        explicit Guard(HANDLE mutex) : mutex_(mutex),
            wait_(mutex ? WaitForSingleObject(mutex, 1000) : WAIT_FAILED) {}
        ~Guard() { if (*this) ReleaseMutex(mutex_); }
        explicit operator bool() const { return wait_ == WAIT_OBJECT_0 || abandoned(); }
        bool abandoned() const { return wait_ == WAIT_ABANDONED; }
    private:
        HANDLE mutex_;
        DWORD wait_;
    };

    static bool NormalizeEndpoint(std::wstring& endpoint) {
        if (endpoint.size() >= ipc::kEndpointIdChars ||
            endpoint.find_first_of(L"\0\r\n", 0, 3) != std::wstring::npos) return false;
        if (!IsAsioRoute(endpoint)) return true;
        const auto* id = endpoint.c_str() + kAsioPrefix.size();
        GUID guid{};
        wchar_t canonical[39]{};
        if (wcslen(id) != 38 || *id != L'{' || FAILED(CLSIDFromString(id, &guid)) ||
            IsEqualGUID(guid, GUID_NULL) || !StringFromGUID2(guid, canonical, 39)) return false;
        endpoint = std::wstring(kAsioPrefix) + canonical;
        return true;
    }

    void PublishLocked(const OutputSelection& selection) const {
        wcscpy_s(view_->endpointId, selection.endpoint.c_str());
        view_->exclusive = selection.exclusive;
    }

    void RestoreLocked() const {
        // One key holds both the device ID and its mode. A missing endpoint
        // is still a valid preference and may become available on the next open.
        constexpr std::wstring_view shared = L"shared:";
        constexpr std::wstring_view exclusive = L"exclusive:";
        wchar_t record[ipc::kEndpointIdChars + exclusive.size() + 1]{};
        const auto n = GetPrivateProfileStringW(L"mod", L"output_selection", L"", record,
            static_cast<DWORD>(std::size(record)), iniPath_.c_str());
        OutputSelection selection;
        if (n) {
            std::wstring_view value(record, n);
            selection.exclusive = value.starts_with(exclusive);
            if (n < std::size(record) - 1 && (selection.exclusive || value.starts_with(shared))) {
                selection.endpoint = value.substr(selection.exclusive ? exclusive.size() : shared.size());
            } else {
                selection = {};
            }
        } else {
            // Read settings from 0.3.4/0.3.5 when upgrading binaries in place.
            wchar_t mode[32]{}, driver[48]{};
            GetPrivateProfileStringW(L"mod", L"mode", L"probe", mode, static_cast<DWORD>(std::size(mode)), iniPath_.c_str());
            GetPrivateProfileStringW(L"mod", L"asio_driver", L"", driver, static_cast<DWORD>(std::size(driver)), iniPath_.c_str());
            selection.exclusive = _wcsicmp(mode, L"exclusive") == 0;
            if (driver[0]) selection.endpoint = std::wstring(kAsioPrefix) + driver;
        }
        if (!NormalizeEndpoint(selection.endpoint)) selection = {};
        PublishLocked(selection);
    }

    bool SaveLocked(OutputSelection selection) {
        if (!NormalizeEndpoint(selection.endpoint)) return false;
        const auto value = (selection.exclusive ? L"exclusive:" : L"shared:") + selection.endpoint;
        if (!WritePrivateProfileStringW(L"mod", L"output_selection", value.c_str(), iniPath_.c_str()))
            return false;
        PublishLocked(selection);
        return true;
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
    const std::filesystem::path iniPath_;
    HANDLE mutex_{};
    HANDLE mapping_{};
    Shared* view_{};
};

} // namespace ammod::output_device
