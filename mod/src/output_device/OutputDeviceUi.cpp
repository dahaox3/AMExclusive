#include "IpcProtocol.h"
#include "IpcTransport.h"
#include "output_device/OutputDeviceLog.h"
#include "output_device/SelectedEndpoint.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Dispatching.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ammod::output_device {
namespace {

using namespace winrt;
namespace Xaml = Microsoft::UI::Xaml;
namespace Controls = Microsoft::UI::Xaml::Controls;
namespace Media = Microsoft::UI::Xaml::Media;
namespace Automation = Microsoft::UI::Xaml::Automation;
using ammod::ipc::Message;
using ammod::ipc::MessageType;
using ammod::ipc::Role;
using ammod::ipc::RuntimeState;

constexpr wchar_t kOutputDeviceButtonName[] = L"AMExclusiveOutputDeviceButton";
constexpr std::wstring_view kWindowProperties[] = {L"MainWindow", L"MiniPlayerWindow"};
constexpr DWORD kAttachIntervalMs = 1500;

std::mutex g_statusMutex;
Message g_status{};
bool g_haveStatus{};
std::atomic<int> g_pendingIntent{-1};

std::mutex g_attachedButtonsMutex;
std::unordered_map<std::uintptr_t, weak_ref<Controls::Button>> g_attachedButtons;

SelectedEndpointStore& Store() {
    static SelectedEndpointStore store;
    return store;
}

Message StatusSnapshot(bool& available) {
    std::lock_guard lock(g_statusMutex);
    available = g_haveStatus;
    return g_status;
}

bool InTransition(const Message& status) {
    return status.state == RuntimeState::Probing || status.state == RuntimeState::Initializing ||
           status.state == RuntimeState::AlignRetry || status.state == RuntimeState::DisablePending;
}

bool UseChineseUi() {
    static const bool chinese = [] {
        try {
            const auto languages = Windows::Globalization::ApplicationLanguages::Languages();
            if (languages && languages.Size() != 0) {
                const std::wstring tag = languages.GetAt(0).c_str();
                return tag.size() >= 2 && (tag[0] == L'z' || tag[0] == L'Z') &&
                       (tag[1] == L'h' || tag[1] == L'H') && (tag.size() == 2 || tag[2] == L'-');
            }
        } catch (...) {
        }
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    }();
    return chinese;
}

std::wstring Localized(std::wstring_view chinese, std::wstring_view english) {
    return std::wstring(UseChineseUi() ? chinese : english);
}

Xaml::DependencyObject FindByName(const Xaml::DependencyObject& node, std::wstring_view name) {
    if (!node) return nullptr;
    try {
        if (auto element = node.try_as<Xaml::FrameworkElement>(); element && element.Name() == name) {
            return node;
        }
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            if (auto found = FindByName(Media::VisualTreeHelper::GetChild(node, index), name)) return found;
        }
    } catch (...) {
    }
    return nullptr;
}

Xaml::DependencyObject FindByAutomationId(const Xaml::DependencyObject& node,
                                          std::wstring_view automationId) {
    if (!node) return nullptr;
    try {
        if (Automation::AutomationProperties::GetAutomationId(node) == automationId) return node;
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            if (auto found = FindByAutomationId(Media::VisualTreeHelper::GetChild(node, index), automationId)) {
                return found;
            }
        }
    } catch (...) {
    }
    return nullptr;
}

Controls::TextBlock FindFirstTextBlock(const Xaml::DependencyObject& node) {
    if (!node) return nullptr;
    try {
        if (auto text = node.try_as<Controls::TextBlock>()) return text;
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            if (auto found = FindFirstTextBlock(Media::VisualTreeHelper::GetChild(node, index))) return found;
        }
    } catch (...) {
    }
    return nullptr;
}

using GetAppPropertyFn = Windows::Foundation::IInspectable(__cdecl*)(hstring const&);

