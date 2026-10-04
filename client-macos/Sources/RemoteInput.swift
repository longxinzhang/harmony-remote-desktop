import Foundation
import CoreGraphics

enum RemoteKeyboardMode: String, CaseIterable, Identifiable {
    case macFriendly
    case raw
    var id: String { rawValue }
    var label: String { self == .macFriendly ? "Mac 习惯（⌘→Ctrl）" : "原始键位（⌘→Meta）" }
}

enum RemoteInputGeometry {
    // NSView coordinates are bottom-left based; the wire uses top-left normalized.
    static func videoRect(bounds: CGRect, width: Int, height: Int) -> CGRect? {
        guard width > 0, height > 0, bounds.width.isFinite, bounds.height.isFinite,
              bounds.width > 0, bounds.height > 0 else { return nil }
        let scale = min(bounds.width / CGFloat(width), bounds.height / CGFloat(height))
        let size = CGSize(width: CGFloat(width) * scale, height: CGFloat(height) * scale)
        return CGRect(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2,
                      width: size.width, height: size.height)
    }
    static func normalized(_ point: CGPoint, in rect: CGRect?) -> CGPoint? {
        guard let rect, point.x.isFinite, point.y.isFinite, rect.width > 0, rect.height > 0,
              point.x >= rect.minX, point.x <= rect.maxX, point.y >= rect.minY, point.y <= rect.maxY else { return nil }
        return CGPoint(x: (point.x - rect.minX) / rect.width, y: (rect.maxY - point.y) / rect.height)
    }
}

// No AppKit dependency: callers supply physical Mac key IDs and focus context.
// Wire codes are platform-independent strings, never NSEvent.keyCode integers.
final class RemoteInputEngine {
    var keyboardMode: RemoteKeyboardMode = .macFriendly
    var onInput: ([String: Any]) -> Void
    var onRelease: () -> Void
    private(set) var enabled = false
    private(set) var focused = false
    private(set) var windowActive = false
    private(set) var insideVideo = false
    private(set) var heldKeys: [UInt16: String] = [:]
    private(set) var heldButtons = Set<String>()
    private var lastCapsState: Bool?
    var capturesInput: Bool { enabled && focused && windowActive && insideVideo }

