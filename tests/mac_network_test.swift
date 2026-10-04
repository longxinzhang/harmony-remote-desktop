import Foundation
import Darwin

private struct TestFailure: Error, CustomStringConvertible {
    let description: String
}

private func require(_ value: @autoclosure () -> Bool, _ message: String) throws {
    if !value() { throw TestFailure(description: message) }
}

private func rejects(_ message: String, _ body: () throws -> Void) throws {
    var rejected = false
    do { try body() } catch { rejected = true }
    try require(rejected, message)
}

private let config = Data([0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0, 0, 1, 0x68, 0xee, 0x3c])
private let idr = Data([0, 0, 1, 0x65, 0x88, 0x84])
private let pframe = Data([0, 0, 1, 0x41, 0x9a, 0x20])

private func packet(_ type: UInt8, _ sequence: UInt32, _ payload: Data = Data(),
                    flags: UInt16 = 0, pts: UInt64 = 0, declaredLength: UInt32? = nil,
                    magic: [UInt8] = [72, 82, 68, 49], version: UInt8 = 1) -> Data {
    var bytes = Data(magic + [version, type])
    var f = flags.bigEndian, s = sequence.bigEndian, p = pts.bigEndian
    var length = (declaredLength ?? UInt32(payload.count)).bigEndian
    withUnsafeBytes(of: &f) { bytes.append(contentsOf: $0) }
    withUnsafeBytes(of: &s) { bytes.append(contentsOf: $0) }
    withUnsafeBytes(of: &p) { bytes.append(contentsOf: $0) }
    withUnsafeBytes(of: &length) { bytes.append(contentsOf: $0) }
    bytes.append(payload)
    return bytes
}

private func stream(dataEOS: Bool = false) -> Data {
    var result = packet(1, 0, config) + packet(2, 1, idr, flags: 1)
    result += packet(3, 2)
    result += packet(2, 3, pframe, flags: dataEOS ? 2 : 0, pts: 33333)
    if !dataEOS { result += packet(2, 4, flags: 2, pts: 33333) }
    return result
}

private func parserTests() throws -> Int {
    var count = 0
    for chunkSize in [1, 2, 7, 24, 65536] {
        let parser = WireVideoParser()
        let wire = stream()
        var decoded: [VideoPacket] = []
        for offset in stride(from: 0, to: wire.count, by: chunkSize) {
            decoded += try parser.feed(wire.subdata(in: offset..<min(wire.count, offset + chunkSize)))
        }
        try parser.finish()
        try require(decoded.map(\.sequence) == [0, 1, 2, 3, 4], "fragmented sequence/framing mismatch")
        try require(decoded.filter { !$0.payload.isEmpty }.reduce(Data()) { $0 + $1.payload } == config + idr + pframe,
                    "fragmented payload bytes changed")
        try require(decoded[3].ptsUs == 33333 && parser.assemblyStartedAt == nil, "big-endian PTS or assembly reset mismatch")
        count += 1
    }
    let dataEOS = WireVideoParser()
    let withFinalAU = try dataEOS.feed(stream(dataEOS: true))
    try dataEOS.finish()
    try require(withFinalAU.last?.payload == pframe, "EOS data AU was lost")
    count += 1

    let badStreams: [(String, Data)] = [
        ("bad magic", packet(1, 0, config, magic: [0, 0, 0, 0])),
        ("bad version", packet(1, 0, config, version: 2)),
        ("unknown type", packet(9, 0)),
        ("unknown flags", packet(1, 0, config, flags: 4)),
        ("oversized AU", packet(2, 0, declaredLength: UInt32.max)),
        ("oversized config", packet(1, 0, declaredLength: 262145)),
        ("empty AU", packet(2, 0)),
        ("payload on keepalive", packet(3, 0, idr)),
        ("flags on config", packet(1, 0, config, flags: 1)),
        ("key flag on empty EOS", packet(1, 0, config) + packet(2, 1, idr, flags: 1) + packet(2, 2, flags: 3)),
        ("sequence gap", packet(1, 0, config) + packet(2, 2, idr, flags: 1)),
        ("duplicate sequence", packet(1, 0, config) + packet(2, 0, idr, flags: 1)),
        ("missing config", packet(2, 0, idr, flags: 1)),
        ("missing PPS", packet(1, 0, Data([0, 0, 1, 0x67, 0x64]))),
        ("VCL in config", packet(1, 0, config + idr)),
        ("missing IDR", packet(1, 0, config) + packet(2, 1, pframe)),
        ("wrong key flag", packet(1, 0, config) + packet(2, 1, pframe, flags: 1)),
        ("backwards PTS", packet(1, 0, config) + packet(2, 1, idr, flags: 1, pts: 2) + packet(2, 2, pframe, pts: 1)),
        ("empty EOS without video", packet(1, 0, config) + packet(2, 1, flags: 2)),
        ("new config without IDR", packet(1, 0, config) + packet(2, 1, idr, flags: 1) + packet(1, 2, config) + packet(2, 3, flags: 2)),
        ("bytes after EOS", stream() + Data([0])),
        ("Annex-B required", packet(1, 0, Data([0x67, 0x64, 0x68, 0xee]))),
        ("empty trailing NAL", packet(1, 0, config + Data([0, 0, 1]))),
        ("forbidden NAL bit", packet(1, 0, Data([0, 0, 1, 0xe7, 0, 0, 1, 0x68])))
    ]
    for (name, wire) in badStreams {
        try rejects(name) { _ = try WireVideoParser().feed(wire) }
        count += 1
    }
    for prefix in [Data(), packet(1, 0, config).prefix(10), packet(1, 0, config).dropLast(1),
                   packet(1, 0, config) + packet(2, 1, idr, flags: 1)] {
        let parser = WireVideoParser()
        _ = try parser.feed(Data(prefix))
        try rejects("premature EOF accepted") { try parser.finish() }
        count += 1
    }
    let parser = WireVideoParser()
    _ = try parser.feed(packet(1, 0, config))
    var largeAU = idr
    largeAU.append(Data(repeating: 0, count: WireProtocol.maxPayload - largeAU.count))
    for sequence in UInt32(1)...7 { _ = try parser.feed(packet(2, sequence, largeAU, flags: 1, pts: UInt64(sequence))) }
    try rejects("64 MiB total limit not enforced before payload") {
        _ = try parser.feed(packet(2, 8, flags: 1, declaredLength: UInt32(largeAU.count)))
    }
    count += 1
    return count
}

