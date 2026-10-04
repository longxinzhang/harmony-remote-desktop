#include "lan_server.h"
#include "clipboard_wire.h"
#include "pairing_identity.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <set>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr size_t MAX_JSON = 4096;
constexpr size_t MAX_AU = 8 * 1024 * 1024;
constexpr size_t MAX_CONFIG = 256 * 1024;
constexpr size_t MAX_QUEUED_AUS = 3;

struct Options {
    uint16_t controlPort = 39871;
    uint16_t videoPort = 39872;
    int heartbeatMs = 2000;
    int heartbeatTimeoutMs = 6000;
    int ioTimeoutMs = 2000;
    int pinLifetimeMs = 300000;
    int sendBufferBytes = 0;
    int reconnectGraceMs = 60000;
    bool allowLoopback = false;
};

struct Socket {
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) close(fd); }
    void Shutdown() const { if (fd >= 0) shutdown(fd, SHUT_RDWR); }
    int fd;
};
using Sock = std::shared_ptr<Socket>;

bool ConfigureSocket(int fd, int sendBuffer = 0)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
#ifdef SO_NOSIGPIPE
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0) return false;
#endif
    if (sendBuffer > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer)) < 0)
        return false;
    return true;
}

bool IsPrivate(uint32_t host)
{
    return (host & 0xff000000U) == 0x0a000000U ||
        (host & 0xfff00000U) == 0xac100000U || (host & 0xffff0000U) == 0xc0a80000U;
}

std::string SelectAddress(const std::string& requested, bool allowLoopback)
{
    in_addr parsed {};
    if (!requested.empty() && inet_pton(AF_INET, requested.c_str(), &parsed) != 1) return {};
    std::vector<std::string> candidates;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    for (ifaddrs* p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP)) continue;
        const auto* a = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        uint32_t host = ntohl(a->sin_addr.s_addr);
        bool allowed = IsPrivate(host) && !(p->ifa_flags & IFF_LOOPBACK);
#ifdef HRD_LAN_TESTING
        allowed = allowed || (allowLoopback && host == 0x7f000001U);
#else
        (void)allowLoopback;
#endif
        if (!allowed) continue;
        char text[INET_ADDRSTRLEN] {};
        if (inet_ntop(AF_INET, &a->sin_addr, text, sizeof(text))) candidates.emplace_back(text);
    }
    freeifaddrs(list);
    std::sort(candidates.begin(), candidates.end());
    if (requested.empty()) return candidates.empty() ? std::string() : candidates.front();
    return std::find(candidates.begin(), candidates.end(), requested) == candidates.end() ? std::string() : requested;
}

Sock Listen(const std::string& ip, uint16_t& port)
{
    Sock s = std::make_shared<Socket>(socket(AF_INET, SOCK_STREAM, 0));
    if (s->fd < 0 || !ConfigureSocket(s->fd)) return {};
    int yes = 1;
    if (setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) return {};
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1 ||
        bind(s->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(s->fd, 8) != 0)
        return {};
    socklen_t length = sizeof(address);
    if (getsockname(s->fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return {};
    port = ntohs(address.sin_port);
    return s;
}

Sock Accept(const Sock& listener, uint32_t& peer, const Options& options)
{
    sockaddr_in address {};
    socklen_t length = sizeof(address);
    int fd = accept(listener->fd, reinterpret_cast<sockaddr*>(&address), &length);
    if (fd < 0) return {};
    Sock s = std::make_shared<Socket>(fd);
    peer = ntohl(address.sin_addr.s_addr);
    bool allowed = IsPrivate(peer);
#ifdef HRD_LAN_TESTING
    allowed = allowed || (options.allowLoopback && peer == 0x7f000001U);
#endif
    int yes = 1;
    if (!allowed || !ConfigureSocket(fd, options.sendBufferBytes) ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) return {};
    return s;
}

bool RandomBytes(uint8_t* bytes, size_t size)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, bytes + done, size - done);
        if (n > 0) done += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else { close(fd); return false; }
    }
    close(fd);
    return true;
}

std::string NewPin()
{
    uint32_t value = 0;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % 1000000U);
    do { if (!RandomBytes(reinterpret_cast<uint8_t*>(&value), sizeof(value))) return {}; } while (value >= limit);
    std::string digits = std::to_string(value % 1000000U);
    return std::string(6 - digits.size(), '0') + digits;
}

std::string NewToken()
{
    std::array<uint8_t, 32> bytes {};
    if (!RandomBytes(bytes.data(), bytes.size())) return {};
    constexpr char HEX[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (uint8_t b : bytes) { token.push_back(HEX[b >> 4]); token.push_back(HEX[b & 15]); }
    return token;
}

bool SecretEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned int diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string Quote(const std::string& input)
{
    std::string out = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c >= 32 && c < 127) out += static_cast<char>(c);
        else out += '?';
    }
    return out + "\"";
}

// Protocol fields are ASCII. This deliberately accepts only flat objects and
// ASCII JSON strings, rejecting duplicate keys, nesting, arrays, and trailing data.
struct Value { std::string text; bool isString = false; };
using Object = std::map<std::string, Value>;
class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}
    bool Parse(Object& object)
    {
        White();
        if (!Take('{')) return false;
        White();
        if (Take('}')) { White(); return at_ == text_.size(); }
        for (size_t count = 0; count < 16; ++count) {
            std::string key;
            if (!String(key) || key.empty() || key.size() > 64) return false;
            White(); if (!Take(':')) return false; White();
            Value value;
            if (at_ < text_.size() && text_[at_] == '"') {
                value.isString = true;
                if (!String(value.text)) return false;
            } else {
                size_t begin = at_;
                while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' && !Space(text_[at_])) ++at_;
                value.text = text_.substr(begin, at_ - begin);
                if (value.text != "true" && value.text != "false" && value.text != "null" && !Number(value.text)) return false;
            }
            if (!object.emplace(key, value).second) return false;
            White();
            if (Take('}')) { White(); return at_ == text_.size(); }
            if (!Take(',')) return false;
            White();
        }
        return false;
    }
private:
    static bool Space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
    void White() { while (at_ < text_.size() && Space(text_[at_])) ++at_; }
    bool Take(char c) { if (at_ >= text_.size() || text_[at_] != c) return false; ++at_; return true; }
    static bool Number(const std::string& s)
    {
        size_t i = 0;
        if (i < s.size() && s[i] == '-') ++i;
        if (i == s.size()) return false;
        if (s[i] == '0') ++i;
        else { if (s[i] < '1' || s[i] > '9') return false; while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
        if (i < s.size() && s[i] == '.') {
            size_t start = ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (start == i) return false;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i; if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            size_t start = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (start == i) return false;
        }
        return i == s.size();
    }
    bool String(std::string& out)
    {
        if (!Take('"')) return false;
        while (at_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"') return true;
            if (c < 32 || c >= 127) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (at_ == text_.size()) return false;
            char escape = text_[at_++];
            switch (escape) {
                case '"': case '\\': case '/': out.push_back(escape); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (at_ == text_.size()) return false;
                        char h = text_[at_++];
                        unsigned int digit;
                        if (h >= '0' && h <= '9') digit = static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') digit = static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') digit = static_cast<unsigned int>(h - 'A' + 10);
                        else return false;
                        code = code * 16 + digit;
                    }
                    if (code >= 127) return false;
                    out.push_back(static_cast<char>(code)); break;
                }
                default: return false;
            }
        }
        return false;
    }
    const std::string& text_;
    size_t at_ = 0;
};

