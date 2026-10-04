import Foundation
import Network

// One outstanding read and synchronous packet delivery on a private queue;
// playback accepts into its own bounded mailbox. Failure affects audio only.
final class AudioConnection {
    private let session: AudioSession
    private let onPacket: (AudioPacket) -> Void
    private let onReady: () -> Void
    private let onFailure: (String) -> Void
    private let queue = DispatchQueue(label: "HarmonyRemote.Audio.Transport")
    private var connection: NWConnection?
    private var timer: DispatchSourceTimer?
    private var active = false
    private var deadline: TimeInterval?
    private var sequence: UInt64 = 0
    private var streamID: UInt64 = 0
    private var hasReset = false
    private let lock = NSLock()
    private var cancelled = false
    private var allowLoopback = false
    #if HRD_AUDIO_TESTING
    private var afterConnectionStarted: (() -> Void)?
    private var afterTransportClosed: (() -> Void)?
    #endif
    init(session: AudioSession, onPacket: @escaping (AudioPacket) -> Void,
         onReady: @escaping () -> Void, onFailure: @escaping (String) -> Void) {
        self.session = session; self.onPacket = onPacket; self.onReady = onReady; self.onFailure = onFailure
    }
    #if HRD_AUDIO_TESTING
    convenience init(testSession: AudioSession, onPacket: @escaping (AudioPacket) -> Void,
                     onReady: @escaping () -> Void, onFailure: @escaping (String) -> Void,
                     afterConnectionStarted: (() -> Void)? = nil, afterTransportClosed: (() -> Void)? = nil) {
        self.init(session: testSession, onPacket: onPacket, onReady: onReady, onFailure: onFailure); allowLoopback = true
        self.afterConnectionStarted = afterConnectionStarted; self.afterTransportClosed = afterTransportClosed
    }
    #endif
    private var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    func start() { queue.async { [weak self] in self?.begin() } }
    func close() {
        lock.lock(); cancelled = true; lock.unlock()
        // The owner drops this channel immediately. Retain it until cancellation
        // runs even if connection preparation has not installed a receive callback.
        queue.async { [self] in finish(nil) }
    }
    private func begin() {
        guard connection == nil, !isCancelled else { return }
        guard (WireProtocol.validHost(session.host) && session.port == 39874) || (allowLoopback && session.host == "127.0.0.1"),
              let port = NWEndpoint.Port(rawValue: session.port), let bind = try? AudioWire.bind(session) else { finish("音频通道参数无效"); return }
        active = true; deadline = ProcessInfo.processInfo.systemUptime + 5
        let parameters = NWParameters.tcp
        (parameters.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options)?.noDelay = true
        let connection = NWConnection(host: .init(session.host), port: port, using: parameters); self.connection = connection
        let timer = DispatchSource.makeTimerSource(queue: queue); self.timer = timer
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self, let deadline = self.deadline else { return }
            if ProcessInfo.processInfo.systemUptime >= deadline { self.finish("音频通道超时") }
        }; timer.resume()
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, self.active, !self.isCancelled else { return }
            switch state {
            case .ready:
                guard let transport = self.connection else { return }
                transport.send(content: bind, completion: .contentProcessed { [weak self] error in
                    guard let self, self.active else { return }
                    if error != nil { self.finish("音频绑定发送失败"); return }
                    self.readExact(4) { prefix in
                        let count = AudioWire.uint(prefix)
                        guard count > 0, count <= 512 else { self.finish("音频绑定响应无效"); return }
                        self.readExact(Int(count)) { body in
                            guard AudioWire.ready(body) else { self.finish("音频绑定未通过"); return }
                            self.deadline = nil
                            DispatchQueue.main.async { [weak self] in guard let self, !self.isCancelled else { return }; self.onReady() }
                            self.readPacket()
                        }
                    }
                })
            case .failed: self.finish("音频连接已断开")
            default: break
            }
        }; connection.start(queue: queue)
        #if HRD_AUDIO_TESTING
        afterConnectionStarted?()
        #endif
    }
    private func readExact(_ count: Int, bytes: Data = Data(), completion: @escaping (Data) -> Void) {
        guard active, !isCancelled, let connection else { return }
        if bytes.count == count { completion(bytes); return }
        connection.receive(minimumIncompleteLength: 1, maximumLength: count - bytes.count) { [weak self] data, _, eof, error in
            guard let self, self.active, !self.isCancelled else { return }
            var result = bytes
            if let data, !data.isEmpty {
                if self.deadline == nil { self.deadline = ProcessInfo.processInfo.systemUptime + 5 }
                result.append(data)
            }
            if result.count == count { completion(result) }
            else if eof || error != nil { self.finish("音频连接已断开") }
            else { self.readExact(count, bytes: result, completion: completion) }
        }
    }
    private func readPacket() {
        readExact(AudioWire.headerSize) { header in
            guard let parsed = try? AudioWire.header(header), parsed.sequence > self.sequence,
                  parsed.streamID >= self.streamID,
                  parsed.kind == .reset || (self.hasReset && parsed.streamID == self.streamID) else {
                self.finish("音频数据格式或顺序无效"); return
            }
            self.readExact(parsed.length) { body in
                self.sequence = parsed.sequence; self.streamID = parsed.streamID
                if parsed.kind == .reset { self.hasReset = true }
                self.deadline = nil
                self.onPacket(AudioPacket(kind: parsed.kind, ptsUs: parsed.ptsUs, sequence: parsed.sequence,
                                          streamID: parsed.streamID, pcm: body))
                self.readPacket()
            }
        }
    }
    private func finish(_ reason: String?) {
        guard active || connection == nil else { return }
        active = false; deadline = nil; timer?.cancel(); timer = nil
        connection?.stateUpdateHandler = nil; connection?.cancel(); connection = nil
        #if HRD_AUDIO_TESTING
        let closed = afterTransportClosed; afterTransportClosed = nil; closed?()
        #endif
        if let reason { DispatchQueue.main.async { [weak self] in guard let self, !self.isCancelled else { return }; self.onFailure(reason) } }
    }
    deinit {
        timer?.cancel()
        connection?.stateUpdateHandler = nil
        connection?.cancel()
    }
}
