import Foundation
import CryptoKit

private struct SessionTestFailure: Error, CustomStringConvertible {
    let description: String
}

private struct SessionChecks {
    var count = 0
    mutating func check(_ condition: @autoclosure () -> Bool, _ title: String) throws {
        guard condition() else { throw SessionTestFailure(description: title) }
        count += 1
        print("PASS: \(title)")
    }
    mutating func rejects(_ title: String, _ action: () throws -> Void) throws {
        var error: Error?
        do { try action() } catch let caught { error = caught }
        try check(error != nil, title)
    }
}

private func close(_ lhs: Double?, _ rhs: Double, tolerance: Double = 0.00001) -> Bool {
    guard let lhs else { return false }
    return abs(lhs - rhs) <= tolerance
}

private func sign(_ text: String, with key: P256.Signing.PrivateKey) throws -> String {
    PairingHandshake.hex(try key.signature(for: Data(text.utf8)).derRepresentation)
}

private func hello(_ key: P256.Signing.PrivateKey, challenge: String) throws -> [String: Any] {
    let host = PairingHandshake.hex(key.publicKey.x963Representation)
    return ["pairingScheme": "p256-sha256-v1", "hostPublicKey": host, "challenge": challenge,
            "hostHelloSignature": try sign("HRDHELLO1\n\(challenge)\n\(host)", with: key)]
}

