#include "encoder_session_policy.h"
#include <iostream>
#include <limits>
#include <stdexcept>

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
int main()
{
    using namespace encoder_session;
    Require(IsLanDuration(0) && IsLanDuration(600), "LAN accepts only permanent or ten-minute debug");
    for (double invalid : {-1.0, 10.0, 300.0, 1800.0, 600.1, 0.1, 599.0, 601.0,
            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        Require(!IsLanDuration(invalid), "NAPI duration must reject old, fractional and non-finite values");
    }
    Require(ValidOptions(10, true, false, false), "independent local 10s probe remains supported");
    Require(ValidOptions(600, false, true, true), "debug LAN must stream without recording");
    Require(ValidOptions(0, false, true, true), "permanent LAN must stream without recording");
    for (int duration : {0, 600}) {
        Require(!ValidOptions(duration, true, true, true), "LAN must never save H264 locally");
        Require(!ValidOptions(duration, false, false, false), "nonrecording session needs a packet sink");
        Require(!ValidOptions(duration, false, false, true), "partial hooks without a packet sink are invalid");
        Require(!ValidOptions(duration, true, false, false), "LAN duration is not a standalone local probe");
    }
    for (int duration : {-1, 10, 300, 1800, 601}) {
        Require(!ValidOptions(duration, false, true, true), "old and arbitrary LAN durations must fail closed");
    }
    Require(!ValidOptions(10, true, true, true), "legacy local-plus-LAN 10s mode is rejected");
    Require(!ValidOptions(10, true, false, true), "standalone development probe must not have network hooks");
    Require(!DurationExpired(600, 599.999) && DurationExpired(600, 600) && DurationExpired(600, 601),
        "ten-minute mode stops at the actual first-frame elapsed boundary");
    Require(!DurationExpired(10, 9.999) && DurationExpired(10, 10), "standalone probe retains its ten-second limit");
    for (double elapsed : {0.0, 10.0, 600.0, 1800.0, 31536000.0, 1e15}) {
        Require(!DurationExpired(0, elapsed), "permanent session must not expire from elapsed time");
    }
    std::cout << "encoder session policy: LAN 600/0, no recording, legacy rejection, local probe and permanent duration regression passed\n";
}
