#include "audio_service.h"
#include "audio_wire.h"
#include "clipboard_wire.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
namespace {
using Clock = std::chrono::steady_clock;
struct Socket { explicit Socket(int value) : fd(value) {} ~Socket() { if (fd >= 0) close(fd); }
    void Shutdown() { if (fd >= 0) shutdown(fd, SHUT_RDWR); } int fd; };
using Sock = std::shared_ptr<Socket>;
bool Setup(int fd)
{
    int f = fcntl(fd, F_GETFL, 0); if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
#ifdef SO_NOSIGPIPE
    int yes = 1; if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) return false;
#endif
    return true;
}
std::string Random(size_t length)
{
    std::array<uint8_t, 32> bytes {}; if (length > bytes.size()) return {};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if (fd < 0) return {};
    size_t n = 0; while (n < length) { ssize_t readCount = read(fd, bytes.data() + n, length - n);
        if (readCount > 0) n += size_t(readCount); else if (readCount < 0 && errno == EINTR) continue; else { close(fd); return {}; } }
    close(fd); constexpr char HEX[] = "0123456789abcdef"; std::string out;
    for (size_t i = 0; i < length; ++i) { out += HEX[bytes[i] >> 4]; out += HEX[bytes[i] & 15]; } return out;
}
bool Equal(const std::string& a, const std::string& b)
{ if (a.size() != b.size()) return false; unsigned n = 0; for (size_t i = 0; i < a.size(); ++i) n |= unsigned(a[i] ^ b[i]); return n == 0; }
}
struct AudioService::Impl {
    explicit Impl(uint16_t p) : port(p) {}
    struct Packet { std::vector<uint8_t> bytes; uint64_t pts = 0, sequence = 0, stream = 0; uint8_t kind = 1; };
    mutable std::mutex mutex; std::mutex lifecycle; std::condition_variable wake;
    std::atomic<bool> running {false}; std::thread worker; Sock listener, client;
    uint16_t port; std::string epoch, token; uint64_t generation = 0, stream = 0, sequence = 0, resetSequence = 0;
    bool bound = false, used = false, streaming = false, resetPending = false;
    std::deque<Packet> queue; size_t queuedBytes = 0;
    uint64_t received = 0, sent = 0, dropped = 0, rejected = 0, failures = 0, highWater = 0;
    void FlushLocked() { queue.clear(); queuedBytes = 0; }
    bool Current(const Sock& socket, uint64_t gen) { std::lock_guard<std::mutex> lock(mutex); return running && generation == gen && client == socket; }
    bool IO(const Sock& socket, uint8_t* bytes, size_t size, bool sendBytes, uint64_t gen, int timeoutMs)
    {
        size_t at = 0; const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (at < size && Clock::now() < deadline && Current(socket, gen)) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            ssize_t n = sendBytes ? send(socket->fd, bytes + at, size - at, flags) : recv(socket->fd, bytes + at, size - at, 0);
            if (n > 0) at += size_t(n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { pollfd p {socket->fd, short(sendBytes ? POLLOUT : POLLIN), 0}; poll(&p, 1, 10); }
            else return false;
        }
        return at == size;
    }
    void Drop(const Sock& socket)
    {
        { std::lock_guard<std::mutex> lock(mutex); if (client != socket) return; client.reset(); bound = false; FlushLocked(); ++failures; }
        socket->Shutdown();
    }
    bool Bind(const Sock& socket, uint64_t gen)
    {
        std::array<uint8_t, 4> prefix {};
        if (!IO(socket, prefix.data(), 4, false, gen, 2000)) return false;
        uint32_t length = 0; for (auto byte : prefix) length = (length << 8) | byte;
        if (!length || length > 512) return false;
        std::string text(length, '\0'); if (!IO(socket, reinterpret_cast<uint8_t*>(text.data()), length, false, gen, 2000)) return false;
        clipboard_wire::Object obj; uint64_t version = 0;
        if (!clipboard_wire::Parser(text).Parse(obj) || obj.size() != 4 ||
            clipboard_wire::Get(obj, "type") != "audio_bind" || !clipboard_wire::Integer(obj, "version", version) || version != 1) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != gen || client != socket || epoch.empty() || used ||
                !Equal(clipboard_wire::Get(obj, "epoch"), epoch) || !Equal(clipboard_wire::Get(obj, "token"), token)) return false;
            used = true;
        }
        std::string reply = "{\"type\":\"audio_bound\",\"version\":1}";
        audio_wire::Put(prefix.data(), reply.size(), 4);
        if (!IO(socket, prefix.data(), 4, true, gen, 2000) || !IO(socket, reinterpret_cast<uint8_t*>(reply.data()), reply.size(), true, gen, 2000)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != gen || client != socket) return false;
        bound = true; resetPending = true; resetSequence = ++sequence; return true;
    }
    void Run()
    {
        while (running) {
            Sock socket, listen; uint64_t gen;
            { std::lock_guard<std::mutex> lock(mutex); socket = client; listen = listener; gen = generation; }
            if (!socket) {
                pollfd p {listen->fd, POLLIN, 0}; if (poll(&p, 1, 250) <= 0) continue;
                sockaddr_in peer {}; socklen_t size = sizeof(peer); int fd = accept(listen->fd, reinterpret_cast<sockaddr*>(&peer), &size);
                if (fd < 0) continue;
                socket = std::make_shared<Socket>(fd); if (!Setup(fd)) continue;
                int snd = 8192; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
                { std::lock_guard<std::mutex> lock(mutex); if (epoch.empty() || used || !running) continue; client = socket; gen = generation; }
                if (!Bind(socket, gen)) Drop(socket);
                continue;
            }
            Packet packet; bool have = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (client != socket || !bound) continue;
                if (resetPending) { resetPending = false; packet.kind = 2; packet.stream = stream; packet.sequence = resetSequence; have = true; }
                else if (!queue.empty()) { packet = std::move(queue.front()); queuedBytes -= packet.bytes.size(); queue.pop_front(); have = true; }
            }
            if (have) {
                auto header = audio_wire::Header(packet.kind, packet.bytes.size(), packet.pts, packet.sequence, packet.stream);
                if (!IO(socket, header.data(), header.size(), true, gen, 100) ||
                    !IO(socket, packet.bytes.data(), packet.bytes.size(), true, gen, 100)) Drop(socket);
                else { std::lock_guard<std::mutex> lock(mutex); if (generation == gen && packet.kind == 1) sent += packet.bytes.size(); }
            } else {
                pollfd p {socket->fd, POLLIN, 0}; if (poll(&p, 1, 0) > 0) {
                    // Channel is receive-only after its single binding. EOF or unexpected data closes audio only.
                    char b; ssize_t n = recv(socket->fd, &b, 1, 0); if (n >= 0 || (errno != EAGAIN && errno != EINTR)) Drop(socket);
                }
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return !running || client != socket || generation != gen || resetPending || !queue.empty();
                });
            }
        }
    }
};
AudioService::AudioService(uint16_t port) : impl_(std::make_unique<Impl>(port)) {}
AudioService::~AudioService() { Stop(); }
bool AudioService::Start(const std::string& address)
{
    auto& p = *impl_; std::lock_guard<std::mutex> life(p.lifecycle); if (p.running) return false;
    if (p.worker.joinable()) p.worker.join();
    in_addr ip {}; if (inet_pton(AF_INET, address.c_str(), &ip) != 1) return false;
    uint32_t v = ntohl(ip.s_addr); bool local = (v >> 24) == 10 || (v >> 20) == 0xac1 || (v >> 16) == 0xc0a8;
#ifdef HRD_AUDIO_TESTING
    local = local || v == 0x7f000001;
#endif
    if (!local) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return false; auto socket = std::make_shared<Socket>(fd);
    int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in endpoint {}; endpoint.sin_family = AF_INET; endpoint.sin_addr = ip; endpoint.sin_port = htons(p.port);
    if (!Setup(fd) || bind(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0 || listen(fd, 4) != 0) return false;
    socklen_t length = sizeof(endpoint); if (getsockname(fd, reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) return false;
    { std::lock_guard<std::mutex> lock(p.mutex); p.port = ntohs(endpoint.sin_port); p.listener = socket; p.running = true; }
    try { p.worker = std::thread([&p] { try { p.Run(); } catch (...) { p.running = false; } }); }
    catch (...) { p.running = false; p.listener.reset(); return false; }
    return true;
}
void AudioService::Stop()
{
    auto& p = *impl_; std::lock_guard<std::mutex> life(p.lifecycle); p.running = false; EndSession();
    { std::lock_guard<std::mutex> lock(p.mutex); if (p.listener) p.listener->Shutdown(); }
    if (p.worker.joinable()) p.worker.join();
    std::lock_guard<std::mutex> lock(p.mutex); p.listener.reset();
}
bool AudioService::BeginSession(std::string& epoch, std::string& token)
{
    EndSession(); auto e = Random(16), t = Random(32); if (e.empty() || t.empty()) return false;
    auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex); if (!p.running) return false;
    p.epoch = e; p.token = t; p.used = false; epoch = e; token = t; return true;
}
void AudioService::EndSession()
{
    auto& p = *impl_; Sock socket;
    { std::lock_guard<std::mutex> lock(p.mutex); ++p.generation; socket = std::move(p.client); p.epoch.clear(); p.token.clear();
      p.bound = false; p.used = false; p.streaming = false; p.resetPending = false; p.FlushLocked(); }
    if (socket) socket->Shutdown();
    p.wake.notify_all();
}
void AudioService::BeginStream()
{
    auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex); ++p.stream; p.streaming = true; p.FlushLocked(); p.resetPending = true; p.resetSequence = ++p.sequence; p.wake.notify_one();
}
void AudioService::EndStream()
{
    auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex); ++p.stream; p.streaming = false; p.FlushLocked(); p.resetPending = true; p.resetSequence = ++p.sequence; p.wake.notify_one();
}
bool AudioService::PublishPCM(const uint8_t* bytes, size_t length, uint64_t ptsUs)
{
    auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex);
    if (!bytes || !length || length % audio_wire::FRAME_BYTES || length > audio_wire::MAX_CALLBACK_BYTES) { ++p.rejected; return false; }
    if (!p.running || !p.bound || !p.streaming) return false;
    p.received += length;
    for (size_t at = 0; at < length;) {
        size_t count = std::min(audio_wire::CHUNK_BYTES, length - at);
        while (p.queuedBytes + count > audio_wire::QUEUE_BYTES && !p.queue.empty()) {
            p.dropped += p.queue.front().bytes.size(); p.queuedBytes -= p.queue.front().bytes.size(); p.queue.pop_front();
        }
        Impl::Packet packet; packet.bytes.assign(bytes + at, bytes + at + count);
        packet.pts = ptsUs + (at / audio_wire::FRAME_BYTES) * 1000000 / audio_wire::RATE;
        packet.sequence = ++p.sequence; packet.stream = p.stream; p.queue.push_back(std::move(packet)); p.queuedBytes += count;
        p.highWater = std::max<uint64_t>(p.highWater, p.queuedBytes); at += count;
    }
    p.wake.notify_one();
    return true;
}
uint16_t AudioService::Port() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->port; }
std::string AudioService::SnapshotJson() const
{
    const auto& p = *impl_; std::lock_guard<std::mutex> lock(p.mutex); std::ostringstream out;
    out << "{\"running\":" << (p.running ? "true" : "false") << ",\"bound\":" << (p.bound ? "true" : "false")
        << ",\"streaming\":" << (p.streaming ? "true" : "false") << ",\"sampleRate\":48000,\"channels\":2,\"format\":\"s16le\""
        << ",\"queuedBytes\":" << p.queuedBytes << ",\"highWaterBytes\":" << p.highWater << ",\"maxQueuedBytes\":" << audio_wire::QUEUE_BYTES
        << ",\"receivedBytes\":" << p.received << ",\"sentBytes\":" << p.sent << ",\"droppedBytes\":" << p.dropped
        << ",\"rejectedBuffers\":" << p.rejected << ",\"channelFailures\":" << p.failures << '}'; return out.str();
}
AudioService& GetAudioService() { static AudioService service; return service; }
