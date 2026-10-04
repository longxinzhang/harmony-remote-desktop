#include "clipboard_platform.h"
#include "clipboard_wire.h"
#include <database/pasteboard/oh_pasteboard.h>
#include <database/udmf/udmf.h>
#include <database/udmf/uds.h>
#include <CryptoArchitectureKit/crypto_digest.h>
#include <cstring>
#include <mutex>
namespace {
template<class T, void (*Destroy)(T*)> using Owner = std::unique_ptr<T, decltype(Destroy)>;
class NativeClipboard final : public ClipboardPlatform {
public:
    NativeClipboard() : board_(OH_Pasteboard_Create(), OH_Pasteboard_Destroy) {}
    uint32_t Revision() override { std::lock_guard<std::mutex> lock(mutex_); return board_ ? OH_Pasteboard_GetChangeCount(board_.get()) : 0; }
    ClipboardText Read() override
    {
        std::lock_guard<std::mutex> lock(mutex_); ClipboardText out;
        if (!board_) { out.code = 12900000; return out; }
        const auto revision = OH_Pasteboard_GetChangeCount(board_.get());
        Owner<OH_UdmfData, OH_UdmfData_Destroy> data(OH_Pasteboard_GetData(board_.get(), &out.code), OH_UdmfData_Destroy);
        if (!data || out.code != 0) return out;
        // Only read the explicit plain representation of one text record.
        // Rich-text alternatives are retained locally and never parsed or sent.
        unsigned int count = 0;
        char** types = OH_UdmfData_GetTypes(data.get(), &count);
        unsigned int records = 0;
        auto items = OH_UdmfData_GetRecords(data.get(), &records);
        out.shape.typeCount = clipboard_text_policy::CappedCount(count);
        out.shape.recordCount = clipboard_text_policy::CappedCount(records);
        out.shape.rejection = clipboard_text_policy::Check(types, count, items ? records : 0);
        if (out.shape.rejection != clipboard_text_policy::Rejection::None) return out;
        Owner<OH_UdsPlainText, OH_UdsPlainText_Destroy> plain(OH_UdsPlainText_Create(), OH_UdsPlainText_Destroy);
        if (!plain) { out.code = 12900000; return out; }
        out.code = OH_UdmfData_GetPrimaryPlainText(data.get(), plain.get());
        if (out.code != 0) return out;
        const char* content = OH_UdsPlainText_GetContent(plain.get());
        if (!content) return out;
        size_t length = strnlen(content, clipboard_wire::MAX_PAYLOAD + 1);
        if (length > clipboard_wire::MAX_PAYLOAD) { out.code = 9200001; return out; }
        out.text.assign(content, length);
        if (!clipboard_wire::UTF8(out.text)) { out.text.clear(); out.code = 9200002; return out; }
        Owner<OH_UdmfProperty, OH_UdmfProperty_Destroy> property(OH_UdmfProperty_Create(data.get()), OH_UdmfProperty_Destroy);
        if (property) { const char* tag = OH_UdmfProperty_GetTag(property.get()); if (tag && strnlen(tag, 129) <= 128) out.tag = tag; }
        out.revision = OH_Pasteboard_GetChangeCount(board_.get());
        if (out.revision != revision) { out.text.clear(); out.code = 9200003; return out; }
        out.supported = true; return out;
    }
    int Write(const std::string& text, const std::string& tag) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!board_ || !clipboard_wire::UTF8(text)) return 401;
        Owner<OH_UdmfData, OH_UdmfData_Destroy> data(OH_UdmfData_Create(), OH_UdmfData_Destroy);
        Owner<OH_UdmfRecord, OH_UdmfRecord_Destroy> record(OH_UdmfRecord_Create(), OH_UdmfRecord_Destroy);
        Owner<OH_UdsPlainText, OH_UdsPlainText_Destroy> plain(OH_UdsPlainText_Create(), OH_UdsPlainText_Destroy);
        if (!data || !record || !plain) return 12900000;
        Owner<OH_UdmfProperty, OH_UdmfProperty_Destroy> property(OH_UdmfProperty_Create(data.get()), OH_UdmfProperty_Destroy);
        if (!property) return 12900000;
        int code = OH_UdsPlainText_SetContent(plain.get(), text.c_str());
        if (!code) code = OH_UdmfRecord_AddPlainText(record.get(), plain.get());
        if (!code) code = OH_UdmfData_AddRecord(data.get(), record.get());
        if (!code) code = OH_UdmfProperty_SetShareOption(property.get(), SHARE_OPTIONS_CROSS_APP);
        if (!code) code = OH_UdmfProperty_SetTag(property.get(), tag.c_str());
        if (!code) code = OH_Pasteboard_SetData(board_.get(), data.get());
        return code;
    }
    bool Digest(const std::string& text, std::string& hex) override
    {
        OH_CryptoDigest* context = nullptr;
        if (OH_CryptoDigest_Create("SHA256", &context) != CRYPTO_SUCCESS || !context) return false;
        Owner<OH_CryptoDigest, OH_DigestCrypto_Destroy> digest(context, OH_DigestCrypto_Destroy);
        Crypto_DataBlob input {reinterpret_cast<uint8_t*>(const_cast<char*>(text.data())), text.size()}, output {};
        // Empty SHA256 is Final on a fresh context; some SDKs reject a zero-sized Update.
        if (!text.empty() && OH_CryptoDigest_Update(context, &input) != CRYPTO_SUCCESS) return false;
        const int code = OH_CryptoDigest_Final(context, &output);
        if (code == CRYPTO_SUCCESS && output.data && output.len == 32) {
            constexpr char HEX[] = "0123456789abcdef"; hex.clear(); hex.reserve(64);
            for (size_t i = 0; i < output.len; ++i) { hex += HEX[output.data[i] >> 4]; hex += HEX[output.data[i] & 15]; }
        }
        OH_Crypto_FreeDataBlob(&output);
        return code == CRYPTO_SUCCESS && hex.size() == 64;
    }
private:
    std::mutex mutex_;
    Owner<OH_Pasteboard, OH_Pasteboard_Destroy> board_;
};
}
std::shared_ptr<ClipboardPlatform> CreateClipboardPlatform() { return std::make_shared<NativeClipboard>(); }
