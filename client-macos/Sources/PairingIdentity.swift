import Foundation
import CryptoKit
import Security

enum PairingError: LocalizedError {
    case unsupported, identityChanged, invalidProof, enrollmentNotAllowed, noLongerTrusted, storage(OSStatus)
    var errorDescription: String? {
        switch self {
        case .enrollmentNotAllowed: return "请先在鸿蒙端设置中开启“允许新增可信设备”，或取消勾选“记住这台设备”。"
        case .noLongerTrusted: return "鸿蒙端已不再信任此配对。请移除本地配对，并使用新的连接码重新配对。"
        case .unsupported: return "对端不支持可信设备配对，请升级鸿蒙端后重试。"
        case .identityChanged: return "这台设备的身份已变化。请核对鸿蒙端，移除旧配对后使用新配对码连接。"
        case .invalidProof: return "设备身份校验失败，连接已停止。"
        case .storage(let code): return "无法访问 Mac 钥匙串（\(code)）；配对未保存。"
        }
    }
}

struct PairingCredentials {
    let privateKey: P256.Signing.PrivateKey
    let hostPublicKey: String?
    let remember: Bool
    let resuming: Bool
    init(privateKey: P256.Signing.PrivateKey = P256.Signing.PrivateKey(),
         hostPublicKey: String? = nil, remember: Bool, resuming: Bool = false) {
        self.privateKey = privateKey; self.hostPublicKey = hostPublicKey
        self.remember = remember; self.resuming = resuming
    }
    var clientPublicKey: String { PairingHandshake.hex(privateKey.publicKey.x963Representation) }
    var fingerprint: String { hostPublicKey.flatMap(PairingHandshake.bytes).map { PairingHandshake.hex(Data(SHA256.hash(data: $0))).prefix(16).description } ?? "" }
}

// Public-key challenge/response authenticates a remembered peer. It does not
// encrypt the existing LAN transport; first enrollment still needs a trusted LAN.
struct PairingHandshake {
    let credentials: PairingCredentials
    let hostKey: String
    let transcript: String
    let clientChallenge: String

    init(hello: [String: Any], credentials: PairingCredentials) throws {
        guard hello["pairingScheme"] as? String == "p256-sha256-v1" else { throw PairingError.unsupported }
        guard let host = hello["hostPublicKey"] as? String, host.count == 130,
              let keyBytes = Self.bytes(host), keyBytes.first == 4,
              let challenge = hello["challenge"] as? String, challenge.count == 64,
              Self.bytes(challenge) != nil,
              let proof = hello["hostHelloSignature"] as? String else { throw PairingError.invalidProof }
        if let expected = credentials.hostPublicKey, expected != host { throw PairingError.identityChanged }
        guard Self.verify(proof, text: "HRDHELLO1\n\(challenge)\n\(host)", publicKey: host) else { throw PairingError.invalidProof }
        self.credentials = credentials; hostKey = host
        var entropy = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, entropy.count, &entropy) == errSecSuccess else { throw PairingError.invalidProof }
        clientChallenge = Self.hex(Data(entropy))
        transcript = "HRDPAIR1\n\(challenge)\n\(clientChallenge)\n\(host)\n\(credentials.clientPublicKey)\n\(credentials.resuming ? "resume" : "pin")\n\(credentials.remember ? "remember" : "session")"
    }

    func request(pin: String) throws -> [String: Any] {
        var object: [String: Any] = ["type": credentials.resuming ? "pair_resume" : "pair",
            "clientPublicKey": credentials.clientPublicKey, "clientChallenge": clientChallenge, "remember": credentials.remember,
            "clientSignature": Self.hex(try credentials.privateKey.signature(for: Data(transcript.utf8)).derRepresentation)]
        if !credentials.resuming { object["pin"] = pin }
        return object
    }

    func finish(_ reply: [String: Any], token: String) throws -> PairingCredentials {
        guard let signature = reply["hostSignature"] as? String,
              Self.verify(signature, text: transcript + "\nhost\n" + token, publicKey: hostKey) else { throw PairingError.invalidProof }
        return PairingCredentials(privateKey: credentials.privateKey, hostPublicKey: hostKey,
                                  remember: credentials.remember, resuming: true)
    }

    static func verify(_ signature: String, text: String, publicKey: String) -> Bool {
        guard signature.count <= 144, let signatureBytes = bytes(signature), let key = bytes(publicKey),
              let parsed = try? P256.Signing.ECDSASignature(derRepresentation: signatureBytes),
              let publicKey = try? P256.Signing.PublicKey(x963Representation: key) else { return false }
        return publicKey.isValidSignature(parsed, for: Data(text.utf8))
    }
    static func hex(_ bytes: Data) -> String { bytes.map { String(format: "%02x", $0) }.joined() }
    static func bytes(_ text: String) -> Data? {
        let input = Array(text.utf8)
        guard !input.isEmpty, input.count % 2 == 0, input.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { return nil }
        func nibble(_ b: UInt8) -> UInt8 { b <= 57 ? b - 48 : b - 87 }
        return Data(stride(from: 0, to: input.count, by: 2).map { nibble(input[$0]) * 16 + nibble(input[$0 + 1]) })
    }
}

struct TrustedPeer: Identifiable {
    let host: String
    let fingerprint: String
    var id: String { host }
}

final class PairingStore {
    private let service = "com.longxin.harmonyremote.paired-peer.v1"
    private struct Record: Codable { let host: String; let hostPublicKey: String; let privateKey: Data }
    private func query(_ host: String? = nil) -> [String: Any] {
        var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service]
        if let host { q[kSecAttrAccount as String] = host }
        return q
    }
    func load(host: String) throws -> PairingCredentials? {
        var q = query(host); q[kSecReturnData as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?; let status = SecItemCopyMatching(q as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw PairingError.storage(status) }
        let record = try JSONDecoder().decode(Record.self, from: data)
        guard record.host == host else { throw PairingError.invalidProof }
        return PairingCredentials(privateKey: try P256.Signing.PrivateKey(rawRepresentation: record.privateKey),
                                  hostPublicKey: record.hostPublicKey, remember: true, resuming: true)
    }
    func save(host: String, credentials: PairingCredentials) throws {
        guard credentials.remember, let hostKey = credentials.hostPublicKey else { return }
        let data = try JSONEncoder().encode(Record(host: host, hostPublicKey: hostKey, privateKey: credentials.privateKey.rawRepresentation))
        // Use the macOS login Keychain. Data Protection Keychain requires a
        // provisioned entitlement absent from the local ad-hoc development app.
        let attributes: [String: Any] = [kSecValueData as String: data]
        var status = SecItemUpdate(query(host) as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(query(host).merging(attributes) { _, new in new } as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw PairingError.storage(status) }
    }
    func forget(host: String) throws {
        let status = SecItemDelete(query(host) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw PairingError.storage(status) }
    }
    func peers() throws -> [TrustedPeer] {
        var q = query(); q[kSecReturnAttributes as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitAll
        var item: CFTypeRef?; let status = SecItemCopyMatching(q as CFDictionary, &item)
        if status == errSecItemNotFound { return [] }
        guard status == errSecSuccess else { throw PairingError.storage(status) }
        let attributes = item as? [[String: Any]] ?? []
        return try attributes.compactMap { value in
            guard let host = value[kSecAttrAccount as String] as? String, let credential = try load(host: host) else { return nil }
            return TrustedPeer(host: host, fingerprint: credential.fingerprint)
        }.sorted { $0.host < $1.host }
    }
}
