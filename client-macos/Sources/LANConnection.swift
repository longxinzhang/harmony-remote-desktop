import Foundation
import Network

struct LANConnectionSnapshot {
    var receivedFrames: Int = 0
    var receivedBytes: Int = 0
    // Successful nonempty access-unit receipt, not TCP activity or heartbeat.
    var lastReceivedFrameAt: TimeInterval?
    var paired: Bool = false
    var videoReady: Bool = false
    var inputSupported: Bool = false
    var inputEnabled: Bool = false
    var inputSent: Int = 0
    var rttMilliseconds: Double?
    var jitterMilliseconds: Double?
    var rttSamples = 0
    var videoMbps = 0.0
    var receivedFPS = 0.0
    var networkType = "未连接"
}

// Waiting for the user to start sharing is bounded. After the first validated,
// nonempty AU, Host controls stream duration; heartbeat/packet deadlines remain.
struct LANSessionDeadline {
    private var expiresAt: TimeInterval?
    private var receivedFirstAccessUnit = false

    init(startingAt now: TimeInterval) { expiresAt = now + 1920 }

    mutating func observe(_ packet: VideoPacket, at now: TimeInterval) {
        guard !hasExpired(at: now), !receivedFirstAccessUnit,
              packet.type == 2, !packet.payload.isEmpty else { return }
        receivedFirstAccessUnit = true
        expiresAt = nil
    }

    func hasExpired(at now: TimeInterval) -> Bool { expiresAt.map { now >= $0 } ?? false }
}

final class LANConnection {
    private let queue = DispatchQueue(label: "HarmonyRemote.LANConnection")
    private let snapshotLock = NSLock()
    private var statistics = LANConnectionSnapshot()
    private let onStatus: (String) -> Void
    private let onPacket: (VideoPacket) -> Void
    private let onEnd: (Result<Void, Error>) -> Void
    private let onClipboardSession: (ClipboardSession?) -> Void
    private let onPasteResult: ([String: Any]) -> Void
    private let onPairedIdentity: (PairingCredentials) -> Void
    private let onAudioSession: (AudioSession?) -> Void
    private var pairing: PairingCredentials?
    private var handshake: PairingHandshake?
    private var measurements = NetworkMeasurements()
    private var rttSupported = false
    private var audioPort: UInt16?
    private var clipboardAdvertised = false
    private var clipboardPort: UInt16 = 39873
    private var clipboardEpoch = ""
    private var control: NWConnection?
    private var video: NWConnection?
    private var timer: DispatchSourceTimer?
    private var generation: UInt64 = 0
    private var active = false
    private var host = ""
    private var pin = ""
    private var token: String?
    private var parser = WireVideoParser(maxTotalBytes: nil)
    private var sessionDeadline = LANSessionDeadline(startingAt: 0)
    private var phaseDeadline: TimeInterval = 0
    private var lastHeartbeat: TimeInterval = 0
    private var nextPing: TimeInterval = 0
    private var controlFrameStart: TimeInterval?
    private var videoFrameStart: TimeInterval?
    private var sends: [UInt64: TimeInterval] = [:]
    private var nextSend: UInt64 = 0
    private struct PendingSend {
        let data: Data
        let connection: NWConnection
        let run: UInt64
        let input: Bool
        let pastePermit: ClipboardPastePermit?
    }
    private var pendingSends: [PendingSend] = []
    private var sendInFlight = false
    // Public UI events enter one bounded mailbox, not one unbounded Dispatch block per event.
    private let inputLock = NSLock()
    private var inputMailbox: [[String: Any]] = []
    private var inputRun: UInt64?
    private var inputOutstanding = 0
    private var inputDrainScheduled = false
    private var inputOverflow = false

    private var controlPort: UInt16 = 39871
    private var videoPort: UInt16 = 39872
    #if HRD_NETWORK_TESTING
    private var testLoopback = false
    private var testSessionClock: (() -> TimeInterval)?
    private var testDisableTimer = false
    private var testPauseSends = false
    #endif

