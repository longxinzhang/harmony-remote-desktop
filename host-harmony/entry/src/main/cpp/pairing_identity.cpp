#include "pairing_identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(HRD_LAN_TESTING) && defined(__APPLE__)
#include <Security/Security.h>
#else
#include <CryptoArchitectureKit/crypto_asym_key.h>
#include <CryptoArchitectureKit/crypto_signature.h>
#include <huks/native_huks_api.h>
#include <huks/native_huks_param.h>
#endif

namespace {
constexpr size_t MAX_PEERS = 16;
constexpr size_t MAX_STORE = 4096;
const std::string FILE_NAME = "/trusted-peers-v1";
thread_local std::string platformFailure;
[[maybe_unused]] const std::array<uint8_t, 26> P256_SPKI_PREFIX {
    0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,
    0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00
};

std::string Hex(const uint8_t* bytes, size_t count)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out; out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) { out += digits[bytes[i] >> 4]; out += digits[bytes[i] & 15]; }
    return out;
}
std::vector<uint8_t> Unhex(const std::string& text)
{
    if (text.empty() || text.size() % 2) return {};
    std::vector<uint8_t> out; out.reserve(text.size() / 2);
    auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        int a = digit(text[i]), b = digit(text[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(static_cast<uint8_t>((a << 4) | b));
    }
    return out;
}

// HUKS implementations may return fixed-width r||s or ASN.1. The wire always
// uses DER, which CryptoKit and CryptoArchitectureKit both verify natively.
std::vector<uint8_t> SignatureDER(const uint8_t* bytes, size_t count)
{
    if (count != 64) {
        if (count >= 8 && count <= 72 && bytes[0] == 0x30 && bytes[1] == count - 2)
            return {bytes, bytes + count};
        return {};
    }
    std::vector<uint8_t> body;
    for (size_t start : {size_t(0), size_t(32)}) {
        size_t first = start; while (first < start + 31 && bytes[first] == 0) ++first;
        bool pad = (bytes[first] & 128) != 0;
        body.push_back(2); body.push_back(static_cast<uint8_t>(start + 32 - first + (pad ? 1 : 0)));
        if (pad) body.push_back(0);
        body.insert(body.end(), bytes + first, bytes + start + 32);
    }
    std::vector<uint8_t> out {0x30, static_cast<uint8_t>(body.size())};
    out.insert(out.end(), body.begin(), body.end()); return out;
}

#if defined(HRD_LAN_TESTING) && defined(__APPLE__)
struct PlatformKey {
    SecKeyRef key = nullptr;
    ~PlatformKey() { if (key) CFRelease(key); }
};
// Test identities are process-local Security.framework keys. Never write test
// private keys or modify the user's Keychain. Production uses HUKS exclusively.
std::mutex testKeysMutex;
std::map<std::string, std::shared_ptr<PlatformKey>> testKeys;
std::shared_ptr<PlatformKey> Identity(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(testKeysMutex);
    auto found = testKeys.find(directory); if (found != testKeys.end()) return found->second;
    int bits = 256; CFNumberRef size = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
    const void* keys[] {kSecAttrKeyType, kSecAttrKeySizeInBits};
    const void* values[] {kSecAttrKeyTypeECSECPrimeRandom, size};
    CFDictionaryRef parameters = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFErrorRef error = nullptr; SecKeyRef key = SecKeyCreateRandomKey(parameters, &error);
    CFRelease(parameters); CFRelease(size); if (error) CFRelease(error);
    if (!key) return {};
    auto identity = std::make_shared<PlatformKey>(); identity->key = key; testKeys[directory] = identity; return identity;
}
std::vector<uint8_t> Export(const std::shared_ptr<PlatformKey>& identity)
{
    SecKeyRef pub = SecKeyCopyPublicKey(identity->key); if (!pub) return {};
    CFErrorRef error = nullptr; CFDataRef data = SecKeyCopyExternalRepresentation(pub, &error);
    CFRelease(pub); if (error) CFRelease(error);
    if (!data) return {};
    std::vector<uint8_t> bytes(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data)); CFRelease(data); return bytes;
}
std::vector<uint8_t> SignPlatform(const std::shared_ptr<PlatformKey>& identity, const std::string& message)
{
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(message.data()), message.size());
    CFErrorRef error = nullptr; CFDataRef signature = SecKeyCreateSignature(identity->key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, data, &error);
    CFRelease(data); if (error) CFRelease(error); if (!signature) return {};
    auto result = SignatureDER(CFDataGetBytePtr(signature), CFDataGetLength(signature)); CFRelease(signature); return result;
}
bool VerifyPlatform(const std::vector<uint8_t>& pub, const std::string& message, const std::vector<uint8_t>& signature)
{
    CFDataRef data = CFDataCreate(nullptr, pub.data(), pub.size());
    int bits = 256; CFNumberRef size = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
    const void* keys[] {kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits};
    const void* values[] {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic, size};
    CFDictionaryRef parameters = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFErrorRef error = nullptr; SecKeyRef key = SecKeyCreateWithData(data, parameters, &error);
    CFRelease(parameters); CFRelease(size); CFRelease(data); if (error) { CFRelease(error); error = nullptr; }
    if (!key) return false;
    CFDataRef input = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(message.data()), message.size());
    CFDataRef sig = CFDataCreate(nullptr, signature.data(), signature.size());
    bool valid = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, input, sig, &error);
    CFRelease(key); CFRelease(input); CFRelease(sig); if (error) CFRelease(error); return valid;
}
#else
struct Parameters {
    OH_Huks_ParamSet* value = nullptr;
    Parameters() {
        OH_Huks_Param params[4] {};
        params[0].tag = OH_HUKS_TAG_ALGORITHM; params[0].uint32Param = OH_HUKS_ALG_ECC;
        params[1].tag = OH_HUKS_TAG_PURPOSE; params[1].uint32Param = OH_HUKS_KEY_PURPOSE_SIGN;
        params[2].tag = OH_HUKS_TAG_KEY_SIZE; params[2].uint32Param = OH_HUKS_ECC_KEY_SIZE_256;
        params[3].tag = OH_HUKS_TAG_DIGEST; params[3].uint32Param = OH_HUKS_DIGEST_SHA256;
        if (OH_Huks_InitParamSet(&value).errorCode != OH_HUKS_SUCCESS ||
            OH_Huks_AddParams(value, params, 4).errorCode != OH_HUKS_SUCCESS ||
            OH_Huks_BuildParamSet(&value).errorCode != OH_HUKS_SUCCESS) OH_Huks_FreeParamSet(&value);
    }
    ~Parameters() { if (value) OH_Huks_FreeParamSet(&value); }
};
struct PlatformKey {};
OH_Huks_Blob Alias()
{
    static std::string name = "harmony.remote.host.p256.v1";
    return {static_cast<uint32_t>(name.size()), reinterpret_cast<uint8_t*>(name.data())};
}
std::shared_ptr<PlatformKey> Identity(const std::string&)
{
    platformFailure.clear();
    auto alias = Alias();
    auto exists = OH_Huks_IsKeyItemExist(&alias, nullptr);
    if (exists.errorCode != OH_HUKS_SUCCESS) {
        if (exists.errorCode != OH_HUKS_ERR_CODE_ITEM_NOT_EXIST) { platformFailure = "key_lookup_" + std::to_string(exists.errorCode); return {}; }
        Parameters params; if (!params.value) { platformFailure = "key_parameters"; return {}; }
        auto generated = OH_Huks_GenerateKeyItem(&alias, params.value, nullptr);
        if (generated.errorCode != OH_HUKS_SUCCESS) { platformFailure = "key_generate_" + std::to_string(generated.errorCode); return {}; }
    }
    return std::make_shared<PlatformKey>();
}
std::vector<uint8_t> Export(const std::shared_ptr<PlatformKey>&)
{
    std::array<uint8_t, 512> bytes {}; OH_Huks_Blob output {bytes.size(), bytes.data()}; auto alias = Alias();
    auto result = OH_Huks_ExportPublicKeyItem(&alias, nullptr, &output);
    if (result.errorCode != OH_HUKS_SUCCESS || output.size > bytes.size()) { platformFailure = "public_export_" + std::to_string(result.errorCode); return {}; }
    if (output.size == 65 && bytes[0] == 4) return {bytes.begin(), bytes.begin() + 65};
    if (output.size == 91 && std::equal(P256_SPKI_PREFIX.begin(), P256_SPKI_PREFIX.end(), bytes.begin()) && bytes[26] == 4)
        return {bytes.begin() + 26, bytes.begin() + 91};
    platformFailure = "public_export_format_" + std::to_string(output.size); return {};
}
std::vector<uint8_t> SignPlatform(const std::shared_ptr<PlatformKey>&, const std::string& message)
{
    Parameters params; if (!params.value) { platformFailure = "sign_parameters"; return {}; }
    auto alias = Alias(); uint64_t handleValue = 0;
    OH_Huks_Blob handle {sizeof(handleValue), reinterpret_cast<uint8_t*>(&handleValue)};
    auto initialized = OH_Huks_InitSession(&alias, params.value, &handle, nullptr);
    if (initialized.errorCode != OH_HUKS_SUCCESS) { platformFailure = "sign_init_" + std::to_string(initialized.errorCode); return {}; }
    OH_Huks_Blob input {static_cast<uint32_t>(message.size()), reinterpret_cast<uint8_t*>(const_cast<char*>(message.data()))};
    std::array<uint8_t, 256> bytes {}; OH_Huks_Blob output {bytes.size(), bytes.data()};
    auto result = OH_Huks_FinishSession(&handle, params.value, &input, &output);
    if (result.errorCode != OH_HUKS_SUCCESS || output.size > bytes.size()) {
        platformFailure = "sign_finish_" + std::to_string(result.errorCode); OH_Huks_AbortSession(&handle, params.value); return {};
    }
    return SignatureDER(bytes.data(), output.size);
}
bool VerifyPlatform(const std::vector<uint8_t>& pub, const std::string& message, const std::vector<uint8_t>& signature)
{
    std::vector<uint8_t> der(P256_SPKI_PREFIX.begin(), P256_SPKI_PREFIX.end()); der.insert(der.end(), pub.begin(), pub.end());
    Crypto_DataBlob encoded {der.data(), der.size()};
    OH_CryptoAsymKeyGenerator* generator = nullptr; OH_CryptoKeyPair* pair = nullptr; OH_CryptoVerify* verifier = nullptr;
    bool valid = OH_CryptoAsymKeyGenerator_Create("ECC256", &generator) == CRYPTO_SUCCESS &&
        OH_CryptoAsymKeyGenerator_Convert(generator, CRYPTO_DER, &encoded, nullptr, &pair) == CRYPTO_SUCCESS &&
        OH_CryptoVerify_Create("ECC|SHA256", &verifier) == CRYPTO_SUCCESS &&
        OH_CryptoVerify_Init(verifier, OH_CryptoKeyPair_GetPubKey(pair)) == CRYPTO_SUCCESS;
    Crypto_DataBlob input {reinterpret_cast<uint8_t*>(const_cast<char*>(message.data())), message.size()};
    Crypto_DataBlob sig {const_cast<uint8_t*>(signature.data()), signature.size()};
    if (valid) valid = OH_CryptoVerify_Final(verifier, &input, &sig);
    if (verifier) OH_CryptoVerify_Destroy(verifier);
    if (pair) OH_CryptoKeyPair_Destroy(pair);
    if (generator) OH_CryptoAsymKeyGenerator_Destroy(generator);
    return valid;
}
#endif

