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
    Require(ProfileCeiling(true,true,true,20)==96000 &&
            ProfileCeiling(true,true,true,15,192000)==48000,
            "default 96 kHz source limit and native 48 kHz tier");
    Require(NormalizeSourceSampleRateLimit(0)==96000 &&
            !IsSourceSampleRateLimit(12345), "invalid configuration uses safe default");
    Selection optical{96000,0};
    for (double rate : {48000.,96000.,192000.}) optical.Observe(true,rate);
    Require(optical.Accept(true,96000) && !optical.Accept(true,48000) &&
            !optical.Accept(true,192000), "96 kHz is retained when available");
    Selection appleFallback{96000,0};
    appleFallback.Observe(true,176400); appleFallback.Observe(true,48000);
    Require(appleFallback.Accept(true,48000) && !appleFallback.Accept(true,176400),
            "select existing Apple 48 kHz source below 176.4 kHz version");
    Selection optical88{96000,0};
    for (double rate : {48000.,88200.,176400.}) optical88.Observe(true,rate);
    Require(optical88.Accept(true,88200), "88.2 kHz is preferred to 48 kHz");
    Selection noEligible{96000,0}; noEligible.Observe(true,192000);
    Require(!noEligible.Accept(true,192000), "no over-limit fallback when Apple has no compliant source");
    Selection explicit192{ProfileCeiling(true,true,true,20,192000),0};
    explicit192.Observe(true,96000); explicit192.Observe(true,192000);
    Require(explicit192.Accept(true,192000), "192 kHz remains available when explicitly configured");
    using namespace ammod::audio_v2;
    AudioCoreGate gate;
    gate.SetUiEnabled(true); const auto token=gate.BeginCapture();
    Require(token.has_value() && !gate.ShouldSuppressNative(*token), "arming does not suppress output");
    Require(gate.MarkPrebuffered(*token) && gate.CommitCutover(*token), "successful output cutover");
    gate.SetUiEnabled(false); gate.SetUiEnabled(true);
    Require(!gate.CanCapture(*token) && !gate.ShouldSuppressNative(*token), "owner changes invalidate old audio callbacks");
    std::puts("Agent ownership, quality selection and stale callback regressions passed");
}