    init(onStatus: @escaping (String) -> Void, onPacket: @escaping (VideoPacket) -> Void,
         onEnd: @escaping (Result<Void, Error>) -> Void,
         onClipboardSession: @escaping (ClipboardSession?) -> Void = { _ in },
         onPasteResult: @escaping ([String: Any]) -> Void = { _ in },
         onPairedIdentity: @escaping (PairingCredentials) -> Void = { _ in },
         onAudioSession: @escaping (AudioSession?) -> Void = { _ in }) {
        self.onPairedIdentity = onPairedIdentity; self.onAudioSession = onAudioSession
        self.onStatus = onStatus
        self.onPacket = onPacket
        self.onEnd = onEnd
        self.onClipboardSession = onClipboardSession; self.onPasteResult = onPasteResult
    }

    #if HRD_NETWORK_TESTING
    convenience init(testControlPort: UInt16, testVideoPort: UInt16,
                     testSessionClock: (() -> TimeInterval)? = nil, testDisableTimer: Bool = false,
                     onStatus: @escaping (String) -> Void, onPacket: @escaping (VideoPacket) -> Void,
                     onEnd: @escaping (Result<Void, Error>) -> Void,
                     onClipboardSession: @escaping (ClipboardSession?) -> Void = { _ in },
                     onPasteResult: @escaping ([String: Any]) -> Void = { _ in },
                     onPairedIdentity: @escaping (PairingCredentials) -> Void = { _ in },
                     onAudioSession: @escaping (AudioSession?) -> Void = { _ in }) {
        self.init(onStatus: onStatus, onPacket: onPacket, onEnd: onEnd,
                  onClipboardSession: onClipboardSession, onPasteResult: onPasteResult,
                  onPairedIdentity: onPairedIdentity, onAudioSession: onAudioSession)
        self.controlPort = testControlPort
        self.videoPort = testVideoPort
        self.testLoopback = true
        self.testSessionClock = testSessionClock
        self.testDisableTimer = testDisableTimer
    }
    #endif

    var snapshot: LANConnectionSnapshot {
        snapshotLock.lock()
        defer { snapshotLock.unlock() }
        return statistics
    }

    private func updateSnapshot(_ update: (inout LANConnectionSnapshot) throws -> Void) rethrows {
        snapshotLock.lock()
        defer { snapshotLock.unlock() }
        try update(&statistics)
    }

    func connect(host: String, pin: String, pairing: PairingCredentials? = nil) {
        queue.async { [weak self] in self?.begin(host: host, pin: pin, pairing: pairing) }
    }

    func disconnect() {
        queue.async { [weak self] in
            guard let self, self.active else { return }
            self.end(.failure(LANError.cancelled), generation: self.generation)
        }
    }

    func requestInput(enabled: Bool) { enqueueInput(["type": "input_enable", "enabled": enabled]) }
    func sendInput(_ object: [String: Any]) {
        guard Self.validInput(object) else {
            inputLock.lock()
            guard let run = inputRun, !inputOverflow else { inputLock.unlock(); return }
            // Invalid submissions share the bounded failure path, rather than
            // scheduling an unbounded number of blocks from a fast producer.
            inputOverflow = true
            let schedule = !inputDrainScheduled
            inputDrainScheduled = true
            inputLock.unlock()
            if schedule { queue.async { [weak self] in self?.drainInput(run) } }
            return
        }
        enqueueInput(object)
    }
    func releaseInputs() { enqueueInput(["type": "release_all_keys"]) }

    func commitPaste(epoch: String, event: String, operation: String, ttl: Int, permit: ClipboardPastePermit) {
        queue.async { [weak self] in
            guard let self, self.active, self.clipboardEpoch == epoch, self.snapshot.inputEnabled,
                  let token = self.token, let control = self.control,
                  ClipboardWire.validEventID(event), ClipboardWire.hex(operation, length: 32), (1...1500).contains(ttl),
                  permit.remainingMilliseconds() != nil else { return }
            self.send(["type": "paste_commit", "sessionToken": token, "clipboardEpoch": epoch,
                       "eventId": event, "operationId": operation, "ttlMs": ttl], on: control, run: self.generation, pastePermit: permit)
        }
    }
    #if HRD_NETWORK_TESTING
    // Simulates an occupied control writer without changing the production queue.
    func pauseControlWriterForTest(_ paused: Bool, completion: @escaping () -> Void) {
        queue.async { self.testPauseSends = paused; if !paused { self.pumpSend() }; completion() }
    }
    #endif

