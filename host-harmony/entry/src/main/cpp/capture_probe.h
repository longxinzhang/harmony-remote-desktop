#ifndef HARMONY_REMOTE_CAPTURE_PROBE_H
#define HARMONY_REMOTE_CAPTURE_PROBE_H

#include <memory>
#include <string>

// Start only schedules work. The asynchronous SDK result is in SnapshotJson().
// Stop is synchronous: it cancels work, releases capture and joins the worker.
class CaptureProbe final {
public:
    CaptureProbe();
    ~CaptureProbe();
    CaptureProbe(const CaptureProbe&) = delete;
    CaptureProbe& operator=(const CaptureProbe&) = delete;

    // 0: accepted; -1: invalid filesDir; -2: busy; -3: worker creation failed.
    int Start(const std::string& filesDir);
    void Stop();
    std::string SnapshotJson();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

CaptureProbe& GetCaptureProbe();

#endif
