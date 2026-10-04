import Foundation
import AppKit
import Darwin

private let epoch = String(repeating: "a", count: 32)
private func check(_ value: @autoclosure () -> Bool, _ message: String = "check", line: Int = #line) {
    if !value() { fatalError("\(message) at line \(line)") }
}
private func rejects(_ body: () throws -> Void, line: Int = #line) {
    do { try body(); fatalError("unexpected success at line \(line)") } catch {}
}
private func update(_ counter: UInt64 = 1, _ data: Data = Data("测试\n text ".utf8), origin: String = "harmony") -> ClipboardFrame {
    ClipboardWire.make("clipboard_update", epoch: epoch,
        extra: ["originId": origin, "counter": String(counter), "eventId": "\(origin)-\(counter)",
                "mime": ClipboardWire.mime, "sha256": ClipboardWire.digest(data)], payload: data)
}
private func applied(_ frame: ClipboardFrame, status: String = "applied", version: ClipboardVersion? = nil) -> ClipboardFrame {
    let current = version ?? frame.version!
    return ClipboardWire.make("clipboard_applied", epoch: epoch,
        extra: ["eventId": frame.version!.eventID, "counter": String(current.counter), "originId": current.origin,
                "status": status, "error": ""], messageID: frame.messageID)
}
private final class FakeClipboard: ClipboardAdapter {
    var revision = 1
    var data = Data("old untouched".utf8)
    var owner: String?
    var reads = 0
    var writes = 0
    var failRead: ClipboardFailure?
    var failWrite: ClipboardFailure?
    var race = false
    var duringRead: (() -> Void)?
    func copy(_ value: String) { revision += 1; data = Data(value.utf8); owner = nil }
    func read(explicit: Bool) throws -> ClipboardSnapshot {
        reads += 1
        if let failRead { throw failRead }
        let result = ClipboardSnapshot(revision: revision, text: data, owner: owner)
        duringRead?()
        if race { revision += 1 }
        return result
    }
    func write(_ text: Data, owner: String, expectedRevision: Int) throws -> Int {
        if let failWrite { throw failWrite }
        guard revision == expectedRevision else { throw ClipboardFailure.changed }
        revision += 1; data = text; self.owner = owner; writes += 1; return revision
    }
}
private final class Rig {
    let board = FakeClipboard()
    var time: TimeInterval = 0
    var frames: [ClipboardFrame] = []
    var commits: [(String, String, String, Int)] = []
    lazy var engine = ClipboardCoordinator(adapter: board, now: { [unowned self] in time })
    init(_ mode: ClipboardMode = .both, allowed: Bool = true) {
        engine.onSend = { [unowned self] in frames.append($0) }
        engine.onCommit = { [unowned self] in commits.append(($0, $1, $2, $3)) }
        engine.start(epoch: epoch, mode: mode); engine.ready(); status(allowed: allowed)
    }
    func status(allowed: Bool = true) {
        try! engine.receive(ClipboardWire.make("clipboard_status", epoch: epoch,
            extra: ["allowed": allowed, "canRead": false, "canWrite": false, "mode": engine.mode.rawValue, "error": ""],
            messageID: frames.last(where: { $0.type == "clipboard_mode" })!.messageID))
    }
    var updates: [ClipboardFrame] { frames.filter { $0.type == "clipboard_update" } }
}

