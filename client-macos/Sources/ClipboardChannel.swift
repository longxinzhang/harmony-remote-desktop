import Foundation
import Network

// Independent from video and control: any failure closes only clipboard. All
// callbacks are serialized on main; the next read waits for the current callback.
final class ClipboardChannel {
    private let queue = DispatchQueue(label: "HarmonyRemote.Clipboard")
    private let session: ClipboardSession
    private let onReady: () -> Void
    private let onFrame: (ClipboardFrame) -> Void
    private let onFailure: (ClipboardFailure) -> Void
    private var connection: NWConnection?
    private var timer: DispatchSourceTimer?
    private var active = false
    private var ready = false
    private var readStarted: TimeInterval?
    private var sendStarted: TimeInterval?
    private var pending: [Data] = []
    private var sending = false
    private var port: UInt16 = 39873
    private var allowLoopback = false
    private let submissionLock = NSLock()
    private var outstanding = 0
    private var cancelled = false

    init(session: ClipboardSession, onReady: @escaping () -> Void,
         onFrame: @escaping (ClipboardFrame) -> Void, onFailure: @escaping (ClipboardFailure) -> Void) {
        self.session = session; self.onReady = onReady; self.onFrame = onFrame; self.onFailure = onFailure
        port = session.port
    }
    #if HRD_CLIPBOARD_TESTING
    convenience init(testPort: UInt16, session: ClipboardSession, onReady: @escaping () -> Void,
                     onFrame: @escaping (ClipboardFrame) -> Void, onFailure: @escaping (ClipboardFailure) -> Void) {
        self.init(session: session, onReady: onReady, onFrame: onFrame, onFailure: onFailure)
        port = testPort; allowLoopback = true
    }
    #endif
    func start() { queue.async { [weak self] in self?.begin() } }
    func close() {
        submissionLock.lock(); cancelled = true; submissionLock.unlock()
        queue.async { [weak self] in self?.finish(nil) }
    }
    private var isCancelled: Bool { submissionLock.lock(); defer { submissionLock.unlock() }; return cancelled }
    func send(_ frame: ClipboardFrame) {
        submissionLock.lock()
        guard !cancelled, outstanding < 8 else {
            let notify = !cancelled; cancelled = true; submissionLock.unlock()
            if notify { queue.async { [weak self] in self?.finish(.malformed) } }
            return
        }
        outstanding += 1; submissionLock.unlock()
        queue.async { [weak self] in
            guard let self else { return }
            guard self.active, !self.isCancelled else { self.removeOutstanding(); return }
            do { self.pending.append(try ClipboardWire.encode(frame, epoch: self.session.epoch)); self.pump() }
            catch { self.removeOutstanding(); self.finish(.malformed) }
        }
    }
    private func removeOutstanding() { submissionLock.lock(); outstanding = max(0, outstanding - 1); submissionLock.unlock() }
    private func begin() {
        guard !isCancelled, connection == nil else { return }
        guard (WireProtocol.validHost(session.host) && port == 39873) || (allowLoopback && session.host == "127.0.0.1"),
              ClipboardWire.hex(session.epoch, length: 32), ClipboardWire.hex(session.binding, length: 64),
              let nwPort = NWEndpoint.Port(rawValue: port) else { finish(.malformed); return }
        active = true; readStarted = ProcessInfo.processInfo.systemUptime
        let parameters = NWParameters.tcp
        (parameters.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options)?.noDelay = true
        let connection = NWConnection(host: NWEndpoint.Host(session.host), port: nwPort, using: parameters)
        self.connection = connection
        let timer = DispatchSource.makeTimerSource(queue: queue); self.timer = timer
        timer.schedule(deadline: .now() + 0.1, repeating: 0.1)
        timer.setEventHandler { [weak self] in
            guard let self, self.active else { return }
            let now = ProcessInfo.processInfo.systemUptime
            if self.readStarted.map({ now - $0 >= 5 }) == true || self.sendStarted.map({ now - $0 >= 5 }) == true {
                self.finish(.timeout)
            }
        }
        timer.resume()
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, self.active, !self.isCancelled else { return }
            switch state {
            case .ready:
                self.send(ClipboardWire.make("data_bind", epoch: self.session.epoch,
                    extra: ["bindToken": self.session.binding, "purpose": "clipboard"]))
                self.readFrame()
            case .failed: self.finish(.transport)
            default: break
            }
        }
        connection.start(queue: queue)
    }
    private func pump() {
        guard active, !sending, !pending.isEmpty, let connection else { return }
        sending = true; sendStarted = ProcessInfo.processInfo.systemUptime
        let bytes = pending.removeFirst()
        connection.send(content: bytes, completion: .contentProcessed { [weak self] error in
            guard let self, self.active else { return }
            if self.sendStarted.map({ ProcessInfo.processInfo.systemUptime - $0 >= 5 }) == true { self.finish(.timeout); return }
            self.sending = false; self.sendStarted = nil; self.removeOutstanding()
            if error != nil { self.finish(.transport) } else { self.pump() }
        })
    }
    private func readExact(_ count: Int, bytes: Data = Data(), completion: @escaping (Data) -> Void) {
        guard active, !isCancelled, let connection else { return }
        if bytes.count == count { completion(bytes); return }
        connection.receive(minimumIncompleteLength: 1, maximumLength: min(65536, count - bytes.count)) { [weak self] data, _, eof, error in
            guard let self, self.active, !self.isCancelled else { return }
            if self.readStarted.map({ ProcessInfo.processInfo.systemUptime - $0 >= 5 }) == true { self.finish(.timeout); return }
            var result = bytes
            if let data, !data.isEmpty {
                if self.readStarted == nil { self.readStarted = ProcessInfo.processInfo.systemUptime }
                result.append(data)
            }
            if result.count == count { completion(result) }
            else if eof || error != nil { self.finish(.transport) }
            else { self.readExact(count, bytes: result, completion: completion) }
        }
    }
    private func readFrame() {
        readExact(4) { [weak self] length in
            guard let self else { return }
            let count = WireProtocol.uint(length)
            guard count > 0, count <= 4096 else { self.finish(.malformed); return }
            self.readExact(Int(count)) { [weak self] body in
                guard let self else { return }
                do {
                    let header = try ClipboardWire.header(body, epoch: self.session.epoch)
                    let count = ClipboardWire.integer(header["payloadLength"], range: 0...ClipboardWire.maxPayload)!
                    self.readExact(count) { [weak self] payload in
                        guard let self else { return }
                        do {
                            let frame = ClipboardFrame(header: header, payload: payload)
                            try ClipboardWire.validate(frame, epoch: self.session.epoch)
                            self.readStarted = nil
                            if !self.ready {
                                guard frame.type == "data_ready" else { throw ClipboardFailure.malformed }
                                self.ready = true
                                self.deliver { self.onReady() }
                            } else {
                                guard ["clipboard_status", "clipboard_update", "clipboard_applied"].contains(frame.type) else { throw ClipboardFailure.malformed }
                                self.deliver { self.onFrame(frame) }
                            }
                        } catch { self.finish(.malformed) }
                    }
                } catch { self.finish(.malformed) }
            }
        }
    }
    private func deliver(_ action: @escaping () -> Void) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.isCancelled else { return }
            action()
            self.queue.async { [weak self] in self?.readFrame() }
        }
    }
    private func finish(_ failure: ClipboardFailure?) {
        let wasActive = active
        active = false; submissionLock.lock(); cancelled = true; outstanding = 0; submissionLock.unlock()
        timer?.cancel(); timer = nil; connection?.cancel(); connection = nil; pending.removeAll()
        if let failure, wasActive { DispatchQueue.main.async { [weak self] in self?.onFailure(failure) } }
    }
    deinit { timer?.cancel(); connection?.cancel() }
}
