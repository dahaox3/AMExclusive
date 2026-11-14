#include "audio_v2/AudioCoreRuntime.h"
#include "SettingsRestartPolicy.h"
#include <cstdio>
#include <cstdlib>

void Check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}

namespace ammod::audio_v2 {
struct AudioCoreRuntimeRegression {
    static void Register(AudioCoreRuntime& r, void* converter, unsigned rate = 96000) {
        ConverterRegistration item{};
        item.converter = converter;
        item.outputFormat = {rate, 2, 24, 24, PcmEncoding::SignedInteger, true, 24};
        item.sourceBitDepth = 24;
        item.encodedFormat = 0x616c6163;
        item.observationId = 1;
        item.mediaGeneration = 1;
        Check(r.registry_.Register(item), "register decoder");
    }
    static void Bind(AudioCoreRuntime& r, void* converter, unsigned rate = 96000) {
        r.uiEnabled_.store(true);
        r.graphUnit_ = reinterpret_cast<void*>(0x2000);
        r.graphConverter_ = converter;
        r.graphRate_ = rate; r.graphChannels_ = 2; r.graphSourceBits_ = 24;
        r.graphObservationId_ = 1;
        r.TryBindGraphLocked();
        Check(r.graphBound_, "capture bound without acquiring audio hardware");
    }
    static void Drain(AudioCoreRuntime& r, void* converter) {
        ConverterRegistrationSnapshot snapshot{};
        Check(r.registry_.Snapshot(converter, snapshot), "drain registration exists");
        r.endOfStreamDrainPending_ = true;
        r.endOfStreamDrainConverter_ = converter;
        r.endOfStreamDrainRegistration_ = snapshot.registrationId;
        r.endOfStreamDrainLastTick_ = GetTickCount64();
        r.endOfStreamDrainRemainingMs_ = 0; // deterministically expire a missing EOF
    }
    static void Pcm(AudioCoreRuntime& r, void* converter, unsigned rate) {
        const float samples[]{0.25f, -0.25f, 0.5f, -0.5f};
        const ApplePcmFormatView format{kAppleLinearPcm, kAppleFormatFlagIsFloat,
                                      8, 1, 8, 2, 32, rate};
        const ApplePcmBufferView buffer{2, sizeof(samples), samples};
        Check(r.OnPcmOutput(converter, format, {1, &buffer}, 2), "capture known PCM frames");
    }
    static void PredecodeCases() {
        const auto decoder = reinterpret_cast<void*>(0x1000);
        const auto successor = reinterpret_cast<void*>(0x3000);
        const auto nextUnit = reinterpret_cast<void*>(0x4000);
        const auto prepare = [&](AudioCoreRuntime& r) {
            Register(r, decoder); Bind(r, decoder);
            r.OnAudioUnitStarted(r.graphUnit_);
            Register(r, successor, 48000);
            r.OnAudioUnitInitialized(nextUnit, successor, 48000, 2, 24, 1);
        };
        {
            AudioCoreRuntime r;
            prepare(r);
            Pcm(r, decoder, 96000);
            unsigned staged = 0;
            while (!r.ShouldDeferPcmPull(successor, 2) && staged < 300) {
                Pcm(r, successor, 48000);
                ++staged;
            }
            Check(staged > 0 && staged < 300, "next-song predecode has bounded backpressure");
            Check(r.coordinator_.BufferedFrames() == 2, "next-song PCM never enters current queue");
            Pcm(r, decoder, 96000);
            Check(r.coordinator_.BufferedFrames() == 4, "full prefetch leaves current decoder runnable");
            r.OnAudioUnitInitialized(r.graphUnit_, decoder, 96000, 2, 24, 1);
            r.OnAudioUnitStarted(nextUnit);
            Drain(r, decoder);
            r.OnAudioUnitUninitialized(r.graphUnit_);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() == successor && r.coordinator_.BufferedFrames() == staged * 2,
                  "old-unit teardown retains all staged successor PCM across EOF");
        }
        for (bool disposeDecoder : {false, true}) {
            AudioCoreRuntime r;
            prepare(r);
            if (disposeDecoder) r.OnConverterDisposed(successor);
            else {
                r.OnAudioUnitStopped(nextUnit);
                Check(r.graphStarted_ && r.BoundConverter() == decoder,
                      "stopping prefetch cannot stop the current song");
                r.OnAudioUnitUninitialized(nextUnit);
                r.registry_.Retire(successor);
            }
            Register(r, successor, 48000); // reused address, unrelated registration
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() != successor && !r.pendingCaptureOwner_,
                  "canceled or disposed prefetch cannot revive through pointer reuse");
        }
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder);
            const auto parent = reinterpret_cast<void*>(0x7000);
            const auto nextParent = reinterpret_cast<void*>(0x8000);
            r.OnAudioUnitStarted(parent);
            Register(r, successor, 48000);
            r.OnAudioUnitInitialized(nextUnit, successor, 48000, 2, 24, 1);
            r.OnAudioUnitStarted(nextParent);
            Check(r.BoundConverter() == decoder && r.graphOutputUnit_ == parent,
                  "prefetched parent Start preserves the current output identity");
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() == successor && r.graphOutputUnit_ == nextParent &&
                  r.graphStarted_, "parent-only Start is retained across natural handoff");
            r.OnAudioUnitStopped(parent);
            Check(r.graphStarted_ && r.BoundConverter() == successor,
                  "late old-parent Stop cannot stop the new parent");
        }
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder);
            const auto unit = r.graphUnit_;
            r.OnAudioUnitStarted(unit);
            Register(r, successor, 48000);
            r.OnAudioUnitInitialized(unit, successor, 48000, 2, 24, 1);
            r.OnAudioUnitStarted(unit);
            Check(r.BoundConverter() == decoder, "reused AudioUnit also defers a distinct decoder");
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() == successor && r.graphStarted_,
                  "reused AudioUnit promotes its next decoder after EOF");
        }
        for (bool confirmBeforeStart : {false, true}) {
            AudioCoreRuntime r;
            prepare(r);
            r.OnTransportSkipArm(10);
            if (confirmBeforeStart) {
                r.OnTransportSkip(10);
                r.TryBindPendingGraphLocked();
                Check(!r.graphBound_, "Next alone cannot choose an unconfirmed prefetched target");
            }
            r.OnAudioUnitStarted(nextUnit);
            if (!confirmBeforeStart) r.OnTransportSkip(10);
            Check(r.BoundConverter() == successor && r.graphStarted_,
                  "Next promotes exact prepared target in either IPC/native ordering");
        }
        {
            AudioCoreRuntime r;
            prepare(r);
            Register(r, successor, 48000); // missed Dispose must still fail the lifetime check
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() != successor && !r.pendingCaptureOwner_,
                  "changed registration identity invalidates a pending handoff");
        }
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder);
            r.OnAudioUnitStarted(r.graphUnit_);
            Register(r, successor, 48000);
            ConverterRegistrationSnapshot snapshot{};
            Check(r.registry_.Snapshot(successor, snapshot), "pending precision registration");
            snapshot.registration.sourceBitDepth = 0;
            Check(r.registry_.Register(snapshot.registration), "register before source cookie");
            r.OnAudioUnitInitialized(nextUnit, successor, 48000, 2, 24, 1);
            r.OnAudioUnitStarted(nextUnit);
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(!r.graphBound_ && r.pendingGraphBind_.converter == successor,
                  "missing source precision keeps the observed successor pending");
            r.OnConverterSourcePrecision(successor, 24, 48000, 2);
            r.TryBindPendingGraphLocked();
            Check(r.BoundConverter() == successor, "late source cookie completes the deferred bind");
        }
        {
            AudioCoreRuntime r;
            prepare(r);
            r.nativeStarted_ = true;
            r.OnAudioUnitStarted(nextUnit); // eager Start predates the user selection
            r.OnTransportSelectionArm(20);
            r.OnTransportSkip(20);
            r.OnAudioUnitStarted(reinterpret_cast<void*>(0x9000));
            Check(r.BoundConverter() == decoder,
                  "unknown parent Start cannot confirm a manually selected prefetch");
            r.OnAudioUnitStopped(r.graphUnit_);
            r.TryBindPendingGraphLocked();
            Check(!r.graphBound_, "row selection cannot play a different prefetched song");
            const auto selected = reinterpret_cast<void*>(0x5000);
            const auto selectedUnit = reinterpret_cast<void*>(0x6000);
            Register(r, selected, 44100);
            r.OnAudioUnitInitialized(selectedUnit, selected, 44100, 2, 24, 1);
            r.OnAudioUnitStarted(selectedUnit);
            Check(r.BoundConverter() == selected && !r.pendingCaptureOwner_,
                  "manual selection replaces unchosen prefetch immediately");
        }
        {
            AudioCoreRuntime r;
            prepare(r);
            r.OnTransportPlaybackState(30, false, 1);
            r.OnAudioUnitStarted(nextUnit);
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() == decoder && r.userPauseIntent_,
                  "prefetched Start cannot revoke an explicit pause");
            r.OnTransportPlaybackState(31, true, 1);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() == successor, "resume permits the pending natural handoff");
        }
        {
            AudioCoreRuntime r;
            prepare(r);
            r.OnTransportPlaybackState(40, false, 1);
            r.OnTransportSelectionArm(41);
            r.OnTransportSkip(41);
            r.OnTransportPlaybackState(42, false, 2);
            r.OnAudioUnitInitialized(nextUnit, successor, 48000, 2, 24, 1);
            r.OnAudioUnitStarted(nextUnit);
            Check(r.BoundConverter() == successor && r.userPauseIntent_,
                  "paused selection binds target while respecting authoritative pause");
            r.OnTransportPlaybackState(43, true, 2);
            Check(!r.userPauseIntent_ && r.BoundConverter() == successor,
                  "confirmed playback of selected song clears the old pause");
        }
        {
            AudioCoreRuntime r;
            prepare(r);
            r.OnTransportSeekBegin(50);
            Check(!r.pendingGraphBind_.converter && !r.pendingCaptureOwner_,
                  "seek cancels prefetched identity and PCM from the old timeline");
        }
    }
    static void Run() {
        PredecodeCases();
        const auto decoder = reinterpret_cast<void*>(0x1000);
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder);
            const auto currentUnit = r.graphUnit_;
            r.OnAudioUnitStarted(currentUnit);
            const auto generation = r.boundMediaGeneration_.load();
            const auto successor = reinterpret_cast<void*>(0x3000);
            const auto nextUnit = reinterpret_cast<void*>(0x4000);
            Register(r, successor, 48000);
            r.OnAudioUnitInitialized(nextUnit, successor, 48000, 2, 24, 1);
            Check(r.BoundConverter() == decoder && r.boundMediaGeneration_.load() == generation,
                  "predecoded AudioUnit init cannot retire the playing decoder");
            Check(r.graphUnit_ == currentUnit && r.graphRate_ == 96000,
                  "prefetch preserves current graph identity and sample rate");
            r.OnAudioUnitStarted(nextUnit);
            Check(r.BoundConverter() == decoder && r.graphOutputUnit_ != nextUnit,
                  "prefetched Start cannot become the current output parent");
            Drain(r, decoder);
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.BoundConverter() == successor && r.graphUnit_ == nextUnit &&
                  r.graphRate_ == 48000 && r.graphStarted_,
                  "natural EOF promotes the started format-changing successor");
            r.OnAudioUnitStopped(currentUnit);
            r.OnAudioUnitUninitialized(currentUnit);
            Check(r.BoundConverter() == successor && r.graphStarted_,
                  "late old-unit lifecycle cannot retire the promoted successor");
        }
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder);
            const auto generation = r.boundMediaGeneration_.load();
            Drain(r, decoder);
            r.endOfStreamDrainRemainingMs_ = 3000;
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.endOfStreamDrainPending_, "unexpired drain preserves tail");
            r.endOfStreamDrainRemainingMs_ = 0;
            r.userPauseIntent_ = true;
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.endOfStreamDrainPending_, "long user pause never expires tail");
            r.OnTransportSelectionArm(1);
            Check(!r.userPauseIntent_ && r.selectionAwaitingStart_, "selection clears stale pause but awaits play");
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.endOfStreamDrainPending_, "highlight alone cannot finalize/restart audio");
            r.selectionAwaitingStart_ = false;
            r.FinalizeEndOfStreamDrainLocked();
            Check(!r.endOfStreamDrainPending_ && r.graphBound_ &&
                  r.boundMediaGeneration_.load() > generation, "stalled EOF rearms reused live decoder");
            Check(r.coordinator_.Phase() == AudioCoreGatePhase::Capturing,
                  "rearmed decoder waits for real PCM, never opens device at stop");
        }
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder); Drain(r, decoder);
            r.registry_.Retire(decoder);
            Register(r, decoder); // same address, new registration
            r.FinalizeEndOfStreamDrainLocked();
            Check(!r.graphBound_, "retired/reused pointer cannot revive an unrelated decoder");
        }
        {
            AudioCoreRuntime r;
            Register(r, decoder); Bind(r, decoder); Drain(r, decoder);
            const auto successor = reinterpret_cast<void*>(0x3000);
            Register(r, successor, 48000);
            r.graphConverter_ = successor; r.graphRate_ = 48000;
            r.TryBindGraphLocked();
            r.FinalizeEndOfStreamDrainLocked();
            Check(r.graphBound_ && r.BoundConverter() == successor && r.graphRate_ == 48000,
                  "format-changing successor wins over same-converter EOF fallback");
        }
        {
            AudioCoreRuntime r;
            r.uiEnabled_.store(true); r.userPauseIntent_ = true;
            r.resumeProducerPending_ = r.resumePrebufferPending_ = true;
            r.OnTransportSelectionArm(7);
            Check(!r.resumeProducerPending_ && !r.resumePrebufferPending_, "selection clears old resume work");
            r.OnTransportPlaybackState(8, false, 42);
            Check(r.userPauseIntent_, "new explicit pause overrides selection");
            r.OnTransportPlaybackState(9, true, 43);
            Check(!r.userPauseIntent_ && !r.selectionAwaitingStart_, "confirmed new play removes pause fence");
            r.userPauseIntent_ = true;
            r.nativeStartedAfterUserPause_ = r.nativeStarted_ = r.graphStarted_ = true;
            r.OnTransportSelectionArm(10);
            Check(!r.selectionAwaitingStart_, "native Start before selection IPC must not leave a stale fence");
        }
    }
};
}

