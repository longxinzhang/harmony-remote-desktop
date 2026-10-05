import Foundation
import CoreMedia
import CoreVideo
import VideoToolbox

struct DecoderSnapshot {
    let decodedFrames: Int
    let width: Int
    let height: Int
    let hardwareAccelerated: Bool?
    let droppedFrames: Int
    let error: String?
    let lastDecodedFrameAt: TimeInterval?
}

enum H264DecoderError: Error, CustomStringConvertible {
    case invalid(String)
    case status(String, OSStatus)
    var description: String {
        switch self {
        case .invalid(let reason): return reason
        case .status(let operation, let status): return "\(operation) failed: \(status)"
        }
    }
}

// configure/decode/finish/close belong to the caller's one serial decode queue.
// VT callbacks only touch this separately locked state, never session lifecycle.
private final class H264DecoderState {
    let condition = NSCondition()
    var inFlight = 0
    var closed = false
    var decoded = 0
    var dropped = 0
    var width = 0
    var height = 0
    var hardware: Bool?
    var error: String?
    var lastDecodedFrameAt: TimeInterval?
}

final class H264Decoder {
    private let state = H264DecoderState()
    private let onFrame: (CVPixelBuffer, UInt64) -> Void
    private var session: VTDecompressionSession?
    private var format: CMVideoFormatDescription?
    private var parameterSets: [Data] = []
    private var requiresIDR = true
    private var finished = false
    private static let maxPacket = 8 * 1024 * 1024
    private static let maxInFlight = 4

    init(onFrame: @escaping (CVPixelBuffer, UInt64) -> Void) { self.onFrame = onFrame }
    deinit { close() }

    var snapshot: DecoderSnapshot {
        state.condition.lock()
        defer { state.condition.unlock() }
        return DecoderSnapshot(decodedFrames: state.decoded, width: state.width, height: state.height,
            hardwareAccelerated: state.hardware, droppedFrames: state.dropped, error: state.error,
            lastDecodedFrameAt: state.lastDecodedFrameAt)
    }

    private func fail(_ error: H264DecoderError) -> H264DecoderError {
        state.condition.lock()
        if state.error == nil { state.error = error.description }
        state.condition.broadcast()
        state.condition.unlock()
        return error
    }

    private static func nals(_ data: Data) throws -> [Data] {
        guard !data.isEmpty, data.count <= maxPacket else { throw H264DecoderError.invalid("empty or oversized Annex B packet") }
        let bytes = [UInt8](data)
        var starts: [(Int, Int)] = []
        var i = 0
        while i + 2 < bytes.count {
            if bytes[i] == 0 && bytes[i + 1] == 0 {
                if bytes[i + 2] == 1 { starts.append((i, 3)); i += 3; continue }
                if i + 3 < bytes.count && bytes[i + 2] == 0 && bytes[i + 3] == 1 {
                    starts.append((i, 4)); i += 4; continue
                }
            }
            i += 1
        }
        guard let first = starts.first, bytes[..<first.0].allSatisfy({ $0 == 0 }), starts.count <= 4096 else {
            throw H264DecoderError.invalid("invalid Annex B start codes or too many NAL units")
        }
        var output: [Data] = []
        for index in starts.indices {
            let begin = starts[index].0 + starts[index].1
            var end = index + 1 < starts.count ? starts[index + 1].0 : bytes.count
            while end > begin && bytes[end - 1] == 0 { end -= 1 }
            guard end - begin >= 2 else { throw H264DecoderError.invalid("empty or truncated NAL unit") }
            let header = bytes[begin]
            let type = header & 31
            guard header & 0x80 == 0, type > 0, type < 24 else { throw H264DecoderError.invalid("invalid H.264 NAL header") }
            if type == 7 && end - begin < 4 { throw H264DecoderError.invalid("truncated SPS") }
            output.append(Data(bytes[begin..<end]))
        }
        return output
    }