GetAppPropertyFn GetAppPropertyFunction() {
    HMODULE utils = GetModuleHandleW(L"AMP.Utils.dll");
    if (!utils) return nullptr;
    constexpr char exportName[] =
        "?GetAppProperty@SharedUtils@@YA?AUIInspectable@Foundation@Windows@winrt@@AEBUhstring@5@@Z";
    return reinterpret_cast<GetAppPropertyFn>(GetProcAddress(utils, exportName));
}

Xaml::Window GetXamlWindowProperty(std::wstring_view propertyName) {
    const auto function = GetAppPropertyFunction();
    if (!function) return nullptr;
    try {
        return function(hstring(propertyName)).try_as<Xaml::Window>();
    } catch (...) {
        return nullptr;
    }
}

struct RenderEndpoint {
    std::wstring id;
    std::wstring name;
};

std::vector<RenderEndpoint> EnumerateRenderEndpoints() {
    std::vector<RenderEndpoint> endpoints;
    com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
        Log(L"output device enumeration could not create MMDeviceEnumerator");
        return endpoints;
    }
    com_ptr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()))) {
        return endpoints;
    }
    UINT count{};
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        com_ptr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.put()))) continue;
        LPWSTR id{};
        if (FAILED(device->GetId(&id)) || !id) continue;
        RenderEndpoint endpoint{id, {}};
        CoTaskMemFree(id);
        com_ptr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put()))) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal) {
                endpoint.name = value.pwszVal;
            }
            PropVariantClear(&value);
        }
        if (endpoint.name.empty()) endpoint.name = endpoint.id;
        endpoints.push_back(std::move(endpoint));
    }
    std::sort(endpoints.begin(), endpoints.end(),
              [](const auto& left, const auto& right) { return left.name < right.name; });
    return endpoints;
}

void SelectOutputDevice(const std::wstring& endpointId, bool exclusive, bool currentlyExclusive) {
    Log(L"user selected output device exclusive=" + std::to_wstring(exclusive) +
        L" endpoint=" + (endpointId.empty() ? std::wstring(L"<default>") : endpointId));
    if (!Store().Write(endpointId)) Log(L"selected endpoint store write failed");
    if (exclusive != currentlyExclusive) g_pendingIntent.store(exclusive ? 1 : 0);
}

Controls::RadioMenuFlyoutItem MakeOutputDeviceItem(const std::wstring& text, const std::wstring& endpointId,
                                                   bool exclusive, const std::wstring& selected,
                                                   bool currentlyExclusive, bool enabled) {
    const bool current = selected == endpointId && currentlyExclusive == exclusive;
    Controls::RadioMenuFlyoutItem item;
    item.Text(hstring(text));
    item.GroupName(L"AMExclusiveOutputDevice");
    item.IsChecked(current);
    item.IsEnabled(enabled);
    item.Click([endpointId, exclusive, current, currentlyExclusive](auto&&, auto&&) {
        if (!current) SelectOutputDevice(endpointId, exclusive, currentlyExclusive);
    });
    return item;
}

void RebuildOutputDeviceMenu(const Controls::MenuFlyout& flyout) {
    bool brokerAvailable{};
    const auto status = StatusSnapshot(brokerAvailable);
    const bool currentlyExclusive = status.enabledIntent != 0;
    const bool enabled = brokerAvailable && !InTransition(status);
    const auto selected = Store().Read();
    auto items = flyout.Items();
    items.Clear();
    items.Append(MakeOutputDeviceItem(Localized(L"默认", L"Default"), {}, false, selected,
                                      currentlyExclusive, enabled));
    const auto endpoints = EnumerateRenderEndpoints();
    for (const auto& endpoint : endpoints) {
        items.Append(MakeOutputDeviceItem(endpoint.name, endpoint.id, false, selected,
                                          currentlyExclusive, enabled));
    }
    if (!endpoints.empty()) items.Append(Controls::MenuFlyoutSeparator());
    for (const auto& endpoint : endpoints) {
        items.Append(MakeOutputDeviceItem(Localized(L"[独占] ", L"[Exclusive] ") + endpoint.name,
                                          endpoint.id, true, selected, currentlyExclusive, enabled));
    }
}