private func controlTests() throws -> Int {
    let hello = try WireProtocol.controlObject(Data("{\"type\":\"hello_ack\",\"protocol\":1,\"pairingRequired\":true,\"nativePtsUnitVerified\":false}".utf8))
    try require(WireProtocol.isIntegerOne(hello["protocol"]), "integer protocol rejected")
    try require(WireProtocol.isBoolean(hello["pairingRequired"], true), "boolean field rejected")
    try require(!WireProtocol.isIntegerOne(true) && !WireProtocol.isBoolean(1, true), "JSON bool/number confused")
    var count = 1
    for text in ["{\"type\":\"a\",\"type\":\"b\"}", "{\"ty\\u0070e\":\"a\",\"type\":\"b\"}",
                 "[]", "{\"type\":1}", "{\"type\":\"x\",\"value\":NaN}", "{\"type\":\"x\",\"nested\":{}}"] {
        try rejects("invalid control JSON accepted") { _ = try WireProtocol.controlObject(Data(text.utf8)) }
        count += 1
    }
    try rejects("invalid UTF-8 accepted") { _ = try WireProtocol.controlObject(Data([255, 255])) }
    try rejects("oversized control accepted") { _ = try WireProtocol.controlObject(Data(repeating: 32, count: 4097)) }
    count += 2
    for host in ["127.0.0.1", "8.8.8.8", "169.254.1.2", "::1", "example.com", "172.32.0.1", "192.168.01.2", "192.168.1.2 "] {
        try require(!WireProtocol.validHost(host), "non-RFC1918 host accepted")
        count += 1
    }
    for host in ["10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.1.2"] {
        try require(WireProtocol.validHost(host), "valid RFC1918 host rejected")
        count += 1
    }
    try require(WireProtocol.validPIN("001234") && !WireProtocol.validPIN("１２３４５６") && !WireProtocol.validPIN("12345"), "PIN constraints failed")
    return count + 1
}

private final class Fixture {
    let process = Process()
    let stdout = Pipe()
    let stderr = Pipe()
    let controlPort: UInt16
    let videoPort: UInt16
    let pin: String
    let expected: Data

