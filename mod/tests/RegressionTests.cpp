#include "AgentOwnership.h"
#include "LosslessQualityPolicy.h"
#include "audio_v2/AudioCoreGate.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

void Require(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}

int main() {
    using namespace ammod::ipc;
    AgentIdentity owner{};
    const AgentIdentity old{16640, 100}, successor{7884, 200};
    for (auto connection : {old, successor, old, successor, old, old, old})
        if (PreferAgent(connection, owner)) owner = connection;
    Require(owner.pid == successor.pid, "old Agent reconnects cannot evict successor");
    Require(!OwnsAudio(owner.pid, old.pid) && OwnsAudio(owner.pid, successor.pid), "only elected Agent enables capture");
    Require(!PreferAgent({0,300}, owner) && !PreferAgent({9000,0},owner), "reject unverifiable process identity");
    owner = {};
    Require(PreferAgent(old,owner), "remaining live Agent can take over after owner exits");
    using namespace ammod::quality;
    Selection item{192000,0};
    item.Observe(true,44100); item.Observe(true,96000); item.Observe(true,192000);
    item.Observe(false,384000);
    Require(item.Accept(true,192000) && !item.Accept(true,96000), "highest available lossless only");
    Require(!item.Accept(false,192000), "codec admission still applies");
    item.Observe(true,48000);
    Require(!item.Accept(true,48000), "bandwidth reevaluation cannot lower established ceiling");
    Require(!item.Accept(true,std::numeric_limits<double>::quiet_NaN()), "invalid rate rejected");
    Selection next{192000,0}; next.Observe(true,44100);
    Require(next.Accept(true,44100), "next item has an independent quality ceiling");
    using namespace ammod::audio_v2;
    AudioCoreGate gate;
    gate.SetUiEnabled(true); const auto token=gate.BeginCapture();
    Require(token.has_value() && !gate.ShouldSuppressNative(*token), "arming does not suppress output");
    Require(gate.MarkPrebuffered(*token) && gate.CommitCutover(*token), "successful output cutover");
    gate.SetUiEnabled(false); gate.SetUiEnabled(true);
    Require(!gate.CanCapture(*token) && !gate.ShouldSuppressNative(*token), "owner changes invalidate old audio callbacks");
    std::puts("Agent ownership, quality selection and stale callback regressions passed");
}
