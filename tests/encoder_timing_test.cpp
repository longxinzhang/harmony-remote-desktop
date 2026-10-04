#include "encoder_timing.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void Near(double value, double expected, double tolerance, const char* message)
{
    Require(std::isfinite(value) && std::abs(value - expected) <= tolerance, message);
}
int main()
{
    using encoder_timing::AssessPts;
    uint64_t streamPtsUs = 99;
    Require(encoder_timing::RelativeCallbackPtsUs(9000000000LL, 9000000000LL, streamPtsUs) && streamPtsUs == 0,
        "first output callback is the stream timestamp origin");
    Require(encoder_timing::RelativeCallbackPtsUs(9000000000LL, 9033333999LL, streamPtsUs) && streamPtsUs == 33333,
        "stream timestamp uses callback nanoseconds divided by 1000, not raw native PTS");
    Require(!encoder_timing::RelativeCallbackPtsUs(9000000000LL, 8999999999LL, streamPtsUs),
        "backwards callback time must be rejected, not unsigned-wrapped");
    // Regression fixture copied from the unchanged device encoder-take-01 report.
    // That v1 report has only total capture wall duration (10.008), not the new
    // first/last callback span. It is a wall-scale proxy for this regression,
    // never presented as a newly measured callback span.
    auto device = AssessPts(59665801444997LL, 59675810141403LL, 224, 10.008, true);
    Require(device.rawSpan == "10008696406", "preserve the exact device raw PTS span");
    Near(device.sdkDeclaredSpanSeconds, 10008.696406, 0.000001, "retain SDK-declared us interpretation");
    Near(device.nanosecondCandidateSpanSeconds, 10.008696406, 0.000000001, "retain separate ns candidate");
    Require(std::string(device.status) == "UNIT_MISMATCH_REQUIRES_REVIEW", "device regression must demand unit review");
    Require(device.observedNanosecondCandidate && !device.sdkDeclaredUnitConsistent && device.reviewRequired,
        "candidate must not silently validate the SDK unit");

    auto normal = AssessPts(5000000, 15000000, 301, 10.0, true);
    Require(std::string(normal.status) == "SDK_MICROSECONDS_CONSISTENT_WITH_WALL", "ordinary microseconds remain consistent");
    Require(normal.sdkDeclaredUnitConsistent && !normal.observedNanosecondCandidate && !normal.reviewRequired,
        "ordinary microseconds must not be marked nanoseconds");

    auto unknown = AssessPts(0, 20000000, 301, 10.0, true);
    Require(unknown.reviewRequired && !unknown.observedNanosecondCandidate,
        "a timing mismatch alone does not prove nanoseconds");
    Require(std::string(AssessPts(0, 200000000, 7, 0.2, true).status) == "INSUFFICIENT_OBSERVATION",
        "short codec bursts do not justify unit inference");
    Require(std::isfinite(AssessPts(0, 1000000000, 2, std::numeric_limits<double>::denorm_min(), true).sdkSpanToWallRatio),
        "tiny wall spans must not produce non-finite JSON numbers");
    Require(std::string(AssessPts(1, 1, 1, 0, true).status) == "INSUFFICIENT_OBSERVATION", "one frame is insufficient");
    Require(AssessPts(0, 0, 10, 10, true).reviewRequired, "repeated zero PTS requires review");
    auto backwards = AssessPts(20, 10, 2, 10, false);
    Require(backwards.rawSpan == "-10" && backwards.reviewRequired && !backwards.observedNanosecondCandidate,
        "backwards PTS must preserve sign and not infer units");
    Require(AssessPts(0, 10000000, 301, 10, false).reviewRequired,
        "matching endpoints cannot hide a PTS regression inside the series");
    Require(AssessPts(0, 10000000, 301, std::numeric_limits<double>::infinity(), true).reviewRequired,
        "invalid wall time cannot validate a unit");
    auto wide = AssessPts(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), 2, 10, true);
    Require(wide.rawSpan == "18446744073709551615" && std::isfinite(wide.sdkDeclaredSpanSeconds),
        "extreme raw timestamps must not cause signed overflow");

    encoder_timing::Cadence cadence;
    constexpr int64_t origin = 1000000000LL;
    for (int64_t offset : {0LL, 30000000LL, 990000000LL, 1000000000LL, 1060000000LL, 15900000000LL, 16000000000LL}) {
        cadence.Observe(origin + offset);
    }
    Require(cadence.observations == 7 && cadence.perSecond[0] == 3 && cadence.perSecond[1] == 2 && cadence.perSecond[15] == 1,
        "per-second buckets must keep boundary frames in the correct interval");
    Require(cadence.overflowObservations == 1 && cadence.perSecond.size() == 16, "late callbacks must not grow bucket storage");
    Near(cadence.SpanSeconds(), 16, 0.000001, "wall span tracks actual first and last callbacks");
    Require(cadence.maxGapNs == 14840000000ULL, "long callback stalls must remain visible");
    cadence.Observe(origin + 15500000000LL);
    Require(cadence.clockRegressions == 1 && cadence.lastSteadyNs == origin + 16000000000LL,
        "out-of-order clock observations must be reported without shortening the observation span");
    std::cout << "encoder timing regression and bounded cadence tests passed\n";
}