    init(path: String, mode: String = "--serve-fixture") throws {
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = [mode]
        process.standardOutput = stdout
        process.standardError = stderr
        try process.run()
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        var line = Data()
        while !line.contains(10) && ProcessInfo.processInfo.systemUptime < deadline {
            var event = pollfd(fd: stdout.fileHandleForReading.fileDescriptor, events: Int16(POLLIN), revents: 0)
            if poll(&event, 1, 100) <= 0 { continue }
            // FileHandle.read(upToCount:) may wait to fill the request on a pipe.
            // A single POSIX read after poll consumes only currently available bytes.
            var block = [UInt8](repeating: 0, count: 4096)
            let count = Darwin.read(stdout.fileHandleForReading.fileDescriptor, &block, block.count)
            if count <= 0 { break }
            line.append(contentsOf: block.prefix(count))
            if line.count > 4096 { break }
        }
        // The fixture PIN is random, consumed only in memory, and never printed.
        guard let newline = line.firstIndex(of: 10),
              let object = try JSONSerialization.jsonObject(with: line.prefix(upTo: newline)) as? [String: Any],
              let cp = object["controlPort"] as? UInt16, let vp = object["videoPort"] as? UInt16,
              let testPIN = object["pin"] as? String, let hex = object["expectedH264Hex"] as? String else {
            process.terminate()
            throw TestFailure(description: "fixture did not announce valid test endpoints")
        }
        controlPort = cp
        videoPort = vp
        pin = testPIN
        var bytes = Data()
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { throw TestFailure(description: "fixture hex invalid") }
            bytes.append(byte)
            index = next
        }
        expected = bytes
    }

    func waitForExit() throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while process.isRunning && ProcessInfo.processInfo.systemUptime < deadline { Thread.sleep(forTimeInterval: 0.02) }
        try require(!process.isRunning && process.terminationStatus == 0, "actual C++ fixture did not finish cleanly")
    }

    deinit {
        if process.isRunning { process.terminate() }
        try? stdout.fileHandleForReading.close()
        try? stderr.fileHandleForReading.close()
    }
}

private func integrationTests(path: String) throws -> Int {
    let fixture = try Fixture(path: path)
    let completed = DispatchSemaphore(value: 0)
    let lock = NSLock()
    var payload = Data()
    var completionCount = 0
    var result: Result<Void, Error>?
    var statuses: [String] = []
    let client = LANConnection(testControlPort: fixture.controlPort, testVideoPort: fixture.videoPort,
                               onStatus: { status in lock.lock(); statuses.append(status); lock.unlock() }, onPacket: { packet in
        lock.lock(); payload.append(packet.payload); lock.unlock()
    }, onEnd: { outcome in
        lock.lock(); completionCount += 1; result = outcome; lock.unlock()
        completed.signal()
    })
    client.connect(host: "127.0.0.1", pin: fixture.pin)
    try require(completed.wait(timeout: .now() + 12) == .success, "NWConnection fixture test timed out")
    lock.lock(); let receivedResult = result; let receivedPayload = payload; let reachedStatuses = statuses; lock.unlock()
    guard let receivedResult else { throw TestFailure(description: "missing NWConnection completion") }
    do { try receivedResult.get() }
    catch { throw TestFailure(description: "NWConnection fixture failed: \(error.localizedDescription); stages: \(reachedStatuses.joined(separator: ", "))") }
    try require(receivedPayload == fixture.expected, "actual C++ to NWConnection payload bytes differ")
    let statistics = client.snapshot
    try require(statistics.receivedFrames == 2 && statistics.receivedBytes == fixture.expected.count && statistics.paired && statistics.videoReady,
                "NWConnection final statistics are incorrect")
    client.disconnect(); client.disconnect()
    try fixture.waitForExit()
    Thread.sleep(forTimeInterval: 0.1)
    lock.lock(); let totalCompletions = completionCount; lock.unlock()
    try require(totalCompletions == 1, "EOS/disconnect caused duplicate completion")

    let generationDone = DispatchSemaphore(value: 0)
    let generationLock = NSLock()
    var ends = 0
    var packetCount = 0
    let generationClient = LANConnection(testControlPort: fixture.controlPort, testVideoPort: fixture.videoPort,
                                         onStatus: { _ in }, onPacket: { _ in generationLock.lock(); packetCount += 1; generationLock.unlock() },
                                         onEnd: { _ in generationLock.lock(); ends += 1; generationLock.unlock(); generationDone.signal() })
    generationClient.connect(host: "127.0.0.1", pin: fixture.pin)
    generationClient.connect(host: "8.8.8.8", pin: fixture.pin)
    try require(generationDone.wait(timeout: .now() + 3) == .success && generationDone.wait(timeout: .now() + 3) == .success,
                "replacement sessions did not each finish")
    generationClient.disconnect()
    Thread.sleep(forTimeInterval: 0.2)
    generationLock.lock(); let finalEnds = ends; let finalPackets = packetCount; generationLock.unlock()
    try require(finalEnds == 2 && finalPackets == 0, "old connection callbacks contaminated replacement session")
    return 2
}

