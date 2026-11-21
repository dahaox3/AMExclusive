#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace ammod::output_device {

inline constexpr std::wstring_view kAsioPrefix = L"asio:";
inline bool IsAsioRoute(std::wstring_view route) noexcept { return route.starts_with(kAsioPrefix); }

struct AsioDevice {
    std::wstring clsid;
    std::wstring name;
};

std::vector<AsioDevice> EnumerateAsioDevices();
bool IsRegisteredAsioDriver(const CLSID& clsid);

} // namespace ammod::output_device
