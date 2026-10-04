import AppKit
import Foundation
import SwiftUI

private enum ViewerFailure: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let value) = self { return value }; return nil }
}

// No unbounded array of compressed packets: each network delivery completes its
// short decoder submission on this serial queue before the next receive proceeds.
// Mutable lifecycle state is confined to queue; decoder.snapshot uses its lock.
private final class DecodePipeline: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.longxin.harmonyremote.decoder", qos: .userInteractive)
    private let decoder: H264Decoder
    private var closed = false
    private var failure: Error?

    init(mailbox: FrameMailbox) {
        decoder = H264Decoder { pixels, pts in mailbox.put(pixels, ptsUs: pts) }
    }
    func submit(_ packet: VideoPacket) throws {
        try queue.sync {
            if let failure { throw failure }
            guard !closed else { throw ViewerFailure.message("解码会话已结束") }
            do {
                if packet.type == 1 { try decoder.configure(packet.payload) }
                if packet.type == 2 && !packet.payload.isEmpty {
                    try decoder.decode(packet.payload, ptsUs: packet.ptsUs, keyframe: packet.flags & 1 != 0)
                }
            } catch { failure = error; throw error }
        }
    }
    func finish(_ result: Result<Void, Error>, completion: @escaping (Result<Void, Error>, DecoderSnapshot) -> Void) {
        queue.async {
            guard !self.closed else { return }
            var outcome = result
            if let failure = self.failure { outcome = .failure(failure) }
            if case .success = outcome {
                do { try self.decoder.finish() } catch { outcome = .failure(error) }
            }
            let snapshot = self.decoder.snapshot
            self.decoder.close(); self.closed = true
            completion(outcome, snapshot)
        }
    }
    func cancel() { queue.async { self.decoder.close(); self.closed = true } }
    var snapshot: DecoderSnapshot { decoder.snapshot }
}

private final class ReplayCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    func cancel() { lock.lock(); stopped = true; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
}

@MainActor
final class ViewerModel: ObservableObject {
    @Published var host = UserDefaults.standard.string(forKey: "lastHost") ?? ""
    @Published var pin = ""
    @Published var active = false
    @Published var reconnecting = false
    @Published var reconnectAttempt = 0
    @Published var automaticReconnect = UserDefaults.standard.object(forKey: "automaticReconnect") as? Bool ?? true
    @Published var rememberDevice = false
    @Published var trustedPeers: [TrustedPeer] = []
    @Published var pairingStatus = "首次配对请核对鸿蒙端连接码。"
    @Published var networkType = "未连接"
    @Published var networkRTT: Double?
    @Published var networkJitter: Double?
    @Published var videoMbps = 0.0
    @Published var receivedFPS = 0.0
    @Published var audioMuted = false
    @Published var audioStatus = "声音未连接"
    private let pairingStore = PairingStore()
    private var resumeIdentity: PairingCredentials?
    private var retryPolicy = ReconnectPolicy()
    private var reconnectWork: DispatchWorkItem?
    private var audioConnection: AudioConnection?
    private var audioPlayback: AudioPlayback?
    private var audioTransportStatus = "声音未连接"

    @Published var state = "等待连接"
    @Published var detail = "在鸿蒙端启动服务，输入 IP 和一次性 PIN。"
    @Published var receivedFrames = 0
    @Published var decodedFrames = 0
    @Published var renderedFrames = 0
    @Published var displayReplacements = 0
    @Published var dimensions = "—"
    @Published var hardware = "待检测"
    @Published var failed = false
    @Published var replay = false
    @Published var inputEnabled = false
    @Published var inputSupported = false
    @Published var keyboardMode: RemoteKeyboardMode = .macFriendly
    @Published var clipboardMode: ClipboardMode = ClipboardMode(rawValue: UserDefaults.standard.integer(forKey: "clipboardMode")) ?? .off
    @Published var clipboardStatus = "剪贴板同步已关闭"
    @Published var clipboardConnected = false
    @Published var clipboardPasteEnabled = false
    @Published var clipboardPullEnabled = false
    private let clipboardAdapter = AppKitClipboardAdapter()
    private lazy var clipboard = ClipboardCoordinator(adapter: clipboardAdapter)
    private var clipboardChannel: ClipboardChannel?
    private var clipboardTimer: Timer?
    @Published var mailbox = FrameMailbox()
    private var connection: LANConnection?
    private var pipeline: DecodePipeline?
    private var replayCancellation: ReplayCancellation?
    private var timer: Timer?
    private var generation = UUID()
    private var startedArguments = false
    private var reportPath: String?
    private var lastResult = "NOT_RUN"
    private var startedAt: Date?
    private var sessionHost = ""

