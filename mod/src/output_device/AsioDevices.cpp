#include "AsioDevices.h"
#include <objbase.h>
#include <algorithm>
#include <iterator>

namespace ammod::output_device {
namespace {

struct Key {
    HKEY handle{};
    ~Key() { if (handle) RegCloseKey(handle); }
};

std::wstring ReadText(HKEY key, const wchar_t* name) {
    wchar_t value[512]{};
    DWORD bytes = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &bytes)
        == ERROR_SUCCESS ? value : L"";
}

} // namespace

std::vector<AsioDevice> EnumerateAsioDevices() {
    std::vector<AsioDevice> devices;
    Key root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY,
                      &root.handle) != ERROR_SUCCESS) return devices;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD chars = static_cast<DWORD>(std::size(name));
        const LSTATUS entry = RegEnumKeyExW(root.handle, i, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (entry == ERROR_MORE_DATA) continue;
        if (entry != ERROR_SUCCESS) break;
        Key key;
        if (RegOpenKeyExW(root.handle, name, 0, KEY_READ | KEY_WOW64_64KEY, &key.handle) != ERROR_SUCCESS)
            continue;
        const auto id = ReadText(key.handle, L"CLSID");
        CLSID clsid{};
        wchar_t canonical[39]{};
        if (id.size() != 38 || id.front() != L'{' || id.back() != L'}' || FAILED(CLSIDFromString(id.c_str(), &clsid)) ||
            IsEqualGUID(clsid, GUID_NULL) || !StringFromGUID2(clsid, canonical, 39)) continue;
        if (std::any_of(devices.begin(), devices.end(), [&](const auto& d) { return d.clsid == canonical; }))
            continue;
        auto label = ReadText(key.handle, L"Description");
        devices.push_back({canonical, label.empty() ? std::wstring(name) : std::move(label)});
    }
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return devices;
}

bool IsRegisteredAsioDriver(const CLSID& clsid) {
    wchar_t canonical[39]{};
    if (!StringFromGUID2(clsid, canonical, 39)) return false;
    const auto drivers = EnumerateAsioDevices();
    return std::any_of(drivers.begin(), drivers.end(), [&](const auto& d) { return d.clsid == canonical; });
}

} // namespace ammod::output_device
