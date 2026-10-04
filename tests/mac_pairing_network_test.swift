import Foundation
import CryptoKit

private struct Failure: Error, CustomStringConvertible { let description: String }
private func check(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() { throw Failure(description: message) }
}

private final class Fixture {
    let process = Process()
    let output = Pipe()
    let controlPort: UInt16
    let videoPort: UInt16
    let pin: String

    init(_ mode: String) throws {
        guard let executable = ProcessInfo.processInfo.environment["LAN_SERVER_FIXTURE"] else { throw Failure(description: "fixture path missing") }
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = [mode]
        process.standardOutput = output
        process.standardError = FileHandle.standardError
        try process.run()
        let object = try Self.line(output.fileHandleForReading)
        guard let control = object["controlPort"] as? Int, let video = object["videoPort"] as? Int,
              let code = object["pin"] as? String else { throw Failure(description: "fixture did not start") }
        controlPort = UInt16(control); videoPort = UInt16(video); pin = code
    }
    static func line(_ file: FileHandle) throws -> [String: Any] {
        var line = Data()
        while line.count < 4096 {
            let byte = file.readData(ofLength: 1)
            guard !byte.isEmpty else { throw Failure(description: "fixture closed before result") }
            if byte.first == 10 { break }
            line.append(byte)
        }
        guard let result = try JSONSerialization.jsonObject(with: line) as? [String: Any] else { throw Failure(description: "invalid fixture result") }
        return result
    }
    func line() throws -> [String: Any] { try Self.line(output.fileHandleForReading) }
    func finish() throws { process.waitUntilExit(); try check(process.terminationStatus == 0, "native fixture assertion failed") }
    deinit { if process.isRunning { process.terminate(); process.waitUntilExit() } }
}

private final class Probe {
    let lock = NSLock()
    let paired = DispatchSemaphore(value: 0)
    let ended = DispatchSemaphore(value: 0)
    private var credential: PairingCredentials?
    private var result: Result<Void, Error>?
    private var packets: [VideoPacket] = []
    func connection(_ fixture: Fixture) -> LANConnection {
        LANConnection(testControlPort: fixture.controlPort, testVideoPort: fixture.videoPort,
            onStatus: { _ in }, onPacket: { [self] packet in
                lock.lock(); packets.append(packet); lock.unlock()
            }, onEnd: { [self] value in
                lock.lock(); result = value; lock.unlock(); ended.signal()
            }, onPairedIdentity: { [self] value in
                lock.lock(); credential = value; lock.unlock(); paired.signal()
            })
    }
    func values() -> (PairingCredentials?, Result<Void, Error>?, [VideoPacket]) {
        lock.lock(); defer { lock.unlock() }; return (credential, result, packets)
    }
    func waitPaired() throws { try check(paired.wait(timeout: .now() + 8) == .success, "modern handshake did not complete") }
    func waitEnd() throws { try check(ended.wait(timeout: .now() + 12) == .success, "connection did not terminate") }
}

@main
struct MacPairingNetworkTests {
    static func main() throws {
        do {
            let fixture = try Fixture("--serve-pairing-reconnect")
            let initial = Probe(), first = initial.connection(fixture)
            first.connect(host: "127.0.0.1", pin: fixture.pin, pairing: PairingCredentials(remember: true))
            try initial.waitPaired(); try initial.waitEnd()
            let (credential, result, packets) = initial.values()
            try check(credential?.remember == true && credential?.resuming == true && credential?.hostPublicKey != nil, "verified persistent identity missing")
            if case .failure(let error)? = result { try check(error is LANError, "transport failure was not reported") }
            else { throw Failure(description: "video interruption was treated as clean EOS") }
            try check(packets.map(\.sequence) == [0, 1] && packets.map(\.type) == [1, 2], "first stream packet ordering changed")
            try check(first.snapshot.rttSamples >= 1 && first.snapshot.rttMilliseconds != nil, "production RTT ping/pong produced no measurement")
            let ready = try fixture.line()
            try check(ready["readyForReconnect"] as? Bool == true, "native Host did not preserve authorized capture")

            let resumed = Probe(), second = resumed.connection(fixture)
            second.connect(host: "127.0.0.1", pin: "", pairing: credential)
            try resumed.waitPaired(); try resumed.waitEnd()
            let (nextCredential, nextResult, nextPackets) = resumed.values()
            if case .success? = nextResult {} else { throw Failure(description: "authenticated resume failed") }
            try check(nextCredential?.hostPublicKey == credential?.hostPublicKey, "resumed host identity changed")
            try check(nextPackets.map(\.sequence) == [0, 1, 2] && nextPackets.map(\.type) == [1, 2, 2], "new stream did not restart parser sequence")
            try check(nextPackets[1].flags == 1 && nextPackets[1].ptsUs == 100000 && nextPackets[2].flags == 2, "fresh IDR and EOS did not reach resumed viewer")
            try check(!second.snapshot.inputEnabled, "reconnect restored remote input automatically")
            let completed = try fixture.line()
            try check(completed["reconnected"] as? Bool == true && completed["trustedDeviceCount"] as? Int == 1, "native persistent pairing was not retained")
            try fixture.finish()
            print("PASS real Swift/C++ persistent enrollment, authenticated resume, fresh CONFIG/IDR, RTT and input-off boundary")
        }
        do {
            let fixture = try Fixture("--serve-pairing")
            let probe = Probe(), connection = probe.connection(fixture)
            connection.connect(host: "127.0.0.1", pin: fixture.pin, pairing: PairingCredentials(remember: false))
            try probe.waitPaired()
            let deadline = ProcessInfo.processInfo.systemUptime + 5
            while !connection.snapshot.videoReady && ProcessInfo.processInfo.systemUptime < deadline { Thread.sleep(forTimeInterval: 0.01) }
            try check(connection.snapshot.videoReady, "session-only video binding did not finish")
            connection.disconnect(); try probe.waitEnd()
            try check(probe.values().0?.remember == false && probe.values().0?.resuming == true, "session-only reconnect identity missing")
            try fixture.finish()
            print("PASS session-only pairing and explicit disconnect through production LANConnection")
        }
        do {
            let fixture = try Fixture("--serve-pairing")
            let probe = Probe(), connection = probe.connection(fixture)
            let wrongHost = PairingHandshake.hex(P256.Signing.PrivateKey().publicKey.x963Representation)
            connection.connect(host: "127.0.0.1", pin: "", pairing: PairingCredentials(hostPublicKey: wrongHost, remember: true, resuming: true))
            try probe.waitEnd()
            guard case .failure(let error)? = probe.values().1, case PairingError.identityChanged = error else { throw Failure(description: "remembered host mismatch did not stop authentication") }
            try check(probe.values().0 == nil && probe.values().2.isEmpty, "identity mismatch exposed a stream or trusted credential")
            print("PASS remembered Host identity mismatch fails closed without PIN downgrade")
        }
        print("Modern LAN integration: 3/3 scenarios passed (real localhost C++/Swift sockets; no device or user Keychain)")
    }
}
