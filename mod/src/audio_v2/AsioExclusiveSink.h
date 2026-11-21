#pragma once

#include "AsioDriverApi.h"
#include "PlaybackSink.h"

#include <atomic>
#include <vector>

namespace ammod::audio_v2 {

using AsioDriverFactory = HRESULT (*)(const CLSID&, asio::IASIO**) noexcept;

class AsioExclusiveSink final : public IPlaybackSink {
public:
    explicit AsioExclusiveSink(AsioDriverFactory factory = nullptr) noexcept;
    ~AsioExclusiveSink() override { Close(); }

    HRESULT Open(const WasapiSinkConfig& config) noexcept override;
    HRESULT Start(IIntegerPcmSource& source) noexcept override;
    HRESULT DrainAndClose() noexcept override;
    void Stop() noexcept override;
    void Close() noexcept override;
    WasapiSinkState State() const noexcept override { return state_.load(); }
    WasapiSinkStats Stats() const noexcept override;
    const PcmFormat& Format() const noexcept override { return format_; }
    std::uint32_t BufferFrames() const noexcept override { return frames_; }
    bool WaitingForSource() const noexcept override { return waiting_.load(); }
    bool EndOfStreamSubmitted() const noexcept override { return eos_.load(); }
    bool EndOfStreamComplete() const noexcept override { return complete_.load(); }
    bool ReconfigurationRequested() const noexcept override { return reopen_.load(); }
    void SetResumeBlocked(bool value) noexcept override { blocked_.store(value); }

private:
    enum class Command { Start, Stop, Exit };
    static DWORD WINAPI RunThread(void* context) noexcept;
    void Run() noexcept;
    HRESULT Call(Command command) noexcept;
    HRESULT CreateDriverBuffers() noexcept;
    void StopDriver() noexcept;
    void RefreshLatency() noexcept;
    void ReleaseDriver() noexcept;
    void Render(long half) noexcept;
    void Fail(HRESULT error) noexcept;
    static void Switch(long half, asio::ASIOBool direct) noexcept;
    static asio::ASIOTime* SwitchTime(asio::ASIOTime* time, long half, asio::ASIOBool direct) noexcept;
    static void RateChanged(double rate) noexcept;
    static long Message(long selector, long value, void*, double*) noexcept;

    AsioDriverFactory factory_;
    WasapiSinkConfig config_{};
    CLSID clsid_{};
    HANDLE thread_{}, request_{}, reply_{}, drained_{}, latencyNotice_{};
    Command command_{Command::Exit};
    HRESULT result_{E_FAIL};
    asio::IASIO* driver_{};
    asio::ASIOCallbacks callbacks_{Switch, RateChanged, Message, SwitchTime};
    asio::ASIOBufferInfo buffers_[2]{};
    PcmFormat format_{};
    asio::ASIOSampleType sampleType_{};
    std::uint32_t frames_{};
    std::atomic<std::uint32_t> drainPeriods_{}, outputLatency_{};
    std::atomic<bool> reopen_{};
    std::atomic<std::uint32_t> expectedRate_{};
    bool buffersCreated_{}, started_{}, outputReady_{};
    std::vector<std::int32_t> scratch_;
    IIntegerPcmSource* source_{};
    std::atomic<WasapiSinkState> state_{WasapiSinkState::Closed};
    std::atomic<bool> running_{}, waiting_{}, eos_{}, complete_{}, blocked_{}, drainRequested_{};
    unsigned quietPeriods_{}; // render callback only
    std::atomic<std::uint64_t> submitted_{}, packets_{}, silence_{}, waits_{}, starts_{};
    std::atomic<HRESULT> error_{S_OK};
};

} // namespace ammod::audio_v2
