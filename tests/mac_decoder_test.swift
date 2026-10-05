import Foundation
import CoreVideo

private func require(_ value: Bool, _ message: String) throws {
    if !value { throw H264DecoderError.invalid("test failed: \(message)") }
}

@main
struct DecoderTests {
    static func main() throws {
        guard CommandLine.arguments.count == 2 else { fatalError("usage: mac_decoder_test fixture.hrd") }
        let wire = [UInt8](try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
        var position = 0
        var config = Data()
        var frames = 0
        var eos = false
        let lock = NSLock()
        var callbacks = 0
        var invalidPixels = 0
        var observedPts: [UInt64] = []
        let decoder = H264Decoder { image, pts in
            let valid = CVPixelBufferGetWidth(image) == 1620 && CVPixelBufferGetHeight(image) == 1080 &&
                CVPixelBufferGetDataSize(image) > 0
            lock.lock()
            callbacks += 1
            if !valid { invalidPixels += 1 }
            observedPts.append(pts)
            lock.unlock()
        }
        try require(decoder.snapshot.lastDecodedFrameAt == nil, "unused decoder has no successful output timestamp")
        defer { decoder.close() }
        while position < wire.count {
            try require(wire.count - position >= 24, "complete HRD1 header")
            let base = position
            func number(_ offset: Int, _ count: Int) -> UInt64 {
                wire[(base + offset)..<(base + offset + count)].reduce(0) { ($0 << 8) | UInt64($1) }
            }
            try require(Array(wire[base..<(base + 4)]) == Array("HRD1".utf8), "fixture magic")
            let type = wire[base + 5]
            let flags = number(6, 2)
            let pts = number(12, 8)
            let size = Int(number(20, 4))
            position += 24
            try require(size <= wire.count - position, "complete payload")
            var payload = Data(wire[position..<(position + size)])
            position += size
            if type == 1 {
                config = payload
                try decoder.configure(payload)
            } else if type == 2 {
                if !payload.isEmpty {
                    try decoder.decode(payload, ptsUs: pts, keyframe: flags & 1 != 0)
                    frames += 1
                    // The decoder must already own a CMBlockBuffer copy: caller memory
                    // can be overwritten immediately while VT is still asynchronous.
                    payload.resetBytes(in: 0..<payload.count)
                }
                if flags & 2 != 0 { eos = true; break }
            }
        }
        try require(eos && position == wire.count && frames == 287, "287 real AUs followed by EOS")
        try decoder.finish()
        let snapshot = decoder.snapshot
        lock.lock()
        let count = callbacks, bad = invalidPixels, ptsCount = observedPts.count
        lock.unlock()
        try require(snapshot.decodedFrames == 287 && count == 287 && ptsCount == 287, "VT fully decoded every real frame")
        try require(snapshot.width == 1620 && snapshot.height == 1080 && bad == 0, "actual nonempty pixel buffers at expected dimensions")
        try require(snapshot.droppedFrames == 0 && snapshot.error == nil, "no suppressed reference frames or decode errors")
        try require(snapshot.lastDecodedFrameAt.map { ProcessInfo.processInfo.systemUptime - $0 < 2 } == true,
                    "successful decode records local monotonic output time")

        func rejects(_ name: String, _ operation: (H264Decoder) throws -> Void) throws {
            let invalid = H264Decoder { _, _ in }
            defer { invalid.close() }
            var failed = false
            do { try operation(invalid) } catch { failed = true }
            try require(failed, "reject \(name)")
            try require(invalid.snapshot.error != nil && invalid.snapshot.lastDecodedFrameAt == nil,
                        "rejected \(name) records explicit error without claiming a successful decode")
        }
        try rejects("empty configuration") { try $0.configure(Data()) }
        try rejects("invalid configuration") { try $0.configure(Data("not Annex B".utf8)) }
        try rejects("truncated SPS") { try $0.configure(Data([0, 0, 0, 1, 0x67, 0x42])) }
        try rejects("decode before configuration") { try $0.decode(config, ptsUs: 0, keyframe: false) }
        try rejects("truncated video NAL") {
            try $0.configure(config)
            try $0.decode(Data([0, 0, 0, 1, 0x65]), ptsUs: 0, keyframe: true)
        }
        let result: [String: Any] = ["decodedFrames": snapshot.decodedFrames, "width": snapshot.width,
            "height": snapshot.height, "droppedFrames": snapshot.droppedFrames,
            "hardwareAccelerated": snapshot.hardwareAccelerated as Any? ?? NSNull(),
            "source": "real lan-take-01 H.264", "timing": "synthetic 30 Hz replay, not live performance",
            "invalidInputCasesRejected": 5]
        let json = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
        print(String(decoding: json, as: UTF8.self))
    }
}
