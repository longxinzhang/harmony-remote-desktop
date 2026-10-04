import Foundation

struct ClipboardSnapshot {
    let revision: Int
    let text: Data
    let owner: String?
}

protocol ClipboardAdapter: AnyObject {
    var revision: Int { get }
    func read(explicit: Bool) throws -> ClipboardSnapshot
    func write(_ text: Data, owner: String, expectedRevision: Int) throws -> Int
}

// All calls are serialized by the UI's main actor. Tests use a fake adapter and clock;
// neither the protocol nor this state machine ever logs clipboard contents/digests.
final class ClipboardCoordinator {
    private let adapter: ClipboardAdapter
    private let now: () -> TimeInterval
    var onSend: (ClipboardFrame) -> Void = { _ in }
    var onCommit: (String, String, String, Int) -> Void = { _, _, _, _ in }
    var onChange: () -> Void = {}
    private(set) var mode: ClipboardMode = .off
    private(set) var connected = false
    private(set) var allowed = false
    private(set) var canRead = false
    private(set) var canWrite = false
    private(set) var status = "剪贴板同步已关闭"
    private(set) var lastBytes = 0
    private(set) var appliedCount = 0
    private(set) var sentCount = 0
    private(set) var committedCount = 0
    private var epoch = ""
    private var observationGeneration = UUID()
    private var observed = 0
    private var logical: UInt64 = 0
    private struct Current {
        let version: ClipboardVersion
        let data: Data
        let revision: Int
        let owner: String?
    }
    private var current: Current?
    // Receive-only mode never reads/sends local text, but a local copy still
    // advances an ordering fence so an already-in-flight remote update loses.
    private var localFence: ClipboardVersion?
    private var currentVersion: ClipboardVersion { current?.version ?? localFence ?? .zero }
    private var remoteKnown: ClipboardVersion?
    private var inFlight: ClipboardFrame?
    private var flightStarted: TimeInterval = 0
    private var latest: ClipboardFrame?
    private var pendingModeID: String?
    private struct Paste {
        let operation: String
        let event: String
        let deadline: TimeInterval
        let valid: () -> Bool
        let permit: ClipboardPastePermit
        var committed = false
    }
    private var paste: Paste?
    var pastePending: Bool { paste != nil }
    var pastePermit: ClipboardPastePermit? { paste?.permit }
    // canRead/canWrite describe the last actual platform result, not permission to
    // attempt the first operation. In particular canWrite starts false on Host.
    var canPaste: Bool { connected && mode.sends && allowed }
    var canPull: Bool { connected && mode.receives && allowed }

