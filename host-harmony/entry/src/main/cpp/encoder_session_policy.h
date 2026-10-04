#ifndef HARMONY_REMOTE_ENCODER_SESSION_POLICY_H
#define HARMONY_REMOTE_ENCODER_SESSION_POLICY_H

namespace encoder_session {
constexpr int LOCAL_PROBE_SECONDS = 10;
constexpr int LAN_DEBUG_SECONDS = 600;
constexpr int LAN_PERMANENT_SECONDS = 0;

// Keep the check on the incoming double: never truncate fractional NAPI values.
inline bool IsLanDuration(double seconds)
{
    return seconds == LAN_PERMANENT_SECONDS || seconds == LAN_DEBUG_SECONDS;
}

inline bool ValidOptions(int seconds, bool recordLocally, bool hasPacketSink, bool hasAnyStreamHook)
{
    if (hasAnyStreamHook) return hasPacketSink && !recordLocally && IsLanDuration(seconds);
    return !hasPacketSink && recordLocally && seconds == LOCAL_PROBE_SECONDS;
}

// Zero removes only the duration limit. Other cancellation/error checks stay active.
inline bool DurationExpired(int seconds, double elapsedSeconds)
{
    return seconds > 0 && elapsedSeconds >= seconds;
}
} // namespace encoder_session
#endif