std::string Field(const Object& o, const std::string& key)
{
    auto it = o.find(key);
    return it != o.end() && it->second.isString ? it->second.text : std::string();
}

void Put32(uint8_t* p, uint32_t value)
{
    for (int i = 3; i >= 0; --i) { p[i] = static_cast<uint8_t>(value); value >>= 8; }
}
void Put64(uint8_t* p, uint64_t value)
{
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(value); value >>= 8; }
}
uint32_t Get32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool SendAll(const Sock& socket, const uint8_t* data, size_t size, Time deadline,
    const std::atomic<bool>& running, const std::atomic<bool>* cancelled = nullptr)
{
    size_t sent = 0;
    while (sent < size) {
        if (!running.load() || (cancelled && cancelled->load()) || Clock::now() >= deadline) return false;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = send(socket->fd, data + sent, size - sent, flags);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p {socket->fd, POLLOUT, 0};
            int result = poll(&p, 1, 20);
            if (result < 0 && errno != EINTR) return false;
            if (result > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool SendJson(const Sock& socket, const std::string& json, int timeout,
    const std::atomic<bool>& running)
{
    if (!socket || json.empty() || json.size() > MAX_JSON) return false;
    std::vector<uint8_t> bytes(4 + json.size());
    Put32(bytes.data(), static_cast<uint32_t>(json.size()));
    std::memcpy(bytes.data() + 4, json.data(), json.size());
    return SendAll(socket, bytes.data(), bytes.size(), Clock::now() + std::chrono::milliseconds(timeout), running);
}

struct Reader {
    std::array<uint8_t, 4> header {};
    size_t headerRead = 0;
    std::string body;
    size_t bodyRead = 0;
    Time firstByte {};
    // 1 complete, 0 incomplete, -1 invalid/error/deadline, -2 clean EOF.
    int Read(const Sock& socket, int timeout, std::string& result)
    {
        if (headerRead && Clock::now() - firstByte >= std::chrono::milliseconds(timeout)) return -1;
        for (int attempt = 0; attempt < 8; ++attempt) {
            bool readingHeader = headerRead < header.size();
            uint8_t* target = readingHeader ? header.data() + headerRead : reinterpret_cast<uint8_t*>(&body[0]) + bodyRead;
            size_t remaining = readingHeader ? header.size() - headerRead : body.size() - bodyRead;
            ssize_t n = recv(socket->fd, target, remaining, 0);
            if (n == 0) return headerRead ? -1 : -2;
            if (n < 0) {
                if (errno == EINTR) continue;
                return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
            }
            if (!headerRead) firstByte = Clock::now();
            if (readingHeader) {
                headerRead += static_cast<size_t>(n);
                if (headerRead == header.size()) {
                    uint32_t length = Get32(header.data());
                    if (length == 0 || length > MAX_JSON) return -1;
                    body.resize(length);
                }
            } else {
                bodyRead += static_cast<size_t>(n);
                if (bodyRead == body.size()) {
                    result = std::move(body);
                    *this = Reader();
                    return 1;
                }
            }
        }
        return 0;
    }
};

struct Packet {
    std::vector<uint8_t> bytes;
    uint64_t pts = 0;
    uint8_t type = 2;
    uint16_t flags = 0;
};
} // namespace


namespace {
bool BooleanField(const Object& object, const char* key, bool& value)
{
    auto it = object.find(key);
    if (it == object.end() || it->second.isString || (it->second.text != "true" && it->second.text != "false")) return false;
    value = it->second.text == "true"; return true;
}
bool NumberField(const Object& object, const char* key, double minimum, double maximum, double& value)
{
    auto it = object.find(key);
    if (it == object.end() || it->second.isString) return false;
    char* end = nullptr; value = std::strtod(it->second.text.c_str(), &end);
    return end != it->second.text.c_str() && *end == 0 && std::isfinite(value) && value >= minimum && value <= maximum;
}
bool KeyCode(const std::string& code)
{
    if (code.size() == 5 && code.compare(0, 4, "KEY_") == 0 &&
        ((code[4] >= 'A' && code[4] <= 'Z') || (code[4] >= '0' && code[4] <= '9'))) return true;
    static const std::set<std::string> keys {
        "KEY_MINUS", "KEY_EQUALS", "KEY_LEFT_BRACKET", "KEY_RIGHT_BRACKET", "KEY_BACKSLASH",
        "KEY_SEMICOLON", "KEY_APOSTROPHE", "KEY_GRAVE", "KEY_COMMA", "KEY_PERIOD", "KEY_SLASH",
        "KEY_ENTER", "KEY_ESCAPE", "KEY_TAB", "KEY_SPACE", "KEY_BACKSPACE", "KEY_DELETE", "KEY_UP", "KEY_DOWN",
        "KEY_LEFT", "KEY_RIGHT", "KEY_HOME", "KEY_END", "KEY_PAGE_UP", "KEY_PAGE_DOWN",
        "KEY_F1", "KEY_F2", "KEY_F3", "KEY_F4", "KEY_F5", "KEY_F6", "KEY_F7", "KEY_F8", "KEY_F9", "KEY_F10", "KEY_F11", "KEY_F12",
        "KEY_SHIFT_LEFT", "KEY_SHIFT_RIGHT", "KEY_CTRL_LEFT", "KEY_CTRL_RIGHT", "KEY_ALT_LEFT", "KEY_ALT_RIGHT",
        "KEY_META_LEFT", "KEY_META_RIGHT", "KEY_CAPS_LOCK"
    };
    return keys.count(code) != 0;
}
bool ParseInput(const Object& object, const std::string& type, RemoteInputEvent& event)
{
    if (type == "mouse_move") {
        event.kind = RemoteInputEvent::Kind::Move;
        return object.size() == 4 && NumberField(object, "x", 0, 1, event.x) && NumberField(object, "y", 0, 1, event.y);
    }
    if (type == "scroll") {
        event.kind = RemoteInputEvent::Kind::Scroll;
        return object.size() == 4 && NumberField(object, "dx", -120, 120, event.dx) && NumberField(object, "dy", -120, 120, event.dy);
    }
    const auto action = Field(object, "action");
    if (object.size() != 4 || (action != "down" && action != "up")) return false;
    event.down = action == "down";
    if (type == "mouse_button") {
        event.kind = RemoteInputEvent::Kind::Button; event.button = Field(object, "button");
        return event.button == "left" || event.button == "right" || event.button == "middle";
    }
    if (type == "key") { event.kind = RemoteInputEvent::Kind::Key; event.code = Field(object, "code"); return KeyCode(event.code); }
    return false;
}
} // namespace

struct LanServer::Impl {
    explicit Impl(Options value = Options()) : options(value) {}
    Options options;
    std::mutex lifecycle;
    std::mutex mutex;
    std::mutex inputMutex; // Lock order: inputMutex, then mutex. External hooks never hold mutex.
    LanInputHooks inputHooks;
    LanClipboardHooks clipboardHooks;
    bool clipboardStarted = false;
    LanAudioHooks audioHooks;
    bool audioStarted = false;
    PairingIdentity identity;
    bool pairingAllowed = false, modernPeer = false;
    std::string challenge, currentPeerKey, capturePeerKey;
    bool reconnecting = false, reconnectRequested = false, keyframeRequested = false, awaitingReconnectIdr = false;
    Time reconnectDeadline {};
    std::vector<uint8_t> latestConfig;
    uint64_t reconnectCount = 0, pairingGeneration = 0;
    std::deque<std::string> pasteOperations;
    bool inputEnabled = false, inputDeactivatePending = false, inputStatusPending = false;
    uint64_t inputAccepted = 0, inputRejected = 0, inputReleases = 0;
    Time lastInputPoll {};
    std::condition_variable cv;
    std::thread network;
    std::thread sender;
    std::atomic<bool> running {false};
    std::atomic<bool> cancelled {true};
    Sock controlListener, videoListener, control, video;
    std::string address, pin, token, error;
    std::string status = "STOPPED";
    Time pinExpires {};
    uint16_t controlPort = 0, videoPort = 0;
    bool pinUsed = false, paired = false, streaming = false;
    bool disconnectRequested = false, needsConfig = true, needsIdr = true;
    bool eosQueued = false, eosSent = false, ending = false, hasAu = false;
    bool encoderEndedSuccess = false, streamCompleted = false, eosSending = false;
    unsigned int pinAttempts = 0;
    uint64_t epoch = 0, streamId = 0, lastPts = 0;
    uint64_t sentFrames = 0, sentBytes = 0, sentPackets = 0, abortedStreams = 0;
    size_t queuedAus = 0, queuedConfigs = 0, queueHighWater = 0;
    uint32_t sequence = 0;
    std::deque<Packet> queue;

    bool InputSupportedLocked() const
    { return inputHooks.enable && inputHooks.enabled && inputHooks.submit && inputHooks.release; }

    void DisableInput()
    {
        std::lock_guard<std::mutex> gate(inputMutex);
        LanInputHooks hooks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            hooks = inputHooks;
            if (inputEnabled) inputStatusPending = true;
            inputEnabled = false; inputDeactivatePending = false;
        }
        // Enable(false) also releases held keys/buttons. It never requests permission.
        try { if (hooks.enable) hooks.enable(false); } catch (...) {}
    }

    bool InputStatus(const Sock& socket)
    {
        std::string session; bool enabled;
        { std::lock_guard<std::mutex> lock(mutex); session = token; enabled = inputEnabled; inputStatusPending = false; }
        return !session.empty() && SendJson(socket, "{\"type\":\"input_status\",\"enabled\":" +
            std::string(enabled ? "true" : "false") + ",\"sessionToken\":" + Quote(session) + "}", options.ioTimeoutMs, running);
    }

    bool HandleInput(const Sock& socket, const Object& object, const std::string& type)
    {
        RemoteInputEvent event; bool requested = false;
        const bool enableMessage = type == "input_enable", releaseMessage = type == "release_all_keys";
        const bool valid = enableMessage ? object.size() == 3 && BooleanField(object, "enabled", requested) :
            releaseMessage ? object.size() == 2 : ParseInput(object, type, event);
        if (!valid) { DisableInput(); ReplyError(socket, "invalid_input_message"); return false; }
        bool rejected = false, sendStatus = enableMessage;
        {
            std::lock_guard<std::mutex> gate(inputMutex);
            LanInputHooks hooks; bool allowed, wasEnabled;
            {
                std::lock_guard<std::mutex> lock(mutex);
                hooks = inputHooks;
                allowed = InputSupportedLocked() && running.load() && paired && video && streaming && hasAu &&
                    !cancelled.load() && !disconnectRequested && !eosQueued && !inputDeactivatePending;
                wasEnabled = inputEnabled;
            }
            try {
                if (enableMessage) {
                    if (requested && !allowed && hooks.enable) hooks.enable(false);
                    const bool enabled = (!requested || allowed) && hooks.enable && hooks.enable(requested) &&
                        (!requested || (hooks.enabled && hooks.enabled()));
                    std::lock_guard<std::mutex> lock(mutex);
                    inputEnabled = requested && enabled && streaming && !cancelled.load() && !eosQueued;
                    // Concurrent stream failure closes the gate even if enable completed later.
                    if (requested && enabled && !inputEnabled) inputDeactivatePending = true;
                } else if (releaseMessage) {
                    if (hooks.release) hooks.release();
                    std::lock_guard<std::mutex> lock(mutex); ++inputReleases;
                } else if (!allowed || !wasEnabled || !hooks.enabled || !hooks.enabled()) {
                    if (hooks.enable) hooks.enable(false);
                    std::lock_guard<std::mutex> lock(mutex); inputEnabled = false; ++inputRejected; sendStatus = true;
                } else if (!hooks.submit(event)) {
                    rejected = true;
                } else { std::lock_guard<std::mutex> lock(mutex); ++inputAccepted; }
            } catch (...) { rejected = true; }
        }
        if (rejected) { DisableInput(); ReplyError(socket, "input_submission_failed"); return false; }
        return !sendStatus || InputStatus(socket);
    }

    bool PollInput(const Sock& socket)
    {
        bool pending, poll;
        { std::lock_guard<std::mutex> lock(mutex); pending = inputDeactivatePending;
          poll = inputEnabled && Clock::now() - lastInputPoll >= std::chrono::milliseconds(100); }
        if (pending) DisableInput();
        if (poll) {
            std::lock_guard<std::mutex> gate(inputMutex);
            LanInputHooks hooks; bool enabled;
            { std::lock_guard<std::mutex> lock(mutex); hooks = inputHooks; enabled = inputEnabled; lastInputPoll = Clock::now(); }
            bool actual = false;
            try { actual = enabled && hooks.enabled && hooks.enabled(); } catch (...) {}
            if (enabled && !actual) {
                try { if (hooks.enable) hooks.enable(false); } catch (...) {}
                std::lock_guard<std::mutex> lock(mutex); inputEnabled = false; inputStatusPending = true;
            }
        }
        bool notify;
        { std::lock_guard<std::mutex> lock(mutex); notify = paired && inputStatusPending; }
        return !socket || !notify || InputStatus(socket);
    }

    void ClearQueueLocked() { queue.clear(); queuedAus = 0; queuedConfigs = 0; }
    void AbortLocked(const std::string& reason)
    {
        if (streaming && !cancelled.load()) ++abortedStreams;
        cancelled.store(true);
        reconnecting = false; reconnectRequested = false; keyframeRequested = false; awaitingReconnectIdr = false;
        inputStatusPending = inputStatusPending || inputEnabled;
        inputEnabled = false; inputDeactivatePending = true;
        streaming = false;
        ending = false;
        ClearQueueLocked();
        error = reason;
        status = "STREAM_FAILED";
        disconnectRequested = true;
        cv.notify_all();
    }

    void Disconnect(const std::string& reason, bool allowCompletedEos = false, bool allowReconnect = false)
    {
        DisableInput();
        LanClipboardHooks clipboard;
        { std::lock_guard<std::mutex> lock(mutex); clipboard = clipboardHooks; pasteOperations.clear(); }
        try { if (clipboard.disconnect) clipboard.disconnect(); } catch (...) {}
        LanAudioHooks audio;
        { std::lock_guard<std::mutex> lock(mutex); audio = audioHooks; }
        try { if (audio.disconnect) audio.disconnect(); } catch (...) {}
        Sock oldControl, oldVideo;
        {
            std::unique_lock<std::mutex> lock(mutex);
            // The receiver can observe EOS and close before SenderLoop records
            // its successful send. Let that bounded send publish its result first.
            if (allowCompletedEos && running.load() && eosSending && !eosSent)
                cv.wait_for(lock, std::chrono::milliseconds(options.ioTimeoutMs), [&] { return !eosSending || !running.load(); });
            oldControl = std::move(control); oldVideo = std::move(video);
            // Client may close immediately after receiving EOS, before the encoder
            // worker calls EndStream(true). Preserve this completed network drain.
            bool normalEosClose = allowCompletedEos && eosSent && !cancelled.load();
            bool wasPaired = paired;
            // Keep only the already authorized capture alive, for a bounded
            // interval. No old frames or input permission cross this boundary.
            bool keepCapture = running.load() && !normalEosClose && !cancelled.load() && !eosQueued &&
                !(allowCompletedEos && reason.empty()) &&
                ((reconnecting && Clock::now() < reconnectDeadline) || (allowReconnect && modernPeer && streaming));
            if (keepCapture && !reconnecting) reconnectDeadline = Clock::now() + std::chrono::milliseconds(options.reconnectGraceMs);
            if (!normalEosClose && !keepCapture && streaming && !cancelled.load()) ++abortedStreams;
            if (!normalEosClose && !keepCapture) cancelled.store(true);
            reconnecting = keepCapture; streaming = keepCapture; paired = false; ending = false; modernPeer = false;
            challenge.clear(); currentPeerKey.clear(); token.clear(); ClearQueueLocked(); disconnectRequested = false; reconnectRequested = false; ++epoch;
            if (keepCapture) { needsConfig = true; needsIdr = true; hasAu = false; sequence = 0; lastPts = 0; }
            if (!normalEosClose && !reason.empty()) error = reason;
            status = running.load() ? (keepCapture ? "RECONNECTING" : "LISTENING") : "STOPPED";
            // Legacy/manual clients always get a new one-use PIN without a
            // service restart. A failed attempt never renews the old PIN TTL.
            if (running.load() && wasPaired) { pin = NewPin(); pinUsed = false; pinAttempts = 0; pinExpires = Clock::now() + std::chrono::milliseconds(options.pinLifetimeMs); }
        }
        // shared ownership prevents descriptor reuse while a sender still holds it.
        if (oldControl) oldControl->Shutdown();
        if (oldVideo) oldVideo->Shutdown();
        cv.notify_all();
    }

    void StopInternal()
    {
        running.store(false);
        Sock a, b;
        {
            std::lock_guard<std::mutex> lock(mutex);
            a = controlListener; b = videoListener;
        }
        if (a) a->Shutdown();
        if (b) b->Shutdown();
        Disconnect("", true);
        cv.notify_all();
        if (network.joinable()) network.join();
        if (sender.joinable()) sender.join();
        LanClipboardHooks clipboard;
        { std::lock_guard<std::mutex> lock(mutex); clipboard = clipboardHooks; clipboardStarted = false; }
        try { if (clipboard.stop) clipboard.stop(); } catch (...) {}
        LanAudioHooks audio;
        { std::lock_guard<std::mutex> lock(mutex); audio = audioHooks; audioStarted = false; }
        try { if (audio.stop) audio.stop(); } catch (...) {}
        identity.ClearEphemeral();
        // A final sweep covers a worker finishing an accept during shutdown.
        Disconnect("", true);
        std::lock_guard<std::mutex> lock(mutex);
        controlListener.reset(); videoListener.reset();
        pin.clear(); token.clear(); status = "STOPPED";
    }

    void SenderLoop()
    {
        while (running.load()) {
            Packet packet;
            Sock socket;
            uint64_t currentEpoch, currentStream;
            uint32_t currentSequence;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return !running.load() || !queue.empty(); });
                if (!running.load()) break;
                packet = std::move(queue.front()); queue.pop_front();
                if (packet.type == 1) --queuedConfigs; else --queuedAus;
                socket = video; currentEpoch = epoch; currentStream = streamId;
                currentSequence = sequence++;
                eosSending = (packet.flags & 2) != 0;
            }
            std::array<uint8_t, 24> header {};
            std::memcpy(header.data(), "HRD1", 4);
            header[4] = 1; header[5] = packet.type;
            header[6] = static_cast<uint8_t>(packet.flags >> 8); header[7] = static_cast<uint8_t>(packet.flags);
            Put32(header.data() + 8, currentSequence); Put64(header.data() + 12, packet.pts);
            Put32(header.data() + 20, static_cast<uint32_t>(packet.bytes.size()));
            Time deadline = Clock::now() + std::chrono::milliseconds(options.ioTimeoutMs);
            bool ok = socket && SendAll(socket, header.data(), header.size(), deadline, running, &cancelled) &&
                SendAll(socket, packet.bytes.data(), packet.bytes.size(), deadline, running, &cancelled);
            std::lock_guard<std::mutex> lock(mutex);
            if (currentEpoch == epoch && currentStream == streamId) eosSending = false;
            cv.notify_all();
            if (currentEpoch != epoch || currentStream != streamId || cancelled.load()) continue;
            if (!ok) {
                if (modernPeer && streaming && !eosQueued) { reconnectRequested = true; disconnectRequested = true; error = "video_send_failed_or_timed_out"; }
                else AbortLocked("video_send_failed_or_timed_out");
                continue;
            }
            ++sentPackets; sentBytes += header.size() + packet.bytes.size();
            if (packet.type == 2 && !packet.bytes.empty()) ++sentFrames;
            if (packet.flags & 2) {
                inputStatusPending = inputStatusPending || inputEnabled;
                inputEnabled = false; inputDeactivatePending = true;
                eosSent = true;
                if (ending) { streaming = false; ending = false; streamCompleted = encoderEndedSuccess; status = "VIDEO_READY"; }
            }
        }
    }

    bool HandleControl(const Sock& socket, const std::string& text, bool& hello, Time& lastRx, Time& lastPing, bool& cleanStop)
    {
        Object object;
        if (!Parser(text).Parse(object)) { ReplyError(socket, "invalid_json"); return false; }
        const std::string type = Field(object, "type");
        bool currentlyPaired;
        std::string session;
        { std::lock_guard<std::mutex> lock(mutex); currentlyPaired = paired; session = token; }
        if (!currentlyPaired) {
            if (!hello) {
                auto protocol = object.find("protocol");
                if (type != "hello" || protocol == object.end() || protocol->second.isString || protocol->second.text != "1") {
                    ReplyError(socket, "hello_protocol_required"); return false;
                }
                hello = true;
                { std::lock_guard<std::mutex> lock(mutex); status = "AWAITING_PAIR"; }
                bool supported; { std::lock_guard<std::mutex> lock(mutex); supported = InputSupportedLocked(); }
                std::string identityFields;
                if (identity.Available()) {
                    std::string fresh = NewToken(), publicKey = identity.PublicKey();
                    std::string proof = identity.Sign("HRDHELLO1\n" + fresh + "\n" + publicKey);
                    if (fresh.empty() || proof.empty()) { ReplyError(socket, "pairing_identity_unavailable"); return false; }
                    bool enrollmentAllowed;
                    { std::lock_guard<std::mutex> lock(mutex); challenge = fresh; enrollmentAllowed = pairingAllowed; }
                    identityFields = ",\"pairingScheme\":\"p256-sha256-v1\",\"hostPublicKey\":" + Quote(publicKey) +
                        ",\"challenge\":" + Quote(fresh) + ",\"hostHelloSignature\":" + Quote(proof) +
                        ",\"rememberAllowed\":" + (enrollmentAllowed ? "true" : "false");
                }
                return SendJson(socket, "{\"type\":\"hello_ack\",\"protocol\":1,\"deviceName\":\"HarmonyOS-PC\",\"os\":\"HarmonyOS 7\",\"pairingRequired\":true,\"timestampSource\":\"encoder_callback_monotonic\",\"nativePtsUnitVerified\":false,\"inputSupported\":" + std::string(supported ? "true" : "false") + ",\"clipboardSupported\":" + (clipboardStarted ? "true" : "false") +
                    (clipboardStarted ? ",\"clipboardPort\":" + std::to_string(clipboardHooks.port()) : "") +
                    ",\"rttSupported\":true,\"audioSupported\":" + (audioStarted ? "true" : "false") +
                    (audioStarted ? ",\"audioPort\":" + std::to_string(audioHooks.port()) : "") + identityFields + "}", options.ioTimeoutMs, running);
            }
            const bool resume = type == "pair_resume";
            if (type != "pair" && !resume) { ReplyError(socket, "pair_required"); return false; }
            const std::string clientKey = Field(object, "clientPublicKey");
            const bool modern = !clientKey.empty() || resume || object.count("remember") || object.count("clientSignature");
            bool remember = false;
            std::string transcript;
            std::string failure;
            uint64_t authorizationGeneration;
            { std::lock_guard<std::mutex> lock(mutex); authorizationGeneration = pairingGeneration; }
            if (modern) {
                std::string fresh; bool enrollmentAllowed;
                { std::lock_guard<std::mutex> lock(mutex); fresh = std::move(challenge); challenge.clear(); enrollmentAllowed = pairingAllowed; }
                if (!identity.Available() || fresh.empty() || !BooleanField(object, "remember", remember) ||
                    !clipboard_wire::Hex(Field(object, "clientChallenge"), 64) ||
                    !PairingIdentity::ValidPublicKey(clientKey) || object.size() != (resume ? 5 : 6)) failure = "invalid_pairing_proof";
                else {
                    transcript = PairingIdentity::Transcript(fresh, Field(object, "clientChallenge"), identity.PublicKey(), clientKey, resume, remember);
                    if (!PairingIdentity::Verify(clientKey, transcript, Field(object, "clientSignature"))) failure = "invalid_pairing_proof";
                    else if (resume && !identity.Trusted(clientKey, remember)) failure = "pairing_not_trusted";
                    else if (!resume && remember && !enrollmentAllowed) failure = "persistent_pairing_not_allowed";
                }
            } else if (object.size() != 2) failure = "invalid_pair_message";
            if (failure.empty() && !resume) {
                std::lock_guard<std::mutex> lock(mutex);
                if (pinUsed || pin.empty() || Clock::now() >= pinExpires) { pin.clear(); failure = "pin_unavailable_restart_server"; }
                else if (!SecretEqual(Field(object, "pin"), pin)) {
                    ++pinAttempts;
                    if (pinAttempts >= 5) { pinUsed = true; pin.clear(); }
                    failure = "invalid_pin";
                }
            }
            if (!failure.empty()) { ReplyError(socket, failure); return false; }
            session = NewToken();
            if (session.empty()) { ReplyError(socket, "entropy_unavailable"); return false; }
            std::string identityFields;
            if (modern) {
                std::string proof = identity.Sign(transcript + "\nhost\n" + session);
                if (proof.empty()) { ReplyError(socket, "pairing_identity_unavailable"); return false; }
                identityFields = ",\"hostSignature\":" + Quote(proof) + ",\"remembered\":" + (remember ? "true" : "false");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running.load() || authorizationGeneration != pairingGeneration || disconnectRequested) return false;
                if (modern && !resume && ((remember && !pairingAllowed) || !identity.Enroll(clientKey, remember))) {
                    error = "pairing_store_failed_or_revoked"; return false;
                }
                if (reconnecting && (!modern || clientKey != capturePeerKey)) {
                    // A different PIN-authorized viewer must request a new
                    // screen share; it cannot inherit another peer's capture.
                    cancelled.store(true); streaming = false; reconnecting = false; ClearQueueLocked();
                }
                currentPeerKey = modern ? clientKey : "";
                pinUsed = true; pin.clear(); token = session; paired = true; modernPeer = modern; status = "PAIRED";
            }
            std::string clipboardFields;
            if (clipboardStarted && clipboardHooks.pair) {
                std::string clipboardEpoch, bindToken;
                if (clipboardHooks.pair(clipboardEpoch, bindToken)) clipboardFields =
                    ",\"clipboardEpoch\":" + Quote(clipboardEpoch) + ",\"clipboardBindToken\":" + Quote(bindToken);
            }
            std::string audioFields;
            if (audioStarted && audioHooks.pair) {
                std::string audioEpoch, bindToken;
                if (audioHooks.pair(audioEpoch, bindToken)) audioFields =
                    ",\"audioPort\":" + std::to_string(audioHooks.port()) + ",\"audioEpoch\":" + Quote(audioEpoch) + ",\"audioBindToken\":" + Quote(bindToken);
            }
            lastRx = lastPing = Clock::now();
            return SendJson(socket, "{\"type\":\"pair_ok\",\"sessionToken\":" + Quote(session) + clipboardFields + audioFields + identityFields + "}", options.ioTimeoutMs, running);
        }
        if (session.empty() || !SecretEqual(Field(object, "sessionToken"), session)) {
            ReplyError(socket, "invalid_session"); return false;
        }
        if (type == "stop") { cleanStop = true; return false; }
        if (type == "paste_commit") {
            lastRx = Clock::now();
            uint64_t ttl = 0;
            const auto operation = Field(object, "operationId");
            auto ttlField = object.find("ttlMs");
            const bool valid = object.size() == 6 && clipboard_wire::Hex(operation, 32) &&
                clipboard_wire::Hex(Field(object, "clipboardEpoch"), 32) &&
                ttlField != object.end() && !ttlField->second.isString && clipboard_wire::Decimal(ttlField->second.text, ttl) && ttl >= 1 && ttl <= 1500 &&
                !Field(object, "eventId").empty() && Field(object, "eventId").size() <= 32;
            std::pair<std::string, std::string> result {"denied", "input_or_clipboard_disabled"};
            if (!valid) result = {"failed", "invalid_paste_commit"};
            else {
                // Bound history and one serialized paste operation; hooks never hold server state mutex.
                std::lock_guard<std::mutex> gate(inputMutex);
                bool allowed = false, duplicate = false; LanClipboardHooks clipboard; LanInputHooks input;
                { std::lock_guard<std::mutex> lock(mutex);
                  duplicate = std::find(pasteOperations.begin(), pasteOperations.end(), operation) != pasteOperations.end();
                  if (!duplicate) { pasteOperations.push_back(operation); if (pasteOperations.size() > 64) pasteOperations.pop_front(); }
                  allowed = inputEnabled && paired && streaming && hasAu && !cancelled.load() && !eosQueued && !disconnectRequested;
                  clipboard = clipboardHooks; input = inputHooks; }
                if (duplicate) result = {"denied", "duplicate_operation"};
                else if (allowed && input.enabled && input.enabled() && clipboard.paste) {
                    try { result = clipboard.paste(Field(object, "clipboardEpoch"), Field(object, "eventId"), lastRx + std::chrono::milliseconds(ttl)); }
                    catch (...) { result = {"failed", "paste_worker_failed"}; }
                }
            }
            return SendJson(socket, "{\"type\":\"paste_result\",\"sessionToken\":" + Quote(session) +
                ",\"operationId\":" + Quote(operation) + ",\"status\":" + Quote(result.first) + ",\"error\":" + Quote(result.second) + "}", options.ioTimeoutMs, running);
        }
        if (type == "input_enable" || type == "release_all_keys" || type == "mouse_move" || type == "mouse_button" || type == "scroll" || type == "key") {
            lastRx = Clock::now(); return HandleInput(socket, object, type);
        }
        if (type != "ping" && type != "pong") { ReplyError(socket, "unsupported_control_type"); return false; }
        lastRx = Clock::now();
        std::string probe;
        if (object.count("probeId")) {
            probe = Field(object, "probeId");
            if (!clipboard_wire::Hex(probe, 16)) { ReplyError(socket, "invalid_probe_id"); return false; }
        }
        if (object.size() != (probe.empty() ? 2 : 3)) { ReplyError(socket, "invalid_heartbeat"); return false; }
        if (type == "ping") return SendJson(socket, "{\"type\":\"pong\",\"sessionToken\":" + Quote(session) +
            (probe.empty() ? "" : ",\"probeId\":" + Quote(probe)) + "}", options.ioTimeoutMs, running);
        return true;
    }

    void ReplyError(const Sock& socket, const std::string& reason)
    {
        DisableInput();
        std::string session;
        { std::lock_guard<std::mutex> lock(mutex); if (paired) session = token; error = reason; }
        std::string reply = "{\"type\":\"error\",\"error\":" + Quote(reason);
        if (!session.empty()) reply += ",\"sessionToken\":" + Quote(session);
        SendJson(socket, reply + "}", options.ioTimeoutMs, running);
    }

    void NetworkLoop()
    {
        Sock socket, attaching;
        Reader controlReader, attachReader;
        bool hello = false;
        uint32_t controlPeer = 0, attachPeer = 0;
        Time acceptedAt {}, attachAt {}, lastRx {}, lastPing {};
        while (running.load()) {
            bool requested, retry;
            { std::lock_guard<std::mutex> lock(mutex); requested = disconnectRequested; retry = reconnectRequested;
              if (reconnecting && Clock::now() >= reconnectDeadline) { AbortLocked("reconnect_grace_expired"); requested = true; retry = false; } }
            if (requested) { Disconnect("", false, retry); socket.reset(); attaching.reset(); controlReader = Reader(); attachReader = Reader(); }
            if (!PollInput(socket)) { Disconnect("input_status_send_failed"); socket.reset(); attaching.reset(); }
            uint32_t peer = 0;
            Sock incoming = Accept(controlListener, peer, options);
            if (incoming && !socket && running.load()) {
                socket = incoming; controlPeer = peer; controlReader = Reader(); hello = false;
                acceptedAt = lastRx = lastPing = Clock::now();
                std::lock_guard<std::mutex> lock(mutex);
                control = socket; status = "AWAITING_HELLO";
            }
            incoming.reset();
            incoming = Accept(videoListener, peer, options);
            if (incoming) {
                bool available;
                { std::lock_guard<std::mutex> lock(mutex); available = paired && !video; }
                if (available && !attaching && peer == controlPeer) {
                    attaching = incoming; attachPeer = peer; attachAt = Clock::now(); attachReader = Reader();
                }
            }
            incoming.reset();

            if (socket) {
                std::string message;
                int read = controlReader.Read(socket, options.ioTimeoutMs, message);
                bool cleanStop = false;
                if (read < 0 || (read == 1 && !HandleControl(socket, message, hello, lastRx, lastPing, cleanStop))) {
                    Disconnect(read < 0 ? "control_closed_invalid_or_partial_timeout" : "", read == -2 || cleanStop, read < 0);
                    socket.reset(); attaching.reset();
                }
            }
            if (socket) {
                bool isPaired;
                std::string session;
                Time pairingDeadline;
                { std::lock_guard<std::mutex> lock(mutex); isPaired = paired; session = token;
                  pairingDeadline = identity.Available() ? acceptedAt + std::chrono::milliseconds(options.heartbeatTimeoutMs) : pinExpires; }
                Time now = Clock::now();
                // Modern clients enter PIN before connecting, and remembered
                // resume must remain available after PIN expiry. Legacy-only
                // servers retain their human PIN-entry deadline.
                if ((!isPaired && ((!hello && now - acceptedAt >= std::chrono::milliseconds(options.heartbeatTimeoutMs)) ||
                        (hello && now >= pairingDeadline))) ||
                    (isPaired && now - lastRx >= std::chrono::milliseconds(options.heartbeatTimeoutMs))) {
                    Disconnect(isPaired ? "heartbeat_timeout" : "handshake_timeout", false, isPaired); socket.reset(); attaching.reset();
                } else if (isPaired && now - lastPing >= std::chrono::milliseconds(options.heartbeatMs)) {
                    lastPing = now;
                    if (!SendJson(socket, "{\"type\":\"ping\",\"sessionToken\":" + Quote(session) + "}", options.ioTimeoutMs, running)) {
                        Disconnect("control_send_failed_or_timed_out", false, true); socket.reset(); attaching.reset();
                    }
                }
            }
            if (attaching) {
                std::string message;
                int read = attachReader.Read(attaching, options.ioTimeoutMs, message);
                if (read < 0 || Clock::now() - attachAt >= std::chrono::milliseconds(options.ioTimeoutMs)) attaching.reset();
                else if (read == 1) {
                    Object object;
                    bool valid = Parser(message).Parse(object) && Field(object, "type") == "video_attach";
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        valid = valid && paired && !video && attachPeer == controlPeer &&
                            !token.empty() && SecretEqual(Field(object, "sessionToken"), token);
                    }
                    if (valid && SendJson(attaching, "{\"type\":\"video_ready\"}", options.ioTimeoutMs, running)) {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (running.load() && paired && !video) {
                            video = attaching; status = "VIDEO_READY";
                            if (reconnecting && Clock::now() < reconnectDeadline && modernPeer) {
                                ClearQueueLocked(); needsConfig = latestConfig.empty();
                                if (!latestConfig.empty()) {
                                    Packet config; config.type = 1; config.pts = 0; config.flags = 0; config.bytes = latestConfig;
                                    queue.push_back(std::move(config)); queuedConfigs = 1;
                                }
                                needsIdr = true; hasAu = false; sequence = 0; lastPts = 0;
                                keyframeRequested = true; awaitingReconnectIdr = true; reconnecting = false; streaming = true; status = "STREAMING"; ++reconnectCount; ++streamId; cv.notify_one();
                            }
                        }
                    }
                    attaching.reset();
                }
            }
            Sock attached;
            { std::lock_guard<std::mutex> lock(mutex); attached = video; }
            if (attached) {
                uint8_t byte;
                ssize_t n = recv(attached->fd, &byte, 1, MSG_PEEK);
                if (n == 0 || n > 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    Disconnect(n > 0 ? "unexpected_video_channel_input" : "video_disconnected", n == 0, n <= 0);
                    socket.reset(); attaching.reset();
                }
            }
            // A bounded poll wakes early for listeners and the current control socket.
            pollfd waiters[3] {{controlListener->fd, POLLIN, 0}, {videoListener->fd, POLLIN, 0},
                {socket ? socket->fd : -1, POLLIN, 0}};
            poll(waiters, 3, 10);
        }
        if (attaching) attaching->Shutdown();
    }

    void WorkerFailed()
    {
        running.store(false);
        Sock a, b, c, d;
        {
            std::lock_guard<std::mutex> lock(mutex);
            AbortLocked("network_worker_exception");
            a = controlListener; b = videoListener; c = control; d = video;
        }
        DisableInput();
        for (const Sock& socket : {a, b, c, d}) if (socket) socket->Shutdown();
        cv.notify_all();
    }
};

