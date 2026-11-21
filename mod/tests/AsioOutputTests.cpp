#include "audio_v2/AsioExclusiveSink.h"
#include "output_device/AsioDevices.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace ammod::audio_v2;
using namespace ammod::audio_v2::asio;

namespace {
void Check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}

struct Source final : IIntegerPcmSource {
    std::uint32_t remaining{513};
    bool silent{}, starved{};
    unsigned fills{};
    HRESULT Prepare(std::uint32_t) noexcept override {
        if (starved) return kAudioSourceWouldBlock;
        return remaining ? S_OK : S_FALSE;
    }
    HRESULT Fill(std::int32_t* dst, std::uint32_t frames, std::uint32_t& n, bool& eof) noexcept override {
        ++fills;
        n = (std::min)(remaining, frames);
        for (unsigned i = 0; i < n; ++i) {
            dst[i * 2] = silent ? 0 : INT32_MIN;
            dst[i * 2 + 1] = silent ? 0 : 0x12345600;
        }
        remaining -= n;
        eof = remaining == 0;
        return S_OK;
    }
};

// Control calls must stay on one initialized STA, even when caller threads
// change between Open, Start, pause and Close. Drive switch callbacks by hand.
struct Driver final : IASIO {
    std::array<std::array<std::array<BYTE, 2048 * 4>, 2>, 2> data{};
    ASIOCallbacks* cb{};
    long type{ASIOSTInt32LSB24}, rightType{-1};
    double rate{48000};
    long preferred{512};
    std::atomic<long> outputDelay{696};
    bool failStart{}, allowRateChange{}, created{}, disposed{}, released{}, running{};
    DWORD tid{};
    void Thread() {
        Check(tid == GetCurrentThreadId(), "driver control remains on its owner thread");
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        Check(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
              (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA), "driver runs in STA");
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { Thread(); released = true; return 0; }
    ASIOBool init(void*) override { Thread(); released = disposed = false; return ASIOTrue; }
    void getDriverName(char*) override {}
    long getDriverVersion() override { return 2; }
    void getErrorMessage(char*) override {}
    ASIOError start() override {
        Thread(); cb->bufferSwitch(0, ASIOTrue); // allowed before start returns
        if (failStart) return ASE_HWMalfunction;
        running = true; return ASE_OK;
    }
    ASIOError stop() override { Thread(); running = false; return ASE_OK; }
    ASIOError getChannels(long* in, long* out) override { *in = 0; *out = 2; return ASE_OK; }
    ASIOError getLatencies(long* in, long* out) override { *in = 560; *out = outputDelay.load(); return ASE_OK; }
    ASIOError getBufferSize(long* lo, long* hi, long* pref, long* gran) override {
        *lo = 64; *hi = 4096; *pref = preferred; *gran = -1; return ASE_OK;
    }
    ASIOError canSampleRate(double r) override { return (r == rate || allowRateChange) ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(double* r) override { *r = rate; return ASE_OK; }
    ASIOError setSampleRate(double r) override { rate = r; return ASE_OK; }
    ASIOError getClockSources(ASIOClockSource*, long*) override { return ASE_NotPresent; }
    ASIOError setClockSource(long) override { return ASE_NotPresent; }
    ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override { return ASE_NotPresent; }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        info->type = info->channel && rightType >= 0 ? rightType : type; return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* info, long channels, long frames, ASIOCallbacks* callbacks) override {
        Thread(); Check(channels == 2 && frames == preferred, "use current driver preference for stereo buffers");
        for (int ch = 0; ch < 2; ++ch) for (int half = 0; half < 2; ++half) {
            data[ch][half].fill(0xaa);
            info[ch].buffers[half] = data[ch][half].data();
        }
        cb = callbacks; created = true; return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        Thread(); Check(created && !running, "stop before dispose"); created = false; disposed = true; return ASE_OK;
    }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long, void*) override { return ASE_NotPresent; }
    ASIOError outputReady() override { return ASE_NotPresent; }
} driver;

HRESULT Factory(const CLSID&, IASIO** ptr) noexcept {
    driver.tid = GetCurrentThreadId(); *ptr = &driver; return S_OK;
}
WasapiSinkConfig Config() {
    WasapiSinkConfig c{};
    c.asioDriverClsid = L"{532E5FC0-C5A7-45C3-B0F3-B8D8EF1A9D29}";
    c.format = {48000, 2, 24, 32}; c.periodFrames = 960;
    return c;
}

