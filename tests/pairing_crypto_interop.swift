import Foundation
import CryptoKit

@main
struct PairingCryptoInterop {
    static func run(_ executable: String, _ arguments: [String]) throws -> (Int32, String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let output = Pipe()
        process.standardOutput = output
        process.standardError = FileHandle.standardError
        try process.run()
        let bytes = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: bytes, as: UTF8.self))
    }
    static func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }
    static func bytes(_ text: String) -> Data {
        precondition(text.count % 2 == 0)
        return Data(stride(from: 0, to: text.count, by: 2).map { offset in
            let start = text.index(text.startIndex, offsetBy: offset)
            return UInt8(text[start..<text.index(start, offsetBy: 2)], radix: 16)!
        })
    }
    static func main() throws {
        guard let executable = ProcessInfo.processInfo.environment["LAN_SERVER_FIXTURE"] else { fatalError("LAN_SERVER_FIXTURE missing") }
        let transcript = "HRDPAIR1\n" + String(repeating: "a", count: 64) + "\n" + String(repeating: "b", count: 64) + "\nhost-public-key\nclient-public-key\nresume\nremember"
        let hostResult = try run(executable, ["--sign-proof", transcript])
        precondition(hostResult.0 == 0)
        let fields = hostResult.1.split(separator: "\n").map(String.init)
        precondition(fields.count == 2)
        let hostKey = try P256.Signing.PublicKey(x963Representation: bytes(fields[0]))
        let hostSignature = try P256.Signing.ECDSASignature(derRepresentation: bytes(fields[1]))
        precondition(hostKey.isValidSignature(hostSignature, for: Data(transcript.utf8)))
        precondition(!hostKey.isValidSignature(hostSignature, for: Data((transcript + "tampered").utf8)))
        let clientKey = P256.Signing.PrivateKey()
        let clientSignature = try clientKey.signature(for: Data(transcript.utf8))
        let verify = try run(executable, ["--verify-proof", hex(clientKey.publicKey.x963Representation), transcript, hex(clientSignature.derRepresentation)])
        precondition(verify.0 == 0)
        let altered = try run(executable, ["--verify-proof", hex(clientKey.publicKey.x963Representation), transcript + "tampered", hex(clientSignature.derRepresentation)])
        precondition(altered.0 == 2)
        let otherKey = P256.Signing.PrivateKey()
        let swapped = try run(executable, ["--verify-proof", hex(otherKey.publicKey.x963Representation), transcript, hex(clientSignature.derRepresentation)])
        precondition(swapped.0 == 2)
        print("Pairing crypto interop: 5/5 passed (native Security.framework and Swift CryptoKit; HUKS device verification pending)")
    }
}
