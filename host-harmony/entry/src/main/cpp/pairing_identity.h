#ifndef HARMONY_REMOTE_PAIRING_IDENTITY_H
#define HARMONY_REMOTE_PAIRING_IDENTITY_H

#include <memory>
#include <string>

// Long-term private key stays in HUKS. Disk contains only a signed public-key
// allowlist. This authenticates reconnects; the existing LAN transport is not TLS.
class PairingIdentity final {
public:
    PairingIdentity();
    ~PairingIdentity();
    PairingIdentity(const PairingIdentity&) = delete;
    PairingIdentity& operator=(const PairingIdentity&) = delete;
    bool Configure(const std::string& appFilesDirectory);
    bool Available() const;
    std::string PublicKey() const;
    std::string Sign(const std::string& message) const;
    static bool Verify(const std::string& publicKey, const std::string& message, const std::string& signature);
    static bool ValidPublicKey(const std::string& publicKey);
    static std::string Transcript(const std::string& challenge, const std::string& clientChallenge, const std::string& hostKey,
        const std::string& clientKey, bool resume, bool remember);
    // Enroll is called only after a successful PIN + fresh signed challenge.
    bool Enroll(const std::string& publicKey, bool remember);
    bool Trusted(const std::string& publicKey, bool remember) const;
    size_t TrustedCount() const;
    void ClearEphemeral();
    bool RevokeAll();
    std::string Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
