#ifndef HARMONY_REMOTE_ENCODER_PROBE_H
#define HARMONY_REMOTE_ENCODER_PROBE_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "encoder_session_policy.h"

struct EncoderStreamHooks {
    // Worker-only, nonblocking sink. Bytes are borrowed only until this call returns.
    // ptsUs is relative encoder-callback monotonic time, not capture time/native PTS.
    std::function<bool(const uint8_t*, size_t, uint64_t, bool, bool, bool)> packet;
    std::function<bool()> cancelled;
    // Capture callback: copy-only sink; internal playback PCM, 48 kHz stereo S16LE.
    std::function<bool(const uint8_t*, size_t, uint64_t)> audioPCM;
    // Worker polls; true requests a fresh IDR after authenticated reconnect.
    std::function<bool()> requestKeyframe;
    // Called exactly once per Start attempt, after owned SDK resources are released.
    std::function<void(bool)> finished;
};

struct EncoderSessionOptions {
    // Default is the independent local development probe, not a LAN mode.
    int durationSeconds = encoder_session::LOCAL_PROBE_SECONDS;
    // LAN accepts only 600 (debug) or 0 (until stopped), always false here.
    bool recordLocally = true;
    int frameRate = 30; // Only 30 or 60; checked against hardware capability.
    bool captureSystemAudio = false; // Applies next share, microphone stays disabled.
    int privacyMaskMode = 1; // 1: mask privacy window; 0: SDK whole-screen fallback for comparison.
};

class EncoderProbe final {
public:
    EncoderProbe();
    ~EncoderProbe();
    EncoderProbe(const EncoderProbe&) = delete;
    EncoderProbe& operator=(const EncoderProbe&) = delete;
    // 0 means asynchronous request accepted; actual SDK results are in JSON.
    // -1 invalid directory or session options; -2 busy; -3 worker creation failed.
    int Start(const std::string& filesDir, EncoderStreamHooks hooks = {}, EncoderSessionOptions options = {});
    void Stop();
    bool IsRunning();
    bool IsSystemAudioRunning();
    std::string SnapshotJson();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
EncoderProbe& GetEncoderProbe();
#endif
