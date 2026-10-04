import AppKit
import AVFoundation
import CoreMedia
import CoreVideo
import SwiftUI

struct DecodedFrame {
    let pixels: CVPixelBuffer
    let ptsUs: UInt64
}

// Decoding preserves reference frames. Only already-decoded display frames may
// be replaced, and the UI retains at most one pending image.
final class FrameMailbox {
    private let lock = NSLock()
    private var latest: DecodedFrame?
    private var replacements = 0
    private var submitted = 0
    private var displayError = ""
    private var frameWidth = 0
    private var frameHeight = 0
    private var onPendingFrame: (() -> Void)?

    func put(_ pixels: CVPixelBuffer, ptsUs: UInt64) {
        lock.lock()
        let notify = latest == nil ? onPendingFrame : nil
        if latest != nil { replacements += 1 }
        latest = DecodedFrame(pixels: pixels, ptsUs: ptsUs)
        frameWidth = CVPixelBufferGetWidth(pixels); frameHeight = CVPixelBufferGetHeight(pixels)
        lock.unlock()
        notify?()
    }
    func observePendingFrames(_ callback: (() -> Void)?) {
        lock.lock()
        onPendingFrame = callback
        let notify = latest != nil ? callback : nil
        lock.unlock()
        notify?()
    }
    func take() -> DecodedFrame? {
        lock.lock(); defer { lock.unlock() }
        let value = latest; latest = nil; return value
    }
    func displayed(error: String? = nil) {
        lock.lock(); defer { lock.unlock() }
        if let error { displayError = error } else { submitted += 1 }
    }
    var statistics: (replaced: Int, submitted: Int, error: String) {
        lock.lock(); defer { lock.unlock() }
        return (replacements, submitted, displayError)
    }
    var dimensions: (width: Int, height: Int) {
        lock.lock(); defer { lock.unlock() }
        return (frameWidth, frameHeight)
    }
}

// One wakeup for pending work, never a repeating idle display timer. All calls
// belong to the main thread; frame arrival is marshalled there by the view.
final class FramePresentationScheduler {
    private var timer: Timer?
    private var lastPresentation: TimeInterval?
    private var stopped = false
    private let interval: TimeInterval
    private let present: () -> Void
    var isScheduled: Bool { timer != nil }

    init(interval: TimeInterval = 1.0 / 30.0, present: @escaping () -> Void) {
        self.interval = interval; self.present = present
    }
    func request() {
        guard !stopped, timer == nil else { return }
        let now = ProcessInfo.processInfo.systemUptime
        let delay = lastPresentation.map { max(0, $0 + interval - now) } ?? 0
        let timer = Timer(timeInterval: delay, repeats: false) { [weak self] _ in
            guard let self, !self.stopped else { return }
            self.timer = nil
            self.lastPresentation = ProcessInfo.processInfo.systemUptime
            self.present()
        }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .common)
    }
    func cancel() { timer?.invalidate(); timer = nil }
    func stop() { stopped = true; cancel() }
    deinit { timer?.invalidate() }
}

final class VideoSurfaceView: NSView {
    let display = AVSampleBufferDisplayLayer()
    var mailbox: FrameMailbox
    private lazy var presentation = FramePresentationScheduler { [weak self] in self?.presentLatest() }
    private var stopped = false
    private var displayStatusObserver: NSKeyValueObservation?
    private let input = RemoteInputEngine()
    private var inputEnabled = false
    private var inputTracking: NSTrackingArea?
    private var observers: [NSObjectProtocol] = []
    private var allowsMomentum = false
    private var clipboardPasteEnabled = false
    private var onPaste: (@escaping () -> Bool) -> Void = { _ in }
    private var onRemoteCopy: () -> Void = {}
    private var focusGeneration: UInt64 = 0