    init(onInput: @escaping ([String: Any]) -> Void = { _ in }, onRelease: @escaping () -> Void = {}) {
        self.onInput = onInput; self.onRelease = onRelease
    }
    func context(enabled: Bool, focused: Bool, windowActive: Bool, insideVideo: Bool) {
        let wasCapturing = capturesInput
        self.enabled = enabled; self.focused = focused
        self.windowActive = windowActive; self.insideVideo = insideVideo
        if wasCapturing && !capturesInput { releaseAll() }
    }
    func releaseAll() {
        // Emit explicit matching ups, then the authoritative release-all fallback.
        let codes = Set(heldKeys.values)
        for code in codes.sorted() { emitKey(code, "up") }
        for button in heldButtons.sorted() { emitButton(button, "up") }
        heldKeys.removeAll(); heldButtons.removeAll(); lastCapsState = nil
        onRelease()
    }
    func move(_ position: CGPoint?) {
        guard capturesInput, let p = position, p.x.isFinite, p.y.isFinite,
              (0...1).contains(p.x), (0...1).contains(p.y) else { return }
        onInput(["type": "mouse_move", "x": Double(p.x), "y": Double(p.y)])
    }
    func button(_ button: String, down: Bool, position: CGPoint?) {
        guard capturesInput, ["left", "right", "middle"].contains(button), let position,
              position.x.isFinite, position.y.isFinite,
              (0...1).contains(position.x), (0...1).contains(position.y) else { return }
        move(position)
        if down {
            guard heldButtons.insert(button).inserted else { return }
            emitButton(button, "down")
        } else if heldButtons.remove(button) != nil { emitButton(button, "up") }
    }
    func scroll(dx: Double, dy: Double, precise: Bool, at position: CGPoint? = nil) {
        guard capturesInput, dx.isFinite, dy.isFinite else { return }
        if let position {
            guard position.x.isFinite, position.y.isFinite,
                  (0...1).contains(position.x), (0...1).contains(position.y) else { return }
        }
        // Fixed wire unit: 10 precise scrolling points = 1 wheel unit. Non-precise
        // device deltas are already wheel units. Host emits begin/update/end per message.
        let scale = precise ? 0.1 : 1.0
        let x = max(-120, min(120, dx * scale)), y = max(-120, min(120, dy * scale))
        guard x != 0 || y != 0 else { return }
        if let position { move(position) }
        onInput(["type": "scroll", "dx": x, "dy": y])
    }
    func keyDown(_ physical: UInt16, isRepeat: Bool = false) {
        guard capturesInput else { return }
        if let original = heldKeys[physical] {
            if isRepeat && !Self.isModifier(physical) && heldKeys.values.filter({ $0 == original }).count == 1 {
                emitKey(original, "up"); emitKey(original, "down")
            }
            return
        }
        guard let mapped = Self.keyCode(physical, mode: keyboardMode) else { return }
        let alreadyHeld = heldKeys.values.contains(mapped)
        heldKeys[physical] = mapped
        if !alreadyHeld { emitKey(mapped, "down") }
    }
    func keyUp(_ physical: UInt16) {
        // The mapping at physical down is authoritative, even after mode changes.
        guard let original = heldKeys.removeValue(forKey: physical) else { return }
        if !heldKeys.values.contains(original) { emitKey(original, "up") }
    }
    func modifier(_ physical: UInt16, down: Bool) {
        guard Self.isModifier(physical) else { return }
        if down { keyDown(physical) } else { keyUp(physical) }
    }
    func capsLockChanged(_ state: Bool) {
        guard capturesInput, lastCapsState != state else { return }
        lastCapsState = state
        emitKey("KEY_CAPS_LOCK", "down"); emitKey("KEY_CAPS_LOCK", "up")
    }
    func shortcutStroke(_ physical: UInt16) {
        // AppKit key equivalents may consume keyUp; these shortcuts are complete
        // strokes. Modifier ownership still follows physical flagsChanged events.
        if heldKeys[physical] != nil { keyUp(physical) }
        keyDown(physical); keyUp(physical)
    }
    private func emitKey(_ code: String, _ action: String) { onInput(["type": "key", "code": code, "action": action]) }
    private func emitButton(_ button: String, _ action: String) { onInput(["type": "mouse_button", "button": button, "action": action]) }

    // Modifier raw flags from the installed IOKit IOLLEvent.h. Aggregate bits
    // match NSEvent.ModifierFlags. Side-specific flags retain physical reference counts.
    static let modifierGroups: [(UInt, UInt16, UInt, UInt16, UInt)] = [
        (1 << 17, 56, 0x2, 60, 0x4), (1 << 18, 59, 0x1, 62, 0x2000),
        (1 << 19, 58, 0x20, 61, 0x40), (1 << 20, 55, 0x8, 54, 0x10)
    ]
    func synchronizeModifiers(rawFlags: UInt, changedKey: UInt16? = nil) {
        guard capturesInput else { return }
        for (aggregate, left, leftMask, right, rightMask) in Self.modifierGroups {
            var desired = Set<UInt16>()
            if rawFlags & aggregate != 0 {
                if rawFlags & (leftMask | rightMask) != 0 {
                    if rawFlags & leftMask != 0 { desired.insert(left) }
                    if rawFlags & rightMask != 0 { desired.insert(right) }
                } else {
                    // Fallback for aggregate-only synthetic/device events.
                    if heldKeys[left] != nil { desired.insert(left) }
                    if heldKeys[right] != nil { desired.insert(right) }
                    if let changedKey, changedKey == left || changedKey == right {
                        if desired.contains(changedKey) { desired.remove(changedKey) } else { desired.insert(changedKey) }
                    }
                    if desired.isEmpty { desired.insert(left) }
                }
            }
            // Add before removing so a left/right handoff does not pulse a shared modifier.
            for physical in [left, right] where desired.contains(physical) { modifier(physical, down: true) }
            for physical in [left, right] where !desired.contains(physical) { modifier(physical, down: false) }
        }
    }
    static func isModifier(_ code: UInt16) -> Bool { [54,55,56,58,59,60,61,62].contains(code) }
    static func isLocalRelease(key: UInt16, command: Bool, shift: Bool) -> Bool { key == 53 && command && shift }
    static func isSystemReserved(key: UInt16, command: Bool, control: Bool) -> Bool {
        (command && (key == 48 || key == 49)) || (control && (123...126).contains(key))
    }
    static func isCommandShortcut(_ key: UInt16) -> Bool { [0,8,9,37].contains(key) } // A,C,V,L

