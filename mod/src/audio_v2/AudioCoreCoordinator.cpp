#include "AudioCoreCoordinator.h"
#include "../AudioErrors.h"

#include <new>

namespace ammod::audio_v2 {

AudioCoreCoordinator::AudioCoreCoordinator() noexcept
    : outputGate_(gate_) {}

AudioCoreCoordinator::~AudioCoreCoordinator() {
    Disable();
}

HRESULT AudioCoreCoordinator::Enable(const AudioCoreCoordinatorConfig& config) noexcept {
    // Enable may follow a runtime-side retirement that already closed this
    // coordinator. Keep Disable() as the safety boundary, but make the no-op
    // case cheap so one Apple graph transition cannot multiply Stop/Close
    // work through Runtime -> Coordinator -> Sink layers.
    Disable();
    if (!IsValidFormat(config.sink.format) || config.queueCapacity == 0 ||
        config.mediaGeneration == 0) {
        return E_INVALIDARG;
    }

    sourceReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!sourceReadyEvent_) return HRESULT_FROM_WIN32(GetLastError());

    gate_.SetUiEnabled(true);
    const auto token = gate_.BeginCapture();
    if (!token) {
        gate_.SetUiEnabled(false);
        CloseHandle(sourceReadyEvent_);
        sourceReadyEvent_ = nullptr;
        return E_UNEXPECTED;
    }

    try {
        queue_ = std::make_unique<PcmQueue>(config.queueCapacity);
    } catch (const std::bad_alloc&) {
        gate_.SetUiEnabled(false);
        CloseHandle(sourceReadyEvent_);
        sourceReadyEvent_ = nullptr;
        return E_OUTOFMEMORY;
    }

    config_ = config;
    SelectConfiguredSink();
    sourceFormat_ = config.sink.format;
    config_.sink.sourceReadyEvent = sourceReadyEvent_;
    token_ = *token;
    nextSequence_ = 0;
    verifier_.Reset(sourceFormat_, config_.mediaGeneration);
    BitPerfectVerifier* verifier =
        config_.verifyBitPerfect &&
        config_.mappingPolicy == PcmMappingPolicy::ExactSourceInteger
            ? &verifier_ : nullptr;
    source_.emplace(*queue_, sourceFormat_, config_.mediaGeneration,
                    verifier, config_.mappingPolicy);
    capturedFrames_.store(0, std::memory_order_release);
    droppedFrames_.store(0, std::memory_order_release);
    return S_OK;
}

void AudioCoreCoordinator::Disable() noexcept {
    if (!gate_.UiEnabled() && !queue_ && !source_ && !sourceReadyEvent_ &&
        Output().State() == WasapiSinkState::Closed) {
        return;
    }

    // Invalidate the callback token first. The hook owner must quiesce the P0
    // callback before this method is called; only then is consumer-side Clear
    // safe under the SPSC contract.
    outputGate_.Reset();
    gate_.SetUiEnabled(false);
    if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
    Output().Stop();
    Output().Close();
    endpointReleased_ = false;
    resumeConfig_ = {};
    resampledSource_.Reset();
    source_.reset();
    if (queue_) queue_->Clear();
    queue_.reset();
    if (sourceReadyEvent_) {
        CloseHandle(sourceReadyEvent_);
        sourceReadyEvent_ = nullptr;
    }
    token_ = {};
    nextSequence_ = 0;
}

bool AudioCoreCoordinator::PushResult(bool pushed, std::uint32_t frames) noexcept {
    if (pushed) {
        capturedFrames_.fetch_add(frames, std::memory_order_relaxed);
        // The producer notification exists only to restart a sink that had to
        // stop for source starvation. Signaling it for every healthy P0 block
        // needlessly wakes the real-time render thread at producer cadence.
        if (sourceReadyEvent_ && Output().WaitingForSource()) SetEvent(sourceReadyEvent_);
    } else {
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        gate_.Fail(token_);
    }
    return pushed;
}

bool AudioCoreCoordinator::PushP0Interleaved(std::uint64_t firstFrame,
                                             const float* interleaved,
                                             std::uint32_t frames,
                                             bool discontinuity,
                                             bool endOfStream) noexcept {
    if (!queue_ || !gate_.CanCapture(token_)) {
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }
    const bool pushed = queue_->TryPush(sourceFormat_, config_.mediaGeneration,
                                        nextSequence_, firstFrame, interleaved, frames,
                                        discontinuity, endOfStream);
    return PushResult(pushed, frames);
}

bool AudioCoreCoordinator::PushP0Planar(std::uint64_t firstFrame,
                                        const float* left,
                                        const float* right,
                                        std::uint32_t frames,
                                        bool discontinuity,
                                        bool endOfStream) noexcept {
    if (!queue_ || !gate_.CanCapture(token_)) {
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }
    const bool pushed = queue_->TryPushPlanar(sourceFormat_, config_.mediaGeneration,
                                              nextSequence_, firstFrame, left, right, frames,
                                              discontinuity, endOfStream);
    return PushResult(pushed, frames);
}

