#ifndef HARMONY_REMOTE_LAN_SERVER_H
#define HARMONY_REMOTE_LAN_SERVER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include "input_event.h"
#include <string>
#include <chrono>
#include <utility>

#ifdef HRD_LAN_TESTING
// Available only to the host test executable, never in the application build.
struct LanServerTestOptions {
    uint16_t controlPort = 0;
    uint16_t videoPort = 0;
    int heartbeatMs = 2000;
    int heartbeatTimeoutMs = 6000;
    int ioTimeoutMs = 2000;
    int pinLifetimeMs = 300000;
    int sendBufferBytes = 0;
    int reconnectGraceMs = 60000;
    bool allowLoopback = true;
};
#endif

struct LanInputHooks {
    std::function<bool(bool)> enable;
    std::function<bool()> enabled;
    std::function<bool(const RemoteInputEvent&)> submit;
    std::function<void()> release;
};

struct LanClipboardHooks {
    std::function<bool(const std::string&)> start;
    std::function<void()> stop;
    std::function<bool(std::string&, std::string&)> pair;
    std::function<void()> disconnect;
    std::function<std::pair<std::string, std::string>(const std::string&, const std::string&,
        std::chrono::steady_clock::time_point)> paste;
    std::function<uint16_t()> port;
};

struct LanAudioHooks {
    std::function<bool(const std::string&)> start;
    std::function<void()> stop;
    std::function<bool(std::string&, std::string&)> pair;
    std::function<void()> disconnect;
    std::function<uint16_t()> port;
};

class LanServer final {
public:
    LanServer();
#ifdef HRD_LAN_TESTING
    explicit LanServer(const LanServerTestOptions& options);
    // Deterministic transport fault for real client integration tests. Never
    // compiled into the application library.
    void InterruptVideoWriteForTest();
#endif
    ~LanServer();
    LanServer(const LanServer&) = delete;
    LanServer& operator=(const LanServer&) = delete;

    // 0: started; -1: invalid/non-local address; -2: already running;
    // -3: entropy unavailable; -4: socket setup failed; -5: thread setup failed.
    // Empty address selects an UP local RFC1918 IPv4 interface. No wildcard bind.
    int Start(const std::string& bindAddress);
    void Stop();
    bool BeginStream();
    // Install before Start. Callbacks are serialized without holding the server state mutex.
    void SetInputHooks(LanInputHooks hooks);
    void SetClipboardHooks(LanClipboardHooks hooks);
    void SetAudioHooks(LanAudioHooks hooks);
    // Configure with UIAbilityContext.filesDir before Start. HUKS keeps the
    // private identity; only signed public peer keys are written here.
    bool ConfigurePairingStorage(const std::string& appFilesDirectory);
    void SetPairingAllowed(bool allowed);
    bool RevokePairedDevices();
    // The existing authorized encoder consumes this after a reconnect. No new
    // screen-capture authorization is requested or restored by the server.
    bool ConsumeKeyframeRequest();
    // True only while a freshly authenticated same-peer session inherits the
    // bounded, still-authorized capture. Used to gate auxiliary audio resume.
    bool CanResumeCapture();
    // Copies into a bounded queue; performs no socket I/O. CONFIG precedes IDR.
    // At most 3 queued AUs (including empty EOS) and one CONFIG, plus sender's packet.
    bool Publish(const uint8_t* data, size_t size, uint64_t ptsUs,
        bool config, bool keyframe, bool eos);
    bool IsStreamCancelled();
    // Success requires a published EOS and allows its queued bytes to drain.
    void EndStream(bool success);
    // PIN is UI-only. Session tokens are never included, even with includePin.
    std::string SnapshotJson(bool includePin = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

LanServer& GetLanServer();
#endif
