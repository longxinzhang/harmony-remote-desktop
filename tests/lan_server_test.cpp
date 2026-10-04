#include "lan_server.h"
#include <atomic>
#include <mutex>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed at line " + std::to_string(__LINE__) + ": " #condition); } while (false)

namespace {
const std::vector<uint8_t> CONFIG {0,0,0,1,0x67,0x64,0,0x1f,0,0,1,0x68,0xee,0x3c};
const std::vector<uint8_t> IDR {0,0,1,0x65,0x88,0x84};
const std::vector<uint8_t> PFRAME {0,0,1,0x41,0x9a,0x20};

struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&& other) noexcept { Close(); value = other.value; other.value = -1; return *this; }
    ~Fd() { Close(); }
    void Close() { if (value >= 0) { shutdown(value, SHUT_RDWR); close(value); value = -1; } }
};

std::string String(const std::string& json, const std::string& key)
{
    const std::string prefix = "\"" + key + "\":\"";
    size_t begin = json.find(prefix);
    CHECK(begin != std::string::npos);
    begin += prefix.size();
    size_t end = json.find('"', begin);
    CHECK(end != std::string::npos);
    return json.substr(begin, end - begin);
}

uint64_t Number(const std::string& json, const std::string& key)
{
    const std::string prefix = "\"" + key + "\":";
    size_t begin = json.find(prefix);
    CHECK(begin != std::string::npos);
    return std::stoull(json.substr(begin + prefix.size()));
}

bool True(const std::string& json, const std::string& key)
{
    return json.find("\"" + key + "\":true") != std::string::npos;
}

void Wait(const std::function<bool()>& condition, int timeoutMs = 1500)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        if (Clock::now() >= deadline) throw std::runtime_error("wait deadline exceeded");
        std::this_thread::sleep_for(2ms);
    }
}

Fd Connect(uint16_t port)
{
    Fd socket(::socket(AF_INET, SOCK_STREAM, 0)); CHECK(socket.value >= 0);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    CHECK(setsockopt(socket.value, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0);
#endif
    sockaddr_in address {}; address.sin_family = AF_INET; address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return socket;
}

void Send(const Fd& socket, const uint8_t* bytes, size_t count)
{
    size_t sent = 0;
    while (sent < count) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = send(socket.value, bytes + sent, count - sent, flags);
        CHECK(n > 0); sent += static_cast<size_t>(n);
    }
}

std::vector<uint8_t> Framed(const std::string& text)
{
    std::vector<uint8_t> out(4 + text.size());
    uint32_t length = htonl(static_cast<uint32_t>(text.size()));
    std::memcpy(out.data(), &length, 4); std::memcpy(out.data() + 4, text.data(), text.size());
    return out;
}

void Json(const Fd& socket, const std::string& text, bool fragment = false)
{
    auto bytes = Framed(text);
    if (!fragment) Send(socket, bytes.data(), bytes.size());
    else for (uint8_t byte : bytes) { Send(socket, &byte, 1); std::this_thread::sleep_for(1ms); }
}

std::vector<uint8_t> Read(const Fd& socket, size_t size, int timeoutMs = 1500)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<uint8_t> bytes(size);
    size_t done = 0;
    while (done < size) {
        CHECK(Clock::now() < deadline);
        pollfd p {socket.value, POLLIN, 0};
        int result = poll(&p, 1, 20); CHECK(result >= 0);
        if (!result) continue;
        ssize_t n = recv(socket.value, bytes.data() + done, size - done, 0);
        CHECK(n > 0); done += static_cast<size_t>(n);
    }
    return bytes;
}

uint32_t Read32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t Read64(const uint8_t* p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}
std::string ReadJson(const Fd& socket)
{
    auto header = Read(socket, 4);
    uint32_t length = Read32(header.data()); CHECK(length > 0 && length <= 4096);
    auto bytes = Read(socket, length);
    return std::string(bytes.begin(), bytes.end());
}

bool Closed(const Fd& socket, int timeoutMs = 1500)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        pollfd p {socket.value, POLLIN, 0};
        if (poll(&p, 1, 10) > 0) {
            uint8_t buffer[512];
            ssize_t n = recv(socket.value, buffer, sizeof(buffer), 0);
            if (n <= 0) return true;
        }
    }
    return false;
}

LanServerTestOptions Defaults()
{
    LanServerTestOptions o;
    o.heartbeatMs = 1000; o.heartbeatTimeoutMs = 3000;
    o.ioTimeoutMs = 250; o.pinLifetimeMs = 5000; o.sendBufferBytes = 4096;
    return o;
}