LanServer::LanServer() : impl_(std::make_unique<Impl>()) {}
#ifdef HRD_LAN_TESTING
LanServer::LanServer(const LanServerTestOptions& options) : impl_(std::make_unique<Impl>())
{
    impl_->options.controlPort = options.controlPort; impl_->options.videoPort = options.videoPort;
    impl_->options.heartbeatMs = options.heartbeatMs; impl_->options.heartbeatTimeoutMs = options.heartbeatTimeoutMs;
    impl_->options.ioTimeoutMs = options.ioTimeoutMs; impl_->options.pinLifetimeMs = options.pinLifetimeMs;
    impl_->options.sendBufferBytes = options.sendBufferBytes; impl_->options.allowLoopback = options.allowLoopback;
    impl_->options.reconnectGraceMs = options.reconnectGraceMs;
}
void LanServer::InterruptVideoWriteForTest()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->video) shutdown(impl_->video->fd, SHUT_WR);
}
#endif
LanServer::~LanServer() { Stop(); }

int LanServer::Start(const std::string& bindAddress)
{
    auto& s = *impl_;
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (s.running.load()) return -2;
    s.StopInternal();
    std::string address = SelectAddress(bindAddress, s.options.allowLoopback);
    if (address.empty()) { std::lock_guard<std::mutex> lock(s.mutex); s.error = "local_rfc1918_address_required"; return -1; }
    std::string pin = NewPin();
    if (pin.empty()) { std::lock_guard<std::mutex> lock(s.mutex); s.error = "entropy_unavailable"; return -3; }
    uint16_t cp = s.options.controlPort, vp = s.options.videoPort;
    Sock controlListener = Listen(address, cp), videoListener = Listen(address, vp);
    if (!controlListener || !videoListener) { std::lock_guard<std::mutex> lock(s.mutex); s.error = "listen_failed"; return -4; }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.address = address; s.pin = pin; s.pinUsed = false; s.pinAttempts = 0;
        s.pinExpires = Clock::now() + std::chrono::milliseconds(s.options.pinLifetimeMs);
        s.controlPort = cp; s.videoPort = vp; s.controlListener = controlListener; s.videoListener = videoListener;
        s.error.clear(); s.status = "LISTENING"; s.running.store(true); s.cancelled.store(true);
        s.inputAccepted = s.inputRejected = s.inputReleases = 0; s.inputStatusPending = false;
        s.sentFrames = s.sentBytes = s.sentPackets = s.abortedStreams = 0; s.queueHighWater = 0;
        s.eosQueued = false; s.eosSent = false; s.encoderEndedSuccess = false; s.streamCompleted = false;
        s.reconnecting = false; s.reconnectRequested = false; s.keyframeRequested = false; s.awaitingReconnectIdr = false; s.modernPeer = false;
        s.challenge.clear(); s.currentPeerKey.clear(); s.capturePeerKey.clear(); s.latestConfig.clear(); s.reconnectCount = 0;
    }
    // Clipboard listener failure leaves the existing video/input service available.
    try { s.clipboardStarted = s.clipboardHooks.start && s.clipboardHooks.port && s.clipboardHooks.pair &&
        s.clipboardHooks.start(address); } catch (...) { s.clipboardStarted = false; }
    try { s.audioStarted = s.audioHooks.start && s.audioHooks.port && s.audioHooks.pair &&
        s.audioHooks.start(address); } catch (...) { s.audioStarted = false; }
    try {
        s.sender = std::thread([&s] { try { s.SenderLoop(); } catch (...) { s.WorkerFailed(); } });
        s.network = std::thread([&s] { try { s.NetworkLoop(); } catch (...) { s.WorkerFailed(); } });
    } catch (...) {
        s.StopInternal();
        std::lock_guard<std::mutex> lock(s.mutex); s.error = "thread_setup_failed"; return -5;
    }
    return 0;
}