struct FakeTransport {
    ammod::media::Identity original{L"Same title", L"Artist A", L"Album", 1, 100};
    ammod::media::Identity neighbor{L"Same title", L"Artist B", L"Album", 2, 100};
    bool playing{true}, changed{}, previousAvailable{true}, singleton{}, wrongReturn{};
    int steps{}, seeks{};
    auto Read() { return changed ? neighbor : original; }
    bool Playing() { return playing; }
    bool SeekStart() { ++seeks; return true; }
    bool Step(bool previous) {
        ++steps;
        if (!changed && previous && !previousAvailable) return false;
        if (!singleton && !(changed && wrongReturn)) changed = !changed;
        return true;
    }
    void Pause() { playing = false; }
    void Wait() {}
};

int main() {
    ammod::audio_v2::AudioCoreRuntimeRegression::Run();
    FakeTransport t;
    Check(ammod::media::Reopen(t, t.original) && t.steps == 2 && !t.changed,
          "reopen verifies artist/album as well as title");
    t = {}; t.playing = false;
    Check(!ammod::media::Reopen(t, t.original) && t.steps == 0 && t.seeks == 0,
          "paused setting change issues no playback commands");
    t = {}; t.previousAvailable = false;
    Check(ammod::media::Reopen(t, t.original) && !t.changed, "first-item fallback returns via next/previous");
    t = {}; t.singleton = true;
    Check(!ammod::media::Reopen(t, t.original) && t.steps == 2,
          "single-item queue terminates without unconfirmed compensating skips");
    t = {}; t.wrongReturn = true;
    Check(!ammod::media::Reopen(t, t.original), "failed return cannot report success");
    std::puts("Playback lifecycle and settings restart regressions passed (no audio device acquired)");
}