    init(mailbox: FrameMailbox) {
        self.mailbox = mailbox
        super.init(frame: .zero)
        wantsLayer = true
        layer = display
        display.backgroundColor = NSColor.black.cgColor
        display.videoGravity = .resizeAspect
        observeMailbox()
        // status is KVO-observable (AVSampleBufferDisplayLayer.h). Keep observing
        // asynchronous failure after the last frame without an idle polling timer.
        displayStatusObserver = display.observe(\.status, options: [.new]) { [weak self] _, change in
            guard change.newValue == .failed else { return }
            DispatchQueue.main.async { [weak self] in
                guard let self, !self.stopped, self.display.status == .failed else { return }
                self.mailbox.displayed(error: "显示层失败：\((self.display.error as NSError?)?.code ?? -1)")
            }
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }
    func stop() {
        stopped = true; mailbox.observePendingFrames(nil); presentation.stop()
        releaseFocus(); removeObservers(); display.flushAndRemoveImage()
        displayStatusObserver?.invalidate(); displayStatusObserver = nil
    }
    deinit {
        mailbox.observePendingFrames(nil); presentation.stop()
        for token in observers { NotificationCenter.default.removeObserver(token) }
        displayStatusObserver?.invalidate()
    }

    private func observeMailbox() {
        let observed = mailbox
        observed.observePendingFrames { [weak self, weak observed] in
            DispatchQueue.main.async { [weak self, weak observed] in
                guard let self, let observed, !self.stopped, self.mailbox === observed else { return }
                self.presentation.request()
            }
        }
    }

    override var acceptsFirstResponder: Bool { inputEnabled && window?.isKeyWindow == true && NSApp.isActive }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { false }
    override func resignFirstResponder() -> Bool { releaseContext(); return super.resignFirstResponder() }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow(); releaseContext(); removeObservers()
        guard let window else { return }
        window.acceptsMouseMovedEvents = true
        observers.append(NotificationCenter.default.addObserver(forName: NSWindow.didResignKeyNotification,
            object: window, queue: .main) { [weak self] _ in self?.releaseFocus() })
        observers.append(NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification,
            object: NSApp, queue: .main) { [weak self] _ in self?.releaseFocus() })
    }
    private func removeObservers() {
        for token in observers { NotificationCenter.default.removeObserver(token) }; observers.removeAll()
    }
    override func updateTrackingAreas() {
        if let inputTracking { removeTrackingArea(inputTracking) }
        let area = NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area); inputTracking = area
        super.updateTrackingAreas()
    }
    override func layout() {
        super.layout()
        if input.capturesInput, let window,
           normalized(convert(window.mouseLocationOutsideOfEventStream, from: nil)) == nil { releaseFocus() }
    }
    func configureInput(enabled: Bool, keyboardMode: RemoteKeyboardMode,
                        onInput: @escaping ([String: Any]) -> Void, onRelease: @escaping () -> Void,
                        clipboardPasteEnabled: Bool = false, onPaste: @escaping (@escaping () -> Bool) -> Void = { _ in },
                        onRemoteCopy: @escaping () -> Void = {}) {
        if inputEnabled && (!enabled || input.keyboardMode != keyboardMode) { releaseFocus() }
        inputEnabled = enabled; input.keyboardMode = keyboardMode
        input.onInput = onInput; input.onRelease = onRelease
        self.clipboardPasteEnabled = clipboardPasteEnabled; self.onPaste = onPaste
        self.onRemoteCopy = onRemoteCopy
        refreshContext(inside: input.insideVideo)
    }
    func replaceMailbox(_ value: FrameMailbox) {
        mailbox.observePendingFrames(nil); presentation.cancel()
        releaseFocus(); display.flushAndRemoveImage(); mailbox = value
        if !stopped { observeMailbox() }
    }
    private func refreshContext(inside: Bool) {
        input.context(enabled: inputEnabled, focused: window?.firstResponder === self,
                      windowActive: window?.isKeyWindow == true && NSApp.isActive, insideVideo: inside)
    }
    private func releaseContext() {
        focusGeneration &+= 1
        allowsMomentum = false
        input.context(enabled: inputEnabled, focused: false,
                      windowActive: window?.isKeyWindow == true && NSApp.isActive, insideVideo: false)
    }
    private func releaseFocus() {
        releaseContext()
        if window?.firstResponder === self { window?.makeFirstResponder(nil) }
    }
    private func normalized(_ point: CGPoint) -> CGPoint? {
        let size = mailbox.dimensions
        return RemoteInputGeometry.normalized(point,
            in: RemoteInputGeometry.videoRect(bounds: bounds, width: size.width, height: size.height))
    }
    private func position(_ event: NSEvent) -> CGPoint? {
        let p = normalized(convert(event.locationInWindow, from: nil))
        if p == nil { releaseFocus() } else { refreshContext(inside: true) }
        return p
    }
    private func keyboardReady() -> Bool {
        guard let window else { releaseContext(); return false }
        let inside = normalized(convert(window.mouseLocationOutsideOfEventStream, from: nil)) != nil
        if !inside { releaseFocus(); return false }
        refreshContext(inside: inside)
        return input.capturesInput
    }
    private func handleButton(_ event: NSEvent, down: Bool) {
        guard inputEnabled, window?.isKeyWindow == true, NSApp.isActive,
              let p = position(event) else { return }
        let button: String
        switch event.buttonNumber { case 0: button = "left"; case 1: button = "right"; case 2: button = "middle"; default: return }
        if down { guard window?.makeFirstResponder(self) == true else { return }; refreshContext(inside: true) }
        input.synchronizeModifiers(rawFlags: event.modifierFlags.rawValue)
        input.button(button, down: down, position: p)
    }
    override func mouseDown(with event: NSEvent) { handleButton(event, down: true) }
    override func mouseUp(with event: NSEvent) { handleButton(event, down: false) }
    override func rightMouseDown(with event: NSEvent) { handleButton(event, down: true) }
    override func rightMouseUp(with event: NSEvent) { handleButton(event, down: false) }
    override func otherMouseDown(with event: NSEvent) { handleButton(event, down: true) }
    override func otherMouseUp(with event: NSEvent) { handleButton(event, down: false) }
    override func mouseMoved(with event: NSEvent) { input.move(position(event)) }
    override func mouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func rightMouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func otherMouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func mouseExited(with event: NSEvent) { releaseFocus() }
    override func scrollWheel(with event: NSEvent) {
        guard let point = position(event), input.capturesInput else { return }
        if event.momentumPhase.isEmpty { allowsMomentum = true }
        else if !allowsMomentum { return }
        input.scroll(dx: Double(event.scrollingDeltaX), dy: Double(event.scrollingDeltaY),
                     precise: event.hasPreciseScrollingDeltas, at: point)
        if event.momentumPhase.contains(.ended) || event.momentumPhase.contains(.cancelled) { allowsMomentum = false }
    }
    private func localKey(_ event: NSEvent) -> Bool {
        let command = event.modifierFlags.contains(.command), shift = event.modifierFlags.contains(.shift)
        if RemoteInputEngine.isLocalRelease(key: event.keyCode, command: command, shift: shift) {
            releaseFocus(); return true
        }
        return false
    }
    private func reservedKey(_ event: NSEvent) -> Bool {
        RemoteInputEngine.isSystemReserved(key: event.keyCode, command: event.modifierFlags.contains(.command),
                                          control: event.modifierFlags.contains(.control))
    }
    private func clipboardKey(_ event: NSEvent) -> Bool {
        guard clipboardPasteEnabled, RemoteInputEngine.isClipboardPaste(key: event.keyCode, mode: input.keyboardMode,
            command: event.modifierFlags.contains(.command), control: event.modifierFlags.contains(.control),
            shift: event.modifierFlags.contains(.shift), option: event.modifierFlags.contains(.option)) else { return false }
        if event.isARepeat { return true }
        // Clear held modifiers before the barrier exists. No V key is forwarded.
        // A later ordinary event will resynchronize physical modifier flags.
        input.releaseAll()
        let generation = focusGeneration
        onPaste { [weak self] in
            guard let self, self.focusGeneration == generation else { return false }
            return self.keyboardReady()
        }
        return true
    }
    private func noteRemoteCopy(_ event: NSEvent) {
        guard clipboardPasteEnabled, !event.isARepeat,
              RemoteInputEngine.isClipboardCopy(key: event.keyCode, mode: input.keyboardMode,
                command: event.modifierFlags.contains(.command), control: event.modifierFlags.contains(.control),
                shift: event.modifierFlags.contains(.shift), option: event.modifierFlags.contains(.option)) else { return }
        onRemoteCopy()
    }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard keyboardReady() else { return super.performKeyEquivalent(with: event) }
        if localKey(event) { return true }
        if reservedKey(event) { releaseFocus(); return super.performKeyEquivalent(with: event) }
        if clipboardKey(event) { return true }
        guard event.modifierFlags.contains(.command), RemoteInputEngine.isCommandShortcut(event.keyCode) else {
            return super.performKeyEquivalent(with: event)
        }
        noteRemoteCopy(event)
        input.synchronizeModifiers(rawFlags: event.modifierFlags.rawValue)
        input.shortcutStroke(event.keyCode)
        return true
    }
    override func keyDown(with event: NSEvent) {
        guard keyboardReady() else { super.keyDown(with: event); return }
        if localKey(event) { return }
        if reservedKey(event) { releaseFocus(); super.keyDown(with: event); return }
        if clipboardKey(event) { return }
        noteRemoteCopy(event)
        input.synchronizeModifiers(rawFlags: event.modifierFlags.rawValue)
        input.keyDown(event.keyCode, isRepeat: event.isARepeat)
    }
    override func keyUp(with event: NSEvent) {
        guard keyboardReady() else { return }
        input.keyUp(event.keyCode)
    }
    override func flagsChanged(with event: NSEvent) {
        guard keyboardReady() else { return }
        if event.keyCode == 57 { input.capsLockChanged(event.modifierFlags.contains(.capsLock)) }
        else { input.synchronizeModifiers(rawFlags: event.modifierFlags.rawValue, changedKey: event.keyCode) }
    }

    private func presentLatest() {
        // Failures may arrive asynchronously after enqueue, including on the last
        // frame. Observe them before an empty-mailbox return or flush resets status.
        if display.status == .failed {
            let code = display.error.map { ($0 as NSError).code } ?? -1
            mailbox.displayed(error: "显示层失败：\(code)")
        }
        guard let frame = mailbox.take() else { return }
        var format: CMVideoFormatDescription?
        let formatStatus = CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault,
            imageBuffer: frame.pixels, formatDescriptionOut: &format)
        guard formatStatus == noErr, let format else {
            mailbox.displayed(error: "图像格式创建失败：\(formatStatus)"); return
        }
        var timing = CMSampleTimingInfo(duration: .invalid,
            presentationTimeStamp: CMTime(value: Int64(clamping: frame.ptsUs), timescale: 1_000_000),
            decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        let status = CMSampleBufferCreateReadyWithImageBuffer(allocator: kCFAllocatorDefault,
            imageBuffer: frame.pixels, formatDescription: format, sampleTiming: &timing, sampleBufferOut: &sample)
        guard status == noErr, let sample else {
            mailbox.displayed(error: "显示帧创建失败：\(status)"); return
        }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true) {
            let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dictionary, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        // These are decoded pixel buffers, so flushing old display images cannot
        // break H.264 reference dependencies. Never build a playback backlog.
        if display.status == .failed || !display.isReadyForMoreMediaData { display.flush() }
        display.enqueue(sample)
        mailbox.displayed(error: display.status == .failed ? "显示层拒绝图像" : nil)
    }
}