void LanServer::Stop()
{
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
    impl_->StopInternal();
}

void LanServer::SetInputHooks(LanInputHooks hooks)
{
    impl_->DisableInput();
    std::lock_guard<std::mutex> gate(impl_->inputMutex);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->inputHooks = std::move(hooks);
}

void LanServer::SetClipboardHooks(LanClipboardHooks hooks)
{
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running.load()) impl_->clipboardHooks = std::move(hooks);
}

void LanServer::SetAudioHooks(LanAudioHooks hooks)
{
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running.load()) impl_->audioHooks = std::move(hooks);
}

bool LanServer::ConfigurePairingStorage(const std::string& directory)
{
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
    if (impl_->running.load()) return false;
    return impl_->identity.Configure(directory);
}

void LanServer::SetPairingAllowed(bool allowed)
{
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->pairingAllowed = allowed;
}

bool LanServer::RevokePairedDevices()
{
    // Stop the current session as well: a revoked device cannot retain an old
    // authenticated socket, pending input, clipboard bind, or reconnect grace.
    impl_->DisableInput();
    { std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->cancelled.store(true); impl_->reconnecting = false; impl_->challenge.clear();
      impl_->disconnectRequested = true; impl_->pairingAllowed = false; ++impl_->pairingGeneration; }
    return impl_->identity.RevokeAll();
}