struct Fixture {
    explicit Fixture(const LanServerTestOptions& o = Defaults()) : server(o)
    {
        CHECK(server.Start("127.0.0.1") == 0);
        auto snapshot = server.SnapshotJson(true);
        cp = static_cast<uint16_t>(Number(snapshot, "controlPort"));
        vp = static_cast<uint16_t>(Number(snapshot, "videoPort"));
        pin = String(snapshot, "pin"); CHECK(pin.size() == 6);
    }
    LanServer server;
    uint16_t cp = 0, vp = 0;
    std::string pin;
};

void Hello(const Fd& control, bool fragment = false)
{
    Json(control, "{\"type\":\"hello\",\"protocol\":1,\"client\":\"macOS\"}", fragment);
    auto reply = ReadJson(control);
    CHECK(String(reply, "type") == "hello_ack");
    CHECK(String(reply, "timestampSource") == "encoder_callback_monotonic");
    CHECK(reply.find("\"nativePtsUnitVerified\":false") != std::string::npos);
}

struct Client {
    Fd control, video;
    std::string token;
};

Client Pair(Fixture& f, bool fragment = false)
{
    Client c;
    c.control = Connect(f.cp); Hello(c.control, fragment);
    Json(c.control, "{\"type\":\"pair\",\"pin\":\"" + f.pin + "\"}", fragment);
    auto reply = ReadJson(c.control); CHECK(String(reply, "type") == "pair_ok");
    c.token = String(reply, "sessionToken"); CHECK(c.token.size() == 64);
    for (char ch : c.token) CHECK((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    return c;
}

std::string Auth(const Client& c, const std::string& type)
{
    return "{\"type\":\"" + type + "\",\"sessionToken\":\"" + c.token + "\"}";
}

void Attach(Fixture& f, Client& c, bool fragment = false)
{
    c.video = Connect(f.vp); Json(c.video, Auth(c, "video_attach"), fragment);
    CHECK(ReadJson(c.video) == "{\"type\":\"video_ready\"}");
    Wait([&] { return True(f.server.SnapshotJson(), "videoReady"); });
}

struct VideoPacket { uint8_t type; uint16_t flags; uint32_t seq; uint64_t pts; std::vector<uint8_t> body; };
VideoPacket Video(const Fd& socket)
{
    auto h = Read(socket, 24);
    CHECK(std::memcmp(h.data(), "HRD1", 4) == 0 && h[4] == 1);
    uint32_t length = Read32(h.data() + 20); CHECK(length <= 8 * 1024 * 1024);
    return {h[5], static_cast<uint16_t>((h[6] << 8) | h[7]), Read32(h.data() + 8), Read64(h.data() + 12), Read(socket, length)};
}

void ConfigAndIdr(Fixture& f, Client& c)
{
    CHECK(f.server.BeginStream());
    CHECK(f.server.Publish(CONFIG.data(), CONFIG.size(), 0, true, false, false));
    auto config = Video(c.video); CHECK(config.type == 1 && config.seq == 0 && config.flags == 0 && config.body == CONFIG);
    CHECK(f.server.Publish(IDR.data(), IDR.size(), 0, false, true, false));
    auto idr = Video(c.video); CHECK(idr.type == 2 && idr.seq == 1 && idr.flags == 1 && idr.body == IDR);
}

struct InputMock {
    std::atomic<bool> allowed {true}, enabled {false}, reject {false};
    std::atomic<unsigned> disabled {0}, released {0};
    std::mutex mutex;
    std::vector<RemoteInputEvent> events;
    LanInputHooks Hooks() {
        return {[this](bool value) { enabled = value && allowed.load(); if (!value) ++disabled; return !value || enabled.load(); },
            [this] { return enabled.load() && allowed.load(); },
            [this](const RemoteInputEvent& event) {
                if (!enabled.load() || !allowed.load() || reject.load()) return false;
                std::lock_guard<std::mutex> lock(mutex); events.push_back(event); return true;
            }, [this] { ++released; }};
    }
    size_t Count() { std::lock_guard<std::mutex> lock(mutex); return events.size(); }
};
std::string Input(const Client& c, const std::string& type, const std::string& fields = "")
{
    auto message = Auth(c, type); message.pop_back(); return message + fields + "}";
}
void Enable(Client& c, bool enabled = true)
{
    Json(c.control, Input(c, "input_enable", std::string(",\"enabled\":") + (enabled ? "true" : "false")));
    auto status = ReadJson(c.control);
    CHECK(String(status, "type") == "input_status" && True(status, "enabled") == enabled);
    CHECK(String(status, "sessionToken") == c.token);
}
int ServeInputFixture(bool overflow)
{
    InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks());
    std::cout << "{\"controlPort\":" << f.cp << ",\"videoPort\":" << f.vp << ",\"pin\":\"" << f.pin
        << "\",\"expectedH264Hex\":\"000000016764001f00000168ee3c000001658884\"}" << std::endl;
    Wait([&] { return True(f.server.SnapshotJson(), "videoReady"); }, 15000);
    CHECK(f.server.BeginStream());
    CHECK(f.server.Publish(CONFIG.data(), CONFIG.size(), 0, true, false, false));
    CHECK(f.server.Publish(IDR.data(), IDR.size(), 0, false, true, false));
    if (overflow) {
        Wait([&] { return f.server.IsStreamCancelled(); }, 5000);
        CHECK(!input.enabled.load()); CHECK(input.Count() == 0);
    } else {
        Wait([&] { return input.Count() == 6 && input.released.load() >= 1; }, 5000);
        {
            std::lock_guard<std::mutex> lock(input.mutex);
            CHECK(input.events[0].kind == RemoteInputEvent::Kind::Move && input.events[0].x == 0.75);
            CHECK(input.events[1].kind == RemoteInputEvent::Kind::Button && input.events[1].down);
            CHECK(input.events[2].kind == RemoteInputEvent::Kind::Button && !input.events[2].down);
            CHECK(input.events[3].kind == RemoteInputEvent::Kind::Key && input.events[3].down);
            CHECK(input.events[4].kind == RemoteInputEvent::Kind::Key && !input.events[4].down);
            CHECK(input.events[5].kind == RemoteInputEvent::Kind::Scroll);
        }
        input.allowed = false;
        Wait([&] { return !True(f.server.SnapshotJson(), "inputEnabled"); });
        CHECK(f.server.Publish(nullptr, 0, 0, false, false, true));
        Wait([&] { return True(f.server.SnapshotJson(), "eosSent"); });
        Wait([&] { return !True(f.server.SnapshotJson(), "paired"); });
        f.server.EndStream(true); CHECK(True(f.server.SnapshotJson(), "streamCompleted"));
    }
    f.server.Stop(); CHECK(!input.enabled.load()); return 0;
}

int ServeFixture()
{
    auto options = Defaults(); options.heartbeatMs = 2000; options.heartbeatTimeoutMs = 6000;
    options.ioTimeoutMs = 2000; options.pinLifetimeMs = 300000;
    Fixture f(options);
    // Test-only random credentials go to a parent process pipe, never application logs.
    std::cout << "{\"controlPort\":" << f.cp << ",\"videoPort\":" << f.vp << ",\"pin\":\"" << f.pin
        << "\",\"expectedH264Hex\":\"000000016764001f00000168ee3c000001658884000001419a20\"}" << std::endl;
    Wait([&] { return True(f.server.SnapshotJson(), "videoReady"); }, 15000);
    CHECK(f.server.BeginStream());
    CHECK(f.server.Publish(CONFIG.data(), CONFIG.size(), 0, true, false, false));
    CHECK(f.server.Publish(IDR.data(), IDR.size(), 0, false, true, false));
    CHECK(f.server.Publish(PFRAME.data(), PFRAME.size(), 33333, false, false, false));
    // Bound the fixture's queue independently of scheduling before publishing EOS.
    Wait([&] { return Number(f.server.SnapshotJson(), "sentFrames") == 2; });
    CHECK(f.server.Publish(nullptr, 0, 33333, false, false, true));
    Wait([&] { return True(f.server.SnapshotJson(), "eosSent"); });
    // Deliberately let the real receiver stop first, exercising the finalize race.
    Wait([&] { return !True(f.server.SnapshotJson(), "paired"); }, 5000);
    CHECK(!f.server.IsStreamCancelled());
    CHECK(!True(f.server.SnapshotJson(), "streamCompleted"));
    f.server.EndStream(true);
    CHECK(True(f.server.SnapshotJson(), "streamCompleted"));
    CHECK(String(f.server.SnapshotJson(), "error").empty());
    f.server.Stop();
    CHECK(Number(f.server.SnapshotJson(), "sentFrames") == 2);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--serve-fixture") return ServeFixture();
        if (argc == 2 && std::string(argv[1]) == "--serve-input-fixture") return ServeInputFixture(false);
        if (argc == 2 && std::string(argv[1]) == "--serve-input-overflow-fixture") return ServeInputFixture(true);
        unsigned int count = 0;
        auto test = [&](const char* name, const std::function<void()>& body) { body(); ++count; std::cout << "PASS " << name << '\n'; };

        test("clipboard optional handshake and paste auth gating bounded operation dedup", [] {
            InputMock input; Fixture f; f.server.Stop(); f.server.SetInputHooks(input.Hooks());
            int commits = 0, disconnects = 0; LanClipboardHooks hooks;
            hooks.start = [](const std::string&) { return true; }; hooks.stop = [] {}; hooks.port = [] { return uint16_t(39873); };
            hooks.pair = [](std::string& epoch, std::string& token) { epoch = std::string(32, 'a'); token = std::string(64, 'b'); return true; };
            hooks.disconnect = [&] { ++disconnects; };
            hooks.paste = [&](const std::string& epoch, const std::string& event, auto deadline) {
                CHECK(epoch == std::string(32, 'a') && event == "mac-1"); CHECK(deadline > std::chrono::steady_clock::now());
                ++commits; return std::make_pair(std::string("committed"), std::string()); };
            f.server.SetClipboardHooks(std::move(hooks)); CHECK(f.server.Start("127.0.0.1") == 0);
            const auto snapshot = f.server.SnapshotJson(true); f.cp = uint16_t(Number(snapshot, "controlPort")); f.vp = uint16_t(Number(snapshot, "videoPort")); f.pin = String(snapshot, "pin");
            auto control = Connect(f.cp); Json(control, "{\"type\":\"hello\",\"protocol\":1}"); auto hello = ReadJson(control);
            CHECK(True(hello, "clipboardSupported") && Number(hello, "clipboardPort") == 39873);
            Json(control, "{\"type\":\"pair\",\"pin\":\"" + f.pin + "\"}"); auto pair = ReadJson(control);
            CHECK(String(pair, "clipboardEpoch") == std::string(32, 'a') && String(pair, "clipboardBindToken") == std::string(64, 'b'));
            Client c; c.control = std::move(control); c.token = String(pair, "sessionToken");
            auto paste = [&](char operation, int ttl = 1000) { return Input(c, "paste_commit", ",\"clipboardEpoch\":\"" + std::string(32, 'a') +
                "\",\"eventId\":\"mac-1\",\"operationId\":\"" + std::string(32, operation) + "\",\"ttlMs\":" + std::to_string(ttl)); };
            Json(c.control, paste('1')); CHECK(String(ReadJson(c.control), "status") == "denied"); CHECK(commits == 0);
            Attach(f, c); ConfigAndIdr(f, c); Enable(c);
            Json(c.control, paste('2')); CHECK(String(ReadJson(c.control), "status") == "committed"); CHECK(commits == 1);
            Json(c.control, paste('2')); auto duplicate = ReadJson(c.control); CHECK(String(duplicate, "error") == "duplicate_operation" && commits == 1);
            Json(c.control, paste('3', 1501)); CHECK(String(ReadJson(c.control), "status") == "failed" && commits == 1);
            Json(c.control, Input(c, "input_enable", ",\"enabled\":false")); CHECK(!True(ReadJson(c.control), "enabled"));
            Json(c.control, paste('4')); CHECK(String(ReadJson(c.control), "status") == "denied" && commits == 1);
            Json(c.control, Auth(c, "stop")); CHECK(Closed(c.control)); Wait([&] { return disconnects > 0; });
        });
        test("input enable requires ready stream and first accepted video AU", [] {
            InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks());
            auto socket = Connect(f.cp); Json(socket, "{\"type\":\"hello\",\"protocol\":1}");
            CHECK(True(ReadJson(socket), "inputSupported")); socket.Close();
            Wait([&] { return String(f.server.SnapshotJson(), "status") == "LISTENING"; });
            auto c = Pair(f);
            Json(c.control, Input(c, "input_enable", ",\"enabled\":true")); CHECK(!True(ReadJson(c.control), "enabled"));
            Attach(f, c);
            Json(c.control, Input(c, "input_enable", ",\"enabled\":true")); CHECK(!True(ReadJson(c.control), "enabled"));
            CHECK(f.server.BeginStream());
            Json(c.control, Input(c, "input_enable", ",\"enabled\":true")); CHECK(!True(ReadJson(c.control), "enabled"));
            Json(c.control, Input(c, "mouse_move", ",\"x\":0.5,\"y\":0.5")); CHECK(!True(ReadJson(c.control), "enabled"));
            CHECK(!input.enabled.load() && input.Count() == 0);
            CHECK(f.server.Publish(CONFIG.data(), CONFIG.size(), 0, true, false, false)); CHECK(Video(c.video).type == 1);
            Json(c.control, Input(c, "input_enable", ",\"enabled\":true")); CHECK(!True(ReadJson(c.control), "enabled"));
            CHECK(f.server.Publish(IDR.data(), IDR.size(), 0, false, true, false)); CHECK(Video(c.video).type == 2);
            Enable(c); CHECK(input.enabled.load()); Enable(c, false); CHECK(!input.enabled.load());
        });
        test("ordered input and release retains enabled session", [] {
            InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks()); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); Enable(c);
            Json(c.control, Input(c, "mouse_move", ",\"x\":0.75,\"y\":0.25"));
            Json(c.control, Input(c, "mouse_button", ",\"button\":\"left\",\"action\":\"down\""));
            Json(c.control, Input(c, "key", ",\"code\":\"KEY_CTRL_LEFT\",\"action\":\"down\""));
            Json(c.control, Input(c, "key", ",\"code\":\"KEY_CTRL_LEFT\",\"action\":\"up\""));
            Json(c.control, Auth(c, "release_all_keys"));
            Wait([&] { return input.Count() == 4 && input.released.load() == 1; });
            CHECK(input.enabled.load()); CHECK(Number(f.server.SnapshotJson(), "inputAccepted") == 4);
            { std::lock_guard<std::mutex> lock(input.mutex); CHECK(input.events[0].x == 0.75 && input.events[1].down && !input.events[3].down); }
            Json(c.control, Auth(c, "stop")); CHECK(Closed(c.control)); CHECK(!input.enabled.load());
            CHECK(f.server.SnapshotJson().find("KEY_CTRL") == std::string::npos);
        });
        test("disabled and revoked input is never injected", [] {
            InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks()); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
            Json(c.control, Input(c, "mouse_move", ",\"x\":0,\"y\":1")); CHECK(!True(ReadJson(c.control), "enabled")); CHECK(input.Count() == 0);
            Enable(c); input.allowed = false;
            auto status = ReadJson(c.control); CHECK(String(status, "type") == "input_status" && !True(status, "enabled"));
            CHECK(!input.enabled.load());
            Json(c.control, Input(c, "key", ",\"code\":\"KEY_A\",\"action\":\"down\"")); CHECK(!True(ReadJson(c.control), "enabled")); CHECK(input.Count() == 0);
        });
        test("input validates token type range and whitelist before submission", [] {
            for (const std::string& body : {",\"x\":-0.1,\"y\":0", ",\"x\":1.1,\"y\":0", ",\"x\":1e999,\"y\":0", ",\"x\":true,\"y\":0", ",\"x\":\"0\",\"y\":0", ",\"x\":0,\"y\":0,\"extra\":1"}) {
                InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks()); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); Enable(c);
                Json(c.control, Input(c, "mouse_move", body)); CHECK(String(ReadJson(c.control), "type") == "error"); CHECK(Closed(c.control)); CHECK(!input.enabled.load() && input.Count() == 0);
            }
            for (const std::string& message : {"{\"type\":\"key\",\"code\":\"KEY_A\",\"action\":\"down\"}", "{\"type\":\"input_enable\",\"sessionToken\":\"wrong\",\"enabled\":true}"}) {
                InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks()); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); Enable(c);
                Json(c.control, message); CHECK(String(ReadJson(c.control), "error") == "invalid_session"); CHECK(Closed(c.control)); CHECK(!input.enabled.load());
            }
            for (const auto& pair : {std::make_pair("key", ",\"code\":\"KEY_CTRL\",\"action\":\"down\""), {"mouse_button", ",\"button\":\"fourth\",\"action\":\"up\""}, {"scroll", ",\"dx\":121,\"dy\":0"}, {"input_enable", ",\"enabled\":1"}}) {
                InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks()); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); Enable(c);
                Json(c.control, Input(c, pair.first, pair.second)); CHECK(String(ReadJson(c.control), "error") == "invalid_input_message"); CHECK(Closed(c.control)); CHECK(!input.enabled.load());
            }
        });
        test("submission failure EOF end and stop release input", [] {
            for (int mode = 0; mode < 4; ++mode) {
                InputMock input; Fixture f; f.server.SetInputHooks(input.Hooks()); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); Enable(c);
                if (mode == 0) { input.reject = true; Json(c.control, Input(c, "scroll", ",\"dx\":0,\"dy\":120")); CHECK(String(ReadJson(c.control), "error") == "input_submission_failed"); }
                else if (mode == 1) c.control.Close();
                else if (mode == 2) f.server.EndStream(false);
                else f.server.Stop();
                Wait([&] { return !input.enabled.load(); }); CHECK(input.Count() == 0);
            }
        });
        test("hooks may inspect snapshot without state mutex deadlock", [] {
            InputMock input; Fixture f; auto hooks = input.Hooks();
            auto enable = hooks.enable; hooks.enable = [&, enable](bool value) { (void)f.server.SnapshotJson(); return enable(value); };
            f.server.SetInputHooks(hooks); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); Enable(c); f.server.Stop();
        });
        test("production rejects nonlocal/public/wildcard/loopback bind", [] {
            LanServer server;
            for (const std::string& ip : {"127.0.0.1", "0.0.0.0", "8.8.8.8", "169.254.1.1", "192.168.255.254", "bad"}) CHECK(server.Start(ip) == -1);
            CHECK(!True(server.SnapshotJson(), "running"));
        });
        test("explicit start, pin redaction, gates, stop", [] {
            Fixture f; CHECK(f.server.Start("127.0.0.1") == -2);
            auto diagnostic = f.server.SnapshotJson();
            CHECK(diagnostic.find("\"pin\"") == std::string::npos && diagnostic.find("sessionToken") == std::string::npos);
            CHECK(diagnostic.find(f.pin) == std::string::npos);
            CHECK(!f.server.BeginStream());
            CHECK(!f.server.Publish(IDR.data(), IDR.size(), 0, false, true, false));
            f.server.Stop(); CHECK(!True(f.server.SnapshotJson(), "running")); CHECK(String(f.server.SnapshotJson(true), "pin").empty());
        });
        test("fragmented hello/pair/video_attach; authenticated ping/pong", [] {
            Fixture f; auto c = Pair(f, true); CHECK(!f.server.BeginStream()); Attach(f, c, true);
            CHECK(String(f.server.SnapshotJson(true), "pin").empty());
            CHECK(f.server.SnapshotJson(true).find(c.token) == std::string::npos);
            Json(c.control, Auth(c, "ping")); auto pong = ReadJson(c.control);
            CHECK(String(pong, "type") == "pong" && String(pong, "sessionToken") == c.token);
        });
        test("single control client", [] {
            Fixture f; auto c = Pair(f); auto other = Connect(f.cp); CHECK(Closed(other));
            Json(c.control, Auth(c, "ping")); CHECK(String(ReadJson(c.control), "type") == "pong");
        });
        test("hello then human PIN wait uses TTL, not heartbeat timeout", [] {
            auto o = Defaults(); o.heartbeatMs = 40; o.heartbeatTimeoutMs = 100; o.pinLifetimeMs = 800;
            Fixture f(o); auto c = Connect(f.cp); Hello(c); std::this_thread::sleep_for(180ms);
            Json(c, "{\"type\":\"pair\",\"pin\":\"" + f.pin + "\"}"); CHECK(String(ReadJson(c), "type") == "pair_ok");
        });
        test("wrong protocol and no hello denied", [] {
            for (const auto& message : {"{\"type\":\"hello\",\"protocol\":2}", "{\"type\":\"pair\",\"pin\":\"123456\"}"}) {
                Fixture f; auto c = Connect(f.cp); Json(c, message); CHECK(String(ReadJson(c), "type") == "error"); CHECK(Closed(c));
            }
        });
        test("flat JSON rejects duplicate, nested, trailing and malformed input", [] {
            for (const auto& message : {"{\"type\":\"hello\",\"type\":\"hello\",\"protocol\":1}", "{\"type\":\"hello\",\"protocol\":1,\"x\":{}}", "{\"type\":\"hello\",\"protocol\":1}x", "{\"type\":\"hello\",\"protocol\":01}"}) {
                Fixture f; auto c = Connect(f.cp); Json(c, message); CHECK(String(ReadJson(c), "type") == "error"); CHECK(Closed(c));
            }
        });
        test("zero/oversized JSON length rejected before allocation", [] {
            for (uint32_t length : {0U, 4097U, 0xffffffffU}) {
                Fixture f; auto c = Connect(f.cp); uint32_t net = htonl(length);
                Send(c, reinterpret_cast<uint8_t*>(&net), 4); CHECK(Closed(c));
            }
        });
        test("partial header and partial body have total deadline", [] {
            for (bool body : {false, true}) {
                Fixture f; auto c = Connect(f.cp); auto frame = Framed("{\"type\":\"hello\",\"protocol\":1}");
                Send(c, frame.data(), body ? 7 : 2); CHECK(Closed(c, 900));
                CHECK(!True(f.server.SnapshotJson(), "paired"));
            }
        });
        test("five wrong PIN attempts invalidate across reconnects", [] {
            Fixture f; const std::string wrong = f.pin == "000000" ? "000001" : "000000";
            for (int i = 0; i < 5; ++i) {
                auto c = Connect(f.cp); Hello(c); Json(c, "{\"type\":\"pair\",\"pin\":\"" + wrong + "\"}");
                CHECK(String(ReadJson(c), "error") == "invalid_pin"); CHECK(Closed(c));
                Wait([&] { return String(f.server.SnapshotJson(), "status") == "LISTENING"; });
            }
            CHECK(!True(f.server.SnapshotJson(true), "pinValid")); CHECK(String(f.server.SnapshotJson(true), "pin").empty());
            auto c = Connect(f.cp); Hello(c); Json(c, "{\"type\":\"pair\",\"pin\":\"" + f.pin + "\"}");
            CHECK(String(ReadJson(c), "error") == "pin_unavailable_restart_server");
        });
        test("PIN expiration and consumed PIN never reused", [] {
            auto o = Defaults(); o.pinLifetimeMs = 60; Fixture expired(o);
            std::this_thread::sleep_for(80ms); CHECK(!True(expired.server.SnapshotJson(true), "pinValid"));
            Fixture f; auto first = Pair(f); Json(first.control, Auth(first, "stop")); CHECK(Closed(first.control));
            Wait([&] { return !True(f.server.SnapshotJson(), "paired"); });
            auto next = Connect(f.cp); Hello(next); Json(next, "{\"type\":\"pair\",\"pin\":\"" + f.pin + "\"}");
            CHECK(String(ReadJson(next), "error") == "pin_unavailable_restart_server");
        });
        test("missing/wrong token and all input controls denied", [] {
            for (const std::string& type : {"missing", "wrong", "input", "mouse_move", "key_event"}) {
                Fixture f; auto c = Pair(f);
                std::string message = type == "missing" ? "{\"type\":\"ping\"}" : type == "wrong" ? "{\"type\":\"ping\",\"sessionToken\":\"wrong\"}" : Auth(c, type);
                Json(c.control, message); CHECK(String(ReadJson(c.control), "type") == "error"); CHECK(Closed(c.control));
                CHECK(!True(f.server.SnapshotJson(), "paired"));
            }
        });
        test("video requires paired token and only one attachment", [] {
            Fixture f; auto unpaired = Connect(f.vp); CHECK(Closed(unpaired));
            auto c = Pair(f); auto bad = Connect(f.vp); Json(bad, "{\"type\":\"video_attach\",\"sessionToken\":\"wrong\"}"); CHECK(Closed(bad));
            Attach(f, c); auto duplicate = Connect(f.vp); CHECK(Closed(duplicate));
        });
        test("server heartbeats authenticate and client pong extends session", [] {
            auto o = Defaults(); o.heartbeatMs = 40; o.heartbeatTimeoutMs = 140;
            Fixture f(o); auto c = Pair(f); Attach(f, c);
            for (int i = 0; i < 6; ++i) {
                auto ping = ReadJson(c.control); CHECK(String(ping, "type") == "ping" && String(ping, "sessionToken") == c.token);
                Json(c.control, Auth(c, "pong"));
            }
            CHECK(True(f.server.SnapshotJson(), "paired")); CHECK(True(f.server.SnapshotJson(), "videoReady"));
        });
        test("heartbeat expiration closes video and cancels active stream", [] {
            auto o = Defaults(); o.heartbeatMs = 30; o.heartbeatTimeoutMs = 120;
            Fixture f(o); auto c = Pair(f); Attach(f, c); CHECK(f.server.BeginStream());
            Wait([&] { return f.server.IsStreamCancelled(); }); CHECK(Closed(c.video));
            CHECK(String(f.server.SnapshotJson(), "error") == "heartbeat_timeout");
        });
        test("wire header endian/sequence/PTS, drain EOS, stats survive Stop", [] {
            Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
            const uint64_t pts = 0x0102030405060708ULL;
            CHECK(f.server.Publish(PFRAME.data(), PFRAME.size(), pts, false, false, false));
            auto p = Video(c.video); CHECK(p.seq == 2 && p.pts == pts && p.body == PFRAME && p.flags == 0);
            CHECK(f.server.Publish(nullptr, 0, pts, false, false, true)); f.server.EndStream(true);
            p = Video(c.video); CHECK(p.type == 2 && p.seq == 3 && p.flags == 2 && p.body.empty());
            Wait([&] { return True(f.server.SnapshotJson(), "streamCompleted"); });
            Json(c.control, Auth(c, "stop")); CHECK(Closed(c.control));
            CHECK(!f.server.IsStreamCancelled()); CHECK(String(f.server.SnapshotJson(), "error").empty());
            f.server.Stop(); CHECK(Number(f.server.SnapshotJson(), "sentFrames") == 2); CHECK(True(f.server.SnapshotJson(), "streamCompleted"));
        });
        test("EOS then immediate stop/EOF before EndStream is not cancellation", [] {
            for (bool stop : {true, false}) {
                Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
                CHECK(f.server.Publish(nullptr, 0, 0, false, false, true)); CHECK(Video(c.video).flags == 2);
                if (stop) { Json(c.control, Auth(c, "stop")); CHECK(Closed(c.control)); }
                else { c.control.Close(); c.video.Close(); }
                Wait([&] { return !True(f.server.SnapshotJson(), "paired"); });
                CHECK(!f.server.IsStreamCancelled()); CHECK(!True(f.server.SnapshotJson(), "streamCompleted"));
                f.server.EndStream(true); CHECK(True(f.server.SnapshotJson(), "streamCompleted")); CHECK(String(f.server.SnapshotJson(), "error").empty());
            }
        });
        test("EOS with final data and encoder failure remains failure", [] {
            Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
            CHECK(f.server.Publish(PFRAME.data(), PFRAME.size(), 33333, false, false, true));
            auto p = Video(c.video); CHECK(p.flags == 2 && p.body == PFRAME);
            Wait([&] { return True(f.server.SnapshotJson(), "eosSent"); });
            f.server.EndStream(false); CHECK(f.server.IsStreamCancelled()); CHECK(!True(f.server.SnapshotJson(), "streamCompleted"));
        });
        test("config and first IDR required; invalid bounds cancel", [] {
            for (int scenario = 0; scenario < 5; ++scenario) {
                Fixture f; auto c = Pair(f); Attach(f, c); CHECK(f.server.BeginStream());
                if (scenario == 1) { CHECK(f.server.Publish(CONFIG.data(), CONFIG.size(), 0, true, false, false)); Video(c.video); }
                bool ok;
                if (scenario == 0) ok = f.server.Publish(IDR.data(), IDR.size(), 0, false, true, false);
                else if (scenario == 1) ok = f.server.Publish(PFRAME.data(), PFRAME.size(), 0, false, false, false);
                else if (scenario == 2) ok = f.server.Publish(CONFIG.data(), 256 * 1024 + 1, 0, true, false, false);
                else if (scenario == 3) ok = f.server.Publish(nullptr, 1, 0, false, true, false);
                else ok = f.server.Publish(IDR.data(), 8 * 1024 * 1024 + 1, 0, false, true, false);
                CHECK(!ok && f.server.IsStreamCancelled()); CHECK(Number(f.server.SnapshotJson(), "queuedAccessUnits") == 0);
            }
        });
        test("reconfiguration requires new IDR and AU PTS cannot regress", [] {
            for (bool regression : {false, true}) {
                Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
                if (regression) {
                    CHECK(f.server.Publish(PFRAME.data(), PFRAME.size(), 100, false, false, false)); Video(c.video);
                    CHECK(!f.server.Publish(PFRAME.data(), PFRAME.size(), 99, false, false, false));
                } else {
                    CHECK(f.server.Publish(CONFIG.data(), CONFIG.size(), 0, true, false, false)); Video(c.video);
                    CHECK(!f.server.Publish(PFRAME.data(), PFRAME.size(), 100, false, false, false));
                }
                CHECK(f.server.IsStreamCancelled());
            }
        });
        test("EndStream success without EOS is rejected", [] {
            Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c); f.server.EndStream(true);
            CHECK(f.server.IsStreamCancelled()); CHECK(String(f.server.SnapshotJson(), "error") == "stream_ended_without_eos");
        });
        test("control/video disconnect before EOS aborts and clears queue", [] {
            for (bool video : {false, true}) {
                Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
                if (video) c.video.Close(); else c.control.Close();
                Wait([&] { return f.server.IsStreamCancelled(); });
                CHECK(!True(f.server.SnapshotJson(), "paired")); CHECK(Number(f.server.SnapshotJson(), "queuedAccessUnits") == 0);
            }
        });
        test("bounded queue overflow aborts instead of dropping reference frames", [] {
            auto o = Defaults(); o.ioTimeoutMs = 800;
            Fixture f(o); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
            std::vector<uint8_t> large(8 * 1024 * 1024, 0x55);
            auto begin = Clock::now(); bool rejected = false;
            for (int i = 0; i < 8; ++i) if (!f.server.Publish(large.data(), large.size(), static_cast<uint64_t>(i + 1), false, false, false)) { rejected = true; break; }
            CHECK(rejected && f.server.IsStreamCancelled()); CHECK(Clock::now() - begin < 500ms);
            CHECK(Number(f.server.SnapshotJson(), "queuedAccessUnits") == 0);
            CHECK(String(f.server.SnapshotJson(), "error") == "video_queue_overflow");
        });
        test("slow video receiver reaches bounded send deadline", [] {
            auto o = Defaults(); o.ioTimeoutMs = 150;
            Fixture f(o); auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
            std::vector<uint8_t> large(8 * 1024 * 1024, 0x55);
            CHECK(f.server.Publish(large.data(), large.size(), 1, false, false, false));
            Wait([&] { return f.server.IsStreamCancelled(); });
            CHECK(String(f.server.SnapshotJson(), "error") == "video_send_failed_or_timed_out");
        });
        test("Stop interrupts active sender promptly; repeated start/stop", [] {
            Fixture f; auto c = Pair(f); Attach(f, c); ConfigAndIdr(f, c);
            std::vector<uint8_t> large(8 * 1024 * 1024, 0x55);
            CHECK(f.server.Publish(large.data(), large.size(), 1, false, false, false));
            auto begin = Clock::now(); f.server.Stop(); CHECK(Clock::now() - begin < 500ms);
            CHECK(!True(f.server.SnapshotJson(), "running"));
            for (int i = 0; i < 4; ++i) { CHECK(f.server.Start("127.0.0.1") == 0); f.server.Stop(); }
        });
        std::cout << "LAN server tests: " << count << "/" << count << " passed (host loopback, no device)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