private enum PeerError: Error { case socket, io }
private final class ClipboardPeer {
    let listener: Int32
    let port: UInt16
    private let lock = NSLock()
    private var _done = false
    private var _error: Error?
    var done: Bool { lock.lock(); defer { lock.unlock() }; return _done }
    var error: Error? { lock.lock(); defer { lock.unlock() }; return _error }
    init(_ body: @escaping (Int32) throws -> Void) throws {
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0); guard fd >= 0 else { throw PeerError.socket }
        listener = fd
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let bound = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        } }
        guard bound == 0, Darwin.listen(fd, 1) == 0 else { Darwin.close(fd); throw PeerError.socket }
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let named = withUnsafeMutablePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) } }
        guard named == 0 else { Darwin.close(fd); throw PeerError.socket }; port = UInt16(bigEndian: address.sin_port)
        DispatchQueue.global().async { [self] in
            let client = accept(fd, nil, nil)
            var error: Error?
            if client >= 0 {
                var timeout = timeval(tv_sec: 8, tv_usec: 0); var one: Int32 = 1
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout.size(ofValue: timeout)))
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one)))
                do { try body(client) } catch let caught { error = caught }
                Darwin.close(client)
            } else { error = PeerError.socket }
            lock.lock(); _error = error; _done = true; lock.unlock()
        }
    }
    deinit { shutdown(listener, SHUT_RDWR); Darwin.close(listener) }
    static func read(_ fd: Int32, _ count: Int) throws -> Data {
        var result = Data()
        while result.count < count {
            var bytes = [UInt8](repeating: 0, count: min(65536, count - result.count))
            let got = recv(fd, &bytes, bytes.count, 0); guard got > 0 else { throw PeerError.io }
            result.append(contentsOf: bytes.prefix(got))
        }
        return result
    }
    static func frame(_ fd: Int32) throws -> ClipboardFrame {
        let size = Int(WireProtocol.uint(try read(fd, 4))); guard size > 0, size <= 4096 else { throw PeerError.io }
        let header = try ClipboardWire.header(try read(fd, size), epoch: epoch)
        let frame = ClipboardFrame(header: header, payload: try read(fd, header["payloadLength"] as! Int))
        try ClipboardWire.validate(frame, epoch: epoch); return frame
    }
    static func send(_ fd: Int32, _ data: Data, fragmented: Bool = false) throws {
        var offset = 0
        while offset < data.count {
            let size = fragmented ? 1 : data.count - offset
            let sent = data.withUnsafeBytes { Darwin.send(fd, $0.baseAddress!.advanced(by: offset), size, 0) }
            guard sent > 0 else { throw PeerError.io }; offset += sent
        }
    }
}
private func runMain(until condition: () -> Bool, timeout: TimeInterval = 8, context: String = "channel") {
    let deadline = Date().addingTimeInterval(timeout)
    while !condition() && Date() < deadline { _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.01)) }
    check(condition(), "main-loop fixture timed out: \(context)")
}
private func channelTest(_ scenario: Int) throws {
    let rig = Rig(); rig.engine.stop(); rig.frames.removeAll()
    let peer = try ClipboardPeer { fd in
        let binding = try ClipboardPeer.frame(fd); check(binding.type == "data_bind")
        let ready = try ClipboardWire.encode(ClipboardWire.make("data_ready", epoch: epoch), epoch: epoch)
        if scenario == 1 { try ClipboardPeer.send(fd, Data([0,0])); Thread.sleep(forTimeInterval: 5.3); return }
        if scenario == 2 { try ClipboardPeer.send(fd, Data([0,0,16,1])); return }
        try ClipboardPeer.send(fd, ready, fragmented: true)
        let mode = try ClipboardPeer.frame(fd); check(mode.type == "clipboard_mode")
        let status = ClipboardWire.make("clipboard_status", epoch: epoch,
            extra: ["allowed": true,"canRead": false,"canWrite": false,"mode": 3,"error": ""], messageID: mode.messageID)
        if scenario == 3 { Thread.sleep(forTimeInterval: 5.2); return }
        var data = try ClipboardWire.encode(status, epoch: epoch)
        data.append(try ClipboardWire.encode(update(), epoch: epoch))
        try ClipboardPeer.send(fd, data)
        let ack = try ClipboardPeer.frame(fd); check(ack.type == "clipboard_applied" && ack.header["status"] as? String == "applied")
    }
    var failed: ClipboardFailure?
    var ready = false
    let session = ClipboardSession(host: "127.0.0.1", epoch: epoch, binding: String(repeating: "b", count: 64))
    let channel = ClipboardChannel(testPort: peer.port, session: session, onReady: { ready = true; rig.engine.ready() },
        onFrame: { frame in try! rig.engine.receive(frame) }, onFailure: { failed = $0 })
    rig.engine.start(epoch: epoch, mode: .both); rig.engine.onSend = { channel.send($0) }; channel.start()
    if scenario == 3 {
        let started = ProcessInfo.processInfo.systemUptime
        runMain(until: { ready && ProcessInfo.processInfo.systemUptime - started > 5.05 })
        check(failed == nil, "idle clipboard was mistaken for partial-frame timeout")
    }
    runMain(until: { peer.done && failed != nil }); channel.close()
    if let error = peer.error { throw error }
    if scenario == 0 { check(ready && rig.board.writes == 1 && failed == .transport) }
    if scenario == 1 { check(!ready && failed == .timeout) }
    if scenario == 2 { check(!ready && failed == .malformed) }
}

