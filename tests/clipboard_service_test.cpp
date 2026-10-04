#include "clipboard_service.h"
#include "clipboard_wire.h"
#include "lan_server.h"
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
using namespace clipboard_wire;
namespace {
#define CHECK(v) do { if (!(v)) throw std::runtime_error(std::string("assertion at line ") + std::to_string(__LINE__) + ": " #v); } while (false)
struct Fake final : ClipboardPlatform {
    std::mutex mutex; std::string text = "initial must not sync", tag; uint32_t revision = 1;
    int readCode = 0, writeCode = 0, reads = 0, writes = 0; bool supported = true, deferRevision = false;
    uint32_t Revision() override { std::lock_guard<std::mutex> l(mutex); return revision; }
    clipboard_text_policy::Shape shape;
    ClipboardText Read() override { std::lock_guard<std::mutex> l(mutex); ++reads; return {text, tag, revision, readCode, supported, shape}; }
    int Write(const std::string& value, const std::string& owner) override {
        std::lock_guard<std::mutex> l(mutex); ++writes; if (writeCode) return writeCode;
        text = value; tag = owner; if (!deferRevision) ++revision; return 0;
    }
    bool Digest(const std::string& value, std::string& hex) override {
        unsigned char bytes[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(value.data(), CC_LONG(value.size()), bytes);
        constexpr char HEX[] = "0123456789abcdef"; hex.clear(); for (auto byte : bytes) { hex += HEX[byte >> 4]; hex += HEX[byte & 15]; } return true;
    }
    void Copy(std::string value, bool keepOwner = false) { std::lock_guard<std::mutex> l(mutex); text = std::move(value); if (!keepOwner) tag.clear(); ++revision; }
    int Writes() { std::lock_guard<std::mutex> l(mutex); return writes; }
    std::string Text() { std::lock_guard<std::mutex> l(mutex); return text; }
};
struct Socket { int fd = -1; ~Socket() { if (fd >= 0) close(fd); } };
std::string Id(unsigned n = 1) { std::string s(32, '0'); constexpr char HEX[] = "0123456789abcdef"; for (int i = 31; n; --i) { s[size_t(i)] = HEX[n & 15]; n >>= 4; } return s; }
void Write(int fd, const void* data, size_t size) { const char* p = static_cast<const char*>(data); while (size) { ssize_t n = send(fd, p, size, 0); CHECK(n > 0); p += n; size -= size_t(n); } }
void Read(int fd, void* data, size_t size) { char* p = static_cast<char*>(data); while (size) { ssize_t n = recv(fd, p, size, 0); CHECK(n > 0); p += n; size -= size_t(n); } }
bool Available(int fd, int timeout = 100) { pollfd p {fd, POLLIN, 0}; return poll(&p, 1, timeout) > 0; }
std::string Base(const std::string& type, const std::string& epoch, unsigned id, size_t bytes = 0)
{ return "{\"version\":1,\"type\":" + Quote(type) + ",\"sessionEpoch\":" + Quote(epoch) + ",\"messageId\":" + Quote(Id(id)) + ",\"payloadLength\":" + std::to_string(bytes); }
void Send(int fd, const std::string& header, const std::string& body = {}, bool fragmented = false) {
    uint32_t length = htonl(uint32_t(header.size()));
    if (fragmented) { for (int i = 0; i < 4; ++i) Write(fd, reinterpret_cast<char*>(&length) + i, 1); for (char c : header) Write(fd, &c, 1); }
    else { Write(fd, &length, 4); Write(fd, header.data(), header.size()); }
    Write(fd, body.data(), body.size());
}
struct Frame { Object header; std::string text; };
Frame Receive(int fd) {
    uint32_t length; Read(fd, &length, 4); length = ntohl(length); CHECK(length > 0 && length <= MAX_HEADER);
    std::string header(length, '\0'); Read(fd, header.data(), length); Frame f; uint64_t bytes;
    CHECK(Parser(header).Parse(f.header)); CHECK(Header(f.header, bytes)); f.text.resize(size_t(bytes)); Read(fd, f.text.data(), f.text.size()); return f;
}
Frame Until(int fd, const std::string& type) { for (int i = 0; i < 16; ++i) { auto f = Receive(fd); if (Get(f.header, "type") == type) return f; } throw std::runtime_error("expected message absent"); }
void Connect(Socket& socket, uint16_t port) {
    socket.fd = ::socket(AF_INET, SOCK_STREAM, 0); CHECK(socket.fd >= 0); int yes = 1; setsockopt(socket.fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    timeval timeout {3, 0}; setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)); setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address {}; address.sin_family = AF_INET; address.sin_port = htons(port); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
}
struct Fixture {
    std::shared_ptr<Fake> platform = std::make_shared<Fake>(); ClipboardService service {platform, 0}; Socket socket; std::string epoch, token;
    Fixture(bool enabled = true) { CHECK(service.Start("127.0.0.1")); CHECK(service.BeginSession(epoch, token)); service.Allow(enabled); service.SetReadPermission(true);
        Connect(socket, service.Port()); Send(socket.fd, Base("data_bind", epoch, 1) + ",\"bindToken\":" + Quote(token) + ",\"purpose\":\"clipboard\"}", {}, true);
        CHECK(Get(Until(socket.fd, "data_ready").header, "messageId") == Id()); Mode(3); }
    void Mode(int mode, unsigned id = 2) { Send(socket.fd, Base("clipboard_mode", epoch, id) + ",\"mode\":" + std::to_string(mode) + "}");
        for (int i = 0; i < 8; ++i) { auto f = Until(socket.fd, "clipboard_status"); if (Get(f.header, "messageId") == Id(id)) return; } CHECK(false); }
    Frame Update(const std::string& text, uint64_t counter = 1, unsigned id = 3, const std::string& origin = "mac", const std::string& hash = {}) {
        std::string digest; platform->Digest(text, digest); if (!hash.empty()) digest = hash;
        Send(socket.fd, Base("clipboard_update", epoch, id, text.size()) + ",\"originId\":" + Quote(origin) + ",\"counter\":" + Quote(std::to_string(counter)) +
            ",\"eventId\":" + Quote(origin + "-" + std::to_string(counter)) + ",\"mime\":\"text/plain;charset=utf-8\",\"sha256\":" + Quote(digest) + "}", text);
        return Until(socket.fd, "clipboard_applied");
    }

};
void Ack(Fixture& f, const Frame& update) {
    std::string h = Base("clipboard_applied", f.epoch, 1);
    auto at = h.find(Id()); h.replace(at, 32, Get(update.header, "messageId"));
    Send(f.socket.fd, h + ",\"eventId\":" + Quote(Get(update.header, "eventId")) + ",\"status\":\"applied\",\"counter\":" +
        Quote(Get(update.header, "counter")) + ",\"originId\":\"harmony\",\"error\":\"\"}");
}
bool Closed(int fd) { auto deadline = std::chrono::steady_clock::now() + 2s; while (std::chrono::steady_clock::now() < deadline) {
    if (Available(fd, 50)) { char data[4096]; ssize_t n = recv(fd, data, sizeof(data), 0); if (n <= 0) return true; } } return false; }
void Wait(const std::function<bool()>& predicate) { auto end = std::chrono::steady_clock::now() + 3s; while (!predicate()) { CHECK(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(5ms); } }
int ServeFixture() {
    auto platform = std::make_shared<Fake>(); ClipboardService clipboard(platform, 0);
    LanServerTestOptions options; options.controlPort = 0; options.videoPort = 0; options.pinLifetimeMs = 60000;
    LanServer server(options); std::atomic<bool> input {false};
    LanInputHooks inputHooks; inputHooks.enable = [&](bool enabled) { input = enabled; return true; }; inputHooks.enabled = [&] { return input.load(); };
    inputHooks.submit = [](const RemoteInputEvent&) { return true; }; inputHooks.release = [] {}; server.SetInputHooks(std::move(inputHooks));
    LanClipboardHooks hooks; hooks.start = [&](const std::string& ip) { return clipboard.Start(ip); }; hooks.stop = [&] { clipboard.Stop(); };
    hooks.pair = [&](std::string& epoch, std::string& token) { return clipboard.BeginSession(epoch, token); }; hooks.disconnect = [&] { clipboard.EndSession(); };
    hooks.port = [&] { return clipboard.Port(); }; hooks.paste = [&](const std::string& epoch, const std::string& event, ClipboardService::Deadline until) {
        return clipboard.Paste(epoch, event, until, [](std::function<bool()> guard, ClipboardService::Deadline) { return guard(); }); };
    server.SetClipboardHooks(std::move(hooks)); CHECK(server.Start("127.0.0.1") == 0); clipboard.Allow(true); clipboard.SetReadPermission(true);
    // Test-only ephemeral PIN. Never contains real clipboard data or a real session credential.
    std::cout << server.SnapshotJson(true) << std::endl;
    bool started = false, copied = false;
    for (int step = 0; step < 6000; ++step) {
        if (!started && server.BeginStream()) {
            const uint8_t config[] {0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0xce,6,0xe2};
            const uint8_t idr[] {0,0,0,1,0x65,0x88,0x84};
            CHECK(server.Publish(config, sizeof(config), 0, true, false, false));
            CHECK(server.Publish(idr, sizeof(idr), 33333, false, true, false)); started = true;
        }
        if (!copied && platform->Writes() > 0) { std::this_thread::sleep_for(100ms); platform->Copy("Harmony fixture → Mac\n末尾空格 "); copied = true; }
        std::this_thread::sleep_for(10ms);
    }
    server.Stop(); return 0;
}
}
std::shared_ptr<ClipboardPlatform> CreateClipboardPlatform() { return std::make_shared<Fake>(); }
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--serve-fixture") return ServeFixture();
        int count = 0; auto test = [&](const char* name, const std::function<void()>& run) { run(); ++count; std::cout << "PASS " << name << '\n'; };
        test("single plain-text record permits known rich text alternate representations", [] {
            const char* types[] {"general.plain-text", "general.text", "general.html", "general.xhtml", "general.rich-text"};
            CHECK(clipboard_text_policy::Check(types, 5, 1) == clipboard_text_policy::Rejection::None); });
        test("text policy never converts files images links or unknown application data", [] {
            for (const char* unsupported : {"general.file-uri", "general.image", "openharmony.pixel-map", "general.hyperlink", "text/uri-list", "application.private-notes"}) {
                const char* types[] {"general.plain-text", unsupported};
                CHECK(clipboard_text_policy::Check(types, 2, 1) == clipboard_text_policy::Rejection::UnknownRepresentation); }
            const char* html[] {"general.html"}; CHECK(clipboard_text_policy::Check(html, 1, 1) == clipboard_text_policy::Rejection::MissingPlainText); });
        test("text policy rejects multiple records and bounds diagnostic metadata", [] {
            const char* types[] {"general.plain-text"};
            CHECK(clipboard_text_policy::Check(types, 1, 2) == clipboard_text_policy::Rejection::AmbiguousRecords);
            CHECK(clipboard_text_policy::Check(types, 33, 1) == clipboard_text_policy::Rejection::TooManyTypes);
            CHECK(clipboard_text_policy::CappedCount(9999) == 33); });
        test("canonical uint64 and overflow", [] { uint64_t n; CHECK(Decimal("18446744073709551615", n)); CHECK(!Next(n)); CHECK(!Decimal("18446744073709551616", n)); CHECK(!Decimal("01", n)); CHECK(!Decimal("1e3", n)); CHECK(!Decimal("-1", n)); });
        test("strict UTF8 preserves empty whitespace and rejects invalid NUL surrogate overlong", [] { CHECK(UTF8("")); CHECK(UTF8(" 中文🙂\n\t ")); CHECK(!UTF8(std::string("a\0b", 3))); CHECK(!UTF8("\xc0\x80")); CHECK(!UTF8("\xed\xa0\x80")); CHECK(!UTF8("\xf4\x90\x80\x80")); CHECK(!UTF8("\xe4\xb8")); });
        test("strict header duplicates unknown numeric ASCII and bounds", [] { for (auto bad : {
            "{\"version\":1,\"version\":1}", "{\"version\":1e0}", "{\"version\":01}", "{\"text\":\"中\"}", "{\"text\":\"\\u0000\"}"}) { Object o; CHECK(!Parser(bad).Parse(o)); }
            Object o; uint64_t bytes; CHECK(Parser(Base("clipboard_pull", Id(), 1) + ",\"extra\":1}").Parse(o)); CHECK(!Header(o, bytes)); });
        test("fragmented binding and mode ACK have no initial text sync", [] { Fixture f; std::this_thread::sleep_for(250ms); CHECK(f.platform->Writes() == 0); CHECK(!Available(f.socket.fd, 100)); });
        test("Unicode exact and empty writes ACK only after platform", [] { Fixture f; std::string text = "😀中文\r\n\t尾空格 "; CHECK(Get(f.Update(text).header, "status") == "applied"); CHECK(f.platform->Text() == text); CHECK(Get(f.Update("", 2, 4).header, "status") == "applied"); CHECK(f.platform->Text().empty()); });
        test("1MiB accepted exact and oversized frame rejected before payload allocation", [] { Fixture f; std::string text(MAX_PAYLOAD, 'x'); CHECK(Get(f.Update(text).header, "status") == "applied"); CHECK(f.platform->Text() == text);
            Send(f.socket.fd, Base("clipboard_pull", f.epoch, 8, MAX_PAYLOAD + 1) + "}"); CHECK(Closed(f.socket.fd)); });
        test("default Host off denies without write", [] { Fixture f(false); CHECK(Get(f.Update("denied").header, "status") == "denied"); CHECK(f.platform->Writes() == 0); });
        test("platform write failure never reports applied", [] { Fixture f; f.platform->writeCode = 201; CHECK(Get(f.Update("denied").header, "status") == "failed"); CHECK(f.service.SnapshotJson().find("\"canWrite\":false") != std::string::npos); });
        test("stale remote version cannot overwrite latest event", [] { Fixture f; CHECK(Get(f.Update("new", 5).header, "status") == "applied"); CHECK(Get(f.Update("old", 4, 4).header, "status") == "stale"); CHECK(f.platform->Text() == "new"); });
        test("mode cycling preserves Lamport and rejects reused event", [] { Fixture f; CHECK(Get(f.Update("new", 5).header, "status") == "applied"); f.Mode(0, 5); f.Mode(3, 6); CHECK(Get(f.Update("old", 5, 7).header, "status") == "stale"); });
        test("self write does not echo but repeated identical user copies are new events", [] { Fixture f; CHECK(Get(f.Update("same").header, "status") == "applied");
            std::this_thread::sleep_for(250ms); while (Available(f.socket.fd, 10)) CHECK(Get(Receive(f.socket.fd).header, "type") == "clipboard_status");
            f.platform->Copy("same", true); auto first = Until(f.socket.fd, "clipboard_update"); CHECK(first.text == "same"); Ack(f, first);
            f.platform->Copy("same", true); auto second = Until(f.socket.fd, "clipboard_update"); CHECK(Get(first.header, "eventId") != Get(second.header, "eventId")); Ack(f, second); });
        test("poll before remote write preserves intervening local copy", [] { Fixture f; CHECK(Get(f.Update("baseline", 1).header, "status") == "applied"); f.platform->Copy("local wins"); auto response = f.Update("remote old", 1, 4); CHECK(Get(response.header, "status") == "stale"); CHECK(f.platform->Text() == "local wins"); });
        test("permission denial stops repeated SDK reads until real state changes", [] { Fixture f; { std::lock_guard<std::mutex> l(f.platform->mutex); f.platform->readCode = 201; } f.platform->Copy("one");
            Wait([&] { return f.service.SnapshotJson().find("\"errorCode\":201") != std::string::npos; }); int before; { std::lock_guard<std::mutex> l(f.platform->mutex); before = f.platform->reads; }
            for (int i = 0; i < 3; ++i) { f.service.SetReadPermission(true); f.platform->Copy("two"); std::this_thread::sleep_for(220ms); }
            std::lock_guard<std::mutex> l(f.platform->mutex); CHECK(f.platform->reads == before); });
        test("transient busy read recovers with bounded retry and unchanged permission", [] { Fixture f;
            { std::lock_guard<std::mutex> l(f.platform->mutex); f.platform->readCode = 12900003; } f.platform->Copy("retry");
            Wait([&] { return f.service.SnapshotJson().find("12900003") != std::string::npos; });
            { std::lock_guard<std::mutex> l(f.platform->mutex); f.platform->readCode = 0; }
            auto update = Until(f.socket.fd, "clipboard_update"); CHECK(update.text == "retry"); Ack(f, update); });
        test("paste guard accepts current ACK and rejects revision changed at execution", [] { Fixture f; CHECK(Get(f.Update("paste").header, "status") == "applied");
            auto good = f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() + 1s, [](auto check, auto) { return check(); }); CHECK(good.first == "committed");
            auto stale = f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() + 1s, [&](auto check, auto) { f.platform->Copy("newer"); return check(); }); CHECK(stale.first == "failed"); });
        test("current Harmony event pastes without replacing original system clipboard", [] { Fixture f;
            f.platform->Copy("local rich text plain representation"); auto update = Until(f.socket.fd, "clipboard_update"); Ack(f, update);
            const auto event = Get(update.header, "eventId"); CHECK(event.find("harmony-") == 0);
            CHECK(f.service.Paste(f.epoch, event, std::chrono::steady_clock::now() + 1s,
                [](auto check, auto) { return check(); }).first == "committed");
            CHECK(f.platform->Writes() == 0); CHECK(f.platform->Text() == update.text);
            CHECK(f.service.Paste(f.epoch, event, std::chrono::steady_clock::now() + 1s,
                [&](auto check, auto) { f.platform->Copy("newer local copy"); return check(); }).first == "failed"); });
        test("unsupported local copy prevents paste of previous Mac event before and after polling", [] { Fixture f;
            CHECK(Get(f.Update("old Terminal text").header, "status") == "applied");
            { std::lock_guard<std::mutex> lock(f.platform->mutex); f.platform->supported = false; }
            f.platform->Copy("unsupported Notes selection");
            CHECK(f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() + 1s,
                [](auto check, auto) { return check(); }).first == "stale");
            Wait([&] { return f.service.SnapshotJson().find("unsupported_clipboard_type") != std::string::npos; });
            CHECK(f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() + 1s,
                [](auto check, auto) { return check(); }).first == "stale");
            CHECK(f.platform->Writes() == 1); CHECK(f.platform->Text() == "unsupported Notes selection"); });
        test("unsupported local copy invalidates earlier Harmony event", [] { Fixture f;
            f.platform->Copy("first local text"); auto update = Until(f.socket.fd, "clipboard_update"); Ack(f, update);
            { std::lock_guard<std::mutex> lock(f.platform->mutex); f.platform->supported = false; }
            f.platform->Copy("new unsupported item");
            CHECK(f.service.Paste(f.epoch, Get(update.header, "eventId"), std::chrono::steady_clock::now() + 1s,
                [](auto check, auto) { return check(); }).first == "stale"); CHECK(f.platform->Writes() == 0); });
        test("unsupported copy drops an older unsent update and exposes bounded categories only", [] { Fixture f;
            f.platform->Copy("first outgoing"); auto first = Until(f.socket.fd, "clipboard_update");
            f.platform->Copy("queued outgoing"); std::this_thread::sleep_for(250ms);
            { std::lock_guard<std::mutex> lock(f.platform->mutex); f.platform->supported = false;
                f.platform->shape = {9999, 9999, clipboard_text_policy::Rejection::UnknownRepresentation}; }
            f.platform->Copy("private unsupported selection");
            Wait([&] { return f.service.SnapshotJson().find("unsupported_representation") != std::string::npos; });
            Ack(f, first); std::this_thread::sleep_for(250ms);
            while (Available(f.socket.fd, 10)) CHECK(Get(Receive(f.socket.fd).header, "type") == "clipboard_status");
            const auto snapshot = f.service.SnapshotJson(); CHECK(snapshot.find("\"readTypeCount\":33") != std::string::npos);
            CHECK(snapshot.find("\"readRecordCount\":33") != std::string::npos);
            CHECK(snapshot.find("private unsupported selection") == std::string::npos); CHECK(f.platform->Writes() == 0); });
        test("local disable and expired deadline reject paste", [] { Fixture f; CHECK(Get(f.Update("paste").header, "status") == "applied");
            CHECK(f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() - 1ms, {}).first == "stale"); f.service.Allow(false);
            CHECK(f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() + 1s, {}).first == "stale"); });
        test("deferred own revision requires tagged content and never authorizes early paste", [] { Fixture f; f.platform->deferRevision = true;
            CHECK(Get(f.Update("deferred").header, "status") == "failed"); CHECK(f.service.Paste(f.epoch, "mac-1", std::chrono::steady_clock::now() + 1s, {}).first == "stale");
            f.platform->Copy("deferred", true); std::this_thread::sleep_for(250ms); while (Available(f.socket.fd, 10)) CHECK(Get(Receive(f.socket.fd).header, "type") == "clipboard_status"); });
        test("wrong SHA fails clipboard only", [] { Fixture f; try { f.Update("bad hash", 1, 3, "mac", std::string(64, '0')); CHECK(false); } catch (const std::runtime_error&) {} CHECK(Closed(f.socket.fd)); CHECK(f.platform->Writes() == 0); });
        test("peer cannot impersonate Harmony origin", [] { Fixture f; try { f.Update("forged", 1, 3, "harmony"); CHECK(false); } catch (const std::runtime_error&) {} CHECK(Closed(f.socket.fd)); CHECK(f.platform->Writes() == 0); });
        test("invalid UTF8 rejected without mutation", [] { Fixture f; try { f.Update("\xc0\x80"); CHECK(false); } catch (const std::runtime_error&) {} CHECK(Closed(f.socket.fd)); CHECK(f.platform->Writes() == 0); });
        test("wrong epoch and duplicate bind close feature", [] { Fixture f; Send(f.socket.fd, Base("clipboard_pull", std::string(32, 'f'), 9) + "}"); CHECK(Closed(f.socket.fd)); Fixture g;
            Send(g.socket.fd, Base("data_bind", g.epoch, 10) + ",\"bindToken\":" + Quote(g.token) + ",\"purpose\":\"clipboard\"}"); CHECK(Closed(g.socket.fd)); });
        test("wrong binding credential cannot read write or reuse an authenticated socket", [] {
            auto platform = std::make_shared<Fake>(); ClipboardService service(platform, 0); CHECK(service.Start("127.0.0.1")); std::string epoch, token;
            CHECK(service.BeginSession(epoch, token)); Socket socket; Connect(socket, service.Port());
            std::string wrong = token; wrong[0] = wrong[0] == '0' ? '1' : '0';
            Send(socket.fd, Base("data_bind", epoch, 1) + ",\"bindToken\":" + Quote(wrong) + ",\"purpose\":\"clipboard\"}");
            CHECK(Closed(socket.fd)); CHECK(platform->Writes() == 0); });
        test("off direction denies writes and late cancelled ACK stays isolated", [] {
            Fixture f; f.Mode(0, 5); CHECK(Get(f.Update("off").header, "status") == "denied"); CHECK(f.platform->Writes() == 0);
            Send(f.socket.fd, Base("clipboard_applied", f.epoch, 99) + ",\"eventId\":\"harmony-42\",\"status\":\"stale\",\"counter\":\"42\",\"originId\":\"harmony\",\"error\":\"cancelled\"}");
            f.Mode(3, 6); CHECK(Get(f.Update("on", 2, 7).header, "status") == "applied"); });
        test("diagnostics exclude clipboard body digest and credentials", [] { Fixture f; CHECK(Get(f.Update("private body").header, "status") == "applied"); std::string digest; f.platform->Digest("private body", digest);
            const auto json = f.service.SnapshotJson(); for (const auto& secret : {std::string("private body"), digest, f.epoch, f.token}) CHECK(json.find(secret) == std::string::npos); });
        std::cout << "clipboard service tests passed: " << count << "; virtual clipboard and loopback only.\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
