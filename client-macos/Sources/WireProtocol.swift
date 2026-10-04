import Foundation
import CoreFoundation

struct VideoPacket {
    let type: UInt8
    let flags: UInt16
    let sequence: UInt32
    let ptsUs: UInt64
    let payload: Data
}

enum LANError: Error, LocalizedError {
    case invalidHost, invalidPIN, cancelled, connectionFailed, connectionClosed
    case deadline, heartbeatTimeout, invalidSession, hostRejected
    case protocolViolation(String)

    var errorDescription: String? {
        switch self {
        case .invalidHost: return "请输入本机可信局域网中的 RFC1918 IPv4 地址。"
        case .invalidPIN: return "PIN 必须是 6 位数字。"
        case .cancelled: return "连接已取消。"
        case .connectionFailed: return "无法连接到 Host。"
        case .connectionClosed: return "收到完整 EOS 之前连接已断开。"
        case .deadline: return "连接或消息接收超时。"
        case .heartbeatTimeout: return "Host 心跳超时。"
        case .invalidSession: return "Host 返回了无效会话。"
        case .hostRejected: return "Host 拒绝或终止了本次连接。"
        case .protocolViolation(let reason): return "协议错误：\(reason)"
        }
    }
}

enum WireProtocol {
    static let headerSize = 24
    static let maxControl = 4096
    static let maxConfig = 256 * 1024
    static let maxPayload = 8 * 1024 * 1024
    static let maxTotal = 64 * 1024 * 1024

    static func addCount(_ value: Int, _ amount: Int) throws -> Int {
        let (result, overflow) = value.addingReportingOverflow(amount)
        guard value >= 0, amount >= 0, !overflow else {
            throw LANError.protocolViolation("累计计数超过整数范围")
        }
        return result
    }

    static func validHost(_ text: String) -> Bool {
        guard let bytes = ipv4(text) else { return false }
        return bytes[0] == 10 || (bytes[0] == 172 && (16...31).contains(bytes[1])) ||
            (bytes[0] == 192 && bytes[1] == 168)
    }

    private static func ipv4(_ text: String) -> [UInt8]? {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        var result: [UInt8] = []
        for part in parts {
            guard !part.isEmpty, part.utf8.allSatisfy({ (48...57).contains($0) }),
                  (part.count == 1 || part.first != "0"), let byte = UInt8(part) else { return nil }
            result.append(byte)
        }
        return result
    }

    static func validPIN(_ text: String) -> Bool {
        text.utf8.count == 6 && text.utf8.allSatisfy { (48...57).contains($0) }
    }

    static func validToken(_ text: String) -> Bool {
        text.utf8.count == 64 && text.utf8.allSatisfy {
            (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0)
        }
    }

    static func uint(_ bytes: Data) -> UInt64 {
        bytes.reduce(0) { ($0 << 8) | UInt64($1) }
    }

    static func controlFrame(_ object: [String: Any]) throws -> Data {
        let body = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        guard !body.isEmpty, body.count <= maxControl else { throw LANError.protocolViolation("控制消息长度无效") }
        var length = UInt32(body.count).bigEndian
        var result = Data(bytes: &length, count: 4)
        result.append(body)
        return result
    }

    static func controlObject(_ body: Data) throws -> [String: Any] {
        guard !body.isEmpty, body.count <= maxControl, String(data: body, encoding: .utf8) != nil else {
            throw LANError.protocolViolation("控制消息长度或 UTF-8 无效")
        }
        do {
            guard let object = try JSONSerialization.jsonObject(with: body) as? [String: Any],
                  object["type"] is String else { throw LANError.protocolViolation("控制消息缺少类型") }
            try validateFlatJSONKeys(body)
            return object
        } catch let error as LANError { throw error }
        catch { throw LANError.protocolViolation("控制 JSON 无效") }
    }

    static func isIntegerOne(_ value: Any?) -> Bool {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() else { return false }
        let encoding = String(cString: number.objCType)
        return encoding != "d" && encoding != "f" && number.int64Value == 1
    }