    static func validInput(_ object: [String: Any]) -> Bool {
        func number(_ key: String, _ low: Double, _ high: Double) -> Bool {
            guard let value = object[key] as? NSNumber,
                  !WireProtocol.isBoolean(value, true), !WireProtocol.isBoolean(value, false) else { return false }
            let double = value.doubleValue
            return double.isFinite && double >= low && double <= high
        }
        guard let type = object["type"] as? String else { return false }
        switch type {
        case "mouse_move": return Set(object.keys) == ["type", "x", "y"] && number("x", 0, 1) && number("y", 0, 1)
        case "scroll": return Set(object.keys) == ["type", "dx", "dy"] && number("dx", -120, 120) && number("dy", -120, 120)
        case "mouse_button", "key":
            guard let action = object["action"] as? String, action == "down" || action == "up" else { return false }
            if type == "mouse_button" {
                return Set(object.keys) == ["type", "button", "action"] && ["left", "right", "middle"].contains(object["button"] as? String ?? "")
            }
            guard Set(object.keys) == ["type", "code", "action"], let code = object["code"] as? String else { return false }
            let singles = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789".map { "KEY_" + String($0) }
            let specials = ["MINUS", "EQUALS", "LEFT_BRACKET", "RIGHT_BRACKET", "BACKSLASH", "SEMICOLON", "APOSTROPHE", "GRAVE", "COMMA", "PERIOD", "SLASH",
                "ENTER", "ESCAPE", "TAB", "SPACE", "BACKSPACE", "DELETE", "UP", "DOWN", "LEFT", "RIGHT", "HOME", "END", "PAGE_UP", "PAGE_DOWN",
                "SHIFT_LEFT", "SHIFT_RIGHT", "CTRL_LEFT", "CTRL_RIGHT", "ALT_LEFT", "ALT_RIGHT", "META_LEFT", "META_RIGHT", "CAPS_LOCK"].map { "KEY_" + $0 }
            return singles.contains(code) || specials.contains(code) || (1...12).contains { code == "KEY_F\($0)" }
        default: return false
        }
    }

    private func enqueueInput(_ object: [String: Any]) {
        inputLock.lock()
        guard let run = inputRun, !inputOverflow else { inputLock.unlock(); return }
        // Only adjacent unsent moves coalesce; no edge or release may be crossed.
        if object["type"] as? String == "mouse_move", inputMailbox.last?["type"] as? String == "mouse_move" {
            inputMailbox[inputMailbox.count - 1] = object
        } else if inputOutstanding >= 128 {
            inputOverflow = true
        } else {
            inputMailbox.append(object)
            inputOutstanding += 1
        }
        let schedule = !inputDrainScheduled
        inputDrainScheduled = true
        inputLock.unlock()
        if schedule { queue.async { [weak self] in self?.drainInput(run) } }
    }

    private func drainInput(_ run: UInt64) {
        inputLock.lock()
        guard inputRun == run else { inputLock.unlock(); return }
        let overflow = inputOverflow
        let batch = inputMailbox
        inputMailbox.removeAll(keepingCapacity: true)
        inputDrainScheduled = false
        inputLock.unlock()
        guard isCurrent(run), let control, let token else { return }
        if overflow { end(.failure(LANError.protocolViolation("输入消息无效或发送队列已满")), generation: run); return }
        for var object in batch {
            guard isCurrent(run) else { break }
            object["sessionToken"] = token
            send(object, on: control, run: run, input: true)
        }
    }