private func inputValidationTests() throws -> Int {
    let good: [[String: Any]] = [
        ["type": "mouse_move", "x": 0, "y": 1], ["type": "scroll", "dx": -120.0, "dy": 120.0],
        ["type": "mouse_button", "button": "middle", "action": "up"],
        ["type": "key", "code": "KEY_META_RIGHT", "action": "down"],
        ["type": "key", "code": "KEY_F12", "action": "up"], ["type": "key", "code": "KEY_0", "action": "up"]]
    let bad: [[String: Any]] = [
        ["type": "mouse_move", "x": -0.001, "y": 0], ["type": "mouse_move", "x": 1.001, "y": 0],
        ["type": "mouse_move", "x": Double.nan, "y": 0], ["type": "mouse_move", "x": Double.infinity, "y": 0],
        ["type": "mouse_move", "x": true, "y": 0], ["type": "mouse_move", "x": "0", "y": 0],
        ["type": "mouse_move", "x": 0, "y": 0, "sessionToken": "injected"],
        ["type": "scroll", "dx": 121, "dy": 0], ["type": "scroll", "dx": 0, "dy": -121],
        ["type": "mouse_button", "button": "fourth", "action": "down"],
        ["type": "key", "code": "KEY_CTRL", "action": "down"], ["type": "key", "code": "KEY_A", "action": "repeat"],
        ["type": "input_enable", "enabled": true], ["type": "release_all_keys"]]
    for object in good { try require(LANConnection.validInput(object), "valid input rejected") }
    for object in bad { try require(!LANConnection.validInput(object), "invalid input accepted") }
    let total = config.count + idr.count + pframe.count
    let parser = WireVideoParser(maxTotalBytes: total)
    _ = try parser.feed(stream()); try parser.finish()
    try rejects("custom cumulative cap was ignored") { _ = try WireVideoParser(maxTotalBytes: total - 1).feed(stream()) }
    return good.count + bad.count + 2
}

private func sessionDeadlineTests() throws -> Int {
    func video(_ type: UInt8, _ payload: Data, flags: UInt16 = 0) -> VideoPacket {
        VideoPacket(type: type, flags: flags, sequence: 0, ptsUs: 0, payload: payload)
    }
    var deadline = LANSessionDeadline(startingAt: 0)
    try require(!deadline.hasExpired(at: 1919.999) && deadline.hasExpired(at: 1920), "waiting for first AU must remain bounded")
    deadline.observe(video(1, config), at: 540)
    try require(deadline.hasExpired(at: 1920), "CONFIG must not start the video deadline")
    deadline.observe(video(3, Data()), at: 540)
    try require(deadline.hasExpired(at: 1920), "keepalive must not start the video deadline")
    deadline.observe(video(2, Data(), flags: 2), at: 540)
    try require(deadline.hasExpired(at: 1920), "empty EOS must not start the video deadline")

    // Host decides active stream lifetime; no fixed client session cutoff remains.
    deadline.observe(video(2, idr, flags: 1), at: 540)
    try require(!deadline.hasExpired(at: 540 + 1920) && !deadline.hasExpired(at: 365 * 24 * 3600),
                "active permanent stream retained a fixed lifetime limit")
    deadline.observe(video(2, pframe), at: 2000)
    deadline.observe(video(1, config), at: 2400)
    deadline.observe(video(2, idr, flags: 1), at: 2450)
    try require(!deadline.hasExpired(at: 2460) && !deadline.hasExpired(at: 365 * 24 * 3600),
                "later P-frame, reconfiguration or IDR restored a fixed deadline")

    deadline = LANSessionDeadline(startingAt: 5000)
    try require(!deadline.hasExpired(at: 6919.999) && deadline.hasExpired(at: 6920), "reconnect retained the old deadline")
    deadline.observe(video(2, idr, flags: 3), at: 5600)
    try require(!deadline.hasExpired(at: 7520), "nonempty EOS AU retained the pre-video waiting deadline")
    for arrival in [1920.0, 1920.001, 3000.0] {
        deadline = LANSessionDeadline(startingAt: 0)
        deadline.observe(video(2, idr, flags: 1), at: arrival)
        try require(deadline.hasExpired(at: arrival), "expired waiting was revived by a late first AU")
    }
    deadline = LANSessionDeadline(startingAt: 0)
    deadline.observe(video(2, idr, flags: 1), at: 1919.999)
    try require(!deadline.hasExpired(at: 1920) && !deadline.hasExpired(at: 365 * 24 * 3600),
                "first AU just before expiry did not enter unlimited active lifetime")
    return 12
}

