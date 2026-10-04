#ifndef HRD_AUDIO_SERVICE_H
#define HRD_AUDIO_SERVICE_H
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
// Internal playback only. This service never captures or writes audio to disk.
// A fresh one-use binding is issued by the authenticated control session.
class AudioService final {
public:
    explicit AudioService(uint16_t port = 39874);
    ~AudioService();
    bool Start(const std::string& localAddress);
    void Stop();
    bool BeginSession(std::string& epoch, std::string& bindToken);
    void EndSession();
    void BeginStream();
    void EndStream();
    // Copies only. Callback-safe; drops oldest queued PCM on overload (120 ms max).
    bool PublishPCM(const uint8_t* bytes, size_t length, uint64_t ptsUs);
    uint16_t Port() const;
    std::string SnapshotJson() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
AudioService& GetAudioService();
#endif