    init(adapter: ClipboardAdapter, now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.adapter = adapter; self.now = now; observed = adapter.revision
    }
    func start(epoch: String, mode: ClipboardMode) {
        stop(); self.epoch = epoch; self.mode = mode; observed = adapter.revision
        status = "正在连接剪贴板通道"; onChange()
    }
    func ready() {
        guard !epoch.isEmpty else { return }
        connected = true; observed = adapter.revision
        sendMode()
        status = mode == .off ? "剪贴板同步已关闭" : "等待鸿蒙端允许剪贴板"; onChange()
    }
    func stop(reason: String = "剪贴板未连接") {
        observationGeneration = UUID()
        paste?.permit.cancel()
        connected = false; allowed = false; canRead = false; canWrite = false
        epoch = ""; logical = 0; current = nil; localFence = nil; remoteKnown = nil; inFlight = nil; latest = nil; paste = nil; pendingModeID = nil
        observed = adapter.revision; status = reason; onChange()
    }
    func setMode(_ value: ClipboardMode) {
        guard mode != value else { return }
        observationGeneration = UUID()
        mode = value; cancelPaste(); inFlight = nil; latest = nil; current = nil; localFence = nil; remoteKnown = nil
        observed = adapter.revision
        if connected { sendMode() }
        status = value == .off ? "剪贴板同步已关闭" : "已更新方向，仅同步之后的复制"; onChange()
    }
    private func sendMode() {
        allowed = false
        let frame = ClipboardWire.make("clipboard_mode", epoch: epoch, extra: ["mode": mode.rawValue])
        pendingModeID = frame.messageID; onSend(frame)
    }
    private func message(_ failure: ClipboardFailure) -> String {
        switch failure {
        case .denied: return "Mac 剪贴板读取需授权；可使用画面内粘贴或在系统设置允许访问"
        case .unsupported: return "仅支持纯文字，文件与图片未同步"
        case .oversized: return "文字超过 1 MiB，未同步"
        case .encoding: return "文字编码无效或包含空字符，未同步"
        case .timeout: return "剪贴板尚未同步，请重试"
        case .changed, .stale: return "剪贴板已变化，本次粘贴已取消"
        case .write: return "系统剪贴板写入失败"
        case .cancelled: return "已取消等待中的粘贴"
        default: return "剪贴板不可用（\(failure.rawValue)）"
        }
    }
    private func fail(_ error: Error) {
        status = message(error as? ClipboardFailure ?? .malformed); onChange()
    }
    func tick() {
        if let paste, now() >= paste.deadline || !paste.valid() { cancelPaste(reason: .timeout) }
        if inFlight != nil && now() - flightStarted >= 5 {
            inFlight = nil; latest = nil; cancelPaste(reason: .timeout); fail(ClipboardFailure.timeout)
        }
        poll()
    }
    func poll() {
        guard connected, mode.sends, allowed, adapter.revision != observed else { return }
        do { _ = try local(explicit: false) }
        catch {
            if error as? ClipboardFailure != .changed { observed = adapter.revision }
            fail(error)
        }
    }
    private func local(explicit: Bool, forceNew: Bool = false) throws -> ClipboardVersion {
        let generation = observationGeneration
        let snapshot = try adapter.read(explicit: explicit)
        _ = try ClipboardWire.text(snapshot.text)
        guard generation == observationGeneration, connected, allowed, mode.sends,
              snapshot.revision == adapter.revision else { throw ClipboardFailure.changed }
        observed = snapshot.revision
        if !forceNew, let current, current.revision == snapshot.revision, current.data == snapshot.text,
           current.owner == snapshot.owner { return current.version }
        // A fresh system revision is a fresh user copy, even when the text is identical.
        guard logical < UInt64.max else { throw ClipboardFailure.overflow }
        logical += 1
        let version = ClipboardVersion(counter: logical, origin: "mac")
        if paste?.event != version.eventID { cancelPaste(reason: .changed) }
        localFence = nil; current = Current(version: version, data: snapshot.text, revision: snapshot.revision, owner: snapshot.owner)
        let frame = ClipboardWire.make("clipboard_update", epoch: epoch,
            extra: ["originId": "mac", "counter": String(logical), "eventId": version.eventID,
                    "mime": ClipboardWire.mime, "sha256": ClipboardWire.digest(snapshot.text)], payload: snapshot.text)
        enqueue(frame); return version
    }
    private func enqueue(_ frame: ClipboardFrame) {
        if inFlight != nil { latest = frame }
        else { inFlight = frame; flightStarted = now(); sentCount += 1; onSend(frame) }
        lastBytes = frame.payload.count; status = "正在同步纯文字 · \(lastBytes) 字节"; onChange()
    }
    private func pump() {
        guard inFlight == nil, let frame = latest else { return }
        latest = nil; enqueue(frame)
    }
    func receive(_ frame: ClipboardFrame) throws {
        try ClipboardWire.validate(frame, epoch: epoch)
        guard connected else { return }
        switch frame.type {
        case "clipboard_status":
            guard frame.header["mode"] as? Int == mode.rawValue else { return } // An earlier mode's in-flight status.
            if let pendingModeID {
                guard frame.messageID == pendingModeID else { return }
                self.pendingModeID = nil; observed = adapter.revision
            }
            let wasAllowed = allowed
            allowed = WireProtocol.isBoolean(frame.header["allowed"], true)
            canRead = WireProtocol.isBoolean(frame.header["canRead"], true)
            canWrite = WireProtocol.isBoolean(frame.header["canWrite"], true)
            if wasAllowed != allowed {
                observationGeneration = UUID()
                observed = adapter.revision; current = nil; localFence = nil; remoteKnown = nil; inFlight = nil; latest = nil; cancelPaste()
            }
            if !allowed { cancelPaste() }
            let error = frame.header["error"] as? String ?? ""
            if mode == .off { status = "剪贴板同步已关闭" }
            else if !allowed { status = "请在鸿蒙端允许剪贴板" }
            else if ["read_permission_denied", "read_failed"].contains(error) {
                status = "鸿蒙读取剪贴板\(error == "read_permission_denied" ? "未授权" : "失败")；\(mode.sends ? "Mac → 鸿蒙仍可尝试" : "反向同步暂不可用")"
            } else if error == "write_failed" { status = "鸿蒙写入剪贴板失败，等待下次复制重试" }
            else if !error.isEmpty { status = "剪贴板部分能力暂不可用，画面与输入继续" }
            else { status = "纯文字同步已启用 · \(canRead || canWrite ? "已有系统读写结果" : "等下一次复制验证读写")" }
            onChange()
        case "clipboard_update":
            guard let version = frame.version, version.origin == "harmony" else { throw ClipboardFailure.malformed }
            logical = max(logical, version.counter)
            guard mode.receives, allowed else { acknowledge(frame, status: "denied", error: "DIRECTION_DENIED"); return }
            // Capture a local copy that arrived before this remote callback, before arbitration.
            if adapter.revision != observed {
                if mode.sends { do { _ = try local(explicit: false) } catch { fail(error); acknowledge(frame, status: "failed", error: "LOCAL_READ_FAILED"); return } }
                else {
                    guard logical < UInt64.max else { acknowledge(frame, status: "failed", error: "COUNTER_EXHAUSTED"); return }
                    logical += 1; observed = adapter.revision; current = nil
                    localFence = ClipboardVersion(counter: logical, origin: "mac")
                }
            }
            if let localFence, version <= localFence { acknowledge(frame, status: "stale", error: "STALE"); return }
            if let current, version <= current.version {
                let same = version == current.version && frame.payload == current.data && adapter.revision == current.revision
                acknowledge(frame, status: same ? "applied" : "stale", error: same ? "" : "STALE"); return
            }
            do {
                let owner = "\(epoch):\(version.eventID):\(ClipboardWire.digest(frame.payload))"
                let revision = try adapter.write(frame.payload, owner: owner, expectedRevision: observed)
                observed = revision; localFence = nil; current = Current(version: version, data: frame.payload, revision: revision, owner: owner)
                remoteKnown = version
                cancelPaste(reason: .changed); appliedCount += 1; lastBytes = frame.payload.count
                status = "已接收纯文字 · \(lastBytes) 字节"; onChange()
                acknowledge(frame, status: "applied", error: "")
            } catch { fail(error); acknowledge(frame, status: "failed", error: (error as? ClipboardFailure ?? .write).rawValue) }
        case "clipboard_applied":
            guard let flight = inFlight, frame.messageID == flight.messageID,
                  frame.header["eventId"] as? String == flight.header["eventId"] as? String else { return }
            guard let version = frame.version else { throw ClipboardFailure.malformed }
            logical = max(logical, version.counter)
            let applied = frame.header["status"] as? String == "applied" && version == flight.version
            if applied { remoteKnown = version }
            inFlight = nil
            if paste?.event == flight.version?.eventID {
                if applied { commitPaste() } else { cancelPaste(reason: .stale) }
            }
            if applied {
                if !pastePending { status = "已同步纯文字 · \(flight.payload.count) 字节"; onChange() }
            } else { fail(ClipboardFailure.stale) }
            pump()
        default: throw ClipboardFailure.malformed
        }
    }
    private func acknowledge(_ frame: ClipboardFrame, status: String, error: String) {
        let version = currentVersion
        onSend(ClipboardWire.make("clipboard_applied", epoch: epoch,
            extra: ["eventId": frame.header["eventId"] as? String ?? "", "status": status,
                    "counter": String(version.counter), "originId": version.origin, "error": error], messageID: frame.messageID))
    }
    func pull() {
        guard canPull else { status = "鸿蒙 → Mac 方向或读取权限尚未开启"; onChange(); return }
        onSend(ClipboardWire.make("clipboard_pull", epoch: epoch)); status = "正在获取远端纯文字"; onChange()
    }
    func requestPaste(valid: @escaping () -> Bool) {
        cancelPaste()
        guard connected else { status = "剪贴板通道未连接，本次粘贴未发送"; onChange(); return }
        guard mode.sends, allowed else { status = "请先开启 Mac → 鸿蒙方向并在鸿蒙端允许剪贴板"; onChange(); return }
        guard valid() else { fail(ClipboardFailure.cancelled); return }
        do {
            // Explicit paste is a fresh user intent. Host must first write a Mac
            // event even if the existing local text originated on Harmony.
            let version = try local(explicit: true, forceNew: true)
            let deadline = now() + 1.5
            paste = Paste(operation: ClipboardWire.identifier(), event: version.eventID, deadline: deadline, valid: valid,
                          permit: ClipboardPastePermit(deadline: deadline, now: now))
            if remoteKnown == version { commitPaste() }
            else if inFlight?.version != version && latest?.version != version, let current {
                enqueue(ClipboardWire.make("clipboard_update", epoch: epoch,
                    extra: ["originId": "mac", "counter": String(version.counter), "eventId": version.eventID,
                            "mime": ClipboardWire.mime, "sha256": ClipboardWire.digest(current.data)], payload: current.data))
            }
        } catch { fail(error) }
    }
    private func commitPaste() {
        guard var pending = paste, !pending.committed else { return }
        guard pending.valid(), canPaste, now() < pending.deadline,
              current?.version.eventID == pending.event, current?.revision == adapter.revision else {
            cancelPaste(reason: .changed); return
        }
        pending.committed = true; paste = pending
        let ttl = max(1, min(1500, Int((pending.deadline - now()) * 1000)))
        onCommit(epoch, pending.event, pending.operation, ttl)
    }
    func pasteResult(_ object: [String: Any]) {
        guard let pending = paste, pending.committed, object["operationId"] as? String == pending.operation else { return }
        pending.permit.cancel()
        paste = nil
        if object["status"] as? String == "committed" { committedCount += 1; status = "远端粘贴按键已完成"; onChange() }
        else { fail(ClipboardFailure.stale) }
    }
    func cancelPaste(reason: ClipboardFailure = .cancelled) {
        guard let paste else { return }; paste.permit.cancel(); self.paste = nil; fail(reason)
    }
}
