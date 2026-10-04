#include "clipboard_service.h"
#include "clipboard_wire.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace clipboard_wire;
struct Socket { explicit Socket(int value) : fd(value) {} ~Socket() { if (fd >= 0) close(fd); }
    void Shutdown() { if (fd >= 0) shutdown(fd, SHUT_RDWR); } int fd; };
using Sock = std::shared_ptr<Socket>;
bool Setup(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
#ifdef SO_NOSIGPIPE
    int yes = 1; if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) return false;
#endif
    return true;
}
std::string Random(size_t length)
{
    std::array<uint8_t, 32> bytes {}; if (length > bytes.size()) return {};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if (fd < 0) return {};
    size_t done = 0; while (done < length) { ssize_t n = read(fd, bytes.data() + done, length - done);
        if (n > 0) done += size_t(n); else if (n < 0 && errno == EINTR) continue; else { close(fd); return {}; } }
    close(fd); std::string result; constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) { result += HEX[bytes[i] >> 4]; result += HEX[bytes[i] & 15]; } return result;
}
bool Equal(const std::string& a, const std::string& b)
{ if (a.size() != b.size()) return false; unsigned diff = 0; for (size_t i = 0; i < a.size(); ++i) diff |= unsigned(a[i] ^ b[i]); return diff == 0; }
struct Frame { Object header; std::string body; };
class Reader {
public:
    int Read(const Sock& socket, Frame& result)
    {
        if (started_ && Clock::now() - began_ >= std::chrono::seconds(5)) return -1;
        for (int steps = 0; steps < 32; ++steps) {
            char* target; size_t remaining;
            if (prefixAt_ < 4) { target = reinterpret_cast<char*>(prefix_.data()) + prefixAt_; remaining = 4 - prefixAt_; }
            else if (headerAt_ < headerText_.size()) { target = headerText_.data() + headerAt_; remaining = headerText_.size() - headerAt_; }
            else if (bodyAt_ < frame_.body.size()) { target = frame_.body.data() + bodyAt_; remaining = frame_.body.size() - bodyAt_; }
            else { result = std::move(frame_); *this = Reader(); return 1; }
            ssize_t n = recv(socket->fd, target, std::min<size_t>(remaining, 65536), 0);
            if (n == 0) return -1;
            if (n < 0) { if (errno == EINTR) continue; return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1; }
            if (!started_) { started_ = true; began_ = Clock::now(); }
            if (prefixAt_ < 4) {
                prefixAt_ += size_t(n);
                if (prefixAt_ == 4) { uint32_t length = 0; for (uint8_t byte : prefix_) length = (length << 8) | byte;
                    if (length == 0 || length > MAX_HEADER) return -1; headerText_.resize(length); }
            } else if (headerAt_ < headerText_.size()) {
                headerAt_ += size_t(n);
                if (headerAt_ == headerText_.size()) { uint64_t length;
                    if (!Parser(headerText_).Parse(frame_.header) || !Header(frame_.header, length)) return -1;
                    frame_.body.resize(size_t(length)); }
            } else bodyAt_ += size_t(n);
        }
        return 0;
    }
private:
    std::array<uint8_t, 4> prefix_ {}; size_t prefixAt_ = 0, headerAt_ = 0, bodyAt_ = 0;
    bool started_ = false; Time began_ {}; std::string headerText_; Frame frame_;
};
struct Update { Version version; std::string message, text, digest; };
}
struct ClipboardService::Impl {
    explicit Impl(std::shared_ptr<ClipboardPlatform> value, uint16_t p) : platform(std::move(value)), port(p) {}
    std::shared_ptr<ClipboardPlatform> platform;
    mutable std::mutex mutex; std::mutex lifecycle;
    std::atomic<bool> running {false}; std::thread worker;
    Sock listener, client;
    uint16_t port;
    std::string epoch, bindToken, error, statusMessage;
    int errorCode = 0, mode = 0;
    bool allowed = false, permission = false, readFailed = false, canRead = false, canWrite = false;
    bool bound = false, used = false, baselinePending = true, statusPending = false, selfWritePending = false;
    bool pasteReady = false;
    uint64_t generation = 0, lamport = 0, sent = 0, received = 0, applied = 0;
    Version current;
    uint32_t baseline = 0, appliedRevision = 0, pasteRevision = 0;
    std::string appliedTag, appliedDigest;
    clipboard_text_policy::Shape readShape;
    Time bindDeadline {}, retryReadAfter {};
    std::optional<Update> latest, awaiting;
    Time ackDeadline {};
    void ResetLocked()
    {
        ++generation; baselinePending = true; statusPending = true;
        latest.reset();
        appliedTag.clear(); appliedDigest.clear(); appliedRevision = 0; selfWritePending = false;
        pasteReady = false;
    }
    void ErrorLocked(const std::string& value, int code = 0) { error = value; errorCode = code; statusPending = true; }
    void Drop(const std::string& reason)
    {
        Sock old;
        { std::lock_guard<std::mutex> lock(mutex); old = std::move(client); bound = false; mode = 0;
          ResetLocked(); awaiting.reset(); if (!reason.empty()) ErrorLocked(reason); }
        if (old) old->Shutdown();
    }
    std::string Base(const std::string& type, const std::string& message, size_t size, const std::string& session)
    { return "{\"version\":1,\"type\":" + Quote(type) + ",\"sessionEpoch\":" + Quote(session) +
        ",\"messageId\":" + Quote(message) + ",\"payloadLength\":" + std::to_string(size); }
    bool Send(const Sock& socket, const std::string& header, const std::string& body = {})
    {
        if (header.size() > MAX_HEADER || body.size() > MAX_PAYLOAD) return false;
        std::array<uint8_t, 4> prefix {}; uint32_t size = uint32_t(header.size());
        for (int i = 3; i >= 0; --i) { prefix[size_t(i)] = uint8_t(size); size >>= 8; }
        const Time deadline = Clock::now() + std::chrono::seconds(5);
        auto part = [&](const uint8_t* bytes, size_t length) {
            size_t done = 0;
            while (running.load() && done < length && Clock::now() < deadline) {
                { std::lock_guard<std::mutex> lock(mutex); if (client != socket || epoch.empty()) return false; }
#ifdef MSG_NOSIGNAL
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                ssize_t n = send(socket->fd, bytes + done, length - done, flags);
                if (n > 0) done += size_t(n);
                else if (n < 0 && errno == EINTR) continue;
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { pollfd wait {socket->fd, POLLOUT, 0}; poll(&wait, 1, 10); }
                else return false;
            }
            return done == length;
        };
        return part(prefix.data(), prefix.size()) && part(reinterpret_cast<const uint8_t*>(header.data()), header.size()) &&
            part(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    }
    bool Status(const Sock& socket)
    {
        std::string header;
        { std::lock_guard<std::mutex> lock(mutex); if (!bound || !statusPending) return true;
          auto id = statusMessage.empty() ? Random(16) : statusMessage; statusMessage.clear(); if (id.empty()) return false;
          header = Base("clipboard_status", id, 0, epoch) + ",\"allowed\":" + (allowed ? "true" : "false") +
            ",\"canRead\":" + (canRead ? "true" : "false") + ",\"canWrite\":" + (canWrite ? "true" : "false") +
            ",\"mode\":" + std::to_string(mode) + ",\"error\":" + Quote(error) + "}"; statusPending = false; }
        return Send(socket, header);
    }
    void PollLocal(bool explicitPull = false)
    {
        uint64_t gen; bool read, publish; uint32_t previous;
        { std::lock_guard<std::mutex> lock(mutex); gen = generation;
          if (!bound || !allowed || mode == 0) return;
          if (permission && !readFailed && Clock::now() < retryReadAfter) return;
          read = permission && !readFailed; publish = mode == 2 || mode == 3; previous = baseline; }
        const uint32_t revision = platform->Revision();
        {
            std::lock_guard<std::mutex> lock(mutex); if (gen != generation) return;
            if (baselinePending) { baseline = revision; baselinePending = false; if (!explicitPull) return; }
            else if (revision == previous && !explicitPull) return;
            // Preserve the pending owner marker until the sample is checked. Never
            // suppress an ordinary repeated user copy solely because its hash matches.
        }
        ClipboardText sample;
        if (read) sample = platform->Read();
        std::string digest;
        if (read && sample.supported && !platform->Digest(sample.text, digest)) { sample.code = 9200004; sample.supported = false; }
        std::lock_guard<std::mutex> lock(mutex); if (gen != generation) return;
        if (platform->Revision() != revision) return; // Retry a newer local revision on the next poll.
        if (read) readShape = sample.shape;
        if (read && sample.code != 0 && sample.code != 201) {
            canRead = false; ErrorLocked("read_failed", sample.code);
            retryReadAfter = Clock::now() + std::chrono::seconds(2);
            // Keep the old baseline so a transient SDK busy can recover even if
            // the user does not copy again. One bounded attempt per two seconds.
            return;
        }
        if (!explicitPull && selfWritePending && revision == appliedRevision && read && sample.supported &&
            sample.code == 0 && sample.tag == appliedTag && Equal(digest, appliedDigest)) {
            baseline = revision; selfWritePending = false; canRead = true; statusPending = true; return;
        }
        selfWritePending = false; appliedTag.clear(); appliedDigest.clear();
        pasteReady = false; latest.reset();
        baseline = revision;
        if (read) {
            canRead = sample.code == 0;
            if (sample.code != 0) { ErrorLocked(sample.code == 201 ? "read_permission_denied" : "read_failed", sample.code); readFailed = true; }
            else if (!sample.supported) ErrorLocked("unsupported_clipboard_type");
            else { error.clear(); errorCode = 0; statusPending = true; }
        }
        if (!Next(lamport)) { ErrorLocked("counter_exhausted"); mode = 0; return; }
        current = {lamport, "harmony"};
        if (!publish || !read || !sample.supported || sample.code != 0) return;
        std::string id = Random(16); if (id.empty()) { ErrorLocked("entropy_unavailable"); return; }
        // A local text copy can be pasted in place. The peer need not write the
        // same content back, which would flatten the original rich clipboard.
        pasteRevision = revision; pasteReady = true;
        latest = Update {current, std::move(id), std::move(sample.text), std::move(digest)};
    }
    bool SendLatest(const Sock& socket)
    {
        Update update; std::string session;
        { std::lock_guard<std::mutex> lock(mutex);
          if (awaiting && Clock::now() >= ackDeadline) return false;
          if (awaiting || !latest || !allowed || (mode != 2 && mode != 3)) return true;
          update = std::move(*latest); latest.reset(); session = epoch; }
        const std::string header = Base("clipboard_update", update.message, update.text.size(), session) +
            ",\"originId\":\"harmony\",\"counter\":" + Quote(std::to_string(update.version.counter)) +
            ",\"eventId\":" + Quote(update.version.Event()) + ",\"mime\":\"text/plain;charset=utf-8\",\"sha256\":" + Quote(update.digest) + "}";
        bool ok = Send(socket, header, update.text);
        if (ok) { std::lock_guard<std::mutex> lock(mutex); if (epoch == session && bound) {
            ++sent; update.text.clear(); update.text.shrink_to_fit(); awaiting = std::move(update); ackDeadline = Clock::now() + std::chrono::seconds(5); } }
        return ok;
    }
    bool Apply(const Sock& socket, const Frame& frame)
    {
        const auto& object = frame.header;
        uint64_t counter = 0; Decimal(Get(object, "counter"), counter);
        if (Get(object, "originId") != "mac" || !UTF8(frame.body)) return false;
        std::string digest; if (!platform->Digest(frame.body, digest) || !Equal(digest, Get(object, "sha256"))) return false;
        { std::lock_guard<std::mutex> lock(mutex); lamport = std::max(lamport, counter); ++received; }
        PollLocal(); // Observe any local copy since the last poll before choosing the winner.
        std::string status, reason, header;
        {
            std::lock_guard<std::mutex> lock(mutex);
            Version incoming {counter, "mac"};
            if (!bound || Get(object, "sessionEpoch") != epoch) return false;
            if (!allowed || (mode != 1 && mode != 3)) { status = "denied"; reason = "direction_disabled"; }
            else if (!(current < incoming)) { status = "stale"; reason = "newer_clipboard_event"; }
            else if (baselinePending || platform->Revision() != baseline) { status = "stale"; reason = "local_revision_changed"; }
            else {
                const std::string tag = "hrd:" + epoch + ":" + incoming.Event();
                // This synchronous mutation is serialized with local disable and mode generation.
                pasteReady = false;
                const int code = platform->Write(frame.body, tag);
                canWrite = code == 0; statusPending = true;
                if (code != 0) { status = "failed"; reason = "write_failed"; ErrorLocked(reason, code); }
                else {
                    const uint32_t writtenRevision = platform->Revision();
                    if (writtenRevision == baseline) {
                        // SetData succeeded but revision publication has not been observed.
                        // Do not authorize paste yet. Suppress only the expected own tagged
                        // revision when a granted read confirms tag AND content later.
                        appliedTag = tag; appliedDigest = digest; appliedRevision = uint32_t(baseline + 1);
                        selfWritePending = true; current = incoming; status = "failed"; reason = "write_revision_unconfirmed";
                        ErrorLocked(reason);
                    } else if (writtenRevision != uint32_t(baseline + 1)) {
                        appliedTag.clear(); appliedDigest.clear(); status = "stale"; reason = "local_revision_changed";
                    } else {
                    baseline = appliedRevision = writtenRevision; current = incoming;
                    pasteRevision = writtenRevision; pasteReady = true;
                    appliedTag = tag; appliedDigest = digest; ++applied; status = "applied";
                    latest.reset(); error.clear(); errorCode = 0;
                    }
                }
            }
            header = Base("clipboard_applied", Get(object, "messageId"), 0, epoch) +
                ",\"eventId\":" + Quote(Get(object, "eventId")) + ",\"status\":" + Quote(status) +
                ",\"counter\":" + Quote(std::to_string(current.counter)) + ",\"originId\":" + Quote(current.origin) + ",\"error\":" + Quote(reason) + "}";
        }
        return Send(socket, header);
    }
    bool Handle(const Sock& socket, const Frame& frame)
    {
        const auto& object = frame.header; const auto type = Get(object, "type");
        bool wasBound; std::string session;
        { std::lock_guard<std::mutex> lock(mutex); wasBound = bound; session = epoch; }
        if (session.empty() || Get(object, "sessionEpoch") != session) return false;
        if (!wasBound) {
            if (type != "data_bind") return false;
            { std::lock_guard<std::mutex> lock(mutex);
              if (used || Clock::now() >= bindDeadline || !Equal(Get(object, "bindToken"), bindToken)) return false;
              used = true; bindToken.clear(); bound = true; ResetLocked(); error.clear(); errorCode = 0; }
            return Send(socket, Base("data_ready", Get(object, "messageId"), 0, session) + "}");
        }
        if (type == "clipboard_mode") {
            uint64_t requested = 0; Integer(object, "mode", requested);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (mode != int(requested)) { mode = int(requested); ResetLocked(); }
                statusMessage = Get(object, "messageId"); statusPending = true;
            }
            // Establish the activation baseline before its correlated mode ACK.
            PollLocal();
            return true;
        }
        if (type == "clipboard_pull") { PollLocal(true); return true; }
        if (type == "clipboard_update") return Apply(socket, frame);
        if (type == "clipboard_applied") {
            std::lock_guard<std::mutex> lock(mutex);
            if (!awaiting || Get(object, "messageId") != awaiting->message || Get(object, "eventId") != awaiting->version.Event()) return true; // A cancelled generation may receive a late ACK.
            uint64_t observed = 0; Decimal(Get(object, "counter"), observed); lamport = std::max(lamport, observed);
            if (Get(object, "status") != "applied") ErrorLocked("peer_update_rejected");
            awaiting.reset(); return true;
        }
        return false;
    }
    void Run()
    {
        Reader reader; Sock previous; Time accepted {};
        auto nextPoll = Clock::now();
        while (running.load()) {
            Sock socket, acceptor;
            { std::lock_guard<std::mutex> lock(mutex); socket = client; acceptor = listener; }
            if (!socket && acceptor) {
                int fd = accept(acceptor->fd, nullptr, nullptr);
                if (fd >= 0) { Sock next = std::make_shared<Socket>(fd);
                    if (Setup(fd)) { std::lock_guard<std::mutex> lock(mutex); if (!epoch.empty() && !used) { client = socket = next; accepted = Clock::now(); } } }
            }
            if (socket != previous) { reader = Reader(); previous = socket; }
            if (socket) {
                bool authenticated; { std::lock_guard<std::mutex> lock(mutex); authenticated = bound; }
                Frame frame; int result = reader.Read(socket, frame);
                if ((!authenticated && Clock::now() - accepted >= std::chrono::seconds(5)) || result < 0 || (result == 1 && !Handle(socket, frame))) {
                    Drop("clipboard_channel_rejected"); continue;
                }
                if (Clock::now() >= nextPoll) { PollLocal(); nextPoll = Clock::now() + std::chrono::milliseconds(200); }
                if (!Status(socket) || !SendLatest(socket)) { Drop("clipboard_send_or_ack_timeout"); continue; }
            }
            pollfd wait {socket ? socket->fd : (acceptor ? acceptor->fd : -1), POLLIN, 0}; poll(&wait, 1, 20);
        }
    }
};
ClipboardService::ClipboardService(std::shared_ptr<ClipboardPlatform> platform, uint16_t port)
    : impl_(std::make_unique<Impl>(std::move(platform), port)) {}