private func sessionLifetimeIntegrationTests(path: String, duringStream: Bool) throws {
    let fixture = try Fixture(path: path)
    let done = DispatchSemaphore(value: 0)
    let lock = NSLock()
    var clock: TimeInterval = 0 // Accessed only by the connection's serial queue after connect.
    var outcome: Result<Void, Error>?
    var packets = 0, frames = 0
    let client = LANConnection(testControlPort: fixture.controlPort, testVideoPort: fixture.videoPort,
        testSessionClock: { clock }, testDisableTimer: true,
        onStatus: { status in
            if !duringStream && status.hasPrefix("视频通道已就绪") { clock = 1920 }
        }, onPacket: { packet in
            packets += 1
            if packet.type == 2 && !packet.payload.isEmpty {
                frames += 1
                if duringStream { clock = 365 * 24 * 3600 }
            }
        }, onEnd: { result in
            lock.lock(); outcome = result; lock.unlock(); done.signal()
        })
    client.connect(host: "127.0.0.1", pin: fixture.pin)
    try require(done.wait(timeout: .now() + 12) == .success, "session lifetime fixture did not finish")
    lock.lock(); let final = outcome; lock.unlock()
    if duringStream {
        guard let final else { throw TestFailure(description: "missing permanent stream completion") }
        try final.get()
        try require(frames == 2 && client.snapshot.receivedFrames == 2 &&
                    client.snapshot.receivedBytes == fixture.expected.count,
                    "active stream failed after advancing clock beyond 32 minutes")
        try fixture.waitForExit()
    } else {
        guard case .failure(let error)? = final, case LANError.deadline = error else {
            throw TestFailure(description: "expired waiting did not terminate with deadline")
        }
        try require(client.snapshot.receivedFrames == 0 && packets == 0, "expired waiting was revived by received video")
        // Timeout intentionally interrupts this normal-EOS fixture; deinit terminates it.
    }
}

private func longStreamParserTests() throws -> Int {
    let live = WireVideoParser(maxTotalBytes: nil)
    _ = try live.feed(packet(1, 0, config))
    var largeAU = idr
    largeAU.append(Data(repeating: 0, count: WireProtocol.maxPayload - largeAU.count))
    // Reuse one bounded AU; process >2 GiB of cumulative traffic without retaining
    // previous packets, writing a video file, or waiting for a long real session.
    for sequence in UInt32(1)...257 {
        let decoded = try live.feed(packet(2, sequence, largeAU, flags: 1, pts: UInt64(sequence)))
        try require(decoded.count == 1 && decoded[0].payload.count == WireProtocol.maxPayload &&
                    live.testCounters.bufferedBytes == 0, "completed packet was retained or changed")
    }
    _ = try live.feed(packet(2, 258, flags: 2, pts: 257))
    try live.finish()
    try require(live.testCounters.bytes == config.count + 257 * WireProtocol.maxPayload &&
                live.testCounters.bytes > 2 * 1024 * 1024 * 1024 && live.testCounters.frames == 257 &&
                live.testCounters.bufferedBytes == 0, "live cumulative traffic was capped at 2 GiB")

    let byteOverflow = WireVideoParser(maxTotalBytes: nil)
    byteOverflow.seedCountersForTesting(bytes: Int.max - config.count, frames: 0)
    _ = try byteOverflow.feed(packet(1, 0, config))
    try rejects("cumulative bytes overflow accepted") {
        _ = try byteOverflow.feed(packet(2, 1, flags: 1, declaredLength: UInt32(idr.count)))
    }
    let frameOverflow = WireVideoParser(maxTotalBytes: nil)
    frameOverflow.seedCountersForTesting(bytes: 0, frames: Int.max)
    _ = try frameOverflow.feed(packet(1, 0, config))
    try rejects("cumulative frame count overflow accepted") { _ = try frameOverflow.feed(packet(2, 1, idr, flags: 1)) }
    let maximumCounter = try WireProtocol.addCount(Int.max - 1, 1)
    try require(maximumCounter == Int.max, "representable counter maximum rejected")
    try rejects("counter overflow accepted") { _ = try WireProtocol.addCount(Int.max, 1) }
    try rejects("negative initial counter accepted") { _ = try WireProtocol.addCount(-1, 1) }
    try rejects("negative counter increment accepted") { _ = try WireProtocol.addCount(1, -1) }
    try rejects("unlimited live mode removed single-packet cap") {
        _ = try WireVideoParser(maxTotalBytes: nil).feed(packet(2, 0, declaredLength: UInt32.max))
    }
    return 8
}

