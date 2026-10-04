import Foundation
import CryptoKit
import CoreFoundation

enum ClipboardMode: Int, CaseIterable, Identifiable {
    case off = 0, toHarmony = 1, toMac = 2, both = 3
    var id: Int { rawValue }
    var sends: Bool { rawValue & 1 != 0 }
    var receives: Bool { rawValue & 2 != 0 }
    var title: String { ["关闭", "Mac → 鸿蒙", "鸿蒙 → Mac", "双向"][rawValue] }
}

struct ClipboardSession {
    let host: String
    let epoch: String
    let binding: String
    var port: UInt16 = 39873
}

// The main-thread focus owner can revoke this while a control message waits on
// the network queue. The writer rechecks immediately before handing bytes to TCP.
final class ClipboardPastePermit {
    private let lock = NSLock()
    private var cancelled = false
    private let deadline: TimeInterval
    private let now: () -> TimeInterval
    init(deadline: TimeInterval, now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.deadline = deadline; self.now = now
    }
    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    func remainingMilliseconds() -> Int? {
        lock.lock(); defer { lock.unlock() }
        let remaining = deadline - now()
        guard !cancelled, remaining > 0 else { return nil }
        return max(1, min(1500, Int(remaining * 1000)))
    }
}

struct ClipboardVersion: Equatable, Comparable {
    let counter: UInt64
    let origin: String
    static let zero = ClipboardVersion(counter: 0, origin: "none")
    var eventID: String { "\(origin)-\(counter)" }
    static func < (lhs: Self, rhs: Self) -> Bool {
        lhs.counter == rhs.counter ? lhs.origin < rhs.origin : lhs.counter < rhs.counter
    }
}

struct ClipboardFrame {
    var header: [String: Any]
    let payload: Data
    var type: String { header["type"] as? String ?? "" }
    var messageID: String { header["messageId"] as? String ?? "" }
    var version: ClipboardVersion? {
        guard let counter = ClipboardWire.counter(header["counter"]), let origin = header["originId"] as? String else { return nil }
        return ClipboardVersion(counter: counter, origin: origin)
    }
}

enum ClipboardFailure: String, Error, LocalizedError {
    case malformed = "INVALID_MESSAGE", encoding = "INVALID_TEXT", oversized = "TEXT_TOO_LARGE"
    case denied = "READ_PERMISSION_REQUIRED", unsupported = "UNSUPPORTED_CONTENT", changed = "CLIPBOARD_CHANGED"
    case write = "WRITE_FAILED", transport = "CHANNEL_CLOSED", timeout = "CLIPBOARD_TIMEOUT"
    case overflow = "COUNTER_EXHAUSTED", stale = "STALE", cancelled = "CANCELLED"
    case remoteCopyPending = "REMOTE_COPY_PENDING"
    case freshCopyRequired = "FRESH_COPY_REQUIRED"
    var errorDescription: String? { rawValue }
}

