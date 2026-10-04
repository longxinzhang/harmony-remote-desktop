#include "audio_service.h"
#include "audio_wire.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
static int checks = 0;
void Check(bool b, const char* name) { if (!b) throw std::runtime_error(name); ++checks; std::cout << "PASS " << name << '\n'; }
int Connect(uint16_t port) { int fd = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a {}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr); if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a))) throw std::runtime_error("connect"); return fd; }
bool Read(int fd, uint8_t* b, size_t n) { size_t at = 0; while (at < n) { pollfd p {fd, POLLIN, 0}; if (poll(&p, 1, 3000) <= 0) return false; auto c = recv(fd, b + at, n - at, 0); if (c <= 0) return false; at += size_t(c); } return true; }
bool Closed(int fd) { char b; pollfd p {fd, POLLIN, 0}; return poll(&p, 1, 3000) > 0 && recv(fd, &b, 1, 0) <= 0; }
uint64_t U(const uint8_t* b, size_t n) { uint64_t v = 0; while (n--) v = (v << 8) | *b++; return v; }
std::string Binding(const std::string& e, const std::string& t) { return "{\"type\":\"audio_bind\",\"version\":1,\"epoch\":\"" + e + "\",\"token\":\"" + t + "\"}"; }
bool Bind(int fd, const std::string& text) { std::array<uint8_t, 4> p {}; audio_wire::Put(p.data(), text.size(), 4); send(fd, p.data(), 4, 0); send(fd, text.data(), text.size(), 0); if (!Read(fd, p.data(), 4)) return false; auto n = U(p.data(), 4); if (n > 512) return false; std::string r(n, '\0'); return Read(fd, reinterpret_cast<uint8_t*>(r.data()), n) && r == "{\"type\":\"audio_bound\",\"version\":1}"; }
struct Packet { uint64_t seq = 0, stream = 0, pts = 0; int kind = 0; std::vector<uint8_t> data; };
Packet Next(int fd) { std::array<uint8_t, 40> h {}; if (!Read(fd, h.data(), h.size())) throw std::runtime_error("packet timeout"); if (std::memcmp(h.data(), "HRDA", 4)) throw std::runtime_error("magic"); Packet p; p.kind = h[5]; p.seq = U(h.data() + 24, 8); p.stream = U(h.data() + 32, 8); p.pts = U(h.data() + 16, 8); p.data.resize(U(h.data() + 8, 4)); if (p.data.size() > 3840 || !Read(fd, p.data.data(), p.data.size())) throw std::runtime_error("payload"); return p; }
int main() {
try {
    AudioService service(0); std::string e, t;
    Check(!service.Start("0.0.0.0"), "wildcard bind denied");
    Check(!service.BeginSession(e, t), "pair denied before listener");
    Check(service.Start("127.0.0.1"), "independent test listener starts");
    int unauth = Connect(service.Port()); Check(Closed(unauth), "unpaired connection gets no audio"); close(unauth);
    Check(service.BeginSession(e, t) && e.size() == 32 && t.size() == 64, "fresh random audio credentials issued");
    int wrong = Connect(service.Port()); Check(!Bind(wrong, Binding(e, std::string(64, '0'))), "wrong binding rejected"); close(wrong);
    int duplicate = Connect(service.Port()); auto duplicateText = Binding(e, t); duplicateText.insert(1, "\"version\":1,");
    Check(!Bind(duplicate, duplicateText), "duplicate auth JSON key rejected"); close(duplicate);
    int client = Connect(service.Port()); Check(Bind(client, Binding(e, t)), "authenticated binding succeeds");
    auto boundReset = Next(client); Check(boundReset.kind == 2 && boundReset.data.empty(), "binding delivers reset before audio");
    std::vector<uint8_t> samples(3840); for (size_t i = 0; i < samples.size(); ++i) samples[i] = uint8_t(i);
    Check(!service.PublishPCM(samples.data(), samples.size(), 100), "audio excluded before share starts");
    service.BeginStream(); auto begin = Next(client); Check(begin.kind == 2 && begin.stream > boundReset.stream, "new share advances stream epoch");
    Check(!service.PublishPCM(samples.data(), 3, 100), "partial sample frame rejected");
    Check(service.PublishPCM(samples.data(), samples.size(), 123000), "system PCM accepted without I/O in producer");
    auto packet = Next(client); Check(packet.kind == 1 && packet.seq > begin.seq && packet.stream == begin.stream && packet.pts == 123000 && packet.data == samples, "PCM bytes timestamps and sequence survive transport");
    std::vector<uint8_t> burst(192000, 44); Check(service.PublishPCM(burst.data(), burst.size(), 200000), "one second producer burst accepted with dropping");
    std::string state = service.SnapshotJson(); Check(state.find("\"highWaterBytes\":23040") != std::string::npos && state.find("\"droppedBytes\":0") == std::string::npos, "queue bounded to 120ms and drops stale PCM");
    Check(state.find(e) == std::string::npos && state.find(t) == std::string::npos, "diagnostics do not expose binding credentials");
    service.EndSession(); Check(!service.PublishPCM(samples.data(), samples.size(), 0), "session revoke denies producer immediately"); close(client);
    const auto oldEpoch = e, oldToken = t; Check(service.BeginSession(e, t) && e != oldEpoch && t != oldToken, "reconnect rotates both credentials");
    int old = Connect(service.Port()); Check(!Bind(old, Binding(oldEpoch, oldToken)), "prior session credentials rejected"); close(old);
    int fresh = Connect(service.Port()); Check(Bind(fresh, Binding(e, t)), "fresh session binds after revocation"); Next(fresh);
    service.BeginStream(); Next(fresh); service.EndStream(); auto end = Next(fresh);
    Check(end.kind == 2 && !service.PublishPCM(samples.data(), samples.size(), 0), "share stop resets playback and prevents additional audio");
    service.Stop(); Check(Closed(fresh), "server stop closes channel"); close(fresh);
    Check(service.Start("127.0.0.1"), "server restarts after explicit stop"); service.Stop();
    std::cout << "Audio service: " << checks << " checks passed\n";
} catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