bool WriteAll(int fd, const std::string& text)
{
    size_t offset = 0;
    while (offset < text.size()) { ssize_t n = write(fd, text.data() + offset, text.size() - offset); if (n > 0) offset += n; else if (n < 0 && errno == EINTR) continue; else return false; }
    return true;
}
}

struct PairingIdentity::Impl {
    mutable std::mutex mutex;
    std::shared_ptr<PlatformKey> identity;
    std::string directory, pub, error;
    std::set<std::string> trusted, ephemeral;
    bool Save(const std::set<std::string>& peers) {
        if (!identity || directory.empty()) return false;
        std::string body = "HRDTRUST1\n" + pub + "\n";
        for (const auto& peer : peers) body += peer + "\n";
        auto signature = SignPlatform(identity, body); if (signature.empty()) return false;
        std::string data = body + Hex(signature.data(), signature.size()) + "\n";
        std::string temporary = directory + "/.trusted-peers-XXXXXX";
        std::vector<char> name(temporary.begin(), temporary.end()); name.push_back(0);
        int fd = mkstemp(name.data()); if (fd < 0) return false;
        bool ok = fchmod(fd, 0600) == 0 && WriteAll(fd, data) && fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok) ok = rename(name.data(), (directory + FILE_NAME).c_str()) == 0;
        if (!ok) unlink(name.data());
        if (ok) { int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dir >= 0) { fsync(dir); close(dir); } }
        return ok;
    }
    bool Load() {
        int fd = open((directory + FILE_NAME).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return errno == ENOENT;
        struct stat info {}; bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
            (info.st_mode & 077) == 0 && info.st_nlink == 1 && info.st_size > 0 && info.st_size <= static_cast<off_t>(MAX_STORE);
        std::string data; if (valid) { data.resize(info.st_size); size_t done = 0;
            while (done < data.size()) { ssize_t n = read(fd, data.data() + done, data.size() - done); if (n > 0) done += n; else if (n < 0 && errno == EINTR) continue; else { valid = false; break; } } }
        close(fd); if (!valid || data.empty() || data.back() != '\n') return false;
        size_t signatureAt = data.rfind('\n', data.size() - 2); if (signatureAt == std::string::npos) return false;
        std::string body = data.substr(0, signatureAt + 1), signature = data.substr(signatureAt + 1, data.size() - signatureAt - 2);
        if (!PairingIdentity::Verify(pub, body, signature)) return false;
        std::string prefix = "HRDTRUST1\n" + pub + "\n";
        if (body.compare(0, prefix.size(), prefix) != 0) return false;
        std::set<std::string> peers; size_t at = prefix.size();
        while (at < body.size()) { size_t end = body.find('\n', at); if (end == std::string::npos) return false;
            auto key = body.substr(at, end - at); if (!PairingIdentity::ValidPublicKey(key) || !peers.insert(key).second || peers.size() > MAX_PEERS) return false; at = end + 1; }
        trusted = std::move(peers); return true;
    }
};