enum ClipboardWire {
    static let maxPayload = 1_048_576
    static let mime = "text/plain;charset=utf-8"
    static func identifier() -> String { UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased() }
    static func hex(_ value: Any?, length: Int) -> Bool {
        guard let text = value as? String, text.utf8.count == length else { return false }
        return text.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    static func integer(_ value: Any?, range: ClosedRange<Int>) -> Int? {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              !["d", "f"].contains(String(cString: number.objCType)),
              let result = Int(number.stringValue), range.contains(result) else { return nil }
        return result
    }
    static func counter(_ value: Any?) -> UInt64? {
        guard let text = value as? String, !text.isEmpty, text.utf8.count <= 20,
              text.utf8.allSatisfy({ (48...57).contains($0) }), text == "0" || text.first != "0" else { return nil }
        return UInt64(text)
    }
    static func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    static func text(_ data: Data) throws -> String {
        guard data.count <= maxPayload else { throw ClipboardFailure.oversized }
        guard !data.contains(0), let value = String(data: data, encoding: .utf8), Data(value.utf8) == data else {
            throw ClipboardFailure.encoding
        }
        return value
    }
    static func make(_ type: String, epoch: String, extra: [String: Any] = [:],
                     payload: Data = Data(), messageID: String = identifier()) -> ClipboardFrame {
        var header: [String: Any] = ["version": 1, "type": type, "sessionEpoch": epoch,
                                    "messageId": messageID, "payloadLength": payload.count]
        for (key, value) in extra { header[key] = value }
        return ClipboardFrame(header: header, payload: payload)
    }
    static func header(_ data: Data, epoch: String) throws -> [String: Any] {
        guard data.allSatisfy({ $0 < 128 }) else { throw ClipboardFailure.malformed }
        let value: [String: Any]
        do { value = try WireProtocol.controlObject(data) } catch { throw ClipboardFailure.malformed }
        guard value.values.allSatisfy({ item in
            guard let string = item as? String else { return true }
            return string.utf8.allSatisfy { $0 >= 32 && $0 < 127 }
        }), integer(value["version"], range: 1...1) == 1,
        hex(value["sessionEpoch"], length: 32), value["sessionEpoch"] as? String == epoch,
        hex(value["messageId"], length: 32), let length = integer(value["payloadLength"], range: 0...maxPayload),
        let type = value["type"] as? String else { throw ClipboardFailure.malformed }
        var extra: Set<String> = []
        switch type {
        case "data_bind":
            extra = ["bindToken", "purpose"]
            guard hex(value["bindToken"], length: 64), value["purpose"] as? String == "clipboard" else { throw ClipboardFailure.malformed }
        case "data_ready", "clipboard_pull": break
        case "clipboard_mode":
            extra = ["mode"]; guard integer(value["mode"], range: 0...3) != nil else { throw ClipboardFailure.malformed }
        case "clipboard_status":
            extra = ["allowed", "canRead", "canWrite", "mode", "error"]
            guard ["allowed", "canRead", "canWrite"].allSatisfy({ WireProtocol.isBoolean(value[$0], true) || WireProtocol.isBoolean(value[$0], false) }),
                  integer(value["mode"], range: 0...3) != nil, let error = value["error"] as? String,
                  error.utf8.count <= 128 else { throw ClipboardFailure.malformed }
        case "clipboard_update":
            extra = ["originId", "counter", "eventId", "mime", "sha256"]
            guard let count = counter(value["counter"]), count > 0,
                  let origin = value["originId"] as? String, ["mac", "harmony"].contains(origin),
                  value["eventId"] as? String == "\(origin)-\(count)", value["mime"] as? String == mime,
                  hex(value["sha256"], length: 64) else { throw ClipboardFailure.malformed }
        case "clipboard_applied":
            extra = ["eventId", "status", "counter", "originId", "error"]
            guard let count = counter(value["counter"]), let origin = value["originId"] as? String,
                  count == 0 ? origin == "none" : ["mac", "harmony"].contains(origin),
                  let event = value["eventId"] as? String, validEventID(event),
                  let status = value["status"] as? String, ["applied", "stale", "denied", "failed"].contains(status),
                  let error = value["error"] as? String, error.utf8.count <= 128 else { throw ClipboardFailure.malformed }
        default: throw ClipboardFailure.malformed
        }
        guard Set(value.keys) == Set(["version", "type", "sessionEpoch", "messageId", "payloadLength"]).union(extra),
              type == "clipboard_update" || length == 0 else { throw ClipboardFailure.malformed }
        return value
    }
    static func validEventID(_ text: String) -> Bool {
        let parts = text.split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count == 2, ["mac", "harmony"].contains(String(parts[0])),
              let value = counter(String(parts[1])), value > 0 else { return false }
        return true
    }
    static func validate(_ frame: ClipboardFrame, epoch: String) throws {
        let body = try JSONSerialization.data(withJSONObject: frame.header, options: [.sortedKeys])
        let parsed = try header(body, epoch: epoch)
        guard integer(parsed["payloadLength"], range: 0...maxPayload) == frame.payload.count else { throw ClipboardFailure.malformed }
        if frame.type == "clipboard_update" {
            _ = try text(frame.payload)
            guard parsed["sha256"] as? String == digest(frame.payload) else { throw ClipboardFailure.malformed }
        }
    }
    static func encode(_ frame: ClipboardFrame, epoch: String) throws -> Data {
        try validate(frame, epoch: epoch)
        var result = try WireProtocol.controlFrame(frame.header)
        result.append(frame.payload)
        return result
    }
}
