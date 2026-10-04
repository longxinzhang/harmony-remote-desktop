import Foundation
import CoreFoundation

struct AudioSession {
    let host: String
    let port: UInt16
    let epoch: String
    let binding: String
}
struct AudioPacket {
    enum Kind: UInt8 { case pcm = 1, reset = 2 }
    let kind: Kind
    let ptsUs: UInt64
    let sequence: UInt64
    let streamID: UInt64
    let pcm: Data
    var frames: Int { pcm.count / 4 }
}
enum AudioWireError: Error { case invalid }
// Authenticated PCM lane v1. Payload S16LE; integer headers are network byte order.
// Timestamps are source-local capture timestamps, never a one-way latency claim.
enum AudioWire {
    static let headerSize = 40, maxPayload = 3840, sampleRate = 48000, maxQueuedFrames = 5760
    static func hex(_ value: String, length: Int) -> Bool {
        value.utf8.count == length && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    static func uint(_ bytes: Data) -> UInt64 { bytes.reduce(0) { ($0 << 8) | UInt64($1) } }
    static func bind(_ session: AudioSession) throws -> Data {
        guard hex(session.epoch, length: 32), hex(session.binding, length: 64) else { throw AudioWireError.invalid }
        let body = try JSONSerialization.data(withJSONObject: ["type": "audio_bind", "version": 1,
            "epoch": session.epoch, "token": session.binding], options: [.sortedKeys])
        var prefix = UInt32(body.count).bigEndian
        var result = Data(bytes: &prefix, count: 4); result.append(body); return result
    }
    static func ready(_ body: Data) -> Bool {
        guard body.count <= 512, let object = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
              object.count == 2, object["type"] as? String == "audio_bound",
              let number = object["version"] as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              String(cString: number.objCType) != "d", number.intValue == 1 else { return false }
        return true
    }
    struct Header {
        let kind: AudioPacket.Kind
        let length: Int
        let ptsUs: UInt64
        let sequence: UInt64
        let streamID: UInt64
    }
    static func header(_ data: Data) throws -> Header {
        guard data.count == headerSize else { throw AudioWireError.invalid }
        let bytes = [UInt8](data)
        guard Array(bytes[0..<4]) == [72, 82, 68, 65], bytes[4] == 1,
              let kind = AudioPacket.Kind(rawValue: bytes[5]), bytes[6] == 2, bytes[7] == 1,
              uint(Data(bytes[12..<16])) == sampleRate else { throw AudioWireError.invalid }
        let length = Int(uint(Data(bytes[8..<12])))
        guard (kind == .pcm && length > 0 && length <= maxPayload && length % 4 == 0) ||
                (kind == .reset && length == 0) else { throw AudioWireError.invalid }
        return Header(kind: kind, length: length, ptsUs: uint(Data(bytes[16..<24])),
                      sequence: uint(Data(bytes[24..<32])), streamID: uint(Data(bytes[32..<40])))
    }
    // Explicit LE loads avoid alignment assumptions and work with Data slices.
    static func sample(_ bytes: UnsafeRawBufferPointer, offset: Int) -> Float {
        let raw = UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8)
        return Float(Int16(bitPattern: raw)) / 32768
    }
}