HRESULT AudioCoreCoordinator::OpenEndpoint() noexcept {
    if (!queue_ || !source_ || !gate_.CanOpenEndpoint(token_)) return E_UNEXPECTED;
    if (queue_->Size() == 0) return E_PENDING;

    HRESULT hr = Output().Open(config_.sink);
    if (UsesAsio() && hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
        hr = config_.sink.allowResampling ? audio::kExclusiveFormatUnavailable : audio::kBitPerfectFormatUnavailable;
    if (SUCCEEDED(hr) && IsConverting()) {
        hr = resampledSource_.Configure(*source_, sourceFormat_, Output().Format(),
                                         Output().BufferFrames());
    }
    if (FAILED(hr)) {
        Output().Close();
        resampledSource_.Reset();
        outputGate_.Reset();
        gate_.Fail(token_);
        return hr;
    }
    if (!gate_.MarkPrebuffered(token_)) {
        Output().Close();
        outputGate_.Reset();
        gate_.Fail(token_);
        return E_UNEXPECTED;
    }
    return S_OK;
}

HRESULT AudioCoreCoordinator::Start() noexcept {
    if (!source_ || !gate_.CanMutateAudio(token_) ||
        gate_.Phase() != AudioCoreGatePhase::Prebuffered) {
        return E_UNEXPECTED;
    }
    const HRESULT hr = Output().Start(ActiveSource());
    if (FAILED(hr)) {
        outputGate_.Reset();
        Output().Close();
        gate_.Fail(token_);
        return hr;
    }
    if (!outputGate_.CommitCutover(token_)) {
        Output().Stop();
        Output().Close();
        outputGate_.Rollback(token_);
        return E_UNEXPECTED;
    }
    return S_OK;
}

void AudioCoreCoordinator::SelectConfiguredSink() noexcept {
    sink_.store(config_.sink.asioDriverClsid.empty() ? static_cast<IPlaybackSink*>(&wasapi_)
                                                     : static_cast<IPlaybackSink*>(&asio_));
}

void AudioCoreCoordinator::SetAsioDriver(std::wstring_view clsid) {
    if (config_.sink.asioDriverClsid == clsid) return;
    config_.sink.asioDriverClsid.assign(clsid);
    if (endpointReleased_) resumeConfig_.asioDriverClsid.assign(clsid);
    // An active backend keeps its callback ownership through Stop/Close.
    if (Output().State() == WasapiSinkState::Closed) SelectConfiguredSink();
}

void AudioCoreCoordinator::PauseEndpoint() noexcept {
    if (gate_.Phase() == AudioCoreGatePhase::Active &&
        !endpointReleased_ && Output().State() != WasapiSinkState::Closed) {
        resumeConfig_ = config_.sink;
        resumeFormat_ = Output().Format();
        // Re-query the selected/default route on resume: a video application
        // may have changed the Windows endpoint while music was paused.
        if (config_.followDefaultEndpointOnResume) resumeConfig_.endpointId.clear();
        resumeConfig_.primeWithSilence = true;
        (void)Output().DrainAndClose();
        SelectConfiguredSink();
        endpointReleased_ = true;
    }
}

HRESULT AudioCoreCoordinator::ResumeEndpoint() noexcept {
    if (!source_ || gate_.Phase() != AudioCoreGatePhase::Active) return E_UNEXPECTED;
    if (Output().State() == WasapiSinkState::Running) return S_OK;
    if (endpointReleased_) {
        const HRESULT open = Output().Open(resumeConfig_);
        if (UsesAsio() && open == AUDCLNT_E_UNSUPPORTED_FORMAT)
            return resumeConfig_.allowResampling ? audio::kExclusiveFormatUnavailable : audio::kBitPerfectFormatUnavailable;
        if (FAILED(open)) return open;
        const auto& expected = resumeFormat_;
        const auto& actual = Output().Format();
        // Converter lookahead belongs to the existing output format. Keep it
        // intact across pause; never silently reconfigure/drop staged samples.
        const bool wasConverted = RequiresPcmConversion(sourceFormat_, expected);
        const bool needsConversion = IsConverting();
        const bool sameConversion = wasConverted && needsConversion &&
            actual.sampleRate == expected.sampleRate && actual.validBits == expected.validBits &&
            actual.encoding == expected.encoding;
        if (sameConversion) {
            const auto hr = resampledSource_.ReserveOutputFrames(Output().BufferFrames());
            if (FAILED(hr)) { Output().Close(); return hr; }
        } else if (wasConverted || needsConversion) {
            if (resampledSource_.PendingSourceFrames()) {
                Output().Close();
                return audio::kExclusiveFormatUnavailable;
            }
            resampledSource_.Reset();
            if (needsConversion) {
                const auto hr = resampledSource_.Configure(*source_, sourceFormat_, actual, Output().BufferFrames());
                if (FAILED(hr)) { Output().Close(); return hr; }
            }
            // A transient Start failure can retry this exact prepared layout.
            resumeFormat_ = actual;
        }
        const HRESULT start = Output().Start(ActiveSource());
        if (FAILED(start)) {
            Output().Close();
            return start;
        }
        endpointReleased_ = false;
        return S_OK;
    }
    if (Output().State() != WasapiSinkState::Open) return E_UNEXPECTED;
    return Output().Start(ActiveSource());
}

IIntegerPcmSource& AudioCoreCoordinator::ActiveSource() noexcept {
    return IsConverting()
        ? static_cast<IIntegerPcmSource&>(resampledSource_)
        : static_cast<IIntegerPcmSource&>(*source_);
}

bool AudioCoreCoordinator::ShouldSuppressNative() const noexcept {
    // The exclusive client remains the sole endpoint owner during a recoverable
    // P0/network wait.  It is stopped without acquiring a buffer and resumed
    // only after a complete source buffer is available.  Reopening Apple's
    // native path here would create a second owner while the exclusive client
    // is still initialized, and would advance Apple's timeline past the P0
    // frames waiting in this generation.
    return outputGate_.ShouldSuppress(token_);
}

const AudioCoreCoordinatorStats AudioCoreCoordinator::Stats() const noexcept {
    AudioCoreCoordinatorStats stats{};
    stats.capturedFrames = capturedFrames_.load(std::memory_order_acquire);
    stats.droppedFrames = droppedFrames_.load(std::memory_order_acquire);
    if (source_) stats.source = source_->Stats();
    return stats;
}

} // namespace ammod::audio_v2
