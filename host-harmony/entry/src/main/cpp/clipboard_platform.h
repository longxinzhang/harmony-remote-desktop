#ifndef HRD_CLIPBOARD_PLATFORM_H
#define HRD_CLIPBOARD_PLATFORM_H
#include "clipboard_text_policy.h"
#include <cstdint>
#include <memory>
#include <string>
// Returned objects never contain logging methods; text and owner tag remain in memory only.
struct ClipboardText {
    std::string text, tag; uint32_t revision = 0; int code = 0; bool supported = false;
    clipboard_text_policy::Shape shape;
};
class ClipboardPlatform {
public:
    virtual ~ClipboardPlatform() = default;
    virtual uint32_t Revision() = 0;
    virtual ClipboardText Read() = 0;
    virtual int Write(const std::string& text, const std::string& tag) = 0;
    virtual bool Digest(const std::string& text, std::string& hex) = 0;
};
std::shared_ptr<ClipboardPlatform> CreateClipboardPlatform();
#endif
