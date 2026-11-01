#pragma once

#include <cstdint>

namespace ammod::ipc {

struct AgentIdentity {
    std::uint32_t pid{};
    std::uint64_t created{};
};

// Connection/retry order is not process ownership. A lingering older Agent
// must never evict its successor merely by reconnecting after one second.
constexpr bool PreferAgent(AgentIdentity candidate, AgentIdentity current) noexcept {
    if (!candidate.pid || !candidate.created) return false;
    if (!current.pid) return true;
    return candidate.created > current.created ||
        (candidate.created == current.created && candidate.pid > current.pid);
}

constexpr bool OwnsAudio(std::uint32_t ownerPid, std::uint32_t selfPid) noexcept {
    return ownerPid != 0 && ownerPid == selfPid;
}

} // namespace ammod::ipc