    static func isBoolean(_ value: Any?, _ expected: Bool) -> Bool {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return false }
        return number.boolValue == expected
    }

    // Control v1 has a flat object of scalar fields. Foundation alone accepts duplicate
    // keys, so inspect original key spellings (including escapes) before trusting it.
    private static func validateFlatJSONKeys(_ body: Data) throws {
        let bytes = [UInt8](body)
        var cursor = 0
        var keys = Set<String>()
        func skipSpace() { while cursor < bytes.count && [9, 10, 13, 32].contains(bytes[cursor]) { cursor += 1 } }
        skipSpace()
        guard cursor < bytes.count, bytes[cursor] == 123 else { throw LANError.protocolViolation("控制 JSON 必须是对象") }
        cursor += 1
        while cursor < bytes.count {
            skipSpace()
            if bytes[cursor] == 125 { return }
            guard bytes[cursor] == 34 else { throw LANError.protocolViolation("控制字段无效") }
            let start = cursor
            cursor += 1
            while cursor < bytes.count {
                if bytes[cursor] == 92 { cursor += 2; continue }
                if bytes[cursor] == 34 { cursor += 1; break }
                cursor += 1
            }
            guard cursor <= bytes.count,
                  let key = try JSONSerialization.jsonObject(with: Data(bytes[start..<cursor]), options: [.fragmentsAllowed]) as? String,
                  keys.insert(key).inserted else { throw LANError.protocolViolation("重复控制字段") }
            skipSpace()
            guard cursor < bytes.count, bytes[cursor] == 58 else { throw LANError.protocolViolation("控制字段缺少值") }
            cursor += 1
            var quoted = false
            while cursor < bytes.count {
                let byte = bytes[cursor]
                if quoted && byte == 92 { cursor += 2; continue }
                if byte == 34 { quoted.toggle() }
                if !quoted {
                    if byte == 91 || byte == 123 { throw LANError.protocolViolation("不支持嵌套控制字段") }
                    if byte == 125 { return }
                    if byte == 44 { cursor += 1; break }
                }
                cursor += 1
            }
        }
    }

    static func nalTypes(_ payload: Data) throws -> Set<UInt8> {
        try payload.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
            var result = Set<UInt8>()
            var index = 0
            var previousHeader: Int?
            var foundStart = false
            while index + 2 < bytes.count {
                var prefix = 0
                if bytes[index] == 0 && bytes[index + 1] == 0 {
                    if bytes[index + 2] == 1 { prefix = 3 }
                    else if index + 3 < bytes.count && bytes[index + 2] == 0 && bytes[index + 3] == 1 { prefix = 4 }
                }
                if prefix == 0 {
                    if !foundStart && bytes[index] != 0 { throw LANError.protocolViolation("缺少 Annex-B 起始码") }
                    index += 1
                    continue
                }
                if let previousHeader, index <= previousHeader { throw LANError.protocolViolation("空 NAL") }
                let headerIndex = index + prefix
                guard headerIndex < bytes.count else { throw LANError.protocolViolation("尾部空 NAL") }
                let header = bytes[headerIndex]
                let kind = header & 31
                guard header & 128 == 0, (1...23).contains(kind) else { throw LANError.protocolViolation("NAL header 无效") }
                result.insert(kind)
                previousHeader = headerIndex
                foundStart = true
                index = headerIndex
            }
            guard !result.isEmpty else { throw LANError.protocolViolation("缺少 Annex-B NAL") }
            return result
        }
    }
}

final class WireVideoParser {
    private let maxTotalBytes: Int?
    private var header = Data()
    private var payload = Data()
    private var packetHeader: (UInt8, UInt16, UInt32, UInt64, Int)?
    private var nextSequence: UInt32 = 0
    private var needIDR = true
    private var hasConfig = false
    private var frames = 0
    private var lastPTS: UInt64?
    private var totalPayload = 0
    private var failed = false
    private(set) var receivedEOS = false
    private(set) var assemblyStartedAt: TimeInterval?

    init(maxTotalBytes: Int? = WireProtocol.maxTotal) {
        // Replay defaults to 64 MiB. Live explicitly passes nil: traffic counters
        // do not retain payload; single-packet and queue bounds still apply.
        self.maxTotalBytes = maxTotalBytes.map { max(0, $0) }
    }

    #if HRD_NETWORK_TESTING
    func seedCountersForTesting(bytes: Int, frames: Int) {
        precondition(bytes >= 0 && frames >= 0 && header.isEmpty && packetHeader == nil)
        totalPayload = bytes; self.frames = frames
    }
    var testCounters: (bytes: Int, frames: Int, bufferedBytes: Int) {
        (totalPayload, frames, header.count + payload.count)
    }
    #endif

    func feed(_ data: Data) throws -> [VideoPacket] {
        guard !failed else { throw LANError.protocolViolation("解析器已终止") }
        do { return try consume(data) }
        catch { failed = true; throw error }
    }

