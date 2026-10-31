#include "ProcessLifetime.h"
#include "audio_v2/NativeRenderGateProxy.h"
#include "output_device/OutputDeviceLog.h"
#include "output_device/SelectedEndpoint.h"

#include <windows.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <string>

namespace ammod::output_device {
namespace {

using GetDefaultAudioEndpointFn =
    HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);

constexpr std::size_t kGetDefaultAudioEndpointSlot = 4;

std::atomic<GetDefaultAudioEndpointFn> g_nextGetDefaultAudioEndpoint{};
std::atomic<bool> g_loggedUnavailable{};

SelectedEndpointStore& Store() {
    static SelectedEndpointStore store;
    return store;
}

IMMDevice* OpenActiveRenderEndpoint(IMMDeviceEnumerator* enumerator, const std::wstring& id) {
    IMMDevice* device{};
    DWORD state{};
    if (SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)) && device &&
        SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
        return device;
    }
    if (device) device->Release();
    return nullptr;
}

HRESULT STDMETHODCALLTYPE RedirectGetDefaultAudioEndpoint(IMMDeviceEnumerator* self, EDataFlow flow,
                                                          ERole role, IMMDevice** device) {
    const auto next = g_nextGetDefaultAudioEndpoint.load(std::memory_order_acquire);
    if (!next) return E_UNEXPECTED;
    const HRESULT result = next(self, flow, role, device);
    if (ammod::ProcessExiting() || FAILED(result) || !device || !*device) return result;
    if (flow != eRender || role == eCommunications || ammod::audio_v2::NativeProxyBypassed()) {
        return result;
    }
    const auto selected = Store().Read();
    if (selected.empty()) return result;
    if (auto* redirected = OpenActiveRenderEndpoint(self, selected)) {
        (*device)->Release();
        *device = redirected;
        Log(L"default render endpoint redirected role=" + std::to_wstring(static_cast<int>(role)) +
            L" endpoint=" + selected);
    } else if (!g_loggedUnavailable.exchange(true)) {
        Log(L"selected endpoint unavailable; keeping Windows default endpoint=" + selected);
    }
    return result;
}

// The slot is swapped rather than detoured with MinHook: upstream MinHooks the
// function the slot points at, and either install order still chains both.
bool PatchGetDefaultAudioEndpointSlot(IMMDeviceEnumerator* enumerator) {
    auto** table = *reinterpret_cast<void***>(enumerator);
    void** slot = &table[kGetDefaultAudioEndpointSlot];
    auto* replacement = reinterpret_cast<void*>(&RedirectGetDefaultAudioEndpoint);
    if (*slot == replacement) return true;
    DWORD oldProtect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log(L"VirtualProtect on IMMDeviceEnumerator vtable failed error=" +
            std::to_wstring(GetLastError()));
        return false;
    }
    g_nextGetDefaultAudioEndpoint.store(reinterpret_cast<GetDefaultAudioEndpointFn>(*slot),
                                        std::memory_order_release);
    InterlockedExchangePointer(slot, replacement);
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

DWORD WINAPI InstallEndpointRedirect(void*) {
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&InstallEndpointRedirect), &pinned);
    InitLog(L"am-exclusive-output-device-hook.log", reinterpret_cast<const void*>(&InstallEndpointRedirect));
    if (!Store().IsValid()) {
        Log(L"selected endpoint store unavailable error=" + std::to_wstring(GetLastError()));
    }
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool installed{};
    for (int attempt = 0; attempt < 30 && !installed && !ammod::ProcessExiting(); ++attempt) {
        IMMDeviceEnumerator* enumerator{};
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&enumerator))) && enumerator) {
            installed = PatchGetDefaultAudioEndpointSlot(enumerator);
            enumerator->Release();
        }
        if (!installed) Sleep(1000);
    }
    if (SUCCEEDED(comInit)) CoUninitialize();
    Log(installed ? L"endpoint redirect installed" : L"endpoint redirect install failed");
    return 0;
}

const bool g_started = [] {
    if (HANDLE thread = CreateThread(nullptr, 0, InstallEndpointRedirect, nullptr, 0, nullptr)) {
        CloseHandle(thread);
        return true;
    }
    return false;
}();

} // namespace
} // namespace ammod::output_device