    func configure(_ annexB: Data) throws {
        do {
            let units = try Self.nals(annexB)
            guard units.allSatisfy({ ($0.first! & 31) == 7 || ($0.first! & 31) == 8 }),
                units.contains(where: { ($0.first! & 31) == 7 }),
                units.contains(where: { ($0.first! & 31) == 8 }), units.count <= 16 else {
                throw H264DecoderError.invalid("configuration must contain SPS and PPS only")
            }
            guard !state.closed else { throw H264DecoderError.invalid("decoder is closed") }
            if units == parameterSets && session != nil && !finished { return }
            if session != nil { try finish(); disposeSession() }
            let storage = units.map { $0 as NSData }
            var pointers = storage.map { $0.bytes.assumingMemoryBound(to: UInt8.self) }
            var sizes = storage.map { $0.length }
            var description: CMFormatDescription?
            let status = withExtendedLifetime(storage) {
                CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: kCFAllocatorDefault,
                    parameterSetCount: pointers.count, parameterSetPointers: &pointers,
                    parameterSetSizes: &sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &description)
            }
            guard status == noErr, let description else { throw H264DecoderError.status("H.264 format description", status) }
            var created: VTDecompressionSession?
            let decoderOptions: [CFString: Any] = [kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: true]
            let imageOptions: [CFString: Any] = [
                kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                kCVPixelBufferIOSurfacePropertiesKey: [:],
                kCVPixelBufferMetalCompatibilityKey: true
            ]
            let result = VTDecompressionSessionCreate(allocator: kCFAllocatorDefault, formatDescription: description,
                decoderSpecification: decoderOptions as CFDictionary, imageBufferAttributes: imageOptions as CFDictionary,
                outputCallback: nil, decompressionSessionOut: &created)
            guard result == noErr, let created else { throw H264DecoderError.status("VTDecompressionSessionCreate", result) }
            session = created
            format = description
            let realtime = VTSessionSetProperty(created, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
            guard realtime == noErr else { disposeSession(); throw H264DecoderError.status("VT real-time mode", realtime) }
            let dimensions = CMVideoFormatDescriptionGetDimensions(description)
            guard dimensions.width > 0, dimensions.height > 0, dimensions.width <= 8192, dimensions.height <= 8192 else {
                disposeSession(); throw H264DecoderError.invalid("invalid or excessive coded dimensions")
            }
            state.condition.lock()
            state.width = Int(dimensions.width); state.height = Int(dimensions.height)
            state.condition.unlock()
            parameterSets = units
            requiresIDR = true
            finished = false
            readHardwareProperty()
        } catch let error as H264DecoderError { throw fail(error) }
    }

    private func reserveFrame() throws {
        state.condition.lock()
        defer { state.condition.unlock() }
        let deadline = Date(timeIntervalSinceNow: 0.4)
        while state.inFlight >= Self.maxInFlight && state.error == nil && !state.closed {
            if !state.condition.wait(until: deadline) {
                state.error = "VideoToolbox backpressure exceeded 400 ms; disconnect required"
                break
            }
        }
        if let message = state.error { throw H264DecoderError.invalid(message) }
        guard !state.closed else { throw H264DecoderError.invalid("decoder is closed") }
        state.inFlight += 1
    }

