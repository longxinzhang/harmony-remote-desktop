#ifndef HARMONY_REMOTE_ENCODER_TIMING_H
#define HARMONY_REMOTE_ENCODER_TIMING_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace encoder_timing {

inline bool RelativeCallbackPtsUs(int64_t firstSteadyNs, int64_t packetSteadyNs, uint64_t& ptsUs)
{
    if (packetSteadyNs < firstSteadyNs) return false;
    ptsUs = (uint64_t(packetSteadyNs) - uint64_t(firstSteadyNs)) / 1000;
    return true;
}

struct PtsAssessment {
    std::string rawSpan = "0";
    const char* status = "INSUFFICIENT_OBSERVATION";
    double sdkDeclaredSpanSeconds = 0;
    double nanosecondCandidateSpanSeconds = 0;
    double outputWallSpanSeconds = 0;
    double sdkSpanToWallRatio = 0;
    double toleranceSeconds = 0;
    bool sdkDeclaredUnitConsistent = false;
    bool observedNanosecondCandidate = false;
    bool reviewRequired = false;
};

// Compare two interpretations with an independent steady-clock observation.
// This never normalizes raw PTS, resolves a unit, or computes playback FPS.
inline PtsAssessment AssessPts(int64_t first, int64_t last, uint64_t frames,
    double wallSpanSeconds, bool monotonic)
{
    PtsAssessment result;
    // Unsigned subtraction retains the exact magnitude even across INT64_MIN/MAX.
    bool negative = last < first;
    uint64_t magnitude = negative ? uint64_t(first) - uint64_t(last) : uint64_t(last) - uint64_t(first);
    result.rawSpan = (negative ? "-" : "") + std::to_string(magnitude);
    double rawSpan = (negative ? -1.0 : 1.0) * static_cast<double>(magnitude);
    result.sdkDeclaredSpanSeconds = rawSpan / 1000000.0;
    result.nanosecondCandidateSpanSeconds = rawSpan / 1000000000.0;
    if (std::isfinite(wallSpanSeconds) && wallSpanSeconds >= 0) result.outputWallSpanSeconds = wallSpanSeconds;
    if (frames < 2) return result;
    if (!monotonic || negative || magnitude == 0) {
        result.status = "NON_MONOTONIC_OR_ZERO_PTS_REQUIRES_REVIEW";
        result.reviewRequired = true;
        return result;
    }
    if (!std::isfinite(wallSpanSeconds) || wallSpanSeconds <= 0) {
        result.status = "INVALID_WALLCLOCK_SPAN_REQUIRES_REVIEW";
        result.reviewRequired = true;
        return result;
    }
    // Short observations are too sensitive to codec startup/batching to infer units.
    if (wallSpanSeconds < 1.0) return result;
    result.sdkSpanToWallRatio = result.sdkDeclaredSpanSeconds / wallSpanSeconds;
    result.toleranceSeconds = std::max(0.25, wallSpanSeconds * 0.05);
    result.sdkDeclaredUnitConsistent = std::abs(result.sdkDeclaredSpanSeconds - wallSpanSeconds) <= result.toleranceSeconds;
    result.observedNanosecondCandidate = !result.sdkDeclaredUnitConsistent &&
        std::abs(result.nanosecondCandidateSpanSeconds - wallSpanSeconds) <= result.toleranceSeconds;
    result.status = result.sdkDeclaredUnitConsistent ? "SDK_MICROSECONDS_CONSISTENT_WITH_WALL" :
        "UNIT_MISMATCH_REQUIRES_REVIEW";
    result.reviewRequired = !result.sdkDeclaredUnitConsistent;
    return result;
}

// Fixed-size cadence evidence: one bucket per second since this series' first
// callback. Later arrivals increment overflow; memory never grows with runtime.
struct Cadence {
    static constexpr size_t BUCKETS = 16;
    uint64_t observations = 0;
    int64_t firstSteadyNs = 0;
    int64_t lastSteadyNs = 0;
    uint64_t maxGapNs = 0;
    uint64_t clockRegressions = 0;
    uint64_t overflowObservations = 0;
    std::array<uint64_t, BUCKETS> perSecond {};

    void Observe(int64_t steadyNs)
    {
        if (!observations) firstSteadyNs = lastSteadyNs = steadyNs;
        else if (steadyNs < lastSteadyNs) ++clockRegressions;
        else {
            maxGapNs = std::max(maxGapNs, uint64_t(steadyNs) - uint64_t(lastSteadyNs));
            lastSteadyNs = steadyNs;
        }
        ++observations;
        uint64_t span = steadyNs >= firstSteadyNs ? uint64_t(steadyNs) - uint64_t(firstSteadyNs) : 0;
        uint64_t bucket = span / 1000000000ULL;
        if (bucket < BUCKETS) ++perSecond[static_cast<size_t>(bucket)];
        else ++overflowObservations;
    }
    double SpanSeconds() const
    {
        return observations > 1 ? double(uint64_t(lastSteadyNs) - uint64_t(firstSteadyNs)) / 1000000000.0 : 0;
    }
};
} // namespace encoder_timing
#endif
