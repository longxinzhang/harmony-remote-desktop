import Foundation
import CoreGraphics

private func check(_ condition: @autoclosure () -> Bool, _ message: String = "check", line: Int = #line) {
    if !condition() { fatalError("\(message) at line \(line)") }
}
private final class Sink {
    var messages: [[String: Any]] = []
    var releases = 0
    lazy var engine = RemoteInputEngine(onInput: { [weak self] in self?.messages.append($0) },
                                       onRelease: { [weak self] in self?.releases += 1 })
    func focus() { engine.context(enabled: true, focused: true, windowActive: true, insideVideo: true) }
    var keys: [String] { messages.filter { $0["type"] as? String == "key" }.map { "\($0["code"]!) \($0["action"]!)" } }
}

@main
enum MacInputTest {
    static func main() {
        var count = 0
        func test(_ name: String, _ body: () -> Void) { body(); count += 1; print("PASS \(name)") }
        test("aspect-fit pillarbox with top-left normalized coordinates") {
            let rect = RemoteInputGeometry.videoRect(bounds: CGRect(x: 0,y: 0,width: 1200,height: 600), width: 1620,height: 1080)!
            check(rect == CGRect(x: 150,y: 0,width: 900,height: 600))
            check(RemoteInputGeometry.normalized(CGPoint(x: 150,y: 600), in: rect) == CGPoint(x: 0,y: 0))
            check(RemoteInputGeometry.normalized(CGPoint(x: 1050,y: 0), in: rect) == CGPoint(x: 1,y: 1))
            check(RemoteInputGeometry.normalized(CGPoint(x: 149,y: 300), in: rect) == nil)
        }
        test("letterbox and nonzero bounds origins") {
            let rect = RemoteInputGeometry.videoRect(bounds: CGRect(x: 10,y: 20,width: 600,height: 900), width: 1620,height: 1080)!
            check(rect == CGRect(x: 10,y: 270,width: 600,height: 400))
            check(RemoteInputGeometry.normalized(CGPoint(x: 310,y: 470), in: rect) == CGPoint(x: 0.5,y: 0.5))
            check(RemoteInputGeometry.normalized(CGPoint(x: 310,y: 269), in: rect) == nil)
        }
        test("no frame, empty bounds and invalid coordinates cannot map") {
            check(RemoteInputGeometry.videoRect(bounds: CGRect(x: 0,y: 0,width: 100,height: 100), width: 0,height: 100) == nil)
            check(RemoteInputGeometry.videoRect(bounds: .zero, width: 100,height: 100) == nil)
            check(RemoteInputGeometry.normalized(CGPoint(x: CGFloat.nan,y: 0), in: CGRect(x: 0,y: 0,width: 100,height: 100)) == nil)
        }
        test("default disabled and each independent focus gate suppress input") {
            let s = Sink(); s.engine.keyDown(0); s.engine.move(CGPoint(x: 0.5,y: 0.5)); check(s.messages.isEmpty)
            for flags in [(false,true,true,true),(true,false,true,true),(true,true,false,true),(true,true,true,false)] {
                s.engine.context(enabled: flags.0, focused: flags.1, windowActive: flags.2, insideVideo: flags.3)
                s.engine.keyDown(0); s.engine.button("left", down: true, position: CGPoint(x: 0,y: 0)); s.engine.scroll(dx: 1,dy: 1,precise: false)
                check(s.messages.isEmpty)
            }
        }
        test("button down moves first, duplicate downs suppressed, up matched") {
            let s = Sink(); s.focus(); let p = CGPoint(x: 0.25,y: 0.75)
            s.engine.button("left", down: true, position: p); s.engine.button("left", down: true, position: p)
            s.engine.button("left", down: false, position: p); s.engine.button("left", down: false, position: p)
            let buttons = s.messages.filter { $0["type"] as? String == "mouse_button" }
            check(buttons.count == 2); check(s.messages[0]["type"] as? String == "mouse_move")
            check(buttons[0]["action"] as? String == "down" && buttons[1]["action"] as? String == "up")
        }
        test("right and middle buttons only, blackbar nil is silent") {
            let s = Sink(); s.focus()
            s.engine.button("right", down: true, position: nil); check(s.messages.isEmpty)
            s.engine.button("left", down: true, position: CGPoint(x: 2,y: 0)); check(s.messages.isEmpty)
            s.engine.button("left", down: true, position: CGPoint(x: CGFloat.nan,y: 0)); check(s.messages.isEmpty)
            for button in ["right","middle"] { s.engine.button(button, down: true, position: CGPoint(x: 0.5,y: 0.5)) }
            s.engine.button("back", down: true, position: CGPoint(x: 0.5,y: 0.5))
            check(s.engine.heldButtons == Set(["right","middle"]))
        }
        test("precise wheel fixed scale and finite bounded deltas") {
            let s = Sink(); s.focus(); s.engine.scroll(dx: 15,dy: -20,precise: true)
            check(s.messages[0]["dx"] as? Double == 1.5 && s.messages[0]["dy"] as? Double == -2)
            s.engine.scroll(dx: 900,dy: -900,precise: false)
            check(s.messages[1]["dx"] as? Double == 120 && s.messages[1]["dy"] as? Double == -120)
            s.engine.scroll(dx: .nan,dy: 2,precise: false); s.engine.scroll(dx: 0,dy: 0,precise: false)
            check(s.messages.count == 2)
        }
        test("physical Mac keycodes map to platform-independent names") {
            check(RemoteInputEngine.keyCode(0,mode: .raw) == "KEY_A")
            check(RemoteInputEngine.keyCode(37,mode: .raw) == "KEY_L")
            check(RemoteInputEngine.keyCode(53,mode: .raw) == "KEY_ESCAPE")
            check(RemoteInputEngine.keyCode(116,mode: .raw) == "KEY_PAGE_UP")
            check(RemoteInputEngine.keyCode(123,mode: .raw) == "KEY_LEFT")
            check(RemoteInputEngine.keyCode(63,mode: .raw) == nil)
            check(RemoteInputEngine.keyCode(65535,mode: .raw) == nil)
        }
        test("scroll updates current cursor before wheel without a mouseMoved event") {
            let s = Sink(); s.focus()
            s.engine.scroll(dx: 0,dy: 10,precise: true,at: CGPoint(x: 0.7,y: 0.2))
            check(s.messages.count == 2 && s.messages[0]["type"] as? String == "mouse_move" && s.messages[1]["type"] as? String == "scroll")
            check(s.messages[0]["x"] as? Double == 0.7 && s.messages[0]["y"] as? Double == 0.2)
            s.engine.scroll(dx: 1,dy: 1,precise: false,at: CGPoint(x: -1,y: 0)); check(s.messages.count == 2)
        }
        test("friendly Command and same-side native Ctrl reference counts") {
            let s = Sink(); s.focus(); s.engine.keyDown(55); s.engine.keyDown(59)
            check(s.keys == ["KEY_CTRL_LEFT down"])
            s.engine.keyUp(55); check(s.keys.count == 1)
            s.engine.keyUp(59); check(s.keys == ["KEY_CTRL_LEFT down","KEY_CTRL_LEFT up"])
        }
        test("raw Command remains Meta; left/right modifiers keep identities") {
            let s = Sink(); s.focus(); s.engine.keyboardMode = .raw
            s.engine.keyDown(54); s.engine.keyDown(55); s.engine.keyDown(62)
            check(s.keys == ["KEY_META_RIGHT down","KEY_META_LEFT down","KEY_CTRL_RIGHT down"])
            s.engine.keyUp(54); check(s.keys.last == "KEY_META_RIGHT up")
        }
        test("physical key up respects original mapping after mode change") {
            let s = Sink(); s.focus(); s.engine.keyDown(55); s.engine.keyboardMode = .raw; s.engine.keyUp(55)
            check(s.keys == ["KEY_CTRL_LEFT down","KEY_CTRL_LEFT up"])
        }
        test("autorepeat is explicit up/down with final physical release") {
            let s = Sink(); s.focus(); s.engine.keyDown(0); s.engine.keyDown(0); s.engine.keyDown(0,isRepeat: true); s.engine.keyUp(0)
            check(s.keys == ["KEY_A down","KEY_A up","KEY_A down","KEY_A up"])
        }
        test("modifier autorepeat never pulses shared Ctrl") {
            let s = Sink(); s.focus(); s.engine.keyDown(55); s.engine.keyDown(55,isRepeat: true)
            check(s.keys == ["KEY_CTRL_LEFT down"])
        }
        test("modifier flags preserve both physical sides and dedup command/control") {
            let s = Sink(); s.focus()
            s.engine.synchronizeModifiers(rawFlags: (1 << 20) | 8 | 16)
            check(s.engine.heldKeys.count == 2)
            s.engine.synchronizeModifiers(rawFlags: (1 << 20) | (1 << 18) | 8 | 1)
            check(s.engine.heldKeys[54] == nil && s.engine.heldKeys[55] == "KEY_CTRL_LEFT" && s.engine.heldKeys[59] == "KEY_CTRL_LEFT")
            check(s.keys.filter { $0 == "KEY_CTRL_LEFT down" }.count == 1)
            s.engine.synchronizeModifiers(rawFlags: 0); check(s.engine.heldKeys.isEmpty)
        }
        test("caps lock toggles have a complete down/up pair") {
            let s = Sink(); s.focus(); s.engine.capsLockChanged(true); s.engine.capsLockChanged(true); s.engine.capsLockChanged(false)
            check(s.keys == ["KEY_CAPS_LOCK down","KEY_CAPS_LOCK up","KEY_CAPS_LOCK down","KEY_CAPS_LOCK up"])
            check(s.engine.heldKeys.isEmpty)
        }
        test("AppKit key equivalents use complete strokes, independent of keyUp") {
            let s = Sink(); s.focus(); s.engine.keyDown(55); s.engine.shortcutStroke(37)
            check(s.keys == ["KEY_CTRL_LEFT down","KEY_L down","KEY_L up"])
            check(s.engine.heldKeys.count == 1); s.engine.keyUp(37); check(s.keys.count == 3)
        }
        test("a shortcut after an already-held letter still emits a full stroke") {
            let s = Sink(); s.focus(); s.engine.keyDown(0); s.engine.keyDown(55); s.engine.shortcutStroke(0)
            check(s.keys == ["KEY_A down","KEY_CTRL_LEFT down","KEY_A up","KEY_A down","KEY_A up"])
        }
        test("focus loss releases all held physical keys/buttons once and stops input") {
            let s = Sink(); s.focus(); s.engine.keyDown(55); s.engine.keyDown(59); s.engine.keyDown(0)
            s.engine.button("left",down: true,position: CGPoint(x: 0.5,y: 0.5))
            s.engine.context(enabled: true,focused: false,windowActive: true,insideVideo: true)
            check(s.engine.heldKeys.isEmpty && s.engine.heldButtons.isEmpty && s.releases == 1)
            check(s.keys.filter { $0 == "KEY_CTRL_LEFT up" }.count == 1)
            let count = s.messages.count; s.engine.keyDown(0); s.engine.move(CGPoint(x: 0.2,y: 0.2)); check(s.messages.count == count)
            s.engine.context(enabled: true,focused: false,windowActive: false,insideVideo: false); check(s.releases == 1)
        }
        test("leave, window resign and opt-out each release state") {
            for flags in [(true,true,true,false),(true,true,false,true),(false,true,true,true)] {
                let s = Sink(); s.focus(); s.engine.keyDown(0)
                s.engine.context(enabled: flags.0,focused: flags.1,windowActive: flags.2,insideVideo: flags.3)
                check(s.releases == 1 && s.keys.last == "KEY_A up" && !s.engine.capturesInput)
            }
        }
        test("system shortcuts stay local; normal Escape remains remote") {
            check(RemoteInputEngine.isSystemReserved(key: 48,command: true,control: false))
            check(RemoteInputEngine.isSystemReserved(key: 49,command: true,control: false))
            for key: UInt16 in 123...126 { check(RemoteInputEngine.isSystemReserved(key: key,command: false,control: true)) }
            check(RemoteInputEngine.isLocalRelease(key: 53,command: true,shift: true))
            check(!RemoteInputEngine.isLocalRelease(key: 53,command: false,shift: false))
            check(RemoteInputEngine.isCommandShortcut(37) && !RemoteInputEngine.isCommandShortcut(48))
            let s = Sink(); s.focus(); s.engine.keyDown(53); s.engine.keyUp(53)
            check(s.keys == ["KEY_ESCAPE down","KEY_ESCAPE up"])
        }
        print("Mac input pure tests: \(count)/\(count) passed; no GUI, sockets, or device input")
    }
}
