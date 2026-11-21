#include "output_device/SelectedEndpoint.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>

using ammod::output_device::OutputSelection;
using ammod::output_device::SelectedEndpointStore;

namespace {

void Check(bool ok, const char* description) {
    if (!ok) throw std::runtime_error(description);
}

struct Fixture {
    Fixture() {
        GUID guid{};
        wchar_t id[39]{};
        Check(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, id, 39), "unique fixture");
        name = std::wstring(L"Local\\AMExclusive.OutputSelectionTest.") + id;
        ini = std::filesystem::temp_directory_path() / (std::wstring(id) + L".ini");
        // Match the installed INI encoding used by the Win32 wide profile APIs.
        std::ofstream file(ini, std::ios::binary);
        constexpr wchar_t contents[] = L"\ufeff[mod]\r\n";
        file.write(reinterpret_cast<const char*>(contents), sizeof(contents) - sizeof(wchar_t));
        Check(file.good(), "create isolated profile");
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove(ini, ec); }
    SelectedEndpointStore Open() const { return SelectedEndpointStore(ini, name); }
    bool Set(const wchar_t* key, const wchar_t* value) const {
        return WritePrivateProfileStringW(L"mod", key, value, ini.c_str()) != FALSE;
    }
    std::wstring name;
    std::filesystem::path ini;
};

void Run() {
    Fixture f;
    const std::wstring guid = L"{C85B4969-F231-4444-ABCD-AB072A36B21F}";
    const std::wstring asio = L"asio:" + guid;
    const std::wstring endpoint = L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000001}";
    const OutputSelection systemDefault{};
    Check(f.Open().Read() == systemDefault, "fresh install follows Windows in shared mode");

    Check(f.Set(L"mode", L"exclusive") && f.Set(L"asio_driver", guid.c_str()), "stage legacy settings");
    Check(f.Open().Read() == OutputSelection{asio, true}, "legacy driver and mode survive startup");
    {
        auto controller = f.Open();
        auto agent = f.Open();
        Check(controller.Write({endpoint, false}), "save explicit shared WASAPI endpoint");
        Check(agent.Read() == OutputSelection{endpoint, false}, "peer sees the complete selection");
    }
    // With all mapping handles gone each call below starts from disk.
    Check(f.Open().Read() == OutputSelection{endpoint, false}, "unplugged shared endpoint survives cold start");
    Check(f.Open().SetExclusive(true), "enable exclusive on the saved WASAPI endpoint");
    Check(f.Open().Read() == OutputSelection{endpoint, true}, "WASAPI mode survives cold start");
    Check(f.Open().Write({L"asio:{c85b4969-f231-4444-abcd-ab072a36b21f}", true}), "switch to ASIO");
    Check(f.Open().Read() == OutputSelection{asio, true}, "ASIO cold start uses enumerator spelling");
    Check(f.Open().SetExclusive(false), "disable exclusive from playback settings");
    Check(f.Open().Read() == OutputSelection{asio, false}, "dormant ASIO driver stays disabled");
    Check(f.Open().SetExclusive(true), "re-enable the same driver");
    Check(f.Open().Read() == OutputSelection{asio, true}, "re-enabled ASIO survives restart");
    Check(f.Open().Write(systemDefault), "choose Windows default explicitly");
    Check(f.Open().Read() == systemDefault, "default clears both old device and mode");
    Check(f.Open().SetExclusive(true) && f.Open().Read() == OutputSelection{{}, true},
          "exclusive default is distinct from shared default");

    std::wstring opaque(255, L'X');
    opaque.replace(0, 4, L"\u8033$=:");
    Check(f.Open().Write({opaque, true}), "save maximum-length opaque Unicode device ID");
    Check(f.Open().Read() == OutputSelection{opaque, true}, "full endpoint round-trips");
    {
        auto store = f.Open();
        Check(!store.Write({opaque + L"X", false}) && !store.Write({L"asio:invalid", false}) &&
              !store.Write({L"line\r\nother=value", false}) &&
              !store.Write({std::wstring(L"A\0B", 3), false}), "reject invalid endpoint records");
        Check(store.Read() == OutputSelection{opaque, true}, "invalid selection leaves live preference intact");
    }
    Check(f.Open().Read() == OutputSelection{opaque, true}, "invalid selection leaves disk preference intact");
    Check(f.Set(L"output_selection", (L"exclusive:" + opaque + L"X").c_str()), "stage truncated record");
    Check(f.Open().Read() == systemDefault, "overlong record never opens a truncated endpoint");
    Check(f.Set(L"output_selection", L"exclusive:asio:invalid"), "stage malformed record");
    Check(f.Open().Read() == systemDefault, "corrupt record avoids stale legacy ASIO");
    SelectedEndpointStore unwritable(f.ini / L"missing.ini", f.name + L".Unwritable");
    Check(!unwritable.Write({endpoint, true}) && unwritable.Read() == systemDefault,
          "failed persistence does not publish a successful choice");
}

} // namespace

int main() {
    try { Run(); }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    std::puts("Output device persistence and cold-start tests passed.");
}
