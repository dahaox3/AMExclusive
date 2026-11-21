#include "AsioExclusiveSink.h"

#include "../output_device/AsioDevices.h"
#include "ResamplingPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ammod::audio_v2 {
namespace {
using namespace asio;

// ASIO callbacks have no context parameter. Only one driver may publish its
// callbacks in this process. Readers enter before loading the target so that
// disposeBuffers and COM Release cannot race an in-flight callback.
std::atomic<AsioExclusiveSink*> active{};
std::atomic<unsigned> readers{};
struct CallbackScope {
    AsioExclusiveSink* target;
    CallbackScope() noexcept { readers.fetch_add(1); target = active.load(); }
    ~CallbackScope() { readers.fetch_sub(1); }
};

HRESULT Activate(const CLSID& id, IASIO** driver) noexcept {
    try {
        if (!output_device::IsRegisteredAsioDriver(id)) return REGDB_E_CLASSNOTREG;
    } catch (...) { return E_OUTOFMEMORY; }
    return CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void**>(driver));
}

bool SetLayout(long type, PcmFormat& f) noexcept {
    switch (type) {
    case ASIOSTInt16LSB: f.validBits = f.containerBits = 16; break;
    case ASIOSTInt24LSB: f.validBits = f.containerBits = 24; break;
    case ASIOSTInt32LSB: f.validBits = f.containerBits = 32; break;
    case ASIOSTInt32LSB16: f.validBits = 16; f.containerBits = 32; break;
    case ASIOSTInt32LSB18: f.validBits = 18; f.containerBits = 32; break;
    case ASIOSTInt32LSB20: f.validBits = 20; f.containerBits = 32; break;
    case ASIOSTInt32LSB24: f.validBits = 24; f.containerBits = 32; break;
    case ASIOSTFloat32LSB:
        f.validBits = f.containerBits = 32;
        f.encoding = PcmEncoding::IeeeFloat;
        break;
    default: return false;
    }
    return true;
}

void WriteChannel(BYTE* dst, const std::int32_t* src, std::uint32_t frames,
                  const PcmFormat& format) noexcept {
    const auto bytes = format.containerBits / 8u;
    for (std::uint32_t i = 0; i < frames; ++i, src += 2, dst += bytes) {
        if (format.encoding == PcmEncoding::IeeeFloat) {
            const float sample = static_cast<float>(*src) * 0x1.0p-31f;
            std::memcpy(dst, &sample, sizeof(sample));
        } else {
            const auto sample = *src >> (32u - format.validBits);
            // ASIO's narrow-in-32 slots are sign extended and right aligned.
            std::memcpy(dst, &sample, bytes);
        }
    }
}
} // namespace

AsioExclusiveSink::AsioExclusiveSink(AsioDriverFactory factory) noexcept
    : factory_(factory ? factory : Activate) {}

HRESULT AsioExclusiveSink::Open(const WasapiSinkConfig& config) noexcept {
    Close();
    if (!IsValidFormat(config.format) || config.asioDriverClsid.size() != 38 ||
        config.asioDriverClsid.front() != L'{' ||
        FAILED(CLSIDFromString(config.asioDriverClsid.c_str(), &clsid_))) return E_INVALIDARG;
    try { config_ = config; } catch (...) { return E_OUTOFMEMORY; }
    submitted_ = packets_ = silence_ = waits_ = starts_ = 0;
    quietPeriods_ = 0;
    running_ = waiting_ = eos_ = complete_ = blocked_ = drainRequested_ = reopen_ = false;
    error_ = S_OK;
    request_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    reply_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    drained_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    latencyNotice_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (request_ && reply_ && drained_ && latencyNotice_)
        thread_ = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
    if (!thread_) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Close();
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    WaitForSingleObject(reply_, INFINITE);
    const HRESULT hr = result_;
    if (FAILED(hr)) Close();
    return hr;
}

HRESULT AsioExclusiveSink::Call(Command command) noexcept {
    command_ = command;
    SetEvent(request_);
    const HANDLE handles[]{reply_, thread_};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 ? result_ : E_FAIL;
}

DWORD WINAPI AsioExclusiveSink::RunThread(void* context) noexcept {
    static_cast<AsioExclusiveSink*>(context)->Run();
    return 0;
}

void AsioExclusiveSink::Run() noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    result_ = FAILED(com) ? com : CreateDriverBuffers();
    if (SUCCEEDED(result_)) state_ = WasapiSinkState::Open;
    SetEvent(reply_);
    const HANDLE events[]{request_, latencyNotice_};
    for (;;) {
        const DWORD wake = MsgWaitForMultipleObjectsEx(2, events, INFINITE, QS_ALLINPUT,
                                                      MWMO_INPUTAVAILABLE);
        if (wake == WAIT_OBJECT_0 + 1) {
            if (buffersCreated_) RefreshLatency();
            continue;
        }
        if (wake == WAIT_OBJECT_0 + 2) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        if (wake != WAIT_OBJECT_0 || command_ == Command::Exit) break;
        result_ = S_OK;
        if (command_ == Command::Start) {
            if (!driver_ || driver_->start() != asio::ASE_OK) {
                result_ = AUDCLNT_E_DEVICE_IN_USE;
            } else {
                started_ = true;
                ++starts_;
                state_ = WasapiSinkState::Running;
                running_ = true;
            }
        } else StopDriver();
        SetEvent(reply_);
    }
    ReleaseDriver();
    if (SUCCEEDED(com)) CoUninitialize();
    state_ = WasapiSinkState::Closed;
}