private func inputIntegrationTests(path: String, overflow: Bool) throws {
    let fixture = try Fixture(path: path, mode: overflow ? "--serve-input-overflow-fixture" : "--serve-input-fixture")
    let done = DispatchSemaphore(value: 0)
    let lock = NSLock()
    var outcome: Result<Void, Error>?
    var enabledObserved = false, revokedObserved = false, requested = false
    var client: LANConnection!
    client = LANConnection(testControlPort: fixture.controlPort, testVideoPort: fixture.videoPort,
        onStatus: { status in
            if status == "远程输入已启用" && !enabledObserved {
                enabledObserved = true
                if overflow {
                    // Runs on the connection queue: the drain cannot execute until
                    // this callback returns, making the 129-edge overflow deterministic.
                    for i in 0..<129 { client.sendInput(["type": "key", "code": "KEY_A", "action": i % 2 == 0 ? "down" : "up"]) }
                } else {
                    client.sendInput(["type": "mouse_move", "x": 0.5, "y": 0.5])
                    client.sendInput(["type": "mouse_move", "x": 0.75, "y": 0.25])
                    client.sendInput(["type": "mouse_button", "button": "left", "action": "down"])
                    client.sendInput(["type": "mouse_button", "button": "left", "action": "up"])
                    client.sendInput(["type": "key", "code": "KEY_CTRL_LEFT", "action": "down"])
                    client.sendInput(["type": "key", "code": "KEY_CTRL_LEFT", "action": "up"])
                    client.sendInput(["type": "scroll", "dx": 1, "dy": -1])
                    client.releaseInputs()
                }
            }
            if status == "远程输入已关闭" && enabledObserved { revokedObserved = true }
        }, onPacket: { _ in
            if !requested { requested = true; client.requestInput(enabled: true) }
        }, onEnd: { result in lock.lock(); outcome = result; lock.unlock(); done.signal() })
    client.connect(host: "127.0.0.1", pin: fixture.pin)
    try require(done.wait(timeout: .now() + 12) == .success, "input integration timed out")
    lock.lock(); let final = outcome; lock.unlock()
    guard let final else { throw TestFailure(description: "missing input completion") }
    if overflow {
        guard case .failure = final else { throw TestFailure(description: "128-event backpressure did not fail closed") }
        try require(client.snapshot.inputSent <= 1, "overflow batch partially injected")
    } else {
        try final.get()
        try require(client.snapshot.inputSent == 8, "accepted input/enable/release send count differs")
        try require(revokedObserved, "Host local revoke was not notified")
    }
    try require(client.snapshot.inputSupported && !client.snapshot.inputEnabled && enabledObserved, "input capability/lifecycle state differs")
    try fixture.waitForExit()
    client = nil
}

// A deterministic wire peer is needed here: a real Host observes video EOF and
// may close control first, hiding whether the Mac incorrectly sent an explicit
// stop. This peer retains control until the Mac closes it, and records the bytes.
private final class CleanupWireFixture {
    let controlPort: UInt16
    let videoPort: UInt16
    private let controlListener: Int32
    private let videoListener: Int32
    private let lock = NSLock()
    private let finished = DispatchSemaphore(value: 0)
    private var videoSocket: Int32 = -1
    private var controls: [String] = []
    private var failure: String?