bool LanServer::ConsumeKeyframeRequest()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    bool requested = impl_->keyframeRequested; impl_->keyframeRequested = false; return requested;
}

bool LanServer::CanResumeCapture()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running.load() && impl_->paired && impl_->modernPeer && impl_->streaming &&
        !impl_->cancelled.load() && !impl_->eosQueued && !impl_->disconnectRequested &&
        !impl_->capturePeerKey.empty() && impl_->currentPeerKey == impl_->capturePeerKey &&
        (!impl_->reconnecting || Clock::now() < impl_->reconnectDeadline);
}

bool LanServer::BeginStream()
{
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.running.load() || !s.paired || !s.video || s.streaming || s.disconnectRequested) return false;
    s.ClearQueueLocked(); s.needsConfig = true; s.needsIdr = true; s.hasAu = false;
    s.eosQueued = false; s.eosSent = false; s.ending = false; s.lastPts = 0; s.sequence = 0;
    s.encoderEndedSuccess = false; s.streamCompleted = false;
    s.eosSending = false;
    s.latestConfig.clear(); s.reconnecting = false; s.keyframeRequested = false; s.awaitingReconnectIdr = false;
    s.capturePeerKey = s.currentPeerKey;
    s.sentFrames = s.sentBytes = s.sentPackets = 0;
    s.streaming = true; s.cancelled.store(false); s.status = "STREAMING"; s.error.clear(); ++s.streamId;
    return true;
}