private func pairingChecks(_ checks: inout SessionChecks) throws {
    // All keys are ephemeral CryptoKit values. This test never instantiates
    // PairingStore or invokes SecItem functions on the user's Keychain.
    let host = P256.Signing.PrivateKey(), otherHost = P256.Signing.PrivateKey()
    let client = P256.Signing.PrivateKey()
    let hostKey = PairingHandshake.hex(host.publicKey.x963Representation)
    let challenge = String(repeating: "34", count: 32)
    let token = String(repeating: "56", count: 32)
    let announcement = try hello(host, challenge: challenge)

    for remember in [false, true] {
        for resuming in [false, true] {
            let credential = PairingCredentials(privateKey: client,
                hostPublicKey: resuming ? hostKey : nil, remember: remember, resuming: resuming)
            let exchange = try PairingHandshake(hello: announcement, credentials: credential)
            let request = try exchange.request(pin: "123456")
            guard let nonce = request["clientChallenge"] as? String,
                  let signature = request["clientSignature"] as? String,
                  let signatureBytes = PairingHandshake.bytes(signature) else {
                throw SessionTestFailure(description: "missing client freshness/proof in request")
            }
            let expected = "HRDPAIR1\n\(challenge)\n\(nonce)\n\(hostKey)\n\(credential.clientPublicKey)\n\(resuming ? "resume" : "pin")\n\(remember ? "remember" : "session")"
            let parsed = try P256.Signing.ECDSASignature(derRepresentation: signatureBytes)
            try checks.check(nonce.count == 64 && PairingHandshake.bytes(nonce)?.count == 32,
                             "client challenge is a 256-bit wire value (remember=\(remember), resume=\(resuming))")
            try checks.check(exchange.transcript == expected && client.publicKey.isValidSignature(parsed, for: Data(expected.utf8)),
                             "CryptoKit verifies complete peer/challenge/mode transcript (remember=\(remember), resume=\(resuming))")
            try checks.check(request["type"] as? String == (resuming ? "pair_resume" : "pair") &&
                             request["remember"] as? Bool == remember &&
                             (resuming ? request["pin"] == nil : request["pin"] as? String == "123456"),
                             "PIN and resume requests preserve remember mode without sending PIN on resume")
            let reply: [String: Any] = ["hostSignature": try sign(expected + "\nhost\n" + token, with: host)]
            let accepted = try exchange.finish(reply, token: token)
            try checks.check(accepted.resuming && accepted.remember == remember && accepted.hostPublicKey == hostKey &&
                             accepted.privateKey.rawRepresentation == client.rawRepresentation && accepted.fingerprint.count == 16,
                             "authenticated finish preserves client identity and session/persistent preference")
            let alteredMode = expected.replacingOccurrences(of: resuming ? "\nresume\n" : "\npin\n",
                                                           with: resuming ? "\npin\n" : "\nresume\n")
            try checks.check(!client.publicKey.isValidSignature(parsed, for: Data(alteredMode.utf8)),
                             "request signature cannot be moved between PIN and resume modes")
            let alteredRemember = String(expected.dropLast(remember ? 8 : 7)) + (remember ? "session" : "remember")
            try checks.check(!client.publicKey.isValidSignature(parsed, for: Data(alteredRemember.utf8)),
                             "request signature rejects a changed persistence choice")
        }
    }

    let credential = PairingCredentials(privateKey: client, hostPublicKey: hostKey, remember: true, resuming: true)
    let first = try PairingHandshake(hello: announcement, credentials: credential)
    let second = try PairingHandshake(hello: announcement, credentials: credential)
    let oldReply: [String: Any] = ["hostSignature": try sign(first.transcript + "\nhost\n" + token, with: host)]
    try checks.check(first.clientChallenge != second.clientChallenge && first.transcript != second.transcript,
                     "a replayed signed host hello still gets a fresh client challenge")
    try checks.rejects("old host finish cannot authenticate a new connection with the same identity") {
        _ = try second.finish(oldReply, token: token)
    }
    try checks.rejects("host proof binds the returned session token") {
        _ = try first.finish(oldReply, token: String(repeating: "78", count: 32))
    }
    try checks.rejects("unrelated private key cannot forge host finish") {
        _ = try first.finish(["hostSignature": sign(first.transcript + "\nhost\n" + token, with: otherHost)], token: token)
    }
    try checks.rejects("missing host finish signature is rejected") { _ = try first.finish([:], token: token) }
    try checks.rejects("oversized host signature is rejected before decoding") {
        _ = try first.finish(["hostSignature": String(repeating: "00", count: 73)], token: token)
    }
    try checks.rejects("remembered host identity cannot silently change") {
        _ = try PairingHandshake(hello: hello(otherHost, challenge: challenge), credentials: credential)
    }
    var changed = announcement
    changed["challenge"] = String(repeating: "91", count: 32)
    try checks.rejects("altered server challenge invalidates signed hello") {
        _ = try PairingHandshake(hello: changed, credentials: credential)
    }
    changed = announcement
    changed["hostHelloSignature"] = try sign("HRDHELLO1\n\(challenge)\n\(hostKey)", with: otherHost)
    try checks.rejects("hello signed by another key is rejected") {
        _ = try PairingHandshake(hello: changed, credentials: credential)
    }
    for (field, value) in [("pairingScheme", "unknown"), ("hostPublicKey", String(repeating: "00", count: 65)),
                           ("challenge", "34"), ("challenge", String(repeating: "GG", count: 32)),
                           ("hostHelloSignature", "0000")] {
        var malformed = announcement
        malformed[field] = value
        try checks.rejects("invalid \(field) is rejected") {
            _ = try PairingHandshake(hello: malformed, credentials: credential)
        }
    }
    try checks.check(PairingHandshake.bytes("00a1ff") == Data([0, 161, 255]), "canonical lowercase hex round-trips")
    for malformed in ["", "0", "abc", "AA", "0g", " 00", "00\n", "é0", "０0", "-1", "00\0"] {
        try checks.check(PairingHandshake.bytes(malformed) == nil, "malformed hex is rejected (\(malformed.utf8.count) bytes)")
    }
    try checks.check(!PairingHandshake.verify("00", text: "test", publicKey: hostKey), "invalid DER signature is rejected")
    let validTestSignature = try sign("test", with: host)
    try checks.check(!PairingHandshake.verify(validTestSignature, text: "test",
                                             publicKey: "04" + String(repeating: "00", count: 64)),
                     "invalid curve point cannot authenticate a signature")
}

private func reconnectChecks(_ checks: inout SessionChecks) throws {
    var policy = ReconnectPolicy()
    let retries: [LANError] = [.connectionFailed, .connectionClosed, .heartbeatTimeout, .deadline]
    for (i, expected) in [1.0, 2, 4, 8, 15, 30, 30, 30].enumerated() {
        try checks.check(policy.nextDelay(for: retries[i % retries.count], enabled: true, hasIdentity: true) == expected,
                         "reconnect backoff attempt \(i + 1) is \(Int(expected)) seconds")
    }
    let before = policy.attempts
    let permanent: [Error] = [LANError.invalidHost, LANError.invalidPIN, LANError.cancelled, LANError.invalidSession,
        LANError.hostRejected, LANError.protocolViolation("bad protocol"), PairingError.identityChanged,
        PairingError.invalidProof, PairingError.unsupported, NSError(domain: "other", code: 1)]
    for error in permanent {
        try checks.check(policy.nextDelay(for: error, enabled: true, hasIdentity: true) == nil,
                         "manual/authentication/protocol/nontransport failure does not retry: \(error)")
    }
    try checks.check(policy.attempts == before, "nonretryable failures do not advance backoff")
    try checks.check(policy.nextDelay(for: LANError.connectionClosed, enabled: false, hasIdentity: true) == nil,
                     "user-disabled reconnect cancels future retries")
    try checks.check(policy.nextDelay(for: LANError.connectionClosed, enabled: true, hasIdentity: false) == nil,
                     "no reconnect without an authenticated peer identity")
    policy.reset()
    try checks.check(policy.attempts == 0 && policy.nextDelay(for: LANError.deadline, enabled: true, hasIdentity: true) == 1,
                     "successful/manual reset starts a new backoff sequence")
}

