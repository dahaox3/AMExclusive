#pragma once

#include "BoundedLog.h"

#include <windows.h>

#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace ammod::output_device {

inline std::mutex logMutex;
inline std::filesystem::path logPath;

inline void InitLog(std::wstring_view fileName, const void* addressInModule) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addressInModule), &module)) {
        return;
    }
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath)))) return;
    logPath = std::filesystem::path(modulePath).parent_path() / fileName;
}

inline void Log(std::wstring_view message) {
    if (logPath.empty()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream line;
    line << std::setfill(L'0') << std::setw(4) << now.wYear << L'-' << std::setw(2) << now.wMonth
         << L'-' << std::setw(2) << now.wDay << L'T' << std::setw(2) << now.wHour << L':'
         << std::setw(2) << now.wMinute << L':' << std::setw(2) << now.wSecond << L'.'
         << std::setw(3) << now.wMilliseconds << L" pid=" << GetCurrentProcessId()
         << L" tid=" << GetCurrentThreadId() << L" " << message;
    ammod::logging::AppendWideLine(logPath, logMutex, line.str(), 1024ull * 1024ull, 1);
}

} // namespace ammod::output_device
