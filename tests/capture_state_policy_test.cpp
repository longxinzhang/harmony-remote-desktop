#include "capture_state_policy.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void Require(bool okay, const char* message)
{
    ++checks;
    if (!okay) throw std::runtime_error(message);
}
struct Session {
    bool started = false, paused = false, privateScene = false, stopping = false, cancelled = false;
    void State(OH_AVScreenCaptureStateCode code)
    {
        // Matches OnCaptureState: cancellation is sticky until the next Start.
        if (capture_state::Apply(code, stopping, started, paused, privateScene)) cancelled = true;
    }
};
}

int main()
{
    Require(std::string(capture_state::Name(-1)) == "UNKNOWN", "idle sentinel has no SDK enum conversion");
    Require(std::string(capture_state::Name(1000)) == "UNKNOWN", "future unknown diagnostic codes remain printable");
    Require(std::string(capture_state::Name(OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE)) == "ENTER_PRIVATE_SCENE",
        "known SDK privacy state keeps a readable diagnostic name");
    Require(capture_state::ValidPrivacyMaskMode(0) && capture_state::ValidPrivacyMaskMode(1),
        "window mask and diagnostic whole-screen fallback are valid");
    for (int mode : {-1, 2, 100}) Require(!capture_state::ValidPrivacyMaskMode(mode), "unsupported mask mode rejected");

    Session s;
    s.State(OH_SCREEN_CAPTURE_STATE_STARTED);
    Require(s.started && !s.cancelled && !s.paused, "start retains normal capture lifecycle");
    for (int scene = 0; scene < 3; ++scene) {
        s.State(OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE);
        Require(s.started && s.privateScene && !s.paused && !s.cancelled,
            "private scene must not stop or pause capture/audio/network");
        s.State(OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE);
        Require(s.started && !s.privateScene && !s.paused && !s.cancelled,
            "private exit requires neither cancellation nor capture restart");
    }
    for (auto pause : {OH_SCREEN_CAPTURE_STATE_PAUSED_BY_USER, OH_SCREEN_CAPTURE_STATE_PAUSED_BY_APP}) {
        s.State(pause);
        s.State(OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE);
        s.State(OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE);
        Require(s.started && s.paused && !s.privateScene && !s.cancelled,
            "privacy exit must not undo an explicit user/app pause");
        s.State(pause == OH_SCREEN_CAPTURE_STATE_PAUSED_BY_USER ?
            OH_SCREEN_CAPTURE_STATE_RESUMED_BY_USER : OH_SCREEN_CAPTURE_STATE_RESUMED_BY_APP);
        Require(!s.paused && s.started && !s.cancelled, "matching resume restores capture without cancellation");
    }
    for (auto code : {OH_SCREEN_CAPTURE_STATE_MIC_UNAVAILABLE, OH_SCREEN_CAPTURE_STATE_MIC_MUTED_BY_USER,
            OH_SCREEN_CAPTURE_STATE_MIC_UNMUTED_BY_USER}) {
        s.State(code);
        Require(s.started && !s.paused && !s.cancelled, "microphone-only notification cannot stop video");
    }
    for (auto stop : {OH_SCREEN_CAPTURE_STATE_CANCELED, OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER,
            OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER, OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL,
            OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES}) {
        Session stopped;
        stopped.State(OH_SCREEN_CAPTURE_STATE_STARTED);
        stopped.State(OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE);
        stopped.State(stop);
        Require(stopped.cancelled && !stopped.started, "real stop must still cancel while a private scene is present");
        stopped.State(OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE);
        Require(stopped.cancelled && !stopped.started && !stopped.privateScene,
            "late privacy exit must not revive cancelled capture");

        Session releasing;
        releasing.State(OH_SCREEN_CAPTURE_STATE_STARTED);
        releasing.stopping = true;
        releasing.State(stop);
        Require(!releasing.cancelled && !releasing.started,
            "stop callback during owned teardown must preserve existing non-cancellation behavior");
    }
    std::cout << "capture privacy state regression: " << checks << " checks passed; SDK enum values used\n";
}