ClipboardService::~ClipboardService() { Stop(); }
bool ClipboardService::Start(const std::string& localAddress)
{
    Stop(); auto& s = *impl_; std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (!s.platform) return false;
    Sock listener = std::make_shared<Socket>(socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address {}; address.sin_family = AF_INET; address.sin_port = htons(s.port);
    int yes = 1;
    if (listener->fd < 0 || !Setup(listener->fd) || localAddress == "0.0.0.0" || inet_pton(AF_INET, localAddress.c_str(), &address.sin_addr) != 1 ||
        setsockopt(listener->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
        bind(listener->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener->fd, 2) != 0) {
        std::lock_guard<std::mutex> lock(s.mutex); s.ErrorLocked("clipboard_listen_failed"); return false; }
    socklen_t length = sizeof(address); if (getsockname(listener->fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
    { std::lock_guard<std::mutex> lock(s.mutex); s.port = ntohs(address.sin_port); s.listener = listener; s.running.store(true); s.error.clear(); s.errorCode = 0; }
    try { s.worker = std::thread([&s] { try { s.Run(); } catch (...) { s.Drop("clipboard_worker_failed"); s.running.store(false); } }); }
    catch (...) { s.running.store(false); std::lock_guard<std::mutex> lock(s.mutex); s.listener.reset(); s.ErrorLocked("clipboard_thread_failed"); return false; }
    return true;
}
void ClipboardService::Stop()
{
    auto& s = *impl_; std::lock_guard<std::mutex> lifecycle(s.lifecycle); s.running.store(false); EndSession();
    Sock listener; { std::lock_guard<std::mutex> lock(s.mutex); listener = s.listener; }
    if (listener) listener->Shutdown(); if (s.worker.joinable()) s.worker.join();
    std::lock_guard<std::mutex> lock(s.mutex); s.listener.reset();
}
bool ClipboardService::BeginSession(std::string& epoch, std::string& token)
{
    EndSession(); auto& s = *impl_; epoch = Random(16); token = Random(32);
    if (epoch.empty() || token.empty()) return false;
    std::lock_guard<std::mutex> lock(s.mutex); if (!s.running.load()) return false;
    s.epoch = epoch; s.bindToken = token; s.bindDeadline = Clock::now() + std::chrono::seconds(30); s.used = false; s.lamport = 0; s.current = {}; s.ResetLocked(); return true;
}
void ClipboardService::EndSession()
{
    auto& s = *impl_; s.Drop(""); std::lock_guard<std::mutex> lock(s.mutex); s.epoch.clear(); s.bindToken.clear(); s.used = false;
}
void ClipboardService::Allow(bool allowed)
{
    auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex);
    if (s.allowed == allowed) return; s.allowed = allowed; s.ResetLocked();
    if (allowed) { s.readFailed = false; s.error.clear(); s.errorCode = 0; }
}
void ClipboardService::SetReadPermission(bool granted)
{
    auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex);
    if (s.permission == granted) return; s.permission = granted; s.canRead = false; s.readFailed = false; s.ResetLocked();
    if (!granted) s.ErrorLocked("read_permission_denied", 201);
}
std::pair<std::string, std::string> ClipboardService::Paste(const std::string& epoch, const std::string& event,
    Deadline deadline, const PasteExecutor& executor)
{
    auto& s = *impl_; uint64_t generation;
    { std::lock_guard<std::mutex> lock(s.mutex); generation = s.generation; }
    auto valid = [&s, epoch, event, deadline, generation] {
        std::lock_guard<std::mutex> lock(s.mutex);
        return Clock::now() < deadline && generation == s.generation && s.bound && s.allowed &&
            (s.mode == 1 || s.mode == 3) && s.epoch == epoch && s.current.Event() == event &&
            s.pasteReady && !s.selfWritePending && s.platform->Revision() == s.pasteRevision && Clock::now() < deadline;
    };
    if (!valid()) return {"stale", "clipboard_changed_or_disabled"};
    bool result = executor && executor(valid, deadline);
    return result ? std::make_pair(std::string("committed"), std::string()) :
        std::make_pair(std::string("failed"), std::string("paste_cancelled_or_injection_failed"));
}
std::string ClipboardService::SnapshotJson() const
{
    const auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex); std::ostringstream out;
    out << "{\"allowed\":" << (s.allowed ? "true" : "false") << ",\"bound\":" << (s.bound ? "true" : "false")
        << ",\"mode\":" << s.mode << ",\"canRead\":" << (s.canRead ? "true" : "false") << ",\"canWrite\":" << (s.canWrite ? "true" : "false")
        << ",\"sent\":" << s.sent << ",\"received\":" << s.received << ",\"applied\":" << s.applied
        << ",\"readTypeCount\":" << clipboard_text_policy::CappedCount(s.readShape.typeCount)
        << ",\"readRecordCount\":" << clipboard_text_policy::CappedCount(s.readShape.recordCount)
        << ",\"readCategory\":" << clipboard_wire::Quote(clipboard_text_policy::Category(s.readShape.rejection))
        << ",\"error\":" << clipboard_wire::Quote(s.error) << ",\"errorCode\":" << s.errorCode << "}"; return out.str();
}
uint16_t ClipboardService::Port() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->port; }
// Construct before LanServer in NAPI initialization so server teardown runs first.
ClipboardService& GetClipboardService() { static ClipboardService service(CreateClipboardPlatform()); return service; }