void TestLayouts() {
    struct Layout { long type; unsigned bits, bytes; };
    for (const Layout l : {Layout{ASIOSTInt16LSB,16,2}, {ASIOSTInt24LSB,24,3},
             {ASIOSTInt32LSB,32,4}, {ASIOSTInt32LSB16,16,4}, {ASIOSTInt32LSB18,18,4},
             {ASIOSTInt32LSB20,20,4}, {ASIOSTInt32LSB24,24,4}, {ASIOSTFloat32LSB,32,4}}) {
        auto cfg = Config();
        cfg.format.validBits = l.bits < 24 ? 16 : 24;
        cfg.allowResampling = l.type == ASIOSTFloat32LSB;
        driver.type = l.type;
        AsioExclusiveSink sink(Factory);
        Source src;
        Check(sink.Open(cfg) == S_OK && sink.Format().validBits == l.bits && sink.BufferFrames() == 512,
              "report real negotiated layout");
        Check(sink.Start(src) == S_OK && !src.fills, "synchronous start callback consumes no media");
        src.starved = true;
        driver.cb->bufferSwitch(0, ASIOTrue);
        Check(sink.WaitingForSource() && !sink.EndOfStreamSubmitted(), "starvation is recoverable");
        src.starved = false;
        driver.cb->bufferSwitchTimeInfo(nullptr, 1, ASIOTrue);
        for (unsigned ch = 0; ch < 2; ++ch) {
            const std::int32_t original = ch ? 0x12345600 : INT32_MIN;
            const auto code = original >> (32u - l.bits);
            const float fp = static_cast<float>(original) * 0x1.0p-31f;
            const void* expected = l.type == ASIOSTFloat32LSB ? static_cast<const void*>(&fp) : &code;
            Check(!std::memcmp(driver.data[ch][1].data(), expected, l.bytes), "signed planar sample layout");
        }
        driver.cb->bufferSwitch(0, ASIOTrue); // one-frame tail in half zero
        Check(sink.EndOfStreamSubmitted() && !sink.EndOfStreamComplete(), "tail reaches hardware");
        Check(driver.data[1][0][l.bytes] == 0, "partial period zero padding");
        for (int i = 0; i < 2; ++i) driver.cb->bufferSwitch(1-(i&1), ASIOTrue);
        Check(!sink.EndOfStreamComplete(), "696-sample delay requires a third switch at 512 frames");
        driver.cb->bufferSwitch(1, ASIOTrue);
        Check(sink.EndOfStreamComplete() && src.fills == 2 && sink.Stats().submittedFrames == 513,
              "complete drain without consuming beyond EOS");
        std::thread closer([&]{ Check(SUCCEEDED(sink.DrainAndClose()), "drain and release"); });
        closer.join();
        Check(driver.disposed && driver.released, "buffers and driver released");
    }
}

void TestFailureAndResume() {
    auto cfg = Config();
    AsioExclusiveSink sink(Factory);
    Source src;
    driver.type = ASIOSTInt16LSB;
    Check(sink.Open(cfg) == AUDCLNT_E_UNSUPPORTED_FORMAT && driver.released, "no implicit precision loss");
    driver.type = ASIOSTInt32LSB24;
    driver.rightType = ASIOSTInt16LSB;
    Check(sink.Open(cfg) == AUDCLNT_E_UNSUPPORTED_FORMAT, "incompatible right channel rejected");
    driver.rightType = -1;
    cfg.format.sampleRate = 96000;
    Check(sink.Open(cfg) == AUDCLNT_E_UNSUPPORTED_FORMAT, "strict mode rejects unsupported source rate");
    cfg.allowResampling = true;
    Check(sink.Open(cfg) == S_OK && sink.Format().sampleRate == 48000, "opt-in negotiates supported sample rate");
    driver.failStart = true;
    Check(FAILED(sink.Start(src)) && !src.fills, "busy Start preserves queued source");
    sink.Close(); driver.failStart = false;
    Check(sink.Open(cfg) == S_OK && sink.Start(src) == S_OK, "reacquire the same driver");
    sink.SetResumeBlocked(true);
    for (int i=0; i<4; ++i) driver.cb->bufferSwitch(i&1, ASIOTrue);
    Check(!src.fills, "pause fence preserves source");
    sink.SetResumeBlocked(false);
    driver.cb->bufferSwitch(1, ASIOTrue);
    Check(src.remaining == 1, "resume reads from the next retained source frame");
    driver.outputDelay = 1024;
    Check(driver.cb->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr) == 1, "latency notice accepted");
    const auto deadline = GetTickCount64() + 1000;
    while (sink.Stats().outputLatencyFrames != 1024 && GetTickCount64() < deadline) Sleep(1);
    Check(sink.Stats().outputLatencyFrames == 1024 && !sink.ReconfigurationRequested(),
          "a latency change updates the running stream without reopening");
    driver.cb->asioMessage(kAsioBufferSizeChange, 1024, nullptr, nullptr);
    driver.cb->bufferSwitch(0, ASIOTrue);
    Check(sink.ReconfigurationRequested() && src.remaining == 1, "changed buffer stops consuming the old layout");
    sink.Close();
    driver.preferred = 1024;
    driver.allowRateChange = true;
    cfg.format.sampleRate = 96000;
    Check(sink.Open(cfg) == S_OK && sink.Format().sampleRate == 96000 &&
        sink.BufferFrames() == 1024 && sink.Start(src) == S_OK, "reopen reads the new clock and period");
    driver.cb->bufferSwitch(0, ASIOTrue);
    Check(src.remaining == 0 && sink.Stats().submittedFrames == 1, "preserve the remaining media after reopen");
    driver.cb->sampleRateDidChange(44100);
    Check(sink.ReconfigurationRequested(), "unexpected clock change requests renegotiation");
    sink.Close();
}

