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

    func put(_ pixels: CVPixelBuffer, ptsUs: UInt64) {
        lock.lock(); defer { lock.unlock() }
        if latest != nil { replacements += 1 }
        latest = DecodedFrame(pixels: pixels, ptsUs: ptsUs)
        frameWidth = CVPixelBufferGetWidth(pixels); frameHeight = CVPixelBufferGetHeight(pixels)
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

final class VideoSurfaceView: NSView {
    let display = AVSampleBufferDisplayLayer()
    var mailbox: FrameMailbox
    private var timer: Timer?
    private let input = RemoteInputEngine()
    private var inputEnabled = false
    private var inputTracking: NSTrackingArea?
    private var observers: [NSObjectProtocol] = []
    private var allowsMomentum = false
    private var clipboardPasteEnabled = false
    private var onPaste: (@escaping () -> Bool) -> Void = { _ in }
    private var focusGeneration: UInt64 = 0

    init(mailbox: FrameMailbox) {
        self.mailbox = mailbox
        super.init(frame: .zero)
        wantsLayer = true
        layer = display
        display.backgroundColor = NSColor.black.cgColor
        display.videoGravity = .resizeAspect
        timer = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in self?.presentLatest() }
        if let timer { RunLoop.main.add(timer, forMode: .common) }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }
    func stop() { releaseFocus(); timer?.invalidate(); timer = nil; removeObservers(); display.flushAndRemoveImage() }
    deinit { timer?.invalidate(); for token in observers { NotificationCenter.default.removeObserver(token) } }

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
                        clipboardPasteEnabled: Bool = false, onPaste: @escaping (@escaping () -> Bool) -> Void = { _ in }) {
        if inputEnabled && (!enabled || input.keyboardMode != keyboardMode) { releaseFocus() }
        inputEnabled = enabled; input.keyboardMode = keyboardMode
        input.onInput = onInput; input.onRelease = onRelease
        self.clipboardPasteEnabled = clipboardPasteEnabled; self.onPaste = onPaste
        refreshContext(inside: input.insideVideo)
    }
    func replaceMailbox(_ value: FrameMailbox) {
        releaseFocus(); display.flushAndRemoveImage(); mailbox = value
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
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard keyboardReady() else { return super.performKeyEquivalent(with: event) }
        if localKey(event) { return true }
        if reservedKey(event) { releaseFocus(); return super.performKeyEquivalent(with: event) }
        if clipboardKey(event) { return true }
        guard event.modifierFlags.contains(.command), RemoteInputEngine.isCommandShortcut(event.keyCode) else {
            return super.performKeyEquivalent(with: event)
        }
        input.synchronizeModifiers(rawFlags: event.modifierFlags.rawValue)
        input.shortcutStroke(event.keyCode)
        return true
    }
    override func keyDown(with event: NSEvent) {
        guard keyboardReady() else { super.keyDown(with: event); return }
        if localKey(event) { return }
        if reservedKey(event) { releaseFocus(); super.keyDown(with: event); return }
        if clipboardKey(event) { return }
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
    func makeNSView(context: Context) -> VideoSurfaceView {
        let view = VideoSurfaceView(mailbox: mailbox)
        view.configureInput(enabled: inputEnabled, keyboardMode: keyboardMode, onInput: onInput, onRelease: onRelease,
                            clipboardPasteEnabled: clipboardPasteEnabled, onPaste: onPaste)
        return view
    }
    func updateNSView(_ view: VideoSurfaceView, context: Context) {
        if view.mailbox !== mailbox { view.replaceMailbox(mailbox) }
        view.configureInput(enabled: inputEnabled, keyboardMode: keyboardMode, onInput: onInput, onRelease: onRelease,
                            clipboardPasteEnabled: clipboardPasteEnabled, onPaste: onPaste)
    }
    static func dismantleNSView(_ view: VideoSurfaceView, coordinator: ()) { view.stop() }
}
