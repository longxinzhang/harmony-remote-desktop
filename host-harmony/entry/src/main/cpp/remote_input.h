#ifndef HARMONY_REMOTE_REMOTE_INPUT_H
#define HARMONY_REMOTE_REMOTE_INPUT_H

#include "input_event.h"
#include <memory>
#include <functional>
#include <chrono>
#include <string>

// User consent is separate from LAN authentication. All SDK calls run on one
// worker. Submit acknowledges queue acceptance, not OS delivery or visual effect.
class RemoteInput {
public:
    RemoteInput();
    ~RemoteInput();
    RemoteInput(const RemoteInput&) = delete;
    RemoteInput& operator=(const RemoteInput&) = delete;
    void SetAllowed(bool allowed);
    bool EnableSession(bool enabled);
    bool IsSessionEnabled() const;
    bool Submit(const RemoteInputEvent& event);
    // Clears queued events and releases held inputs before later submissions.
    // Preserves an enabled session only when cleanup succeeds.
    void ReleaseAll();
    // Runs on the input worker. Returns only after the complete independent Ctrl+V
    // stroke, or cancellation. The validator runs again immediately before injection.
    bool Paste(std::function<bool()> validate, std::chrono::steady_clock::time_point deadline);
    std::string SnapshotJson() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

RemoteInput& GetRemoteInput();

#endif
