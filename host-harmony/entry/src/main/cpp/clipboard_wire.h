#ifndef HRD_CLIPBOARD_WIRE_H
#define HRD_CLIPBOARD_WIRE_H
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <limits>
namespace clipboard_wire {
constexpr size_t MAX_HEADER = 4096, MAX_PAYLOAD = 1048576;
struct Value { std::string text; bool string = false; };
using Object = std::map<std::string, Value>;
inline bool Decimal(const std::string& s, uint64_t& value)
{
    if (s.empty() || (s.size() > 1 && s[0] == '0')) return false;
    value = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || value > (UINT64_MAX - unsigned(c - '0')) / 10) return false;
        value = value * 10 + unsigned(c - '0');
    }
    return true;
}
inline bool Hex(const std::string& s, size_t size)
{ if (s.size() != size) return false; for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false; return true; }
inline std::string Get(const Object& o, const std::string& key)
{ auto i = o.find(key); return i != o.end() && i->second.string ? i->second.text : std::string(); }
inline bool Integer(const Object& o, const std::string& key, uint64_t& n)
{ auto i = o.find(key); return i != o.end() && !i->second.string && Decimal(i->second.text, n); }
inline bool Boolean(const Object& o, const std::string& key)
{ auto i = o.find(key); return i != o.end() && !i->second.string && (i->second.text == "true" || i->second.text == "false"); }
inline std::string Quote(const std::string& s)
{ std::string r = "\""; for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; } return r + '"'; }
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool Parse(Object& out)
    {
        if (s_.empty() || s_.size() > MAX_HEADER) return false;
        Skip(); if (!Take('{')) return false; Skip();
        for (size_t count = 0; count < 16; ++count) {
            std::string key; Value value;
            if (!String(key) || key.empty() || key.size() > 64) return false;
            Skip(); if (!Take(':')) return false; Skip();
            if (at_ < s_.size() && s_[at_] == '"') { value.string = true; if (!String(value.text)) return false; }
            else {
                size_t start = at_;
                while (at_ < s_.size() && s_[at_] != ',' && s_[at_] != '}' && !Space(s_[at_])) ++at_;
                value.text = s_.substr(start, at_ - start); uint64_t n;
                if (value.text != "true" && value.text != "false" && !Decimal(value.text, n)) return false;
            }
            if (!out.emplace(key, std::move(value)).second) return false;
            Skip(); if (Take('}')) { Skip(); return at_ == s_.size(); }
            if (!Take(',')) return false; Skip();
        }
        return false;
    }
private:
    static bool Space(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
    void Skip() { while (at_ < s_.size() && Space(s_[at_])) ++at_; }
    bool Take(char c) { if (at_ >= s_.size() || s_[at_] != c) return false; ++at_; return true; }
    bool String(std::string& out)
    {
        if (!Take('"')) return false;
        while (at_ < s_.size()) {
            unsigned char c = s_[at_++]; if (c == '"') return true;
            if (c < 32 || c >= 127) return false;
            if (c == '\\') {
                if (at_ == s_.size()) return false;
                c = s_[at_++]; if (c != '"' && c != '\\' && c != '/') return false;
            }
            out += char(c);
        }
        return false;
    }
    const std::string& s_; size_t at_ = 0;
};
inline bool UTF8(const std::string& text)
{
    if (text.size() > MAX_PAYLOAD) return false;
    for (size_t i = 0; i < text.size();) {
        uint32_t c = static_cast<unsigned char>(text[i++]);
        if (c == 0) return false;
        if (c < 128) continue;
        unsigned continuation = 0; uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; continuation = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; continuation = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; continuation = 3; minimum = 0x10000; }
        else return false;
        if (text.size() - i < continuation) return false;
        while (continuation--) { unsigned char next = text[i++]; if ((next & 0xc0) != 0x80) return false; c = (c << 6) | (next & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
inline bool Header(const Object& o, uint64_t& size)
{
    uint64_t version;
    if (!Integer(o, "version", version) || version != 1 || !Integer(o, "payloadLength", size) || size > MAX_PAYLOAD ||
        !Hex(Get(o, "messageId"), 32) || !Hex(Get(o, "sessionEpoch"), 32)) return false;
    std::set<std::string> keys {"version", "type", "sessionEpoch", "messageId", "payloadLength"};
    const auto type = Get(o, "type");
    auto extra = [&](std::initializer_list<const char*> names) { for (auto name : names) keys.insert(name); };
    if (type == "data_bind") { extra({"bindToken", "purpose"}); if (!Hex(Get(o, "bindToken"), 64) || Get(o, "purpose") != "clipboard") return false; }
    else if (type == "data_ready" || type == "clipboard_pull") {}
    else if (type == "clipboard_mode") { extra({"mode"}); uint64_t n; if (!Integer(o, "mode", n) || n > 3) return false; }
    else if (type == "clipboard_status") { extra({"allowed", "canRead", "canWrite", "mode", "error"}); uint64_t n;
        if (!Integer(o, "mode", n) || n > 3 || !Boolean(o, "allowed") || !Boolean(o, "canRead") || !Boolean(o, "canWrite")) return false; }
    else if (type == "clipboard_update") {
        extra({"originId", "counter", "eventId", "mime", "sha256"}); uint64_t counter;
        auto origin = Get(o, "originId"); auto decimal = Get(o, "counter");
        if ((origin != "mac" && origin != "harmony") || !Decimal(decimal, counter) || counter == 0 ||
            Get(o, "eventId") != origin + "-" + decimal || Get(o, "mime") != "text/plain;charset=utf-8" || !Hex(Get(o, "sha256"), 64)) return false;
    } else if (type == "clipboard_applied") {
        extra({"eventId", "status", "counter", "originId", "error"}); uint64_t counter;
        const auto status = Get(o, "status"), origin = Get(o, "originId");
        if (!Decimal(Get(o, "counter"), counter) || (origin != "mac" && origin != "harmony" && origin != "none") ||
            (status != "applied" && status != "stale" && status != "denied" && status != "failed")) return false;
    } else return false;
    if (type != "clipboard_update" && size != 0) return false;
    if (o.size() != keys.size()) return false;
    for (const auto& entry : o) if (!keys.count(entry.first)) return false;
    for (const char* key : {"error", "eventId"}) { auto it = o.find(key); if (it != o.end() && (!it->second.string || it->second.text.size() > 128)) return false; }
    return true;
}
struct Version {
    uint64_t counter = 0; std::string origin = "none";
    bool operator<(const Version& b) const { return counter < b.counter || (counter == b.counter && origin < b.origin); }
    std::string Event() const { return origin + "-" + std::to_string(counter); }
};
inline bool Next(uint64_t& counter) { if (counter == UINT64_MAX) return false; ++counter; return true; }
}
#endif