bool LanServer::Publish(const uint8_t* data, size_t size, uint64_t ptsUs, bool config, bool keyframe, bool eos)
{
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.running.load() || !s.streaming || s.cancelled.load() || s.ending || s.eosQueued) return false;
    if ((size && !data) || size > (config ? MAX_CONFIG : MAX_AU) || (!size && !eos) || (config && (eos || keyframe))) {
        s.AbortLocked("invalid_video_packet"); return false;
    }
    if (config) {
        try { s.latestConfig.assign(data, data + size); }
        catch (...) { s.AbortLocked("video_allocation_failed"); return false; }
    }
    if (s.reconnecting || s.reconnectRequested) {
        if (eos) { s.AbortLocked("capture_ended_during_reconnect"); return false; }
        return true; // The authorized capture advances; disconnected data is discarded.
    }
    if (s.awaitingReconnectIdr && !config && !keyframe && !eos) return true;
    if (config) {
        // Requesting a new IDR may also emit a fresh codec header. While the
        // resumed stream has no accepted AU, replace its unsent cached header
        // instead of overflowing the one-CONFIG queue.
        if (s.awaitingReconnectIdr && s.queuedConfigs) {
            for (auto& pending : s.queue) if (pending.type == 1) {
                try { pending.bytes = s.latestConfig; }
                catch (...) { s.AbortLocked("video_allocation_failed"); return false; }
                s.needsConfig = false; return true;
            }
        }
        if (s.queuedConfigs) { s.AbortLocked("config_queue_overflow"); return false; }
    } else {
        if (s.needsConfig || (s.needsIdr && (!keyframe || !size))) { s.AbortLocked("config_then_idr_required"); return false; }
        if (size && s.hasAu && ptsUs < s.lastPts) { s.AbortLocked("nonmonotonic_callback_pts"); return false; }
        if (s.queuedAus >= MAX_QUEUED_AUS) { s.AbortLocked("video_queue_overflow"); return false; }
    }
    Packet packet;
    try { if (size) packet.bytes.assign(data, data + size); }
    catch (...) { s.AbortLocked("video_allocation_failed"); return false; }
    packet.pts = ptsUs; packet.type = config ? 1 : 2;
    packet.flags = static_cast<uint16_t>((keyframe ? 1 : 0) | (eos ? 2 : 0));
    try { s.queue.push_back(std::move(packet)); }
    catch (...) { s.AbortLocked("video_allocation_failed"); return false; }
    if (config) { ++s.queuedConfigs; s.needsConfig = false; s.needsIdr = true; }
    else {
        ++s.queuedAus;
        if (size) { s.needsIdr = false; s.awaitingReconnectIdr = false; s.hasAu = true; s.lastPts = ptsUs; }
        if (eos) s.eosQueued = true;
    }
    s.queueHighWater = std::max(s.queueHighWater, s.queue.size());
    s.cv.notify_one();
    return true;
}

