#pragma once

#include <string>
#include <cstdint>

namespace ammod::media {
struct Identity {
    std::wstring title, artist, albumTitle;
    std::int32_t trackNumber{};
    std::int64_t timelineEnd100ns{};
    bool Valid() const noexcept { return !title.empty(); }
    bool operator==(const Identity&) const = default;
};

// Reopening must create a new Apple playback item. A seek alone does not
// renegotiate the source. Never issue the compensating skip without an
// observed item change, and verify the complete identity on return.
template<class Transport>
bool Reopen(Transport& transport, const Identity& original) {
    if (!original.Valid() || !transport.Playing() || transport.Read() != original) return false;
    if (!transport.SeekStart()) return false;
    for (const bool previous : {true, false}) {
        if (!transport.Playing() || transport.Read() != original) return false;
        if (!transport.Step(previous)) continue;
        Identity visited;
        bool changed = false;
        for (int n = 0; n < 40; ++n) {
            transport.Wait();
            visited = transport.Read();
            if (visited.Valid() && visited != original) { changed = true; break; }
            if (!transport.Playing()) return false;
        }
        if (!changed) continue; // first/last item may reject a direction
        // A concurrent user selection must not be skipped again.
        if (transport.Read() != visited) return false;
        const bool paused = !transport.Playing();
        if (!transport.Step(!previous)) return false;
        for (int n = 0; n < 40; ++n) {
            transport.Wait();
            if (transport.Read() == original) {
                if (paused || !transport.Playing()) transport.Pause();
                return true;
            }
        }
        if (paused) transport.Pause();
        return false;
    }
    return false;
}
} // namespace ammod::media
