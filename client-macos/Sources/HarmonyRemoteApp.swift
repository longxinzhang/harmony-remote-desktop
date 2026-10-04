import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var model: ViewerModel?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { model?.disconnect() }
}

@main
@MainActor
struct HarmonyRemoteApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = ViewerModel()
    var body: some Scene {
        WindowGroup("Harmony Remote") {
            ViewerWindow(model: model).onAppear {
                delegate.model = model; model.startArguments()
            }
        }
        .defaultSize(width: 1180, height: 870)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

@MainActor
struct ViewerWindow: View {
    @ObservedObject var model: ViewerModel
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(systemName: "desktopcomputer").font(.system(size: 24)).foregroundStyle(.blue)
                VStack(alignment: .leading, spacing: 3) {
                    Text("Harmony Remote").font(.headline)
                    Text(model.inputEnabled ? "鸿蒙桌面 · 已启用控制" : "鸿蒙桌面 · 仅查看").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                TextField("鸿蒙 PC 的 IPv4", text: $model.host)
                    .textFieldStyle(.roundedBorder).frame(width: 180).disabled(model.active)
                    .accessibilityLabel("鸿蒙 PC 地址")
                SecureField("6 位 PIN", text: $model.pin)
                    .textFieldStyle(.roundedBorder).frame(width: 115).disabled(model.active)
                    .onSubmit { model.connect() }.accessibilityLabel("配对 PIN")
                Button("连接") { model.connect() }.buttonStyle(.borderedProminent).disabled(model.active)
                Button("断开") { model.disconnect() }.disabled(!model.active)
            }.padding(16)
            Divider()
            HStack(spacing: 14) {
                Button(model.inputEnabled ? "关闭控制" : "启用键鼠控制") { model.toggleInput() }
                    .disabled(!model.canControl)
                Picker("键盘映射", selection: Binding(get: { model.keyboardMode }, set: { model.changeKeyboardMode($0) })) {
                    Text("Mac Friendly · ⌘ → Ctrl").tag(RemoteKeyboardMode.macFriendly)
                    Text("Raw · ⌘ → Meta").tag(RemoteKeyboardMode.raw)
                }.frame(width: 280)
                Text(model.inputEnabled ? "点击画面后操作 · ⌘⇧Esc 释放焦点" : "鸿蒙端允许控制后，在此启用")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
            }.padding(.horizontal, 16).padding(.vertical, 9)
            Divider()
            ZStack {
                Color.black
                VideoSurface(mailbox: model.mailbox, inputEnabled: model.canControl && model.inputEnabled,
                    keyboardMode: model.keyboardMode, onInput: model.sendInput, onRelease: model.releaseInputs)
                if model.decodedFrames == 0 {
                    VStack(spacing: 12) {
                        Image(systemName: "display.2").font(.system(size: 48))
                        Text(model.active ? "等待鸿蒙共享屏幕" : "连接你的鸿蒙 PC").font(.title3)
                        Text("在鸿蒙端启动服务，配对后由你允许共享屏幕。")
                            .font(.callout).foregroundStyle(.white.opacity(0.55))
                    }.foregroundStyle(.white.opacity(0.8)).allowsHitTesting(false)
                }
                VStack {
                    HStack {
                        if model.replay {
                            Text("本地验证回放 · 非实时").font(.caption.weight(.semibold))
                                .padding(8).background(.orange.opacity(0.9), in: Capsule())
                        } else if !model.active && model.decodedFrames > 0 {
                            Text("共享已结束 · 最后一帧").font(.caption)
                                .padding(8).background(.black.opacity(0.65), in: Capsule())
                        }
                        Spacer()
                    }
                    Spacer()
                }.padding(16).foregroundStyle(.white).allowsHitTesting(false)
            }.frame(minWidth: 800, minHeight: 420)
            Divider()
            HStack(alignment: .top, spacing: 18) {
                Circle().fill(model.failed ? .red : model.active ? .green : .secondary).frame(width: 8, height: 8).padding(.top, 5)
                VStack(alignment: .leading, spacing: 4) {
                    Text(model.state).font(.callout.weight(.medium))
                    Text(model.detail).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                }
                Spacer()
                metric(model.replay ? "回放解码帧数" : "接收 / 解码", model.replay ? "\(model.decodedFrames)" : "\(model.receivedFrames) / \(model.decodedFrames)")
                metric("画面", model.dimensions)
                metric("解码方式", model.hardware)
                Button { model.exportDiagnostics() } label: { Image(systemName: "square.and.arrow.down") }
                    .help("导出不含 PIN 的诊断")
            }.padding(14)
        }
        .frame(minWidth: 950, minHeight: 640)
    }
    private func metric(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.caption.monospacedDigit())
        }
    }
}