    init() {
        clipboard.onChange = { [weak self] in self?.refreshClipboard() }
        clipboard.onSend = { [weak self] frame in self?.clipboardChannel?.send(frame) }
        clipboard.onCommit = { [weak self] epoch, event, operation, ttl in
            guard let self, self.canControl, self.inputEnabled, let permit = self.clipboard.pastePermit else { return }
            self.connection?.commitPaste(epoch: epoch, event: event, operation: operation, ttl: ttl, permit: permit)
        }
    }

    private func startStatisticsTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    deinit { timer?.invalidate(); clipboardTimer?.invalidate(); reconnectWork?.cancel() }

    func startArguments() {
        guard !startedArguments else { return }; startedArguments = true
        let arguments = ProcessInfo.processInfo.arguments
        func value(_ name: String) -> String? {
            guard let i = arguments.firstIndex(of: name), arguments.indices.contains(i + 1) else { return nil }
            return arguments[i + 1]
        }
        if let address = value("--host") { host = address }
        reportPath = value("--diagnostics")
        if let file = value("--replay") { startReplay(file) }
    }

    private func prepare(isReplay: Bool) -> (UUID, DecodePipeline) {
        stopClipboard(); stopAudio()
        replayCancellation?.cancel(); connection?.disconnect(); pipeline?.cancel()
        generation = UUID(); mailbox = FrameMailbox()
        let decoder = DecodePipeline(mailbox: mailbox); pipeline = decoder
        active = true; failed = false; replay = isReplay
        startStatisticsTimer()
        inputEnabled = false; inputSupported = false
        receivedFrames = 0; decodedFrames = 0; renderedFrames = 0; displayReplacements = 0
        dimensions = "—"; hardware = "待检测"; lastResult = "RUNNING"; startedAt = Date()
        return (generation, decoder)
    }