private func measurementChecks(_ checks: inout SessionChecks) throws {
    var measured = NetworkMeasurements()
    measured.receivedPong("unknown", at: 1)
    try checks.check(measured.rttSamples == 0 && measured.rttMilliseconds == nil && measured.jitterMilliseconds == nil,
                     "unsolicited pong never fabricates RTT")
    measured.sentProbe("one", at: 1)
    measured.receivedPong("one", at: 1.025)
    try checks.check(measured.rttSamples == 1 && close(measured.rttMilliseconds, 25), "matched probe measures monotonic RTT")
    measured.receivedPong("one", at: 1.050)
    try checks.check(measured.rttSamples == 1 && close(measured.rttMilliseconds, 25), "duplicate pong cannot add a latency sample")
    measured.sentProbe("two", at: 2)
    measured.receivedPong("two", at: 2.045)
    try checks.check(close(measured.jitterMilliseconds, 20), "jitter uses successive valid RTT differences")
    measured.sentProbe("expired", at: 3)
    measured.receivedPong("expired", at: 9.001)
    measured.sentProbe("clock", at: 10)
    measured.receivedPong("clock", at: 9)
    try checks.check(measured.rttSamples == 2, "expired and backwards-clock pongs are ignored")

    var bounded = NetworkMeasurements()
    for i in 0..<5 { bounded.sentProbe("probe-\(i)", at: 0) }
    bounded.receivedPong("probe-4", at: 0.01)
    try checks.check(bounded.rttSamples == 0, "at most four outstanding probes are tracked")
    for i in 0..<4 { bounded.receivedPong("probe-\(i)", at: 0.01) }
    try checks.check(bounded.rttSamples == 4, "retained bounded probes remain matchable")
    bounded.sentProbe("old", at: 1)
    bounded.sentProbe("fresh", at: 8)
    bounded.receivedPong("old", at: 8.001)
    bounded.receivedPong("fresh", at: 8.01)
    try checks.check(bounded.rttSamples == 5, "sending a fresh probe evicts expired outstanding probes")

    var window = NetworkMeasurements()
    window.sentProbe("spike", at: 0)
    window.receivedPong("spike", at: 1)
    for i in 1...31 {
        window.sentProbe("sample-\(i)", at: Double(i * 2))
        window.receivedPong("sample-\(i)", at: Double(i * 2) + 0.01)
    }
    try checks.check(window.rttSamples == 32 && close(window.jitterMilliseconds, 0),
                     "jitter window evicts samples older than the latest thirty RTTs")

    var rates = NetworkMeasurements()
    rates.sample(bytes: 100, frames: 3, at: 0)
    rates.sample(bytes: 250100, frames: 18, at: 0.5)
    try checks.check(rates.videoMbps == 0 && rates.receivedFPS == 0, "rates wait for a full measurement interval")
    rates.sample(bytes: 1_000_100, frames: 63, at: 2)
    try checks.check(close(rates.videoMbps, 4) && close(rates.receivedFPS, 30), "video bytes and frames produce Mbps/FPS over elapsed time")
    rates.sample(bytes: 2_000_100, frames: 123, at: 3)
    try checks.check(close(rates.videoMbps, 8) && close(rates.receivedFPS, 60), "rates adapt from thirty to sixty received frames per second")
    rates.sample(bytes: 100, frames: 2, at: 4)
    try checks.check(rates.videoMbps == 0 && rates.receivedFPS == 0, "counter reset never reports negative network rates")
    rates.sample(bytes: 125100, frames: 32, at: 5)
    try checks.check(close(rates.videoMbps, 1) && close(rates.receivedFPS, 30), "measurement baseline recovers after counter reset")
    rates.sample(bytes: 250100, frames: 62, at: 4)
    try checks.check(close(rates.videoMbps, 1) && close(rates.receivedFPS, 30), "backwards timestamp cannot overwrite a valid rate")
}

@main
struct SessionTests {
    static func main() throws {
        var checks = SessionChecks()
        try pairingChecks(&checks)
        try reconnectChecks(&checks)
        try measurementChecks(&checks)
        print("\(checks.count) session checks passed; generated in-memory keys only, no network, Keychain or login-item mutation.")
    }
}