Controls::Button CreateOutputDeviceButton(const Xaml::FrameworkElement& volume) {
    Controls::Button button;
    button.Name(kOutputDeviceButtonName);
    const auto label = Localized(L"输出设备", L"Output device");
    Automation::AutomationProperties::SetAutomationId(button, kOutputDeviceButtonName);
    Automation::AutomationProperties::SetName(button, label);
    Controls::ToolTipService::SetToolTip(button, box_value(label));
    Controls::FontIcon icon;
    icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    icon.Glyph(L"");
    if (auto volumeText = FindFirstTextBlock(volume)) icon.FontSize(volumeText.FontSize());
    button.Content(icon);
    if (auto nativeButton = volume.try_as<Controls::Button>()) {
        button.Style(nativeButton.Style());
        button.Padding(nativeButton.Padding());
        button.Background(nativeButton.Background());
        button.BorderBrush(nativeButton.BorderBrush());
        button.BorderThickness(nativeButton.BorderThickness());
        button.Foreground(nativeButton.Foreground());
        button.CornerRadius(nativeButton.CornerRadius());
    }
    button.Width(volume.ActualWidth() > 0 ? volume.ActualWidth() : volume.Width());
    button.Height(volume.ActualHeight() > 0 ? volume.ActualHeight() : volume.Height());
    button.HorizontalAlignment(volume.HorizontalAlignment());
    Controls::MenuFlyout flyout;
    flyout.Opening([](Windows::Foundation::IInspectable const& sender, auto&&) {
        if (auto opening = sender.try_as<Controls::MenuFlyout>()) RebuildOutputDeviceMenu(opening);
    });
    button.Flyout(flyout);
    return button;
}

bool ButtonStillAttached(std::uintptr_t windowIdentity) {
    std::lock_guard lock(g_attachedButtonsMutex);
    const auto found = g_attachedButtons.find(windowIdentity);
    if (found == g_attachedButtons.end()) return false;
    auto button = found->second.get();
    return button && Media::VisualTreeHelper::GetParent(button);
}

void RememberButton(std::uintptr_t windowIdentity, const Controls::Button& button) {
    std::lock_guard lock(g_attachedButtonsMutex);
    g_attachedButtons[windowIdentity] = make_weak(button);
}

void AttachOutputDeviceButton(const Xaml::Window& window, std::uintptr_t windowIdentity) {
    if (ButtonStillAttached(windowIdentity)) return;
    auto content = window.Content().try_as<Xaml::DependencyObject>();
    if (!content) return;
    auto volumeNode = FindByName(content, L"VolumeButton");
    if (!volumeNode) volumeNode = FindByAutomationId(content, L"VolumeButton");
    if (!volumeNode) return;
    Xaml::FrameworkElement anchor{nullptr};
    Controls::StackPanel row{nullptr};
    auto current = volumeNode;
    for (unsigned depth = 0; current && depth < 6; ++depth) {
        auto parent = Media::VisualTreeHelper::GetParent(current);
        if (auto stack = parent.try_as<Controls::StackPanel>();
            stack && stack.Orientation() == Controls::Orientation::Horizontal) {
            anchor = current.try_as<Xaml::FrameworkElement>();
            row = stack;
            break;
        }
        current = parent;
    }
    if (!anchor || !row) return;
    for (const auto& child : row.Children()) {
        if (auto element = child.try_as<Controls::Button>(); element && element.Name() == kOutputDeviceButtonName) {
            RememberButton(windowIdentity, element);
            return;
        }
    }
    auto volume = volumeNode.try_as<Xaml::FrameworkElement>();
    uint32_t anchorIndex{};
    if (!volume || !row.Children().IndexOf(anchor, anchorIndex)) return;
    auto button = CreateOutputDeviceButton(volume);
    button.Margin(anchor.Margin());
    button.VerticalAlignment(anchor.VerticalAlignment());
    row.Children().InsertAt(anchorIndex, button);
    RememberButton(windowIdentity, button);
    Log(L"output device button attached index=" + std::to_wstring(anchorIndex));
}