    func decode(_ annexB: Data, ptsUs: UInt64, keyframe: Bool) throws {
        do {
            guard let session, let format, !finished else { throw H264DecoderError.invalid("decoder is not configured or is finished") }
            guard ptsUs <= UInt64(Int64.max) else { throw H264DecoderError.invalid("presentation timestamp overflow") }
            let units = try Self.nals(annexB)
            let hasIDR = units.contains { ($0.first! & 31) == 5 }
            guard units.contains(where: { ($0.first! & 31) == 1 || ($0.first! & 31) == 5 }) else {
                throw H264DecoderError.invalid("video packet contains no complete coded picture")
            }
            guard !keyframe || hasIDR, !requiresIDR || hasIDR else { throw H264DecoderError.invalid("IDR required before inter frames") }
            for unit in units where (unit.first! & 31) == 7 || (unit.first! & 31) == 8 {
                guard parameterSets.contains(unit) else { throw H264DecoderError.invalid("changed SPS/PPS require explicit configuration") }
            }
            var avcc = Data()
            avcc.reserveCapacity(annexB.count + units.count * 4)
            for unit in units {
                var length = UInt32(unit.count).bigEndian
                withUnsafeBytes(of: &length) { avcc.append(contentsOf: $0) }
                avcc.append(unit)
            }
            var block: CMBlockBuffer?
            let allocation = CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil,
                blockLength: avcc.count, blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
                offsetToData: 0, dataLength: avcc.count, flags: 0, blockBufferOut: &block)
            guard allocation == noErr, let block else { throw H264DecoderError.status("CMBlockBuffer allocation", allocation) }
            let copy = avcc.withUnsafeBytes { bytes in
                CMBlockBufferReplaceDataBytes(with: bytes.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: bytes.count)
            }
            guard copy == noErr else { throw H264DecoderError.status("CMBlockBuffer copy", copy) }
            var timing = CMSampleTimingInfo(duration: .invalid,
                presentationTimeStamp: CMTime(value: Int64(ptsUs), timescale: 1_000_000), decodeTimeStamp: .invalid)
            var sampleSize = avcc.count
            var sample: CMSampleBuffer?
            let sampleResult = CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block,
                formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 1, sampleTimingArray: &timing,
                sampleSizeEntryCount: 1, sampleSizeArray: &sampleSize, sampleBufferOut: &sample)
            guard sampleResult == noErr, let sample else { throw H264DecoderError.status("CMSampleBufferCreateReady", sampleResult) }
            try reserveFrame()
            let state = self.state
            let onFrame = self.onFrame
            var info: VTDecodeInfoFlags = []
            let result = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample,
                flags: [._EnableAsynchronousDecompression], infoFlagsOut: &info) { [sample] status, flags, image, _, _ in
                    // Retain copied compressed storage through the entire callback.
                    defer { withExtendedLifetime(sample) {} }
                    state.condition.lock()
                    state.inFlight -= 1
                    let dropped = flags.contains(.frameDropped)
                    if dropped { state.dropped += 1 }
                    var deliver = false
                    if status != noErr || dropped || image == nil {
                        if state.error == nil { state.error = "VT decode output failed: status=\(status), dropped=\(dropped), image=\(image != nil)" }
                    } else if let image {
                        state.decoded += 1
                        state.lastDecodedFrameAt = ProcessInfo.processInfo.systemUptime
                        state.width = CVPixelBufferGetWidth(image); state.height = CVPixelBufferGetHeight(image)
                        deliver = !state.closed && state.error == nil
                    }
                    state.condition.broadcast()
                    state.condition.unlock()
                    if deliver, let image { onFrame(image, ptsUs) }
                }
            if result != noErr {
                // The output-handler API guarantees no handler invocation on an immediate error.
                state.condition.lock(); state.inFlight -= 1; state.condition.broadcast(); state.condition.unlock()
                throw H264DecoderError.status("VTDecompressionSessionDecodeFrame", result)
            }
            requiresIDR = false
            if let error = snapshot.error { throw H264DecoderError.invalid(error) }
        } catch let error as H264DecoderError { throw fail(error) }
    }

    func finish() throws {
        guard let session else { return }
        let delayed = VTDecompressionSessionFinishDelayedFrames(session)
        let drained = VTDecompressionSessionWaitForAsynchronousFrames(session)
        finished = true
        readHardwareProperty()
        if delayed != noErr { throw fail(.status("VT finish delayed frames", delayed)) }
        if drained != noErr { throw fail(.status("VT wait for asynchronous frames", drained)) }
        if let error = snapshot.error { throw H264DecoderError.invalid(error) }
        state.condition.lock(); let remaining = state.inFlight; state.condition.unlock()
        if remaining != 0 { throw fail(.invalid("VT finished with outstanding frames")) }
    }

    private func readHardwareProperty() {
        guard let session else { return }
        var value: Unmanaged<CFTypeRef>?
        let result = VTSessionCopyProperty(session, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
            allocator: kCFAllocatorDefault, valueOut: &value)
        let copiedValue = value?.takeRetainedValue()
        state.condition.lock()
        state.hardware = result == noErr ? (copiedValue as? NSNumber)?.boolValue : nil
        state.condition.unlock()
    }

    private func disposeSession() {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil; format = nil
    }

    func close() {
        state.condition.lock(); state.closed = true; state.condition.broadcast(); state.condition.unlock()
        if let session {
            VTDecompressionSessionFinishDelayedFrames(session)
            VTDecompressionSessionWaitForAsynchronousFrames(session)
        }
        disposeSession()
        finished = true
    }
}