    private func resetInput(run: UInt64?) {
        inputLock.lock()
        inputRun = run; inputMailbox.removeAll(); inputOutstanding = 0
        inputDrainScheduled = false; inputOverflow = false
        inputLock.unlock()
    }

    private func begin(host: String, pin: String, pairing: PairingCredentials?) {
        if active { end(.failure(LANError.cancelled), generation: generation) }
        generation &+= 1
        let run = generation
        active = true
        self.host = host
        self.pin = pin
        self.pairing = pairing; handshake = nil; measurements = NetworkMeasurements(); rttSupported = false; audioPort = nil
        token = nil
        clipboardAdvertised = false; clipboardEpoch = ""
        parser = WireVideoParser(maxTotalBytes: nil)
        sends.removeAll()
        pendingSends.removeAll(); sendInFlight = false
        resetInput(run: nil)
        controlFrameStart = nil
        videoFrameStart = nil
        updateSnapshot { $0 = LANConnectionSnapshot() }
        var validHost = WireProtocol.validHost(host)
        #if HRD_NETWORK_TESTING
        validHost = validHost || (testLoopback && host == "127.0.0.1")
        #endif
        guard validHost else { end(.failure(LANError.invalidHost), generation: run); return }
        guard pairing?.resuming == true || WireProtocol.validPIN(pin) else { end(.failure(LANError.invalidPIN), generation: run); return }
        let now = ProcessInfo.processInfo.systemUptime
        sessionDeadline = LANSessionDeadline(startingAt: sessionTime())
        phaseDeadline = now + 6
        lastHeartbeat = now
        nextPing = now + 2
        startTimer(run)
        onStatus("正在连接 Host…")
        let connection = makeConnection(host: host, port: controlPort)
        control = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, self.isCurrent(run) else { return }
            switch state {
            case .ready: self.hello(run)
            case .failed: self.end(.failure(LANError.connectionFailed), generation: run)
            default: break
            }
        }
        connection.start(queue: queue)
    }

    private func makeConnection(host: String, port: UInt16) -> NWConnection {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: tcp)
        return NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!, using: parameters)
    }

    private func isCurrent(_ run: UInt64) -> Bool { active && run == generation }

    private func sessionTime() -> TimeInterval {
        #if HRD_NETWORK_TESTING
        if let testSessionClock { return testSessionClock() }
        #endif
        return ProcessInfo.processInfo.systemUptime
    }

    private func startTimer(_ run: UInt64) {
        #if HRD_NETWORK_TESTING
        // Tests can prove receive callbacks enforce expiry without a timer tick.
        if testDisableTimer { return }
        #endif
        let source = DispatchSource.makeTimerSource(queue: queue)
        source.schedule(deadline: .now() + .milliseconds(100), repeating: .milliseconds(100))
        source.setEventHandler { [weak self] in self?.tick(run) }
        timer = source
        source.resume()
    }

    private func tick(_ run: UInt64) {
        guard isCurrent(run) else { return }
        let now = ProcessInfo.processInfo.systemUptime
        let current = snapshot
        measurements.sample(bytes: current.receivedBytes, frames: current.receivedFrames, at: now)
        updateSnapshot {
            $0.videoMbps = measurements.videoMbps; $0.receivedFPS = measurements.receivedFPS
            $0.rttMilliseconds = measurements.rttMilliseconds; $0.jitterMilliseconds = measurements.jitterMilliseconds
            $0.rttSamples = measurements.rttSamples
            if let path = control?.currentPath {
                $0.networkType = path.status == .satisfied ? (path.usesInterfaceType(.wiredEthernet) ? "有线网络" : path.usesInterfaceType(.wifi) ? "Wi-Fi" : "局域网") : "网络不可用"
            }
        }
        if sessionDeadline.hasExpired(at: sessionTime()) || (phaseDeadline > 0 && now >= phaseDeadline) ||
            [controlFrameStart, videoFrameStart, parser.assemblyStartedAt].compactMap({ $0 }).contains(where: { now - $0 >= 2 }) ||
            sends.values.contains(where: { now - $0 >= 2 }) {
            end(.failure(LANError.deadline), generation: run)
            return
        }
        guard let token else { return }
        if now - lastHeartbeat >= 6 {
            end(.failure(LANError.heartbeatTimeout), generation: run)
        } else if now >= nextPing, let control {
            nextPing = now + 2
            var ping: [String: Any] = ["type": "ping", "sessionToken": token]
            if rttSupported {
                let id = String(UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased().prefix(16))
                measurements.sentProbe(id, at: now); ping["probeId"] = id
            }
            send(ping, on: control, run: run)
        }
    }

    private func send(_ object: [String: Any], on connection: NWConnection, run: UInt64, input: Bool = false,
                      pastePermit: ClipboardPastePermit? = nil) {
        guard isCurrent(run) else { return }
        do {
            let frame = try WireProtocol.controlFrame(object)
            guard pendingSends.count + (sendInFlight ? 1 : 0) < 128 else {
                end(.failure(LANError.protocolViolation("控制发送队列已满")), generation: run); return
            }
            pendingSends.append(PendingSend(data: frame, connection: connection, run: run, input: input, pastePermit: pastePermit))
            pumpSend()
        } catch { end(.failure(error), generation: run) }
    }

    private func pumpSend() {
        #if HRD_NETWORK_TESTING
        if testPauseSends { return }
        #endif
        guard !sendInFlight, !pendingSends.isEmpty else { return }
        let item = pendingSends.removeFirst()
        guard isCurrent(item.run) else { return }
        var outgoing = item.data
        if let permit = item.pastePermit {
            guard let remaining = permit.remainingMilliseconds() else { pumpSend(); return }
            do {
                var object = try WireProtocol.controlObject(Data(item.data.dropFirst(4)))
                object["ttlMs"] = remaining
                outgoing = try WireProtocol.controlFrame(object)
            } catch { end(.failure(error), generation: item.run); return }
        }
        sendInFlight = true
        nextSend &+= 1
        let sendID = nextSend
        sends[sendID] = ProcessInfo.processInfo.systemUptime
        item.connection.send(content: outgoing, completion: .contentProcessed { [weak self] error in
            guard let self, self.isCurrent(item.run) else { return }
            self.sends.removeValue(forKey: sendID)
            self.sendInFlight = false
            if item.input {
                self.inputLock.lock()
                if self.inputRun == item.run { self.inputOutstanding -= 1 }
                self.inputLock.unlock()
                if error == nil {
                    do { try self.updateSnapshot { $0.inputSent = try WireProtocol.addCount($0.inputSent, 1) } }
                    catch { self.end(.failure(error), generation: item.run); return }
                }
            }
            if error != nil { self.end(.failure(LANError.connectionFailed), generation: item.run); return }
            self.pumpSend()
        })
    }

    private func readExact(_ count: Int, from connection: NWConnection, videoHandshake: Bool,
                           run: UInt64, accumulated: Data = Data(), completion: @escaping (Data) -> Void) {
        guard isCurrent(run) else { return }
        connection.receive(minimumIncompleteLength: 1, maximumLength: count - accumulated.count) { [weak self] data, _, complete, error in
            guard let self, self.isCurrent(run) else { return }
            var bytes = accumulated
            if let data, !data.isEmpty {
                let now = ProcessInfo.processInfo.systemUptime
                if videoHandshake { if self.videoFrameStart == nil { self.videoFrameStart = now } }
                else { if self.controlFrameStart == nil { self.controlFrameStart = now } }
                bytes.append(data)
            }
            if bytes.count == count { completion(bytes); return }
            if error != nil || complete { self.end(.failure(LANError.connectionClosed), generation: run); return }
            self.readExact(count, from: connection, videoHandshake: videoHandshake, run: run, accumulated: bytes, completion: completion)
        }
    }

    private func readJSON(from connection: NWConnection, videoHandshake: Bool = false,
                          run: UInt64, completion: @escaping ([String: Any]) -> Void) {
        readExact(4, from: connection, videoHandshake: videoHandshake, run: run) { [weak self] lengthData in
            guard let self, self.isCurrent(run) else { return }
            let size = Int(WireProtocol.uint(lengthData))
            guard size > 0, size <= WireProtocol.maxControl else {
                self.end(.failure(LANError.protocolViolation("控制长度超限")), generation: run); return
            }
            self.readExact(size, from: connection, videoHandshake: videoHandshake, run: run) { [weak self] body in
                guard let self, self.isCurrent(run) else { return }
                if videoHandshake { self.videoFrameStart = nil } else { self.controlFrameStart = nil }
                do { completion(try WireProtocol.controlObject(body)) }
                catch { self.end(.failure(error), generation: run) }
            }
        }
    }

    private func hello(_ run: UInt64) {
        guard let control, isCurrent(run) else { return }
        phaseDeadline = ProcessInfo.processInfo.systemUptime + 6
        send(["type": "hello", "protocol": 1, "client": "macOS", "clientVersion": "0.6.0"], on: control, run: run)
        readJSON(from: control, run: run) { [weak self] object in
            guard let self, self.isCurrent(run) else { return }
            guard object["type"] as? String == "hello_ack", WireProtocol.isIntegerOne(object["protocol"]),
                  WireProtocol.isBoolean(object["pairingRequired"], true),
                  object["timestampSource"] as? String == "encoder_callback_monotonic",
                  WireProtocol.isBoolean(object["nativePtsUnitVerified"], false) else {
                self.end(.failure(LANError.protocolViolation("Host hello 不兼容")), generation: run); return
            }
            let advertisedPort = ClipboardWire.integer(object["clipboardPort"], range: 1...65535)
            var allowedPort = advertisedPort == 39873
            #if HRD_NETWORK_TESTING
            if self.testLoopback && advertisedPort != nil { allowedPort = true }
            #endif
            self.clipboardAdvertised = WireProtocol.isBoolean(object["clipboardSupported"], true) && allowedPort
            self.clipboardPort = UInt16(advertisedPort ?? 39873)
            let supported = WireProtocol.isBoolean(object["inputSupported"], true)
            self.updateSnapshot { $0.inputSupported = supported }
            self.rttSupported = WireProtocol.isBoolean(object["rttSupported"], true)
            if let port = ClipboardWire.integer(object["audioPort"], range: 1...65535),
               port == 39874, WireProtocol.isBoolean(object["audioSupported"], true) { self.audioPort = UInt16(port) }
            do {
                if let credential = self.pairing, object["pairingScheme"] != nil {
                    let proof = try PairingHandshake(hello: object, credentials: credential)
                    self.handshake = proof
                    self.send(try proof.request(pin: self.pin), on: control, run: run)
                } else {
                    if let credential = self.pairing, credential.resuming || credential.remember { throw PairingError.unsupported }
                    self.pairing = nil
                    self.send(["type": "pair", "pin": self.pin], on: control, run: run)
                }
            } catch { self.end(.failure(error), generation: run); return }
            self.pin = ""
            self.phaseDeadline = ProcessInfo.processInfo.systemUptime + 6
            self.readJSON(from: control, run: run) { [weak self] object in
                guard let self, self.isCurrent(run) else { return }
                if object["type"] as? String == "error" {
                    let reason = object["error"] as? String ?? ""
                    if reason == "persistent_pairing_not_allowed" { self.end(.failure(PairingError.enrollmentNotAllowed), generation: run); return }
                    if reason == "pairing_not_trusted" { self.end(.failure(PairingError.noLongerTrusted), generation: run); return }
                }
                guard object["type"] as? String == "pair_ok", let token = object["sessionToken"] as? String,
                      WireProtocol.validToken(token) else { self.end(.failure(LANError.hostRejected), generation: run); return }
                do {
                    if let proof = self.handshake {
                        let verified = try proof.finish(object, token: token)
                        self.pairing = verified; self.onPairedIdentity(verified)
                    }
                } catch { self.end(.failure(error), generation: run); return }
                self.handshake = nil
                self.token = token
                if let audioPort = self.audioPort,
                   let epoch = object["audioEpoch"] as? String, ClipboardWire.hex(epoch, length: 32),
                   let binding = object["audioBindToken"] as? String, ClipboardWire.hex(binding, length: 64) {
                    self.onAudioSession(AudioSession(host: self.host, port: audioPort, epoch: epoch, binding: binding))
                }
                if self.clipboardAdvertised,
                   let epoch = object["clipboardEpoch"] as? String, ClipboardWire.hex(epoch, length: 32),
                   let binding = object["clipboardBindToken"] as? String, ClipboardWire.hex(binding, length: 64) {
                    self.clipboardEpoch = epoch
                    self.onClipboardSession(ClipboardSession(host: self.host, epoch: epoch, binding: binding, port: self.clipboardPort))
                }
                self.updateSnapshot { $0.paired = true }
                self.lastHeartbeat = ProcessInfo.processInfo.systemUptime
                self.nextPing = self.lastHeartbeat + 2
                self.onStatus("已配对，正在连接视频通道…")
                self.readControl(run)
                self.openVideo(run)
            }
        }
    }

    private func readControl(_ run: UInt64) {
        guard let control, isCurrent(run) else { return }
        readJSON(from: control, run: run) { [weak self] object in
            guard let self, self.isCurrent(run) else { return }
            guard let token = self.token, object["sessionToken"] as? String == token else {
                self.end(.failure(LANError.invalidSession), generation: run); return
            }
            switch object["type"] as? String {
            case "ping":
                var pong: [String: Any] = ["type": "pong", "sessionToken": token]
                if let id = object["probeId"] as? String, ClipboardWire.hex(id, length: 16) { pong["probeId"] = id }
                self.send(pong, on: control, run: run)
            case "pong":
                if let id = object["probeId"] as? String { self.measurements.receivedPong(id, at: ProcessInfo.processInfo.systemUptime) }
            case "input_status":
                guard WireProtocol.isBoolean(object["enabled"], true) || WireProtocol.isBoolean(object["enabled"], false),
                      self.snapshot.inputSupported else {
                    self.end(.failure(LANError.protocolViolation("输入状态无效")), generation: run); return
                }
                let enabled = WireProtocol.isBoolean(object["enabled"], true)
                self.updateSnapshot { $0.inputEnabled = enabled }
                self.onStatus(enabled ? "远程输入已启用" : "远程输入已关闭")
            case "paste_result":
                guard ClipboardWire.hex(object["operationId"], length: 32),
                      Set(object.keys) == Set(["type", "sessionToken", "operationId", "status", "error"]),
                      let status = object["status"] as? String, ["committed", "stale", "denied", "failed"].contains(status),
                      let error = object["error"] as? String, error.utf8.count <= 128, error.utf8.allSatisfy({ $0 >= 32 && $0 < 127 }) else {
                    // A malformed clipboard extension must not end screen sharing.
                    self.onClipboardSession(nil); break
                }
                self.onPasteResult(object)
            case "stop", "error", "display_changed": self.end(.failure(LANError.hostRejected), generation: run); return
            default: self.end(.failure(LANError.protocolViolation("未知控制消息")), generation: run); return
            }
            self.lastHeartbeat = ProcessInfo.processInfo.systemUptime
            self.readControl(run)
        }
    }

    private func openVideo(_ run: UInt64) {
        guard isCurrent(run) else { return }
        phaseDeadline = ProcessInfo.processInfo.systemUptime + 6
        let connection = makeConnection(host: host, port: videoPort)
        video = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, self.isCurrent(run) else { return }
            switch state {
            case .ready:
                guard let token = self.token else { self.end(.failure(LANError.invalidSession), generation: run); return }
                self.send(["type": "video_attach", "sessionToken": token], on: connection, run: run)
                self.readJSON(from: connection, videoHandshake: true, run: run) { [weak self] object in
                    guard let self, self.isCurrent(run) else { return }
                    guard object["type"] as? String == "video_ready" else { self.end(.failure(LANError.hostRejected), generation: run); return }
                    self.phaseDeadline = 0
                    self.updateSnapshot { $0.videoReady = true }
                    if self.snapshot.inputSupported { self.resetInput(run: run) }
                    self.onStatus("视频通道已就绪；请在鸿蒙端开始共享屏幕。")
                    self.readVideo(run)
                }
            case .failed: self.end(.failure(LANError.connectionFailed), generation: run)
            default: break
            }
        }
        connection.start(queue: queue)
    }

    private func readVideo(_ run: UInt64) {
        guard let video, isCurrent(run) else { return }
        video.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, complete, error in
            guard let self, self.isCurrent(run) else { return }
            if self.sessionDeadline.hasExpired(at: self.sessionTime()) {
                self.end(.failure(LANError.deadline), generation: run); return
            }
            do {
                if let data, !data.isEmpty {
                    let packets = try self.parser.feed(data)
                    for packet in packets {
                        let now = self.sessionTime()
                        if self.sessionDeadline.hasExpired(at: now) {
                            self.end(.failure(LANError.deadline), generation: run); return
                        }
                        self.sessionDeadline.observe(packet, at: now)
                        try self.updateSnapshot {
                            let bytes = try WireProtocol.addCount($0.receivedBytes, packet.payload.count)
                            let frames = try WireProtocol.addCount($0.receivedFrames,
                                packet.type == 2 && !packet.payload.isEmpty ? 1 : 0)
                            $0.receivedBytes = bytes; $0.receivedFrames = frames
                            if packet.type == 2 && !packet.payload.isEmpty {
                                $0.lastReceivedFrameAt = ProcessInfo.processInfo.systemUptime
                            }
                        }
                        if packet.type == 2 && !packet.payload.isEmpty && self.snapshot.receivedFrames == 1 {
                            self.onStatus("正在共享鸿蒙桌面")
                        }
                        self.onPacket(packet)
                    }
                    if self.parser.receivedEOS {
                        try self.parser.finish()
                        self.end(.success(()), generation: run)
                        return
                    }
                }
                if error != nil || complete {
                    throw LANError.connectionClosed
                }
                self.readVideo(run)
            } catch { self.end(.failure(error), generation: run) }
        }
    }

    private func end(_ result: Result<Void, Error>, generation run: UInt64) {
        guard isCurrent(run) else { return }
        active = false
        clipboardEpoch = ""; clipboardAdvertised = false; onClipboardSession(nil); onAudioSession(nil)
        handshake = nil; pairing = nil; audioPort = nil
        resetInput(run: nil)
        pendingSends.removeAll(); sendInFlight = false
        updateSnapshot { $0.inputEnabled = false }
        timer?.cancel()
        timer = nil
        let oldControl = control
        let finalToken = token
        control = nil
        video?.stateUpdateHandler = nil
        video?.cancel()
        video = nil
        pin = ""
        token = nil
        sends.removeAll()
        oldControl?.stateUpdateHandler = nil
        let transportInterrupted: Bool
        if case .failure(let error) = result, let reason = error as? LANError {
            switch reason {
            case .connectionFailed, .connectionClosed, .heartbeatTimeout, .deadline: transportInterrupted = true
            default: transportInterrupted = false
            }
        } else { transportInterrupted = false }
        // An explicit stop ends capture; a broken transport must instead let
        // Host's EOF cleanup release input and preserve its bounded grace period.
        if !transportInterrupted, let oldControl, let finalToken,
           let release = try? WireProtocol.controlFrame(["type": "release_all_keys", "sessionToken": finalToken]),
           let stop = try? WireProtocol.controlFrame(["type": "stop", "sessionToken": finalToken]) {
            // One ordered write: release all held input before stopping the session.
            oldControl.send(content: release + stop, completion: .contentProcessed { _ in oldControl.cancel() })
            queue.asyncAfter(deadline: .now() + 1) { oldControl.cancel() }
        } else { oldControl?.cancel() }
        onEnd(result)
    }

    deinit {
        timer?.cancel()
        control?.cancel()
        video?.cancel()
    }
}