private func nativeIntegration(_ path: String) throws {
    let process = Process(); let output = Pipe(); let errors = Pipe()
    process.executableURL = URL(fileURLWithPath: path); process.arguments = ["--serve-fixture"]
    process.standardOutput = output; process.standardError = errors; try process.run()
    defer { if process.isRunning { process.terminate() }; process.waitUntilExit() }
    var line = Data()
    while line.count < 16384 {
        let byte = output.fileHandleForReading.readData(ofLength: 1)
        guard !byte.isEmpty else { throw PeerError.io }
        if byte[0] == 10 { break }; line.append(byte)
    }
    // The readiness line contains a random fixture PIN. Consume only in memory.
    let ready = try JSONSerialization.jsonObject(with: line) as! [String: Any]
    guard let control = ready["controlPort"] as? UInt16, let video = ready["videoPort"] as? UInt16,
          let pin = ready["pin"] as? String else { throw PeerError.io }
    let board = FakeClipboard(); let engine = ClipboardCoordinator(adapter: board)
    var channel: ClipboardChannel?
    var failure: Error?
    var commitRequests = 0
    var nativePasteResults = 0
    var connection: LANConnection!
    connection = LANConnection(testControlPort: control, testVideoPort: video,
        onStatus: { _ in }, onPacket: { _ in }, onEnd: { result in
            if case .failure(let error) = result { DispatchQueue.main.async { failure = error } }
        }, onClipboardSession: { session in
            DispatchQueue.main.async {
                guard let session else { channel?.close(); return }
                engine.start(epoch: session.epoch, mode: .both)
                channel = ClipboardChannel(testPort: session.port, session: session,
                    onReady: { engine.ready() }, onFrame: { frame in
                        do { try engine.receive(frame) } catch { failure = error }
                    }, onFailure: { failure = $0 })
                channel?.start()
            }
        }, onPasteResult: { result in DispatchQueue.main.async { nativePasteResults += 1; engine.pasteResult(result) } })
    engine.onSend = { channel?.send($0) }
    engine.onCommit = {
        guard let permit = engine.pastePermit else { return }
        commitRequests += 1
        connection.commitPaste(epoch: $0, event: $1, operation: $2, ttl: $3, permit: permit)
    }
    connection.connect(host: "127.0.0.1", pin: pin)
    defer { channel?.close(); connection.disconnect() }
    func until(_ stage: String, _ predicate: () -> Bool) throws {
        runMain(until: { engine.tick(); return failure != nil || predicate() }, timeout: 10, context: stage)
        if let failure { throw failure }
    }
    try until("native mode ACK and first video AU") { engine.allowed && connection.snapshot.videoReady && connection.snapshot.receivedFrames > 0 }
    check(board.reads == 0 && board.writes == 0, "interop performed initial sync")
    connection.requestInput(enabled: true); try until("native input enabled") { connection.snapshot.inputEnabled }
    board.copy("Mac fixture 中文\r\n e\u{301}🙂 "); engine.poll()
    try until("native reverse update") { engine.appliedCount == 1 }
    check(board.data == Data("Harmony fixture → Mac\n末尾空格 ".utf8), "native to Mac payload mismatch")
    func pauseWriter(_ paused: Bool) {
        var done = false
        connection.pauseControlWriterForTest(paused) { DispatchQueue.main.async { done = true } }
        runMain(until: { done }, context: "test control writer gate")
    }
    // Clipboard uses its independent socket while the control writer is occupied.
    // A late focus release or timeout must revoke an already-queued paste_commit.
    pauseWriter(true); engine.requestPaste(valid: { true })
    try until("paste queued behind busy writer") { commitRequests == 1 }
    engine.cancelPaste(); pauseWriter(false)
    var waitUntil = ProcessInfo.processInfo.systemUptime + 0.2
    try until("cancelled control queue drained") { ProcessInfo.processInfo.systemUptime >= waitUntil }
    check(nativePasteResults == 0, "focus-cancelled queued paste reached native")
    pauseWriter(true); engine.requestPaste(valid: { true })
    try until("second paste queued behind busy writer") { commitRequests == 2 }
    waitUntil = ProcessInfo.processInfo.systemUptime + 1.6
    try until("queued paste deadline expires") { ProcessInfo.processInfo.systemUptime >= waitUntil }
    pauseWriter(false); waitUntil = ProcessInfo.processInfo.systemUptime + 0.2
    try until("expired control queue drained") { ProcessInfo.processInfo.systemUptime >= waitUntil }
    check(nativePasteResults == 0, "expired queued paste reached native")
    connection.releaseInputs(); engine.requestPaste(valid: { true })
    try until("native paste result") { engine.committedCount == 1 }
    check(engine.sentCount == 4 && nativePasteResults == 1, "paste emitted more than once or lost a fresh Mac event")
    engine.pull(); try until("native explicit pull") { engine.appliedCount == 2 }
    check(board.data == Data("Harmony fixture → Mac\n末尾空格 ".utf8))
    engine.setMode(.off); try until("native off") { !engine.allowed || engine.mode == .off }
    check(engine.committedCount == 1)
}