void ScheduleAttach() {
    for (const auto property : kWindowProperties) {
        auto window = GetXamlWindowProperty(property);
        if (!window) continue;
        const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(window));
        try {
            auto dispatcher = window.DispatcherQueue();
            if (!dispatcher) continue;
            dispatcher.TryEnqueue([weakWindow = make_weak(window), identity] {
                try {
                    if (auto live = weakWindow.get()) AttachOutputDeviceButton(live, identity);
                } catch (const hresult_error& error) {
                    Log(L"output device button attach failed hr=" + std::to_wstring(error.code().value));
                }
            });
        } catch (...) {
        }
    }
}

DWORD WINAPI AttachThread(void*) {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        ScheduleAttach();
        Sleep(kAttachIntervalMs);
    }
}

void PublishStatus(const Message& incoming, std::uint64_t& lastGeneration) {
    if (incoming.type != MessageType::StatusChanged || incoming.role != Role::Broker) return;
    if (incoming.generation < lastGeneration) return;
    lastGeneration = incoming.generation;
    std::lock_guard lock(g_statusMutex);
    g_status = incoming;
    g_haveStatus = true;
}

void ForgetStatus() {
    std::lock_guard lock(g_statusMutex);
    g_haveStatus = false;
}

// Upstream UI owns broker startup; this Controller connection only waits for it.
DWORD WINAPI BrokerThread(void*) {
    const auto name = ammod::ipc::PipeName();
    for (;;) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        auto hello = ammod::ipc::NewMessage(MessageType::Hello, Role::Controller);
        hello.uiPid = GetCurrentProcessId();
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) ||
            !ammod::ipc::WriteMessage(pipe, hello)) {
            CloseHandle(pipe);
            Sleep(1000);
            continue;
        }
        Log(L"controller connected to broker");
        std::uint64_t lastGeneration = 0;
        Message incoming{};
        for (;;) {
            if (const int pending = g_pendingIntent.exchange(-1); pending >= 0) {
                auto request = ammod::ipc::NewMessage(MessageType::SetEnabled, Role::Controller);
                request.uiPid = GetCurrentProcessId();
                request.enabledIntent = static_cast<std::uint8_t>(pending);
                if (!ammod::ipc::WriteMessage(pipe, request)) {
                    g_pendingIntent.store(pending);
                    break;
                }
            }
            if (!ammod::ipc::ReadMessageTimeout(pipe, incoming, 100)) {
                if (GetLastError() == ERROR_TIMEOUT) continue;
                break;
            }
            PublishStatus(incoming, lastGeneration);
        }
        ForgetStatus();
        CloseHandle(pipe);
        Log(L"controller disconnected from broker");
        Sleep(1000);
    }
}

DWORD WINAPI Bootstrap(void*) {
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&Bootstrap), &pinned);
    InitLog(L"am-exclusive-output-device-ui.log", reinterpret_cast<const void*>(&Bootstrap));
    if (!Store().IsValid()) {
        Log(L"selected endpoint store unavailable error=" + std::to_wstring(GetLastError()));
    }
    if (HANDLE broker = CreateThread(nullptr, 0, BrokerThread, nullptr, 0, nullptr)) CloseHandle(broker);
    return AttachThread(nullptr);
}

const bool g_started = [] {
    if (HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr)) {
        CloseHandle(thread);
        return true;
    }
    return false;
}();

} // namespace
} // namespace ammod::output_device