    private func consume(_ data: Data) throws -> [VideoPacket] {
        var cursor = data.startIndex
        var packets: [VideoPacket] = []
        while cursor < data.endIndex {
            guard !receivedEOS else { throw LANError.protocolViolation("EOS 后存在额外数据") }
            if assemblyStartedAt == nil { assemblyStartedAt = ProcessInfo.processInfo.systemUptime }
            if packetHeader == nil {
                let count = min(WireProtocol.headerSize - header.count, data.endIndex - cursor)
                header.append(contentsOf: data[cursor..<(cursor + count)])
                cursor += count
                if header.count < WireProtocol.headerSize { continue }
                let bytes = [UInt8](header)
                guard Array(bytes[0..<4]) == [72, 82, 68, 49], bytes[4] == 1 else {
                    throw LANError.protocolViolation("视频 magic/version 不匹配")
                }
                let type = bytes[5]
                let flags = UInt16(WireProtocol.uint(header.subdata(in: 6..<8)))
                let sequence = UInt32(WireProtocol.uint(header.subdata(in: 8..<12)))
                let pts = WireProtocol.uint(header.subdata(in: 12..<20))
                let size = Int(WireProtocol.uint(header.subdata(in: 20..<24)))
                guard (1...3).contains(type), flags & ~UInt16(3) == 0, sequence == nextSequence,
                      size <= WireProtocol.maxPayload, size <= Int.max - totalPayload else {
                    throw LANError.protocolViolation("视频类型、序号、flags 或长度无效")
                }
                if let maxTotalBytes, size > maxTotalBytes - totalPayload {
                    throw LANError.protocolViolation("视频累计长度超限")
                }
                guard !(type == 1 && (flags != 0 || size == 0 || size > WireProtocol.maxConfig)),
                      !(type == 3 && (flags != 0 || size != 0)),
                      !(type == 2 && size == 0 && flags != 2) else {
                    throw LANError.protocolViolation("视频包内容与类型不匹配")
                }
                packetHeader = (type, flags, sequence, pts, size)
                payload.reserveCapacity(size)
            }
            guard let (type, flags, sequence, pts, size) = packetHeader else { continue }
            let count = min(size - payload.count, data.endIndex - cursor)
            if count > 0 { payload.append(contentsOf: data[cursor..<(cursor + count)]); cursor += count }
            if payload.count < size { continue }
            let packet = VideoPacket(type: type, flags: flags, sequence: sequence, ptsUs: pts, payload: payload)
            try validate(packet)
            totalPayload = try WireProtocol.addCount(totalPayload, size)
            nextSequence &+= 1
            packets.append(packet)
            header.removeAll(keepingCapacity: true)
            payload = Data()
            packetHeader = nil
            assemblyStartedAt = nil
        }
        return packets
    }

    private func validate(_ packet: VideoPacket) throws {
        if packet.type == 1 {
            let types = try WireProtocol.nalTypes(packet.payload)
            guard types.contains(7), types.contains(8), types.isDisjoint(with: Set<UInt8>(1...5)) else {
                throw LANError.protocolViolation("config 缺少 SPS/PPS 或包含视频 slice")
            }
            hasConfig = true
            needIDR = true
        } else if packet.type == 2 && !packet.payload.isEmpty {
            let types = try WireProtocol.nalTypes(packet.payload)
            let keyframe = packet.flags & 1 != 0
            guard hasConfig, !types.isDisjoint(with: Set<UInt8>(1...5)), keyframe == types.contains(5),
                  !needIDR || keyframe else { throw LANError.protocolViolation("AU 缺少 config/IDR 或 keyframe 标志无效") }
            if let lastPTS, packet.ptsUs < lastPTS { throw LANError.protocolViolation("AU 时间戳倒退") }
            lastPTS = packet.ptsUs
            needIDR = false
            frames = try WireProtocol.addCount(frames, 1)
        }
        if packet.type == 2 && packet.flags & 2 != 0 {
            guard frames > 0, hasConfig, !needIDR else { throw LANError.protocolViolation("EOS 之前没有完整视频序列") }
            receivedEOS = true
        }
    }

    func finish() throws {
        guard !failed, receivedEOS, header.isEmpty, packetHeader == nil else {
            throw LANError.protocolViolation("连接结束时视频不完整或缺少 EOS")
        }
    }
}
