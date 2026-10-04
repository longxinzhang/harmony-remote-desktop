#ifndef HRD_CLIPBOARD_SERVICE_H
#define HRD_CLIPBOARD_SERVICE_H
#include "clipboard_platform.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
class ClipboardService final {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    using PasteExecutor = std::function<bool(std::function<bool()>, Deadline)>;
    explicit ClipboardService(std::shared_ptr<ClipboardPlatform> platform, uint16_t port = 39873);
    ~ClipboardService();
    bool Start(const std::string& localAddress);
    void Stop();
    bool BeginSession(std::string& epoch, std::string& bindToken);
    void EndSession();
    void Allow(bool allowed);
    void SetReadPermission(bool granted);
    std::pair<std::string, std::string> Paste(const std::string& epoch, const std::string& event,
        Deadline deadline, const PasteExecutor& executor);
    std::string SnapshotJson() const;
    uint16_t Port() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
ClipboardService& GetClipboardService();
#endif