PairingIdentity::PairingIdentity() : impl_(std::make_unique<Impl>()) {}
PairingIdentity::~PairingIdentity() = default;
bool PairingIdentity::Configure(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->identity.reset(); impl_->pub.clear(); impl_->trusted.clear(); impl_->ephemeral.clear(); impl_->directory.clear(); impl_->error.clear();
    struct stat info {};
    if (directory.empty() || directory[0] != '/' || lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0022)) {
        impl_->error = "pairing_directory_unavailable"; return false;
    }
    impl_->identity = Identity(directory); if (!impl_->identity) { impl_->error = "pairing_keystore_unavailable_" + platformFailure; return false; }
    auto pub = Export(impl_->identity); impl_->pub = Hex(pub.data(), pub.size());
    auto proof = SignPlatform(impl_->identity, "HRD identity self-test v1");
    if (!ValidPublicKey(impl_->pub) || proof.empty() || !VerifyPlatform(pub, "HRD identity self-test v1", proof)) {
        impl_->identity.reset(); impl_->pub.clear(); impl_->error = "pairing_keystore_self_test_failed_" + platformFailure; return false;
    }
    impl_->directory = directory;
    if (!impl_->Load()) { impl_->error = "pairing_allowlist_invalid"; return false; }
    return true;
}
bool PairingIdentity::Available() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->identity && !impl_->pub.empty(); }
std::string PairingIdentity::PublicKey() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->pub; }
std::string PairingIdentity::Sign(const std::string& message) const {
    std::lock_guard<std::mutex> lock(impl_->mutex); if (!impl_->identity || message.size() > MAX_STORE) return {};
    auto bytes = SignPlatform(impl_->identity, message); return Hex(bytes.data(), bytes.size());
}
bool PairingIdentity::ValidPublicKey(const std::string& publicKey) { return publicKey.size() == 130 && publicKey.compare(0, 2, "04") == 0 && Unhex(publicKey).size() == 65; }
bool PairingIdentity::Verify(const std::string& publicKey, const std::string& message, const std::string& signature) {
    if (!ValidPublicKey(publicKey) || message.size() > MAX_STORE || signature.size() < 16 || signature.size() > 144) return false;
    auto sig = Unhex(signature); if (sig.empty()) return false;
    return VerifyPlatform(Unhex(publicKey), message, sig);
}
std::string PairingIdentity::Transcript(const std::string& challenge, const std::string& clientChallenge, const std::string& hostKey, const std::string& clientKey, bool resume, bool remember) {
    return "HRDPAIR1\n" + challenge + "\n" + clientChallenge + "\n" + hostKey + "\n" + clientKey + "\n" + (resume ? "resume" : "pin") + "\n" + (remember ? "remember" : "session");
}
bool PairingIdentity::Enroll(const std::string& publicKey, bool remember) {
    std::lock_guard<std::mutex> lock(impl_->mutex); if (!impl_->identity || !ValidPublicKey(publicKey)) return false;
    if (!remember) { impl_->ephemeral.clear(); impl_->ephemeral.insert(publicKey); return true; }
    auto updated = impl_->trusted; updated.insert(publicKey); if (updated.size() > MAX_PEERS || !impl_->Save(updated)) { impl_->error = "pairing_allowlist_write_failed"; return false; }
    impl_->trusted = std::move(updated); impl_->error.clear(); return true;
}
bool PairingIdentity::Trusted(const std::string& publicKey, bool remember) const {
    std::lock_guard<std::mutex> lock(impl_->mutex); const auto& peers = remember ? impl_->trusted : impl_->ephemeral; return peers.count(publicKey) != 0;
}
size_t PairingIdentity::TrustedCount() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->trusted.size(); }
void PairingIdentity::ClearEphemeral() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->ephemeral.clear(); }
bool PairingIdentity::RevokeAll() {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->ephemeral.clear(); impl_->trusted.clear();
    bool ok = impl_->Save({}); impl_->error = ok ? "" : "pairing_revoke_write_failed"; return ok;
}
std::string PairingIdentity::Error() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }
