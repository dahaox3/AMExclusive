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
    static void Run() {
        const auto decoder = reinterpret_cast<void*>(0x1000);
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