    func reloadTrustedPeers() {
        do { trustedPeers = try pairingStore.peers() }
        catch { pairingStatus = error.localizedDescription }
    }
    func forgetPeer(_ peer: TrustedPeer) {
        if sessionHost == peer.host && (active || reconnecting) { disconnect() }
        do {
            try pairingStore.forget(host: peer.host)
            if sessionHost == peer.host { resumeIdentity = nil }
            reloadTrustedPeers(); pairingStatus = "已移除此 Mac 保存的配对；鸿蒙端也可撤销所有可信设备。"
        } catch { pairingStatus = error.localizedDescription }
    }
    func connectTrusted(_ peer: TrustedPeer) { guard !active && !reconnecting else { return }; host = peer.host; pin = ""; connect() }
    func setAutomaticReconnect(_ enabled: Bool) {
        automaticReconnect = enabled; UserDefaults.standard.set(enabled, forKey: "automaticReconnect")
        if !enabled && reconnecting { disconnect() }
    }
    func connect() {
        guard !active && !reconnecting else { return }
        let address = host.trimmingCharacters(in: .whitespacesAndNewlines)
        guard WireProtocol.validHost(address) else {
            failed = true; state = "请检查连接信息"; detail = "需要局域网 IPv4 地址。"; return
        }
        do {
            let credential: PairingCredentials
            if WireProtocol.validPIN(pin) { credential = PairingCredentials(remember: rememberDevice) }
            else if pin.isEmpty, let saved = try pairingStore.load(host: address) { credential = saved }
            else { throw LANError.invalidPIN }
            let secret = pin; pin = ""; resumeIdentity = nil
            retryPolicy.reset(); reconnectAttempt = 0; reconnectWork?.cancel(); reconnectWork = nil
            beginConnection(address: address, pin: secret, credential: credential)
        } catch { failed = true; state = "请检查连接信息"; detail = error.localizedDescription }
    }
    private func beginConnection(address: String, pin secret: String, credential: PairingCredentials) {
        let (id, decoder) = prepare(isReplay: false)
        sessionHost = address
        state = credential.resuming ? "正在验证已配对设备" : "正在配对"
        detail = "连接后，请在鸿蒙端开始共享；恢复连接不会回放旧输入或粘贴。"
        let network = LANConnection(onStatus: { [weak self] status in
            DispatchQueue.main.async {
                guard let self, self.generation == id, self.active else { return }
                self.state = status
            }
        }, onPacket: { [weak self] packet in
            do { try decoder.submit(packet) }
            catch {
                DispatchQueue.main.async {
                    guard let self, self.generation == id else { return }
                    self.complete(id, decoder, .failure(error))
                    self.connection?.disconnect()
                }
            }
        }, onEnd: { [weak self] result in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                self.complete(id, decoder, result)
            }
        }, onClipboardSession: { [weak self] session in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                if let session { self.startClipboard(session, generation: id) } else { self.stopClipboard() }
            }
        }, onPasteResult: { [weak self] result in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                self.clipboard.pasteResult(result)
            }
        }, onPairedIdentity: { [weak self] verified in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                self.resumeIdentity = verified
                self.pairingStatus = "设备身份已校验 · \(verified.fingerprint)"
                if verified.remember && !credential.resuming {
                    do { try self.pairingStore.save(host: address, credentials: verified); self.reloadTrustedPeers() }
                    catch { self.pairingStatus = error.localizedDescription }
                }
            }
        }, onAudioSession: { [weak self] session in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                if let session { self.startAudio(session, generation: id) } else { self.stopAudio() }
            }
        })
        connection = network
        UserDefaults.standard.set(address, forKey: "lastHost")
        network.connect(host: address, pin: secret, pairing: credential)
    }

    private func complete(_ id: UUID, _ decoder: DecodePipeline, _ result: Result<Void, Error>) {
        decoder.finish(result) { [weak self] outcome, snapshot in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                self.updateDecoder(snapshot); self.active = false; self.inputEnabled = false
                self.stopClipboard(); self.stopAudio()
                self.timer?.invalidate(); self.timer = nil
                self.reconnecting = false
                switch outcome {
                case .success:
                    self.lastResult = self.replay ? "LOCAL_REPLAY_DECODED" : "LIVE_STREAM_DECODED"
                    self.state = self.replay ? "验证回放结束" : "共享已结束"
                    self.detail = self.replay ? "这是本地录屏验证画面，不是实时连接。" : "当前保留最后一帧。可使用已保存的配对重新连接，或获取新的连接码。"
                case .failure(let error):
                    self.lastResult = "FAILED"; self.failed = true; self.state = "连接已结束"
                    self.detail = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
                    if !self.replay, let delay = self.retryPolicy.nextDelay(for: error, enabled: self.automaticReconnect, hasIdentity: self.resumeIdentity != nil),
                       let identity = self.resumeIdentity {
                        self.reconnecting = true; self.failed = false; self.reconnectAttempt = self.retryPolicy.attempts
                        self.state = "连接中断，等待重连"
                        self.detail = "\(Int(delay)) 秒后进行第 \(self.reconnectAttempt) 次重连；可随时取消。"
                        let retry = DispatchWorkItem { [weak self] in
                            guard let self, self.generation == id, self.reconnecting else { return }
                            self.reconnectWork = nil
                            self.beginConnection(address: self.sessionHost, pin: "", credential: identity)
                        }
                        self.reconnectWork = retry
                        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: retry)
                    }
                }
                self.refresh()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                    guard let self, self.generation == id else { return }
                    self.refresh(); self.writeAutomaticReport()
                }
            }
        }
    }

    func disconnect() {
        reconnectWork?.cancel(); reconnectWork = nil; reconnecting = false; retryPolicy.reset(); resumeIdentity = nil
        stopClipboard(); stopAudio()
        connection?.releaseInputs()
        generation = UUID(); replayCancellation?.cancel()
        connection?.disconnect(); pipeline?.cancel()
        inputEnabled = false
        active = false; state = "已断开"; detail = "当前保留最后一帧。"; lastResult = "USER_STOPPED"
        timer?.invalidate(); timer = nil; refresh()
        pin = ""
    }

    var canControl: Bool { active && !replay && decodedFrames > 0 && inputSupported && !failed }

    func toggleInput() {
        guard canControl else { return }
        let enabled = !inputEnabled
        connection?.requestInput(enabled: enabled)
        if !enabled { clipboard.cancelPaste(); inputEnabled = false }
        detail = enabled ? "请先在鸿蒙端允许远程控制；启用后点击画面获得键盘焦点。" : "已关闭键鼠控制，继续查看画面。"
    }

    func sendInput(_ object: [String: Any]) {
        guard canControl && inputEnabled else { return }
        clipboard.cancelPaste()
        connection?.sendInput(object)
    }

    func releaseInputs() { clipboard.cancelPaste(); connection?.releaseInputs() }

    private func startAudio(_ session: AudioSession, generation id: UUID) {
        stopAudio()
        let playback = AudioPlayback()
        playback.setMuted(audioMuted); audioPlayback = playback
        audioTransportStatus = "正在连接声音通道"; refreshAudioStatus()
        let channel = AudioConnection(session: session, onPacket: { packet in playback.append(packet) }, onReady: { [weak self] in
            guard let self, self.generation == id else { return }
            self.audioTransportStatus = "声音通道就绪，等待鸿蒙共享声音"; self.refreshAudioStatus()
        }, onFailure: { [weak self] reason in
            guard let self, self.generation == id else { return }
            self.stopAudio(); self.audioTransportStatus = "声音不可用：" + reason; self.refreshAudioStatus()
        })
        audioConnection = channel; channel.start()
    }
    private func stopAudio() {
        audioConnection?.close(); audioConnection = nil; audioPlayback?.stop(); audioPlayback = nil
        audioTransportStatus = "声音未连接"; refreshAudioStatus()
    }
    func setAudioMuted(_ muted: Bool) { audioMuted = muted; audioPlayback?.setMuted(muted); refreshAudioStatus() }

    private func refreshAudioStatus() {
        guard let audio = audioPlayback?.snapshot() else { audioStatus = audioTransportStatus; return }
        if !audio.error.isEmpty { audioStatus = audio.error }
        else if audioMuted { audioStatus = "已静音" }
        else if audio.running { audioStatus = "正在播放系统声音" }
        else { audioStatus = audioTransportStatus }
    }

    private func startClipboard(_ session: ClipboardSession, generation id: UUID) {
        stopClipboard(); clipboard.start(epoch: session.epoch, mode: clipboardMode)
        let channel = ClipboardChannel(session: session, onReady: { [weak self] in
            guard let self, self.generation == id else { return }; self.clipboard.ready()
        }, onFrame: { [weak self] frame in
            guard let self, self.generation == id else { return }
            do { try self.clipboard.receive(frame) }
            catch { self.stopClipboard(reason: "剪贴板协议错误，画面与输入继续") }
        }, onFailure: { [weak self] failure in
            guard let self, self.generation == id else { return }
            self.stopClipboard(reason: "剪贴板通道不可用（\(failure.rawValue)），画面与输入继续")
        })
        clipboardChannel = channel; channel.start()
    }
    private func stopClipboard(reason: String = "剪贴板未连接") {
        clipboardChannel?.close(); clipboardChannel = nil; clipboard.stop(reason: reason)
    }
    private func refreshClipboard() {
        if clipboardStatus != clipboard.status { clipboardStatus = clipboard.status }
        if clipboardConnected != clipboard.connected { clipboardConnected = clipboard.connected }
        // Consume paste even while permission/transport is unavailable.
        if clipboardPasteEnabled != clipboard.mode.sends { clipboardPasteEnabled = clipboard.mode.sends }
        if clipboardPullEnabled != clipboard.canPull { clipboardPullEnabled = clipboard.canPull }
        if clipboard.connected && clipboard.mode.sends && clipboard.allowed {
            if clipboardTimer == nil {
                let poll = Timer(timeInterval: 0.15, repeats: true) { [weak self] _ in
                    Task { @MainActor in self?.clipboard.tick() }
                }
                clipboardTimer = poll; RunLoop.main.add(poll, forMode: .common)
            }
        } else { clipboardTimer?.invalidate(); clipboardTimer = nil }
    }
    func setClipboardMode(_ mode: ClipboardMode) {
        if !clipboardMode.sends && mode.sends { clipboardAdapter.allowOneAutomaticReadAttempt() }
        clipboardMode = mode; UserDefaults.standard.set(mode.rawValue, forKey: "clipboardMode")
        clipboard.setMode(mode); refreshClipboard()
    }
    func pullClipboard() { clipboard.pull() }
    func remoteCopyIntent() {
        guard canControl, inputEnabled else { return }
        clipboard.noteRemoteCopyIntent()
    }
    func pasteClipboard(valid: @escaping () -> Bool) {
        guard canControl, inputEnabled else { return }
        clipboard.requestPaste(valid: { [weak self] in
            guard let self else { return false }; return self.canControl && self.inputEnabled && valid()
        })
    }

    func changeKeyboardMode(_ mode: RemoteKeyboardMode) {
        // VideoSurface releases its physical state before adopting the new map.
        // Sending release_all here first would make its later key-ups unmatched.
        if keyboardMode != mode { clipboard.cancelPaste() }
        keyboardMode = mode
    }

    private func refresh() {
        if !replay, let snapshot = connection?.snapshot {
            networkType = active ? snapshot.networkType : "已断开"
            networkRTT = active ? snapshot.rttMilliseconds : nil
            networkJitter = active ? snapshot.jitterMilliseconds : nil
            videoMbps = active ? snapshot.videoMbps : 0; receivedFPS = active ? snapshot.receivedFPS : 0
            if active && snapshot.videoReady { reconnecting = false }
            if active && snapshot.receivedFrames > 0 { retryPolicy.reset(); reconnectAttempt = 0 }
            if receivedFrames != snapshot.receivedFrames { receivedFrames = snapshot.receivedFrames }
            if inputSupported != snapshot.inputSupported { inputSupported = snapshot.inputSupported }
            let enabled = active && snapshot.inputEnabled
            if inputEnabled != enabled { inputEnabled = enabled }
        }
        if active, let snapshot = pipeline?.snapshot { updateDecoder(snapshot) }
        refreshAudioStatus()
        let display = mailbox.statistics
        if renderedFrames != display.submitted { renderedFrames = display.submitted }
        if displayReplacements != display.replaced { displayReplacements = display.replaced }
        if !display.error.isEmpty {
            if !failed { failed = true }
            if detail != display.error { detail = display.error }
        }
    }
    private func updateDecoder(_ snapshot: DecoderSnapshot) {
        if decodedFrames != snapshot.decodedFrames { decodedFrames = snapshot.decodedFrames }
        if snapshot.width > 0 {
            let size = "\(snapshot.width) × \(snapshot.height)"
            if dimensions != size { dimensions = size }
        }
        let acceleration = snapshot.hardwareAccelerated.map { $0 ? "已确认硬解" : "软件解码" } ?? "未确认"
        if hardware != acceleration { hardware = acceleration }
    }

    private func reportData() throws -> Data {
        let display = mailbox.statistics
        let report: [String: Any] = ["schemaVersion": 1, "appVersion": "0.6.0", "appBuild": 8, "result": lastResult,
            "mode": replay ? "local_replay" : "live_lan", "host": replay ? "" : sessionHost,
            "startedAt": startedAt.map { ISO8601DateFormatter().string(from: $0) } ?? "",
            "recordedAt": ISO8601DateFormatter().string(from: Date()),
            "receivedFrames": receivedFrames, "decodedFrames": decodedFrames,
            "displaySubmittedFrames": display.submitted, "displayReplacedDecodedFrames": display.replaced,
            "displayError": display.error, "dimensions": dimensions, "hardwareDecode": hardware,
            "inputControlMessagesSent": connection?.snapshot.inputSent ?? 0,
            "inputEnabledAtReport": inputEnabled,
            "networkRTTMilliseconds": networkRTT as Any? ?? NSNull(),
            "networkRTTJitterMilliseconds": networkJitter as Any? ?? NSNull(),
            "networkRTTSamples": connection?.snapshot.rttSamples ?? 0,
            "networkType": networkType, "videoPayloadMbps": videoMbps, "receivedFPS": receivedFPS,
            "reconnectAttempt": reconnectAttempt, "reconnecting": reconnecting,
            "audioMuted": audioMuted, "audioStatus": audioStatus,
            "clipboardMode": clipboardMode.rawValue, "clipboardConnected": clipboardConnected,
            "clipboardUpdatesSent": clipboard.sentCount, "clipboardUpdatesApplied": clipboard.appliedCount,
            "clipboardPasteCommitted": clipboard.committedCount, "clipboardLastBytes": clipboard.lastBytes,
            "boundary": "Display submissions are not scanout timestamps. Replay uses synthetic pacing and does not prove live LAN. Input send counts do not prove target-app behavior. No end-to-end latency measurement."]
        return try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
    }
    private func writeAutomaticReport() {
        guard let reportPath else { return }
        let original = URL(fileURLWithPath: reportPath)
        // Each run keeps its own evidence, including replay followed by live use.
        let url = FileManager.default.fileExists(atPath: reportPath)
            ? original.deletingPathExtension().appendingPathExtension("\(generation.uuidString).json") : original
        do { try reportData().write(to: url, options: .withoutOverwriting) }
        catch { detail += " 诊断保存失败：\(error.localizedDescription)" }
    }
    func exportDiagnostics() {
        let panel = NSSavePanel(); panel.nameFieldStringValue = "harmony-remote-diagnostics.json"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try reportData().write(to: url, options: .atomic) }
        catch { detail = "诊断保存失败：\(error.localizedDescription)" }
    }

    // Development validation only: a bounded HRD1 packet archive built from a
    // captured stream. It is prominently identified as replay in the window.
    private func startReplay(_ path: String) {
        let (id, decoder) = prepare(isReplay: true)
        connection = nil
        let cancellation = ReplayCancellation(); replayCancellation = cancellation
        state = "本地验证回放"; detail = "回放已采集的鸿蒙桌面，用于验证 Mac 解码与显示；不是实时连接。"
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var outcome: Result<Void, Error> = .success(())
            do {
                let url = URL(fileURLWithPath: path)
                let attributes = try FileManager.default.attributesOfItem(atPath: path)
                guard let size = attributes[.size] as? NSNumber, size.int64Value > 0,
                    size.int64Value <= 70 * 1024 * 1024 else { throw ViewerFailure.message("回放文件大小不符合限制") }
                let file = try FileHandle(forReadingFrom: url); defer { try? file.close() }
                let parser = WireVideoParser()
                let start = ProcessInfo.processInfo.systemUptime
                var ended = false
                var count = 0
                while let chunk = try file.read(upToCount: 65_536), !chunk.isEmpty {
                    guard !cancellation.isCancelled else { throw ViewerFailure.message("回放已取消") }
                    for packet in try parser.feed(chunk) {
                        guard !ended else { throw ViewerFailure.message("EOS 后存在额外数据") }
                        if packet.type == 2 && !packet.payload.isEmpty {
                            let due = start + Double(packet.ptsUs) / 1_000_000
                            let delay = due - ProcessInfo.processInfo.systemUptime
                            if delay > 0 { Thread.sleep(forTimeInterval: min(delay, 0.1)) }
                            count += 1
                        }
                        try decoder.submit(packet)
                        if packet.type == 2 && packet.flags & 2 != 0 { ended = true }
                    }
                }
                try parser.finish()
                guard ended else { throw ViewerFailure.message("回放缺少完整 EOS") }
                let total = count
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.generation == id else { return }; self.receivedFrames = total
                }
            } catch { outcome = .failure(error) }
            let result = outcome
            DispatchQueue.main.async { [weak self] in
                guard let self, self.generation == id else { return }
                self.complete(id, decoder, result)
            }
        }
    }
}