    init() throws {
        let control = try Self.listen()
        controlListener = control.0; controlPort = control.1
        do {
            let video = try Self.listen()
            videoListener = video.0; videoPort = video.1
        } catch { Darwin.close(control.0); throw error }
        DispatchQueue(label: "HarmonyRemote.CleanupWireFixture").async { [self] in
            do { try run() } catch { lock.lock(); failure = String(describing: error); lock.unlock() }
            finished.signal()
        }
    }

    deinit { Darwin.close(controlListener); Darwin.close(videoListener) }

    private static func listen() throws -> (Int32, UInt16) {
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw TestFailure(description: "cleanup fixture socket failed") }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let bound = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 && Darwin.listen(fd, 1) == 0 else {
            Darwin.close(fd); throw TestFailure(description: "cleanup fixture bind/listen failed")
        }
        var size = socklen_t(MemoryLayout<sockaddr_in>.size)
        let named = withUnsafeMutablePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &size) }
        }
        guard named == 0 else { Darwin.close(fd); throw TestFailure(description: "cleanup fixture port lookup failed") }
        return (fd, UInt16(bigEndian: address.sin_port))
    }

    private func accept(_ listener: Int32) throws -> Int32 {
        var event = pollfd(fd: listener, events: Int16(POLLIN), revents: 0)
        guard poll(&event, 1, 5000) > 0 else { throw TestFailure(description: "cleanup fixture accept timeout") }
        let fd = Darwin.accept(listener, nil, nil)
        guard fd >= 0 else { throw TestFailure(description: "cleanup fixture accept failed") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        return fd
    }

    private func read(_ count: Int, socket: Int32) throws -> Data? {
        var data = Data()
        while data.count < count {
            var block = [UInt8](repeating: 0, count: count - data.count)
            let received = recv(socket, &block, block.count, 0)
            if received == 0 && data.isEmpty { return nil }
            if received < 0 && errno == EINTR { continue }
            guard received > 0 else { throw TestFailure(description: "cleanup fixture partial read/timeout") }
            data.append(contentsOf: block.prefix(received))
        }
        return data
    }

    private func readObject(_ socket: Int32) throws -> [String: Any]? {
        guard let header = try read(4, socket: socket) else { return nil }
        let length = Int(WireProtocol.uint(header))
        guard (1...4096).contains(length), let body = try read(length, socket: socket) else {
            throw TestFailure(description: "cleanup fixture invalid control frame")
        }
        return try WireProtocol.controlObject(body)
    }

    private func write(_ data: Data, socket: Int32) throws {
        try data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
            var offset = 0
            while offset < bytes.count {
                let sent = Darwin.send(socket, bytes.baseAddress!.advanced(by: offset), bytes.count - offset, 0)
                if sent < 0 && errno == EINTR { continue }
                guard sent > 0 else { throw TestFailure(description: "cleanup fixture send failed") }
                offset += sent
            }
        }
    }

    private func run() throws {
        let control = try accept(controlListener)
        defer { Darwin.close(control) }
        let greeting = try readObject(control)
        try require(greeting?["type"] as? String == "hello", "cleanup fixture expected hello")
        try write(WireProtocol.controlFrame(["type": "hello_ack", "protocol": 1, "pairingRequired": true,
            "timestampSource": "encoder_callback_monotonic", "nativePtsUnitVerified": false,
            "inputSupported": true]), socket: control)
        let pairing = try readObject(control)
        try require(pairing?["type"] as? String == "pair", "cleanup fixture expected pair")
        let token = String(repeating: "ab", count: 32)
        try write(WireProtocol.controlFrame(["type": "pair_ok", "sessionToken": token]), socket: control)
        let video = try accept(videoListener)
        defer { lock.lock(); videoSocket = -1; lock.unlock(); Darwin.close(video) }
        lock.lock(); videoSocket = video; lock.unlock()
        let attach = try readObject(video)
        try require(attach?["type"] as? String == "video_attach", "cleanup fixture expected video attach")
        try write(WireProtocol.controlFrame(["type": "video_ready"]), socket: video)
        try write(packet(1, 0, config) + packet(2, 1, idr, flags: 1), socket: video)
        while let object = try readObject(control) {
            if let type = object["type"] as? String {
                lock.lock(); controls.append(type); lock.unlock()
                if type == "ping" {
                    try write(WireProtocol.controlFrame(["type": "pong", "sessionToken": token]), socket: control)
                }
            }
        }
    }

    func breakOnlyVideoOutput() throws {
        lock.lock(); defer { lock.unlock() }
        try require(videoSocket >= 0 && shutdown(videoSocket, SHUT_WR) == 0, "cleanup fixture could not half-close video")
    }

    func waitForControls() throws -> [String] {
        try require(finished.wait(timeout: .now() + 6) == .success, "cleanup fixture control did not close")
        lock.lock(); let error = failure; let captured = controls; lock.unlock()
        if let error { throw TestFailure(description: error) }
        return captured
    }
}