HRESULT AsioExclusiveSink::CreateDriverBuffers() noexcept {
    using namespace asio;
    const HRESULT activated = factory_(clsid_, &driver_);
    if (FAILED(activated)) return activated;
    if (!driver_ || !driver_->init(GetDesktopWindow())) return AUDCLNT_E_DEVICE_IN_USE;
    format_ = config_.format;
    double current{};
    driver_->getSampleRate(&current);
    const auto hint = current >= 8000 && current <= 768000 ? static_cast<unsigned>(current) : 0u;
    const auto rates = SelectOutputSampleRates(config_.format.sampleRate, config_.allowResampling, hint);
    expectedRate_ = 0;
    for (std::size_t i = 0; i < rates.count; ++i) {
        const auto rate = rates.values[i];
        if (current != rate && (driver_->canSampleRate(rate) != ASE_OK ||
                                driver_->setSampleRate(rate) != ASE_OK)) continue;
        if (driver_->getSampleRate(&current) == ASE_OK && std::abs(current - rate) < 0.5) {
            expectedRate_ = format_.sampleRate = rate;
            break;
        }
    }
    if (!expectedRate_) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    long inputs{}, outputs{};
    if (driver_->getChannels(&inputs, &outputs) != ASE_OK || outputs < 2)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    format_.sourceValidBits = EffectiveSourceValidBits(config_.format);
    for (long ch = 0; ch < 2; ++ch) {
        ASIOChannelInfo info{};
        info.channel = ch;
        if (driver_->getChannelInfo(&info) != ASE_OK) return E_FAIL;
        if (ch == 0) sampleType_ = info.type;
        if (info.type != sampleType_ || !SetLayout(info.type, format_))
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (!IsValidEndpointFormat(format_, config_.allowResampling))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    long minimum{}, maximum{}, preferred{}, granularity{};
    if (driver_->getBufferSize(&minimum, &maximum, &preferred, &granularity) != ASE_OK ||
        minimum <= 0 || preferred < minimum || preferred > maximum || preferred > 65536)
        return AUDCLNT_E_BUFFER_SIZE_ERROR;
    frames_ = static_cast<unsigned>(preferred);
    try { scratch_.resize(static_cast<std::size_t>(frames_) * 2u); }
    catch (...) { return E_OUTOFMEMORY; }
    buffers_[0] = {};
    buffers_[1] = {};
    buffers_[1].channelNum = 1;
    AsioExclusiveSink* empty{};
    if (!active.compare_exchange_strong(empty, this)) return AUDCLNT_E_DEVICE_IN_USE;
    if (driver_->createBuffers(buffers_, 2, preferred, &callbacks_) != ASE_OK)
        return AUDCLNT_E_DEVICE_IN_USE;
    buffersCreated_ = true;
    for (const auto& channel : buffers_) {
        for (void* buffer : channel.buffers) {
            if (!buffer) return E_POINTER;
            std::memset(buffer, 0, static_cast<size_t>(frames_) * (format_.containerBits / 8u));
        }
    }
    RefreshLatency();
    outputReady_ = driver_->outputReady() == ASE_OK;
    return S_OK;
}

void AsioExclusiveSink::RefreshLatency() noexcept {
    long input{}, output{};
    if (driver_->getLatencies(&input, &output) != asio::ASE_OK || output < 0)
        output = static_cast<long>(frames_ * 2u);
    outputLatency_ = static_cast<unsigned>(output);
    drainPeriods_ = std::max(2u, 1u + (static_cast<unsigned>(output) + frames_ - 1u) / frames_);
}

HRESULT AsioExclusiveSink::Start(IIntegerPcmSource& source) noexcept {
    if (State() != WasapiSinkState::Open) return E_UNEXPECTED;
    source_ = &source;
    return Call(Command::Start);
}

void AsioExclusiveSink::Fail(HRESULT hr) noexcept {
    error_ = hr;
    state_ = WasapiSinkState::Faulted;
    SetEvent(drained_);
}

void AsioExclusiveSink::Render(long half) noexcept {
    if (half < 0 || half > 1 || !running_) return;
    std::uint32_t written{};
    bool ended = false;
    bool havePacket = false;
    if (!reopen_ && !drainRequested_ && !blocked_ && !eos_ && State() == WasapiSinkState::Running) {
        HRESULT hr = source_->Prepare(frames_);
        if (hr == S_OK) {
            hr = source_->Fill(scratch_.data(), frames_, written, ended);
            if (hr == S_OK && (written > frames_ || (written < frames_ && !ended))) hr = E_UNEXPECTED;
        } else if (hr == S_FALSE) ended = true;
        if (hr == kAudioSourceWouldBlock) {
            if (!waiting_.exchange(true)) ++waits_;
            written = 0;
            ended = false;
        } else if (FAILED(hr)) {
            Fail(hr);
            written = 0;
            ended = false;
        } else {
            waiting_ = false;
            havePacket = written != 0;
        }
    }
    const auto bytes = format_.containerBits / 8u;
    for (unsigned channel = 0; channel < 2; ++channel) {
        auto* dst = static_cast<BYTE*>(buffers_[channel].buffers[half]);
        if (written) WriteChannel(dst, scratch_.data() + channel, written, format_);
        if (written < frames_) std::memset(dst + written * bytes, 0, (frames_ - written) * bytes);
    }
    if (outputReady_) driver_->outputReady();
    if (havePacket) { submitted_ += written; ++packets_; }
    silence_ += frames_ - written;
    if (ended) eos_ = true;
    if (havePacket) quietPeriods_ = 0;
    else if (quietPeriods_ < drainPeriods_) ++quietPeriods_;
    if ((drainRequested_ || (eos_ && !complete_)) && quietPeriods_ >= drainPeriods_) {
        if (eos_) complete_ = true;
        SetEvent(drained_);
    }
}

void AsioExclusiveSink::Switch(long half, asio::ASIOBool) noexcept {
    CallbackScope scope;
    if (scope.target) scope.target->Render(half);
}
asio::ASIOTime* AsioExclusiveSink::SwitchTime(asio::ASIOTime* time, long half, asio::ASIOBool direct) noexcept {
    Switch(half, direct);
    return time;
}
void AsioExclusiveSink::RateChanged(double rate) noexcept {
    CallbackScope scope;
    if (scope.target && scope.target->running_ &&
        std::abs(rate - scope.target->expectedRate_.load()) >= 0.5)
        scope.target->reopen_ = true;
}
long AsioExclusiveSink::Message(long selector, long value, void*, double*) noexcept {
    using namespace asio;
    if (selector == kAsioSelectorSupported)
        return value == kAsioEngineVersion || value == kAsioSupportsTimeInfo ||
               value == kAsioResetRequest || value == kAsioResyncRequest ||
               value == kAsioLatenciesChanged || value == kAsioBufferSizeChange;
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioSupportsTimeInfo) return 1;
    if (selector != kAsioResetRequest && selector != kAsioResyncRequest &&
        selector != kAsioLatenciesChanged && selector != kAsioBufferSizeChange) return 0;
    CallbackScope scope;
    if (scope.target && scope.target->running_) {
        if (selector == kAsioLatenciesChanged) SetEvent(scope.target->latencyNotice_);
        else scope.target->reopen_ = true;
    }
    return 1;
}