struct VideoSurface: NSViewRepresentable {
    let mailbox: FrameMailbox
    var inputEnabled: Bool = false
    var keyboardMode: RemoteKeyboardMode = .macFriendly
    var onInput: ([String: Any]) -> Void = { _ in }
    var onRelease: () -> Void = {}
    var clipboardPasteEnabled = false
    var onPaste: (@escaping () -> Bool) -> Void = { _ in }
    var onRemoteCopy: () -> Void = {}
    func makeNSView(context: Context) -> VideoSurfaceView {
        let view = VideoSurfaceView(mailbox: mailbox)
        view.configureInput(enabled: inputEnabled, keyboardMode: keyboardMode, onInput: onInput, onRelease: onRelease,
                            clipboardPasteEnabled: clipboardPasteEnabled, onPaste: onPaste, onRemoteCopy: onRemoteCopy)
        return view
    }
    func updateNSView(_ view: VideoSurfaceView, context: Context) {
        if view.mailbox !== mailbox { view.replaceMailbox(mailbox) }
        view.configureInput(enabled: inputEnabled, keyboardMode: keyboardMode, onInput: onInput, onRelease: onRelease,
                            clipboardPasteEnabled: clipboardPasteEnabled, onPaste: onPaste, onRemoteCopy: onRemoteCopy)
    }
    static func dismantleNSView(_ view: VideoSurfaceView, coordinator: ()) { view.stop() }
}