private func transportCleanupIntegrationTests(manual: Bool) throws {
    let fixture = try CleanupWireFixture()
    let firstFrame = DispatchSemaphore(value: 0), completed = DispatchSemaphore(value: 0)
    let lock = NSLock()
    var outcome: Result<Void, Error>?
    var completions = 0
    let client = LANConnection(testControlPort: fixture.controlPort, testVideoPort: fixture.videoPort,
        onStatus: { _ in }, onPacket: { if $0.type == 2 { firstFrame.signal() } }, onEnd: { result in
            lock.lock(); outcome = result; completions += 1; lock.unlock(); completed.signal()
        })
    client.connect(host: "127.0.0.1", pin: "123456")
    defer { client.disconnect() }
    try require(firstFrame.wait(timeout: .now() + 6) == .success, "cleanup test did not receive first AU")
    try require(client.snapshot.paired && client.snapshot.videoReady && client.snapshot.receivedFrames == 1,
                "cleanup test must start from a live paired video connection")
    if manual { client.disconnect() } else { try fixture.breakOnlyVideoOutput() }
    try require(completed.wait(timeout: .now() + 6) == .success, "cleanup test client did not finish")
    let captured = try fixture.waitForControls().filter { $0 != "ping" && $0 != "pong" }
    lock.lock(); let final = outcome; let count = completions; lock.unlock()
    try require(count == 1 && !client.snapshot.inputEnabled, "cleanup did not finish exactly once with input disabled")
    if manual {
        guard case .failure(let error)? = final, case LANError.cancelled = error else {
            throw TestFailure(description: "manual disconnect did not remain explicit cancellation")
        }
        try require(captured == ["release_all_keys", "stop"], "manual disconnect must release input before explicit stop")
    } else {
        guard case .failure(let error)? = final, case LANError.connectionClosed = error else {
            throw TestFailure(description: "video-only EOF did not report a retryable transport failure")
        }
        try require(captured.isEmpty, "video-only failure sent stop over live control, cancelling Host capture grace")
    }
}

@main
private enum NetworkTests {
    static func main() throws {
        let parserCount = try parserTests()
        let controlCount = try controlTests()
        guard let fixture = ProcessInfo.processInfo.environment["LAN_SERVER_FIXTURE"] else {
            throw TestFailure(description: "LAN_SERVER_FIXTURE is required; do not skip actual C++ interoperability")
        }
        let integrationCount = try integrationTests(path: fixture)
        let inputCount = try inputValidationTests()
        let deadlineCount = try sessionDeadlineTests()
        let longStreamCount = try longStreamParserTests()
        try sessionLifetimeIntegrationTests(path: fixture, duringStream: false)
        try sessionLifetimeIntegrationTests(path: fixture, duringStream: true)
        try inputIntegrationTests(path: fixture, overflow: false)
        try inputIntegrationTests(path: fixture, overflow: true)
        try transportCleanupIntegrationTests(manual: false)
        try transportCleanupIntegrationTests(manual: true)
        print("mac network tests passed: \(parserCount) parser, \(controlCount) control/address, \(integrationCount) NWConnection integration/lifecycle")
        print("input tests passed: \(inputCount) validation/cap cases, 2 actual C++ ordered-input/revoke/overflow fixtures")
        print("session deadline tests passed: \(deadlineCount) simulated-clock cases; bounded first-AU wait, no active lifetime limit")
        print("session receive tests passed: 2 actual C++/NWConnection fixtures; expired waiting rejects video, active stream survives simulated one-year clock advance")
        print("long stream tests passed: \(longStreamCount) cases; >2 GiB processed in bounded batches, overflow guarded, replay64MiB/single-packet limits retained")
        print("transport cleanup tests passed: 2 real NWConnection/POSIX peers; video-only EOF keeps control free of stop, manual cancel sends release then stop")
        print("Actual C++ fixture payload matched byte-for-byte; this does not verify a real device or decoder.")
    }
}
