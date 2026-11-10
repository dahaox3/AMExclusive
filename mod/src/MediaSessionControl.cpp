#include "BoundedLog.h"
#include "SettingsRestartPolicy.h"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <chrono>

using namespace winrt;
using namespace Windows::Media::Control;

namespace {

void LogRecovery(const std::string& message) {
    static std::mutex logMutex;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    const auto logPath = std::filesystem::path(path).parent_path() / L"am-exclusive-recovery.log";
    std::wstring line = L"uptimeMs=" + std::to_wstring(GetTickCount64()) +
                        L" pid=" + std::to_wstring(GetCurrentProcessId()) + L" ";
    line.append(message.begin(), message.end());
    ammod::logging::AppendWideLine(logPath, logMutex, line, 1ull * 1024ull * 1024ull, 2);
}

using MediaIdentity = ammod::media::Identity;

MediaIdentity ReadMediaIdentity(const GlobalSystemMediaTransportControlsSession& session) {
    const auto media = session.TryGetMediaPropertiesAsync().get();
    const auto timeline = session.GetTimelineProperties();
    return {
        media.Title().c_str(),
        media.Artist().c_str(),
        media.AlbumTitle().c_str(),
        media.TrackNumber(),
        timeline.EndTime().count(),
    };
}

bool HasConfirmedItemChange(const MediaIdentity& before, const MediaIdentity& current) {
    if (!before.title.empty() && current.title.empty()) return false;
    if (!before.title.empty() && current.title != before.title) return true;
    if (!before.artist.empty() && !current.artist.empty() && current.artist != before.artist) return true;
    if (!before.albumTitle.empty() && !current.albumTitle.empty() && current.albumTitle != before.albumTitle) return true;
    if (current.trackNumber != 0 && before.trackNumber != 0 && current.trackNumber != before.trackNumber) return true;
    return current.timelineEnd100ns > 0 && before.timelineEnd100ns > 0 &&
           current.timelineEnd100ns != before.timelineEnd100ns;
}

bool HasReturnedToItem(const MediaIdentity& expected, const MediaIdentity& current) {
    if (!expected.title.empty()) return current.title == expected.title;
    if (!expected.artist.empty() && current.artist != expected.artist) return false;
    if (!expected.albumTitle.empty() && current.albumTitle != expected.albumTitle) return false;
    if (expected.trackNumber != 0 && current.trackNumber != expected.trackNumber) return false;
    if (expected.timelineEnd100ns > 0 && current.timelineEnd100ns != expected.timelineEnd100ns) return false;
    return !expected.artist.empty() || !expected.albumTitle.empty() ||
           expected.trackNumber != 0 || expected.timelineEnd100ns > 0;
}

struct RoundTripResult {
    bool primaryAccepted{};
    bool itemChangeConfirmed{};
    std::uint32_t itemChangeWaitMs{};
    MediaIdentity visited{};
    bool compensatingAccepted{};
    bool returned{};
    std::uint32_t returnWaitMs{};
};

bool RestartCurrentItem(const GlobalSystemMediaTransportControlsSession& session,
                        const MediaIdentity& original,
                        RoundTripResult& result) {
    result = {};
    result.primaryAccepted = session.TrySkipPreviousAsync().get();
    if (!result.primaryAccepted) return false;

    constexpr std::uint32_t pollMs = 50;
    constexpr std::uint32_t timeoutMs = 6000;
    while (result.itemChangeWaitMs < timeoutMs) {
        Sleep(pollMs);
        result.itemChangeWaitMs += pollMs;
        result.visited = ReadMediaIdentity(session);
        if (HasConfirmedItemChange(original, result.visited)) {
            result.itemChangeConfirmed = true;
            break;
        }
    }
    if (!result.itemChangeConfirmed) return false;

    result.compensatingAccepted = session.TrySkipNextAsync().get();
    if (!result.compensatingAccepted) return false;

    while (result.returnWaitMs < timeoutMs) {
        Sleep(pollMs);
        result.returnWaitMs += pollMs;
        if (HasReturnedToItem(original, ReadMediaIdentity(session))) {
            result.returned = true;
            break;
        }
    }
    return result.returned;
}

template<class Operation>
auto BoundedResult(Operation operation) {
    if (operation.wait_for(std::chrono::seconds(3)) != Windows::Foundation::AsyncStatus::Completed) {
        operation.Cancel();
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.GetResults();
}

struct SettingsTransport {
    GlobalSystemMediaTransportControlsSession session;
    HANDLE cancelled{};
    bool Cancelled() const { return WaitForSingleObject(cancelled, 0) != WAIT_TIMEOUT; }
    MediaIdentity Read() {
        const auto media = BoundedResult(session.TryGetMediaPropertiesAsync());
        return {media.Title().c_str(), media.Artist().c_str(), media.AlbumTitle().c_str(),
                media.TrackNumber(), session.GetTimelineProperties().EndTime().count()};
    }
    bool Playing() { return !Cancelled() && session.GetPlaybackInfo().PlaybackStatus() ==
        GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing; }
    bool SeekStart() { return !Cancelled() && BoundedResult(session.TryChangePlaybackPositionAsync(
        session.GetTimelineProperties().StartTime().count())); }
    bool Step(bool previous) {
        if (Cancelled()) return false;
        const auto controls = session.GetPlaybackInfo().Controls();
        if (previous ? !controls.IsPreviousEnabled() : !controls.IsNextEnabled()) return false;
        return previous ? BoundedResult(session.TrySkipPreviousAsync())
                        : BoundedResult(session.TrySkipNextAsync());
    }
    void Pause() { if (!Cancelled()) (void)BoundedResult(session.TryPauseAsync()); }
    void Wait() { Sleep(50); }
};

int ApplySettingsRestart(DWORD ownerPid, const wchar_t* scope) {
    // Scope requests to the elected Agent lifetime. Concurrent setting changes
    // signal the existing worker instead of starting competing skip sequences.
    const std::wstring prefix = scope;
    winrt::handle cancelled{OpenEventW(SYNCHRONIZE, FALSE, scope)};
    winrt::handle pending{CreateEventW(nullptr, TRUE, FALSE, (prefix + L".Pending").c_str())};
    winrt::handle single{CreateMutexW(nullptr, FALSE, (prefix + L".Worker").c_str())};
    winrt::handle owner{OpenProcess(SYNCHRONIZE, FALSE, ownerPid)};
    if (!pending || !single || !owner || !cancelled) return 6;
    SetEvent(pending.get());
    const auto acquired = WaitForSingleObject(single.get(), 0);
    if (acquired == WAIT_TIMEOUT) return 0;
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) return 6;
    struct Unlock { HANDLE handle; ~Unlock() { ReleaseMutex(handle); } } unlock{single.get()};
    const HANDLE waits[]{owner.get(), cancelled.get(), pending.get()};
    // Stay idle on the event until this Agent exits. This also closes the race
    // between the last request check and worker exit when a new edit arrives.
    while (WaitForMultipleObjects(3, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 2) {
        ResetEvent(pending.get());
        try {
            const auto manager = BoundedResult(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
            GlobalSystemMediaTransportControlsSession session{nullptr};
            for (const auto& candidate : manager.GetSessions()) {
                if (std::wstring(candidate.SourceAppUserModelId()).find(L"AppleMusic") != std::wstring::npos) {
                    session = candidate;
                    break;
                }
            }
            if (!session) continue;
            SettingsTransport transport{session, cancelled.get()};
            const auto original = transport.Read();
            if (!original.Valid()) continue;
            for (;;) {
                // Debounce rapid edits and wait for user-driven resume. A new song
                // already uses the latest settings, so never jump back to the old one.
                bool itemChanged = false;
                do {
                    if (WaitForMultipleObjects(2, waits, FALSE, 500) != WAIT_TIMEOUT) return 0;
                    if (transport.Read() != original) { itemChanged = true; break; }
                    if (WaitForSingleObject(pending.get(), 0) == WAIT_OBJECT_0) {
                        ResetEvent(pending.get());
                        continue;
                    }
                    if (transport.Playing()) break;
                } while (true);
                if (itemChanged) break;
                const bool reopened = ammod::media::Reopen(transport, original);
                LogRecovery(std::string("settings-restart confirmed=") + (reopened ? "1" : "0"));
                // Failure is explicit in the recovery log; never endlessly skip.
                if (!reopened) break;
                if (WaitForSingleObject(pending.get(), 0) != WAIT_OBJECT_0) break;
                ResetEvent(pending.get());
            }
        } catch (const hresult_error& error) {
            LogRecovery("settings-restart failed HRESULT=" + std::to_string(error.code().value));
        } catch (...) {
            LogRecovery("settings-restart failed unexpected exception");
        }
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && _wcsicmp(argv[1], L"settings-restart") == 0) {
        init_apartment(apartment_type::multi_threaded);
        return ApplySettingsRestart(wcstoul(argv[2], nullptr, 10), argv[3]);
    }
    if (argc != 2 || (_wcsicmp(argv[1], L"restart-current") != 0 &&
                      _wcsicmp(argv[1], L"reject-current") != 0)) return 2;

    init_apartment(apartment_type::multi_threaded);
    const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    GlobalSystemMediaTransportControlsSession appleSession{nullptr};
    for (const auto& session : manager.GetSessions()) {
        const std::wstring source = session.SourceAppUserModelId().c_str();
        if (source.find(L"AppleMusic") != std::wstring::npos) {
            appleSession = session;
            break;
        }
    }
    if (!appleSession) return 3;

    if (_wcsicmp(argv[1], L"reject-current") == 0) {
        const bool stopAccepted = appleSession.TryStopAsync().get();
        bool stopConfirmed = false;
        if (stopAccepted) {
            constexpr std::uint32_t pollMs = 25;
            constexpr std::uint32_t timeoutMs = 750;
            for (std::uint32_t waited = 0; waited <= timeoutMs; waited += pollMs) {
                if (appleSession.GetPlaybackInfo().PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped) {
                    stopConfirmed = true;
                    break;
                }
                Sleep(pollMs);
            }
        }

        // Stop is the strongest public GSMTC teardown for the current media
        // session. If Apple refuses it, fail closed with pause + rewind while
        // AME has already retired its own captured/load state.
        bool paused = stopConfirmed;
        bool reset = stopConfirmed;
        if (!stopConfirmed) {
            paused = appleSession.TryPauseAsync().get();
            const auto timeline = appleSession.GetTimelineProperties();
            reset = appleSession.TryChangePlaybackPositionAsync(
                timeline.StartTime().count()).get();
        }
        LogRecovery("reject-current stopAccepted=" + std::to_string(stopAccepted) +
                    " stopConfirmed=" + std::to_string(stopConfirmed) +
                    " paused=" + std::to_string(paused) +
                    " reset=" + std::to_string(reset));
        return (stopConfirmed || (paused && reset)) ? 0 : 5;
    }

    const MediaIdentity original = ReadMediaIdentity(appleSession);
    const auto controls = appleSession.GetPlaybackInfo().Controls();
    if (!controls.IsPreviousEnabled()) return 4;

    RoundTripResult result{};
    LogRecovery("begin strategy=previous-confirm-next originalEnd100ns=" +
                std::to_string(original.timelineEnd100ns));
    const bool succeeded = RestartCurrentItem(appleSession, original, result);
    LogRecovery("complete primaryAccepted=" + std::to_string(result.primaryAccepted) +
                " changed=" + std::to_string(result.itemChangeConfirmed) +
                " compensatingAccepted=" + std::to_string(result.compensatingAccepted) +
                " returned=" + std::to_string(result.returned));
    return succeeded ? 0 : 5;
}
