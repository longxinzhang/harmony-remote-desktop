#ifndef HARMONY_REMOTE_CAPTURE_STATE_POLICY_H
#define HARMONY_REMOTE_CAPTURE_STATE_POLICY_H

#include <multimedia/player_framework/native_avscreen_capture_base.h>

namespace capture_state {
inline bool ValidPrivacyMaskMode(int mode) { return mode == 0 || mode == 1; }

// A privacy notification describes masking; it is not a stop or a pause signal.
// Keep this SDK-enum-based reducer shared with the regression test so private
// scene transitions cannot accidentally enter the real cancellation path.
inline bool Apply(OH_AVScreenCaptureStateCode code, bool stopping,
    bool& started, bool& paused, bool& privateScene)
{
    switch (code) {
        case OH_SCREEN_CAPTURE_STATE_STARTED: started = true; break;
        case OH_SCREEN_CAPTURE_STATE_PAUSED_BY_USER:
        case OH_SCREEN_CAPTURE_STATE_PAUSED_BY_APP: paused = true; break;
        case OH_SCREEN_CAPTURE_STATE_RESUMED_BY_USER:
        case OH_SCREEN_CAPTURE_STATE_RESUMED_BY_APP: paused = false; break;
        case OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE: privateScene = true; break;
        case OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE: privateScene = false; break;
        case OH_SCREEN_CAPTURE_STATE_CANCELED:
        case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER:
        case OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER:
        case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL:
        case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES:
            started = false;
            return !stopping;
        default: break; // Includes microphone-only and future SDK notifications.
    }
    return false;
}

// Accept an integer so the idle sentinel (-1) and future diagnostic codes do
// not require an out-of-range conversion to the SDK's unfixed enum type.
inline const char* Name(int code)
{
    switch (code) {
        case OH_SCREEN_CAPTURE_STATE_STARTED: return "STARTED";
        case OH_SCREEN_CAPTURE_STATE_CANCELED: return "CANCELED";
        case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER: return "STOPPED_BY_USER";
        case OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER: return "INTERRUPTED_BY_OTHER";
        case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL: return "STOPPED_BY_CALL";
        case OH_SCREEN_CAPTURE_STATE_MIC_UNAVAILABLE: return "MIC_UNAVAILABLE";
        case OH_SCREEN_CAPTURE_STATE_MIC_MUTED_BY_USER: return "MIC_MUTED_BY_USER";
        case OH_SCREEN_CAPTURE_STATE_MIC_UNMUTED_BY_USER: return "MIC_UNMUTED_BY_USER";
        case OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE: return "ENTER_PRIVATE_SCENE";
        case OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE: return "EXIT_PRIVATE_SCENE";
        case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES: return "STOPPED_BY_USER_SWITCHES";
        case OH_SCREEN_CAPTURE_STATE_PAUSED_BY_USER: return "PAUSED_BY_USER";
        case OH_SCREEN_CAPTURE_STATE_RESUMED_BY_USER: return "RESUMED_BY_USER";
        case OH_SCREEN_CAPTURE_STATE_PAUSED_BY_APP: return "PAUSED_BY_APP";
        case OH_SCREEN_CAPTURE_STATE_RESUMED_BY_APP: return "RESUMED_BY_APP";
        default: return "UNKNOWN";
    }
}
} // namespace capture_state
#endif
