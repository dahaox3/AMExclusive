// Explicit in-process smoke probe for a locally installed CoreMedia build.
// Creates empty synthetic filter trees; does not access media or DRM data.
#include "LosslessQualityLock.h"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::wstring messages;
void Log(const std::wstring& text) { messages += text + L"\n"; }
template<class T> T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
struct Scope {
    void(__cdecl* release)(const void*){};
    std::vector<void*> objects;
    ~Scope() {
        ammod::quality::SetEnabled(false);
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        for (auto object : objects) if (object) release(object);
    }
    void* Keep(void* object) { objects.push_back(object); return object; }
};
}

extern "C" __declspec(dllexport) const wchar_t* QualityProbeLog() { return messages.c_str(); }
extern "C" __declspec(dllexport) int RunQualityAbiProbe() {
    messages.clear();
    auto media = GetModuleHandleW(L"CoreMedia.dll");
    auto cf = GetModuleHandleW(L"CoreFoundation.dll");
    if (!media || !cf) return 1;
    // Do not overlay a pre-existing quality hook installed by another DLL.
    const auto* entry = reinterpret_cast<const unsigned char*>(GetProcAddress(media, "FigSimpleAlternateFilterCreate"));
    if (!entry || entry[0] == 0xE9 || entry[0] == 0xEB) return 2;
    if (MH_Initialize() != MH_OK) return 3;
    Scope scope{Export<void(__cdecl*)(const void*)>(cf, "CFRelease")};
    if (!ammod::quality::Install(media, cf, Log)) return 4;
    ammod::quality::SetEnabled(true);
    auto number = Export<void*(__cdecl*)(void*, int, const void*)>(cf, "CFNumberCreate");
    auto array = Export<void*(__cdecl*)(void*, const void**, std::int64_t, const void*)>(cf, "CFArrayCreate");
    auto count = Export<std::int64_t(__cdecl*)(const void*)>(cf, "CFArrayGetCount");
    const auto callbacks = GetProcAddress(cf, "kCFTypeArrayCallBacks");
    const int alac = 0x616c6163, qlac = 0x716c6163;
    const void* codecs[]{scope.Keep(number(nullptr, 3, &alac)), scope.Keep(number(nullptr, 3, &qlac))};
    auto allowed = scope.Keep(array(nullptr, codecs, 2, callbacks));
    auto factory = Export<ammod::quality::SubtypeFactory>(media, "FigAlternateAllowableMediaSubtypeFilterCreate");
    void* strict{};
    if (ammod::quality::CreateStrictLosslessFilter(factory, nullptr, allowed, 20, &strict) != 0 || !strict) return 5;
    scope.Keep(strict);
    auto createTree = Export<int(__cdecl*)(void*, void**)>(media, "FigAlternateFilterTreeCreate");
    auto insert = Export<int(__cdecl*)(void*, void*, unsigned)>(media, "FigAlternateFilterTreeInsertLeaf");
    auto createFilter = Export<int(__cdecl*)(void*, void*, unsigned, void**)>(media, "FigAlternateFilterTreeCreateFilter");
    void* tree{};
    if (createTree(nullptr, &tree) != 0 || !tree) return 6;
    scope.Keep(tree);
    if (insert(tree, strict, 0x375) != 0) return 7;
    void* name = *reinterpret_cast<void**>(GetProcAddress(media, "kFigAlternateFilterName_AllowableMediaSubtypes"));
    void* filter{};
    if (createFilter(tree, name, 0, &filter) != 0 || !filter) return 8;
    scope.Keep(filter);
    auto table = *reinterpret_cast<char**>(static_cast<char*>(filter) + 0x18);
    auto methods = *reinterpret_cast<char**>(table + 0x10);
    auto apply = *reinterpret_cast<int(__cdecl**)(void*, void*, void**, void*)>(methods + 0x20);
    auto input = scope.Keep(array(nullptr, nullptr, 0, callbacks));
    void* output{};
    const auto result = apply(filter, input, &output, nullptr);
    scope.Keep(output);
    if (result != 0 || !output || count(output) != 0) return 9;
    Log(L"strict filter construction and native tree Apply passed");
    return 0;
}