// Invoked only by an operator. No physical driver is loaded in the CTest run.
int Inspect(const wchar_t* clsid) {
    CLSID id{};
    if (FAILED(CLSIDFromString(clsid, &id)) || !ammod::output_device::IsRegisteredAsioDriver(id)) return 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    IASIO* d{};
    const auto hr = CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void**>(&d));
    double rate{};
    if (SUCCEEDED(hr) && d && d->init(GetDesktopWindow()) && d->getSampleRate(&rate) == ASE_OK) {
        long mi{}, ma{}, preferred{}, step{}, in{}, out{};
        d->getBufferSize(&mi, &ma, &preferred, &step);
        d->getLatencies(&in, &out);
        std::printf("currentRate=%.0f min=%ld max=%ld preferred=%ld inputDelay=%ld outputDelay=%ld\n",
                    rate, mi, ma, preferred, in, out);
    }
    if (d) d->Release();
    CoUninitialize();
    return rate > 0 ? 0 : 3;
}
int Hardware(const wchar_t* clsid, unsigned rate) {
    auto cfg = Config();
    cfg.asioDriverClsid = clsid;
    cfg.format.sampleRate = rate;
    cfg.format.validBits = 16;
    for (int cycle = 1; cycle <= 2; ++cycle) {
        AsioExclusiveSink sink;
        Source src; src.silent = true;
        const auto open = sink.Open(cfg);
        if (FAILED(open)) { std::printf("cycle=%d open=0x%08lx\n", cycle, static_cast<unsigned long>(open)); return 2; }
        src.remaining = sink.BufferFrames() * 5 + 1;
        const auto frames = src.remaining;
        const auto hr = sink.Start(src);
        if (FAILED(hr)) { std::printf("start=0x%08lx\n", static_cast<unsigned long>(hr)); return 3; }
        const auto deadline = GetTickCount64() + 4000;
        while (!sink.EndOfStreamComplete() && sink.State() == WasapiSinkState::Running &&
               GetTickCount64() < deadline) Sleep(3);
        const bool complete = sink.EndOfStreamComplete();
        const auto submitted = sink.Stats().submittedFrames;
        std::printf("cycle=%d rate=%u bits=%u container=%u period=%u outputDelay=%u frames=%llu eos=%d\n",
                    cycle, sink.Format().sampleRate, sink.Format().validBits, sink.Format().containerBits,
                    sink.BufferFrames(), sink.Stats().outputLatencyFrames, static_cast<unsigned long long>(submitted), complete);
        if (FAILED(sink.DrainAndClose()) || !complete || submitted != frames) return 4;
        if (cycle == 1) Sleep(100);
    }
    return 0;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !std::wcscmp(argv[1], L"--inspect")) return Inspect(argv[2]);
    if (argc == 4 && !std::wcscmp(argv[1], L"--silence"))
        return Hardware(argv[2], std::wcstoul(argv[3], nullptr, 10));
    if (argc != 1) return 1;
    TestLayouts();
    TestFailureAndResume();
    std::puts("ASIO output regressions passed");
    return 0;
}
