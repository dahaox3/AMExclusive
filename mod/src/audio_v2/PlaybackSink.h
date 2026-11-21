#pragma once

#include "IntegerPcmSource.h"
#include "PcmTypes.h"

#include <string>

namespace ammod::audio_v2 {

// Lifecycle notifications are optional and diagnostic-only.  They are invoked
// on the thread that performs the corresponding sink operation and carry the
// HRESULT observed at the operation boundary.  The production hook uses them
// to build a QPC ownership timeline; tests leave the callback unset.
enum class WasapiSinkLifecycleEvent : std::uint8_t {
    OpenEnter,
    OpenExit,
    InitializeEnter,
    InitializeExit,
    StartEnter,
    StartExit,
    StopEnter,
    StopExit,
    CloseEnter,
    CloseExit,
};

using WasapiSinkLifecycleCallback = void (*)(
    void* context, WasapiSinkLifecycleEvent event, HRESULT result) noexcept;

struct WasapiSinkConfig final {
    PcmFormat format{};
    std::array<PcmFormat, kMaxFormatCandidates> formatCandidates{};
    std::uint32_t formatCandidateCount{};
    std::wstring endpointId;
    std::wstring asioDriverClsid; // Empty selects WASAPI.
    std::uint32_t periodFrames{};
    std::uint32_t alignmentRetries{2};
    bool allowResampling{};
    // One zero-filled device period before the first media packet. Opt-in
    // for an endpoint rebuilt after pause; never consume/drop source frames.
    bool primeWithSilence{};
    // Non-owning producer notification. The coordinator owns this handle and
    // keeps it valid until the sink has stopped and closed.
    HANDLE sourceReadyEvent{};
    void* lifecycleContext{};
    WasapiSinkLifecycleCallback onLifecycle{};
};

struct WasapiSinkStats final {
    std::uint64_t submittedFrames{};
    std::uint64_t submittedBuffers{};
    std::uint64_t controlledSilenceFrames{};
    std::uint64_t sourceWaits{};
    std::uint64_t underruns{};
    std::uint64_t getBufferFailures{};
    std::uint64_t releaseBufferFailures{};
    HRESULT lastError{S_OK};
    std::uint64_t clockPosition{};
    std::uint64_t clockTarget{};
    std::uint64_t clockFrequency{};
    std::uint64_t drainElapsedMs{};
    HRESULT clockReadResult{S_OK};
    // Successful IAudioClient::Start calls this stream, and how many of them
    // happened before any frame was in the endpoint buffer. The second value
    // must stay 0: the first Start of a stream follows priming, and a sealed
    // empty stream never starts the endpoint at all.
    std::uint64_t clientStarts{};
    std::uint64_t startsBeforeFirstBuffer{};
    std::uint32_t endpointPeriodFrames{}, outputLatencyFrames{};
};

enum class WasapiSinkState : std::uint8_t {
    Closed,
    Open,
    Running,
    Faulted,
};

// Endpoint control is serialized by the coordinator. The producer reads only
// WaitingForSource() while the selected backend is fixed for its generation.
class IPlaybackSink {
public:
    virtual ~IPlaybackSink() = default;
    virtual HRESULT Open(const WasapiSinkConfig& config) noexcept = 0;
    virtual HRESULT Start(IIntegerPcmSource& source) noexcept = 0;
    virtual HRESULT DrainAndClose() noexcept = 0;
    virtual void Stop() noexcept = 0;
    virtual void Close() noexcept = 0;
    virtual WasapiSinkState State() const noexcept = 0;
    virtual WasapiSinkStats Stats() const noexcept = 0;
    virtual const PcmFormat& Format() const noexcept = 0;
    virtual std::uint32_t BufferFrames() const noexcept = 0;
    virtual bool WaitingForSource() const noexcept = 0;
    virtual bool EndOfStreamSubmitted() const noexcept = 0;
    virtual bool EndOfStreamComplete() const noexcept = 0;
    virtual bool ReconfigurationRequested() const noexcept { return false; }
    virtual void SetResumeBlocked(bool blocked) noexcept = 0;
};

} // namespace ammod::audio_v2