void AsioExclusiveSink::StopDriver() noexcept {
    running_ = false;
    if (started_) { driver_->stop(); started_ = false; }
    if (buffersCreated_) state_ = WasapiSinkState::Open;
}

void AsioExclusiveSink::ReleaseDriver() noexcept {
    StopDriver();
    AsioExclusiveSink* self = this;
    if (active.compare_exchange_strong(self, nullptr)) {
        while (readers.load()) Sleep(1);
    }
    if (buffersCreated_) { driver_->disposeBuffers(); buffersCreated_ = false; }
    if (driver_) { driver_->Release(); driver_ = nullptr; }
}

void AsioExclusiveSink::Stop() noexcept {
    if (thread_ && (State() == WasapiSinkState::Running || State() == WasapiSinkState::Faulted))
        (void)Call(Command::Stop);
}

HRESULT AsioExclusiveSink::DrainAndClose() noexcept {
    HRESULT hr = error_;
    if (State() == WasapiSinkState::Running) {
        drainRequested_ = true;
        const auto ms = static_cast<std::uint64_t>(drainPeriods_ + 1u) * frames_ * 1000u / format_.sampleRate;
        const auto timeout = static_cast<DWORD>(std::clamp<std::uint64_t>(ms + 250u, 300u, 2000u));
        hr = WaitForSingleObject(drained_, timeout) == WAIT_OBJECT_0
            ? error_.load() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    Close();
    return hr;
}

void AsioExclusiveSink::Close() noexcept {
    if (thread_) {
        command_ = Command::Exit;
        SetEvent(request_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    for (HANDLE* handle : {&request_, &reply_, &drained_, &latencyNotice_}) {
        if (*handle) { CloseHandle(*handle); *handle = nullptr; }
    }
    source_ = nullptr;
    frames_ = 0;
    waiting_ = false;
    state_ = WasapiSinkState::Closed;
}

WasapiSinkStats AsioExclusiveSink::Stats() const noexcept {
    WasapiSinkStats stats{};
    stats.submittedFrames = submitted_;
    stats.submittedBuffers = packets_;
    stats.controlledSilenceFrames = silence_;
    stats.sourceWaits = waits_;
    stats.clientStarts = starts_;
    stats.lastError = error_;
    stats.endpointPeriodFrames = frames_;
    stats.outputLatencyFrames = outputLatency_;
    return stats;
}

} // namespace ammod::audio_v2