@main private enum ClipboardTests {
    static func main() throws {
        setbuf(stdout, nil)
        var count = 0
        func test(_ name: String, _ body: () throws -> Void) rethrows { try body(); count += 1; print("PASS \(name)") }
        try test("Unicode whitespace empty and maximum plaintext round trip") {
            for data in [Data(), Data(" 中文\r\n e\u{301}🙂 \t".utf8), Data(repeating: 65, count: ClipboardWire.maxPayload)] {
                let original = update(1, data); let bytes = try ClipboardWire.encode(original, epoch: epoch)
                let length = Int(WireProtocol.uint(bytes.prefix(4)))
                let header = try ClipboardWire.header(bytes.subdata(in: 4..<(4 + length)), epoch: epoch)
                let frame = ClipboardFrame(header: header, payload: bytes.suffix(data.count))
                try ClipboardWire.validate(frame, epoch: epoch); check(frame.payload == data)
            }
        }
        test("oversized malformed UTF8 and embedded NUL rejected") {
            for data in [Data(repeating: 65, count: ClipboardWire.maxPayload + 1), Data([0xc0,0xaf]), Data([0xed,0xa0,0x80]), Data([65,0,66])] {
                rejects { _ = try ClipboardWire.encode(update(1, data), epoch: epoch) }
            }
        }
        test("mismatched hash and length rejected") {
            var frame = update(); frame.header["sha256"] = String(repeating: "0", count: 64)
            rejects { try ClipboardWire.validate(frame, epoch: epoch) }
            frame = update(); frame.header["payloadLength"] = frame.payload.count + 1
            rejects { try ClipboardWire.validate(frame, epoch: epoch) }
        }
        test("canonical uint64 rejects precision loss leading zeros negative overflow") {
            check(ClipboardWire.counter(String(UInt64.max)) == UInt64.max)
            for value: Any in ["01", "-1", "18446744073709551616", "1.0", "", 1, true] { check(ClipboardWire.counter(value) == nil) }
        }
        test("strict headers reject extra field epoch duplicates nonascii bool/floating integers") {
            for (key, value): (String, Any) in [("extra",1),("version",true),("payloadLength",1.5),("sessionEpoch",String(repeating:"b",count:32)),("error","中文")] {
                var frame = update(); frame.header[key] = value
                rejects { try ClipboardWire.validate(frame, epoch: epoch) }
            }
            let body = "{\"type\":\"data_ready\",\"version\":1,\"sessionEpoch\":\"\(epoch)\",\"messageId\":\"\(epoch)\",\"payloadLength\":0,\"type\":\"data_ready\"}"
            rejects { _ = try ClipboardWire.header(Data(body.utf8), epoch: epoch) }
            rejects { _ = try ClipboardWire.header(Data(body.replacingOccurrences(of: "\"version\":1", with: "\"version\":1e0").utf8), epoch: epoch) }
        }
        test("error size and event zero consistency enforced") {
            var frame = applied(update()); frame.header["error"] = String(repeating: "E", count: 129)
            rejects { try ClipboardWire.validate(frame, epoch: epoch) }
            frame = applied(update(), version: .zero); try! ClipboardWire.validate(frame, epoch: epoch)
            frame.header["originId"] = "mac"; rejects { try ClipboardWire.validate(frame, epoch: epoch) }
        }
        test("no initial sync mode change or reconnect reads old clipboard") {
            let r = Rig(); r.engine.tick(); check(r.board.reads == 0 && r.updates.isEmpty)
            r.engine.setMode(.toHarmony); r.status(); r.engine.tick(); check(r.board.reads == 0)
            r.engine.stop(); r.board.copy("during disconnect"); r.engine.start(epoch: epoch, mode: .both); r.engine.ready(); r.status(); r.engine.tick()
            check(r.updates.isEmpty && r.board.reads == 0)
        }
        test("first write permitted with diagnostic canWrite=false and repeated text remains fresh event") {
            let r = Rig(); r.board.copy("same"); r.engine.tick(); check(r.updates.count == 1)
            try! r.engine.receive(applied(r.updates[0])); r.board.copy("same"); r.engine.tick()
            check(r.updates.count == 2 && r.updates[1].version!.counter == 2)
        }
        test("owner revision content writeback suppresses only exact current write") {
            let r = Rig(); try! r.engine.receive(update()); check(r.board.writes == 1)
            r.engine.tick(); check(r.updates.isEmpty)
            r.board.copy(String(data: r.board.data, encoding: .utf8)!); r.engine.tick(); check(r.updates.count == 1)
            check(r.updates[0].version!.counter == 2)
        }
        test("same revision content changes cannot reuse paste event") {
            let r = Rig(); r.board.copy("one"); r.engine.tick(); try! r.engine.receive(applied(r.updates[0]))
            r.board.data = Data("two".utf8); r.engine.requestPaste(valid: { true }); check(r.commits.isEmpty && r.updates.count == 2)
        }
        test("mode disabled and local Host permission both gate operations") {
            let r = Rig(.off); r.board.copy("new"); r.engine.tick(); check(r.updates.isEmpty)
            try! r.engine.receive(update()); check(r.board.writes == 0 && r.frames.last?.header["status"] as? String == "denied")
            r.engine.setMode(.both); r.status(allowed: false); r.engine.requestPaste(valid: { true }); check(r.commits.isEmpty)
        }
        test("selected sending direction survives channel failure and denied Host without raw-paste fallback") {
            let r = Rig(); r.engine.stop(reason: "test channel failure")
            check(r.engine.mode.sends && !r.engine.canPaste)
            r.engine.requestPaste(valid: { true }); check(r.commits.isEmpty && r.engine.status.contains("通道未连接"))
            let denied = Rig(.toHarmony, allowed: false)
            check(denied.engine.mode.sends && !denied.engine.canPaste)
            denied.engine.requestPaste(valid: { true }); check(denied.commits.isEmpty && denied.updates.isEmpty)
        }
        test("remote-only receives without local read and continues without video focus") {
            let r = Rig(.toMac); try! r.engine.receive(update())
            check(r.board.writes == 1 && r.board.reads == 0 && r.updates.isEmpty)
        }
        test("receive-only local revision fences out delayed remote update without reading or sending local text") {
            let r = Rig(.toMac); r.board.copy("local new"); try! r.engine.receive(update(7))
            check(r.board.writes == 0 && r.board.reads == 0 && r.updates.isEmpty)
            check(r.frames.last?.header["status"] as? String == "stale" && r.frames.last?.header["counter"] as? String == "8")
            try! r.engine.receive(update(9)); check(r.board.writes == 1)
        }
        test("Lamport capture of intervening local copy wins stale remote event") {
            let r = Rig(); r.board.copy("local"); try! r.engine.receive(update(50))
            check(r.board.writes == 0 && r.updates[0].version!.counter == 51)
            check(r.frames.last?.header["status"] as? String == "stale")
        }
        test("equal counter ASCII origin ordering rejects remote harmony after mac") {
            let r = Rig(); r.board.copy("local"); r.engine.tick(); try! r.engine.receive(update(1))
            check(r.board.writes == 0)
        }
        test("duplicate remote update id is idempotent only when payload agrees") {
            let r = Rig(); let frame = update(); try! r.engine.receive(frame); try! r.engine.receive(frame)
            check(r.board.writes == 1 && r.frames.last?.header["status"] as? String == "applied")
            try! r.engine.receive(update(1, Data("different".utf8)))
            check(r.board.writes == 1 && r.frames.last?.header["status"] as? String == "stale")
        }
        test("one in-flight plus latest outbound drops obsolete copies without queue growth") {
            let r = Rig(); for i in 0..<100 { r.board.copy("sample \(i)"); r.engine.tick() }
            check(r.updates.count == 1); try! r.engine.receive(applied(r.updates[0])); check(r.updates.count == 2)
            check(r.updates[1].payload == Data("sample 99".utf8))
        }
        test("paste waits applied exact message event and current version then commits once") {
            let r = Rig(); r.engine.requestPaste(valid: { true }); check(r.updates.count == 1 && r.commits.isEmpty)
            var wrong = applied(r.updates[0]); wrong.header["messageId"] = ClipboardWire.identifier()
            try! r.engine.receive(wrong); check(r.commits.isEmpty)
            try! r.engine.receive(applied(r.updates[0])); check(r.commits.count == 1)
            try! r.engine.receive(applied(r.updates[0])); check(r.commits.count == 1)
        }
        test("explicit paste after remote event writes a new Mac event before commit") {
            let r = Rig(); try! r.engine.receive(update()); r.engine.requestPaste(valid: { true })
            check(r.updates.count == 1 && r.commits.isEmpty && r.updates[0].version?.eventID == "mac-2")
            try! r.engine.receive(applied(r.updates[0])); check(r.commits.count == 1 && r.commits[0].1 == "mac-2")
        }
        test("focus loss timeout mode change disconnect cancel before delayed applied") {
            for reason in 0..<4 {
                let r = Rig(); var focus = true; r.engine.requestPaste(valid: { focus }); let frame = r.updates[0]
                switch reason { case 0: focus = false; case 1: r.time = 2; case 2: r.engine.setMode(.off); default: r.engine.stop() }
                if reason != 3 { try! r.engine.receive(applied(frame)); r.engine.tick() }
                check(r.commits.isEmpty)
            }
        }
        test("copy after request or read race never commits stale content") {
            let r = Rig(); r.engine.requestPaste(valid: { true }); let frame = r.updates[0]; r.board.copy("new")
            try! r.engine.receive(applied(frame)); check(r.commits.isEmpty)
            let race = Rig(); race.board.race = true; race.engine.requestPaste(valid: { true }); check(race.updates.isEmpty)
        }
        test("platform read returning after mode or session change cannot publish stale result") {
            for stop in [false, true] {
                let r = Rig(); r.board.duringRead = { if stop { r.engine.stop() } else { r.engine.setMode(.off) } }
                r.engine.requestPaste(valid: { true }); check(r.updates.isEmpty && r.commits.isEmpty)
                r.board.duringRead = nil
            }
        }
        test("write failure never sends applied and no fake success on denied read") {
            let r = Rig(); r.board.failWrite = .write; try! r.engine.receive(update())
            check(r.frames.last?.header["status"] as? String == "failed")
            r.board.failRead = .denied; r.engine.requestPaste(valid: { true }); check(r.commits.isEmpty && r.updates.isEmpty)
        }
        test("overflowing Lamport rejected and explicit pull allowed before first successful read") {
            let r = Rig(); try! r.engine.receive(update(UInt64.max)); r.board.copy("after max"); r.engine.tick(); check(r.updates.isEmpty)
            r.engine.pull(); check(r.frames.last?.type == "clipboard_pull")
        }
        test("permission re-enable resets baseline without sending stale local copy") {
            let r = Rig(); r.status(allowed: false); r.board.copy("during denied"); r.status(); r.engine.tick(); check(r.updates.isEmpty)
        }
        test("mode acknowledgement barrier rejects queued old frames and late status") {
            let r = Rig(); let previousMode = r.frames[0]
            r.engine.setMode(.toHarmony); r.engine.setMode(.both)
            r.board.copy("copy while awaiting new mode")
            r.engine.tick(); try! r.engine.receive(update()); check(r.updates.isEmpty && r.board.writes == 0)
            try! r.engine.receive(ClipboardWire.make("clipboard_status", epoch: epoch,
                extra: ["allowed": true, "canRead": true, "canWrite": true, "mode": 3, "error": ""], messageID: previousMode.messageID))
            check(!r.engine.allowed); r.status(); r.engine.tick(); check(r.updates.isEmpty && r.board.writes == 0)
            r.board.copy("after new mode"); r.engine.tick(); check(r.updates.count == 1)
        }
        test("Host read permission error is not reported as full bidirectional readiness") {
            let r = Rig(); try! r.engine.receive(ClipboardWire.make("clipboard_status", epoch: epoch,
                extra: ["allowed": true, "canRead": false, "canWrite": false, "mode": 3, "error": "read_permission_denied"]))
            check(r.engine.status.contains("未授权") && r.engine.status.contains("仍可尝试") && r.engine.canPaste)
        }
        test("paste result applies only matching operation and no automatic retry") {
            let r = Rig(); r.engine.requestPaste(valid: { true }); try! r.engine.receive(applied(r.updates[0]))
            r.engine.pasteResult(["operationId": "other", "status": "committed"]); check(r.engine.committedCount == 0)
            r.engine.pasteResult(["operationId": r.commits[0].2, "status": "committed"]); check(r.engine.committedCount == 1)
            r.time = 10; r.engine.tick(); check(r.commits.count == 1)
        }
        test("queued paste permit revoked by release, expiration, mode and disconnect") {
            for reason in 0..<4 {
                let r = Rig(); r.engine.requestPaste(valid: { true }); try! r.engine.receive(applied(r.updates[0]))
                let permit = r.engine.pastePermit!; check(permit.remainingMilliseconds() == 1500)
                switch reason { case 0: r.engine.cancelPaste(); case 1: r.time = 1.5; case 2: r.engine.setMode(.off); default: r.engine.stop() }
                check(permit.remainingMilliseconds() == nil)
            }
            var time = 0.0; let permit = ClipboardPastePermit(deadline: 1.5, now: { time })
            time = 1.0; check(permit.remainingMilliseconds() == 500)
        }
        test("CmdV mapping only MacFriendly, CtrlV either mode; other chords unchanged") {
            for mode in [RemoteKeyboardMode.macFriendly, .raw] {
                check(RemoteInputEngine.isClipboardPaste(key: 9, mode: mode, command: false, control: true, shift: false, option: false))
                check(RemoteInputEngine.isClipboardPaste(key: 9, mode: mode, command: true, control: false, shift: false, option: false) == (mode == .macFriendly))
                check(!RemoteInputEngine.isClipboardPaste(key: 8, mode: mode, command: true, control: false, shift: false, option: false))
            }
        }
        test("paste release clears held modifiers before barrier and next chord restores flags") {
            var keys: [String] = []; var releases = 0
            let input = RemoteInputEngine(onInput: { if let key = $0["code"] as? String { keys.append(key + " " + ($0["action"] as! String)) } }, onRelease: { releases += 1 })
            input.context(enabled: true, focused: true, windowActive: true, insideVideo: true)
            input.modifier(55, down: true); input.modifier(56, down: true); input.releaseAll()
            check(input.heldKeys.isEmpty && releases == 1 && !keys.contains("KEY_V down"))
            input.synchronizeModifiers(rawFlags: (1 << 20) | 8); input.shortcutStroke(37)
            check(keys.suffix(3) == ["KEY_CTRL_LEFT down", "KEY_L down", "KEY_L up"])
        }
        if CommandLine.arguments.contains("--named-pasteboard") {
            try test("AppKit private named board roundtrip owner and Unicode; General board untouched") {
                let board = NSPasteboard.withUniqueName(); defer { board.releaseGlobally() }
                let adapter = AppKitClipboardAdapter(board: board)
                let data = Data(" Clipboard fixture 中文\r\n🙂 e\u{301} ".utf8)
                let revision = try adapter.write(data, owner: "test-owner", expectedRevision: adapter.revision)
                let snapshot = try adapter.read(explicit: false)
                check(snapshot.text == data && snapshot.owner == "test-owner" && revision == snapshot.revision)
            }
            test("AppKit named file image promises excluded even alongside text; HTML allowed") {
                let board = NSPasteboard.withUniqueName(); defer { board.releaseGlobally() }
                let adapter = AppKitClipboardAdapter(board: board)
                for type in [NSPasteboard.PasteboardType.fileURL, .png, NSPasteboard.PasteboardType("com.apple.pasteboard.promised-file-url")] {
                    let item = NSPasteboardItem(); item.setString("fixture", forType: .string); item.setData(Data([1]), forType: type)
                    board.clearContents(); check(board.writeObjects([item])); rejects { _ = try adapter.read(explicit: true) }
                }
                let item = NSPasteboardItem(); item.setString("plain fixture", forType: .string); item.setString("<b>fixture</b>", forType: .html)
                board.clearContents(); check(board.writeObjects([item])); check(try! adapter.read(explicit: true).text == Data("plain fixture".utf8))
            }
        }
        if CommandLine.arguments.contains("--network") {
            try test("NWConnection real loopback fragmented bind plus coalesced status/update/ACK") { try channelTest(0) }
            try test("NWConnection partial header deadline is bounded to five seconds") { try channelTest(1) }
            try test("NWConnection oversized header rejected before payload allocation") { try channelTest(2) }
            try test("NWConnection healthy idle clipboard exceeds assembly deadline without timeout") { try channelTest(3) }
        }
        if let path = ProcessInfo.processInfo.environment["CLIPBOARD_SERVICE_FIXTURE"] {
            try test("actual native clipboard bidirection, mode barrier, pull, paste result, congested focus-cancel and expired commits") {
                try nativeIntegration(path)
            }
        }
        print("Mac clipboard tests passed: \(count). Fake state/clock and optional private named pasteboard only; no device or General clipboard validation.")
    }
}
