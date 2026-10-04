import Foundation
import AVFoundation

@main struct AudioTests {
    static var count = 0
    static func check(_ value: Bool, _ message: String) {
        guard value else { fputs("FAIL \(message)\n", stderr); exit(1) }
        count += 1; print("PASS \(message)")
    }
    static func header(kind: UInt8 = 1, length: UInt32 = 4, rate: UInt32 = 48000, channels: UInt8 = 2, sequence: UInt64 = 1) -> Data {
        var data = Data([72,82,68,65,1,kind,channels,1])
        func append<T: FixedWidthInteger>(_ value: T) { var n = value.bigEndian; withUnsafeBytes(of: &n) { data.append(contentsOf: $0) } }
        append(length); append(rate); append(UInt64(20000)); append(sequence); append(UInt64(1)); return data
    }
    static func wait(_ predicate: () -> Bool, seconds: Double = 5) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while !predicate(), Date() < end { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        return predicate()
    }
    static func main() throws {
        check((try? AudioWire.header(header()))?.length == 4, "PCM header accepts supported format")
        check((try? AudioWire.header(header(length: 3))) == nil, "partial PCM sample rejected")
        check((try? AudioWire.header(header(length: 3844))) == nil, "oversized frame rejected")
        check((try? AudioWire.header(header(rate: 44100))) == nil, "unnegotiated sample rate rejected")
        check((try? AudioWire.header(header(channels: 1))) == nil, "unnegotiated channel count rejected")
        check((try? AudioWire.header(header(kind: 2, length: 0)))?.kind == .reset, "reset frame carries no audio")
        check((try? AudioWire.header(header(kind: 2, length: 4))) == nil, "reset payload rejected")
        check((try? AudioWire.header(header(kind: 3))) == nil, "unknown frame kind rejected")
        check(!AudioWire.ready(Data("{\"type\":\"audio_bound\",\"version\":true}".utf8)), "boolean version cannot authenticate")
        check((try? AudioWire.bind(AudioSession(host: "127.0.0.1", port: 1, epoch: "x", binding: "x"))) == nil, "malformed binding credentials rejected")
        let pcm = Data([0,128,255,127,0,0,255,255])
        let format = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
        let buffer = AudioPlayback.buffer(pcm, format: format)!
        check(buffer.frameLength == 2 && buffer.floatChannelData![0][0] == -1 && buffer.floatChannelData![1][0] > 0.9999, "signed little-endian PCM channels converted exactly")
        check(buffer.floatChannelData![0][1] == 0 && buffer.floatChannelData![1][1] == -1.0 / 32768, "zero and negative one preserve channel ordering")
        check(AudioPlayback.buffer(Data([0]), format: format) == nil, "playback refuses misaligned bytes")
        // Exercise the same PCM conversion through the actual AVAudioEngine offline
        // graph. This renders in memory, never emits sound or reads the microphone.
        let engine = AVAudioEngine(), player = AVAudioPlayerNode()
        engine.attach(player); engine.connect(player, to: engine.mainMixerNode, format: format)
        try engine.enableManualRenderingMode(.offline, format: format, maximumFrameCount: 960)
        try engine.start(); let tone = Data(repeating: 0x22, count: 3840)
        player.scheduleBuffer(AudioPlayback.buffer(tone, format: format)!); player.play()
        let output = AVAudioPCMBuffer(pcmFormat: engine.manualRenderingFormat, frameCapacity: 960)!
        let status = try engine.renderOffline(960, to: output)
        check(status == .success && output.frameLength == 960, "AVAudioEngine renders production PCM buffer offline")
        check(abs(output.floatChannelData![0][100] - Float(0x2222) / 32768) < 0.0001, "offline output contains expected PCM amplitude")
        player.stop(); engine.stop()
        let playback = AudioPlayback(); playback.setMuted(true)
        for i in 0..<100 { playback.append(AudioPacket(kind: .pcm, ptsUs: 0, sequence: UInt64(i + 1), streamID: 1, pcm: tone)) }
        check(playback.snapshot().receivedFrames == 96000 && playback.snapshot().droppedFrames == 96000 && playback.snapshot().queuedFrames == 0,
              "mute discards PCM before audio engine or queue allocation")
        playback.stop(); let previous = playback.snapshot().receivedFrames
        playback.append(AudioPacket(kind: .pcm, ptsUs: 0, sequence: 999, streamID: 1, pcm: tone))
        check(playback.snapshot().receivedFrames == previous, "stopped playback rejects late session packets")

        let child = Process(); child.executableURL = URL(fileURLWithPath: CommandLine.arguments[1])
        let childIn = Pipe(), childOut = Pipe(); child.standardInput = childIn; child.standardOutput = childOut
        try child.run()
        var line = Data()
        while true { let byte = childOut.fileHandleForReading.readData(ofLength: 1); guard !byte.isEmpty else { fatalError("fixture failed") }; if byte[0] == 10 { break }; line.append(byte) }
        let parts = String(data: line, encoding: .utf8)!.split(separator: " ")
        let session = AudioSession(host: "127.0.0.1", port: UInt16(parts[0])!, epoch: String(parts[1]), binding: String(parts[2]))
        let resultLock = NSLock(); var packets = 0, resets = 0, exact = true; var failure = ""; var ready = false
        // Hold the transport queue just after NWConnection starts. Drop the last
        // owner before the queued close can execute, reproducing cancellation
        // during .preparing without depending on network timing.
        let began = DispatchSemaphore(value: 0), resumeQueue = DispatchSemaphore(value: 0)
        let cleaned = DispatchSemaphore(value: 0)
        var preparing: AudioConnection? = AudioConnection(testSession: session, onPacket: { _ in },
            onReady: {}, onFailure: { _ in }, afterConnectionStarted: {
                began.signal(); _ = resumeQueue.wait(timeout: .now() + 5)
            }, afterTransportClosed: { cleaned.signal() })
        weak var releasedPreparing = preparing
        preparing?.start()
        check(began.wait(timeout: .now() + 5) == .success, "audio cancellation fixture reaches connection preparation")
        preparing?.close(); preparing = nil; resumeQueue.signal()
        check(cleaned.wait(timeout: .now() + 5) == .success, "queued audio close completes after owner releases channel")
        check(wait { releasedPreparing == nil }, "cancelled preparing audio channel has no retained transport cycle")
        releasedPreparing = nil
        let channel = AudioConnection(testSession: session, onPacket: { packet in
            resultLock.lock(); defer { resultLock.unlock() }
            if packet.kind == .reset { resets += 1 }
            else { packets += 1; exact = exact && packet.pcm.count == 3840 && packet.pcm.prefix(4) == Data([0xe8,0x03,0x18,0xfc]) }
        }, onReady: { ready = true; childIn.fileHandleForWriting.write(Data("start\n".utf8)) }, onFailure: { failure = $0 })
        channel.start()
        check(wait { resultLock.lock(); defer { resultLock.unlock() }; return resets >= 3 || !failure.isEmpty }, "native service to Swift transport completes stream")
        resultLock.lock(); let packetCount = packets, resetCount = resets, exactBytes = exact; resultLock.unlock()
        check(ready && failure.isEmpty && packetCount == 12 && exactBytes, "authenticated native PCM received with exact samples")
        check(resetCount == 3, "bind share start and share stop each reset playback")
        channel.close(); childIn.fileHandleForWriting.write(Data("stop\n".utf8)); child.waitUntilExit()
        check(child.terminationStatus == 0, "native audio bridge exits cleanly")
        print("Mac audio: \(count) checks passed")
    }
}
