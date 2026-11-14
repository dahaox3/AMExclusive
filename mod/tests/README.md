# WASAPI drain hardware regression probe

## Fork regression checks

0.3.3 reproduces a successor AudioUnit initialization prematurely retiring the
current decoder, then verifies the production-runtime protection. Coverage
includes early child/parent Starts, same-unit reuse, 96-to-48 kHz handoff,
bounded prefetch backpressure without blocking current PCM, complete staged
PCM promotion after old-unit teardown, canceled/reused registrations, late
source precision, both Next event orderings, manual selection of a different
song, explicit pause/resume and seek cancellation. These tests never start a
runtime worker or acquire an audio device; actual Apple Music/optical playback
is still a post-install acceptance check.

0.3.1 adds a hardware-free playback lifecycle test linked against the production
runtime. It checks unexpired/paused drains, bounded EOF recovery, same-decoder
rearming, retired pointer reuse, a 96-to-48 kHz successor, and selection/play
pause fences. Mock media transport tests check confirmed round trips, same-title
different-artist identities, paused requests, first-item fallback and unavailable
single-item queues. These tests acquire no audio endpoint and do not control
the user's Apple Music session. Settings replay and actual natural transitions
still require post-install Apple Music/device acceptance testing.

The reopen helper runs only after the elected Agent applies changed settings.
It coalesces requests, waits through user pauses and cancels an old item when
the user changes tracks. Reopen uses a verified previous/next (or next/previous)
round trip, starts the song from the beginning, and is best-effort when Apple
disables queue navigation. It records confirmation/failure in
`am-exclusive-recovery.log`; API acceptance alone is never reported as success.

0.3.0 also checks the default 96 kHz ceiling, 96/88.2 preservation, fallback
to Apple's 48 kHz candidate, no over-limit admission, and explicit 192 kHz.
`am_exclusive_source_selection_probe` is an explicit test DLL excluded from
normal builds and installers. It includes the production quality-selector
translation unit and calls its Create/ApplyTree callbacks directly on temporary
native filters and captured Apple candidate arrays, without installing hooks
or changing the real player selection. On package 1.1792 it returned [96000]
for [96000,48000] at a 96000 cap, [48000] for [176400,48000], and [176400]
for that same second list at a 192000 cap. It does not test an actual download
or audible playback of the selected source.

`ctest --test-dir <build-directory> -C Release --output-on-failure` runs the
Agent election/quality/callback tests and the PowerShell installer SID tests.
They do not acquire audio endpoints or modify scheduled tasks.

Build `am_exclusive_endpoint_lease_probe` explicitly. Use `--list-devices`, then
`--run-on-endpoint <endpoint-ID>` on an idle endpoint. It sends silence and
checks pause/release, a real shared video-style client (including a still-open
client on music resume), another exclusive owner, retry without consuming
queued music, and resume without a fresh producer block. The 48 kHz/16-bit,
44.1 kHz/24-bit and 96 kHz/24-bit source cases each run three cycles. This was
verified on an NVIDIA display-audio endpoint; TOPPING and Apple Music UI
integration require separate device/application validation.

`am_exclusive_quality_probe` is an explicit test DLL, excluded from normal
builds and installer payloads. `RunQualityAbiProbe` requires an already loaded
CoreMedia/CoreFoundation, temporarily installs its own quality hooks, constructs
a strict ALAC filter and empty native filter tree, calls tree Apply, then
removes its hooks and releases its objects. It rejects an already hooked simple
factory. The probe passed inside package `1.1792.39682.0`; policy tests separately
check highest-rate admission and downgrade rejection. This is not a streamed
catalog/bandwidth or audible bit-perfect acceptance test.

## Existing drain probe

Build explicitly (the target is excluded from normal builds):

```powershell
cmake --build <build-directory> --config Release --target am_exclusive_drain_probe
<build-directory>\Release\am_exclusive_drain_probe.exe --run-on-default-device
```

This acquires the current default render endpoint in exclusive mode and sends
only silence. Stop other playback first. The device must support stereo packed
24-bit and 16-bit PCM at 192, 96 and 44.1 kHz. Unsupported formats are reported
as failures, not skipped. It is deliberately not registered as an unattended
CTest.

The 36 cases cover empty, partial and full terminal packets, source starvation,
pause/resume during drain, initially empty sources, and producer notifications
as well as polling. The source rejects any request after its terminal packet.
Each case requires clock-certified completion without underruns or errors.

Motivation: on a C-Media-based USB endpoint (Fosi Audio DA2120C), end-of-stream
drains that followed a source starvation wait timed out intermittently (3-4 of
the 36 cases per run, always `wait=1` cases). The device loses its event/clock
cycle when a packet is released into a stopped, previously run exclusive
stream: IAudioClock parks below the committed end-of-stream position and the
drain can only fault. Restarting the preserved endpoint queue before releasing
the next packet (with a readiness check first, so the restarted stream is
filled on the same wake) made all 36 cases pass repeatedly on the same
hardware.

These are transport/control tests. They do not establish audible correctness
or Apple Music lifecycle behavior.