    // HIToolbox Events.h kVK values map physical ANSI positions; text/IME injection
    // is not synthesized. Unsupported Fn/media/IME keys are not sent.
    static func keyCode(_ code: UInt16, mode: RemoteKeyboardMode) -> String? {
        if code == 55 { return mode == .macFriendly ? "KEY_CTRL_LEFT" : "KEY_META_LEFT" }
        if code == 54 { return mode == .macFriendly ? "KEY_CTRL_RIGHT" : "KEY_META_RIGHT" }
        if code == 59 { return "KEY_CTRL_LEFT" }; if code == 62 { return "KEY_CTRL_RIGHT" }
        if code == 56 { return "KEY_SHIFT_LEFT" }; if code == 60 { return "KEY_SHIFT_RIGHT" }
        if code == 58 { return "KEY_ALT_LEFT" }; if code == 61 { return "KEY_ALT_RIGHT" }
        return [
            0:"KEY_A",1:"KEY_S",2:"KEY_D",3:"KEY_F",4:"KEY_H",5:"KEY_G",6:"KEY_Z",7:"KEY_X",8:"KEY_C",9:"KEY_V",
            11:"KEY_B",12:"KEY_Q",13:"KEY_W",14:"KEY_E",15:"KEY_R",16:"KEY_Y",17:"KEY_T",
            18:"KEY_1",19:"KEY_2",20:"KEY_3",21:"KEY_4",22:"KEY_6",23:"KEY_5",24:"KEY_EQUALS",25:"KEY_9",
            26:"KEY_7",27:"KEY_MINUS",28:"KEY_8",29:"KEY_0",30:"KEY_RIGHT_BRACKET",31:"KEY_O",32:"KEY_U",
            33:"KEY_LEFT_BRACKET",34:"KEY_I",35:"KEY_P",36:"KEY_ENTER",37:"KEY_L",38:"KEY_J",39:"KEY_APOSTROPHE",
            40:"KEY_K",41:"KEY_SEMICOLON",42:"KEY_BACKSLASH",43:"KEY_COMMA",44:"KEY_SLASH",45:"KEY_N",46:"KEY_M",
            47:"KEY_PERIOD",48:"KEY_TAB",49:"KEY_SPACE",50:"KEY_GRAVE",51:"KEY_BACKSPACE",53:"KEY_ESCAPE",
            65:"KEY_PERIOD",75:"KEY_SLASH",76:"KEY_ENTER",78:"KEY_MINUS",81:"KEY_EQUALS",
            82:"KEY_0",83:"KEY_1",84:"KEY_2",85:"KEY_3",86:"KEY_4",87:"KEY_5",88:"KEY_6",89:"KEY_7",91:"KEY_8",92:"KEY_9",
            96:"KEY_F5",97:"KEY_F6",98:"KEY_F7",99:"KEY_F3",100:"KEY_F8",101:"KEY_F9",103:"KEY_F11",
            109:"KEY_F10",111:"KEY_F12",115:"KEY_HOME",116:"KEY_PAGE_UP",117:"KEY_DELETE",118:"KEY_F4",119:"KEY_END",
            120:"KEY_F2",121:"KEY_PAGE_DOWN",122:"KEY_F1",123:"KEY_LEFT",124:"KEY_RIGHT",125:"KEY_DOWN",126:"KEY_UP"
        ][Int(code)]
    }
}