bool LanServer::IsStreamCancelled() { return impl_->cancelled.load(); }

void LanServer::EndStream(bool success)
{
    auto& s = *impl_;
    s.DisableInput();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cancelled.load() || (!s.streaming && !s.eosSent)) return;
    if (!success || !s.eosQueued) { s.AbortLocked(success ? "stream_ended_without_eos" : "encoder_stream_failed"); return; }
    s.encoderEndedSuccess = true;
    s.ending = true; s.status = "DRAINING";
    if (s.eosSent) {
        s.streaming = false; s.ending = false; s.streamCompleted = true;
        s.status = !s.running.load() ? "STOPPED" : (s.video ? "VIDEO_READY" : "LISTENING");
    }
}

std::string LanServer::SnapshotJson(bool includePin)
{
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    bool pinValid = s.running.load() && !s.pinUsed && !s.pin.empty() && Clock::now() < s.pinExpires;
    std::ostringstream out;
    out << std::boolalpha << "{\"running\":" << s.running.load() << ",\"status\":" << Quote(s.status)
        << ",\"bindAddress\":" << Quote(s.address) << ",\"controlPort\":" << s.controlPort << ",\"videoPort\":" << s.videoPort
        << ",\"paired\":" << s.paired << ",\"pinValid\":" << pinValid;
    if (includePin) out << ",\"pin\":" << Quote(pinValid ? s.pin : "");
    out << ",\"videoReady\":" << (s.paired && static_cast<bool>(s.video) && !s.disconnectRequested)
        << ",\"streaming\":" << s.streaming << ",\"cancelled\":" << s.cancelled.load()
        << ",\"sentFrames\":" << s.sentFrames << ",\"sentPackets\":" << s.sentPackets << ",\"sentBytes\":" << s.sentBytes
        << ",\"queuedAccessUnits\":" << s.queuedAus << ",\"queuedConfigs\":" << s.queuedConfigs
        << ",\"queueHighWaterPackets\":" << s.queueHighWater << ",\"abortedStreams\":" << s.abortedStreams
        << ",\"eosQueued\":" << s.eosQueued << ",\"eosSent\":" << s.eosSent
        << ",\"encoderEndedSuccess\":" << s.encoderEndedSuccess << ",\"streamCompleted\":" << s.streamCompleted
        << ",\"inputSupported\":" << s.InputSupportedLocked() << ",\"inputEnabled\":" << s.inputEnabled
        << ",\"inputAccepted\":" << s.inputAccepted << ",\"inputRejected\":" << s.inputRejected << ",\"inputReleases\":" << s.inputReleases
        << ",\"reconnecting\":" << s.reconnecting << ",\"reconnectCount\":" << s.reconnectCount
        << ",\"reconnectGraceMs\":" << (s.reconnecting ? std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(s.reconnectDeadline - Clock::now()).count()) : 0)
        << ",\"pairingAvailable\":" << s.identity.Available() << ",\"pairingAllowed\":" << s.pairingAllowed
        << ",\"trustedDeviceCount\":" << s.identity.TrustedCount() << ",\"pairingError\":" << Quote(s.identity.Error())
        << ",\"audioSupported\":" << s.audioStarted
        << ",\"error\":" << Quote(s.error) << "}";
    return out.str();
}

LanServer& GetLanServer() { static LanServer server; return server; }
