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
        .defaultSize(width: 1180, height: 800)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

private enum WorkspacePage: String, CaseIterable, Identifiable {
    case desktop = "远程桌面", settings = "设置", development = "开发测试"
    var id: Self { self }
    var symbol: String {
        switch self {
        case .desktop: return "desktopcomputer"
        case .settings: return "slider.horizontal.3"
        case .development: return "wrench.and.screwdriver"
        }
    }
}

private enum WorkspaceColors {
    static let blue = Color(red: 0.15, green: 0.37, blue: 0.90)
    static let ink = Color(red: 0.12, green: 0.17, blue: 0.25)
    static let muted = Color(red: 0.43, green: 0.48, blue: 0.56)
    static let canvas = Color(red: 0.96, green: 0.97, blue: 0.985)
    static let line = Color(red: 0.88, green: 0.90, blue: 0.94)
}

@MainActor
struct ViewerWindow: View {
    @ObservedObject var model: ViewerModel
    @State private var page: WorkspacePage = .desktop
    @StateObject private var startup = StartupController()

    private var showsSession: Bool { (model.active || model.reconnecting || model.decodedFrames > 0) && !showConnectionForm }
    private var clipboardSelection: Binding<ClipboardMode> {
        Binding(get: { model.clipboardMode }, set: { model.setClipboardMode($0) })
    }
    private var keyboardSelection: Binding<RemoteKeyboardMode> {
        Binding(get: { model.keyboardMode }, set: { model.changeKeyboardMode($0) })
    }
    private var sessionTitle: String {
        if model.reconnecting { return "正在重连 · 第 \(model.reconnectAttempt) 次" }
        if model.replay { return "本地回放" }
        if model.failed { return "连接遇到问题" }
        if model.active { return model.decodedFrames > 0 ? "正在共享屏幕" : "正在建立连接" }
        return model.decodedFrames > 0 ? "会话已结束" : "尚未连接"
    }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Rectangle().fill(WorkspaceColors.line).frame(width: 1)
            VStack(spacing: 0) {
                // Keep the video view mounted across navigation. Its one-frame
                // mailbox and existing presentation scheduler stay bounded; the
                // input gate is closed whenever another page is visible.
                ZStack {
                    desktopPage
                        .opacity(page == .desktop ? 1 : 0)
                        .allowsHitTesting(page == .desktop)
                        .accessibilityHidden(page != .desktop)
                    if page == .settings { settingsPage }
                    if page == .development { developmentPage }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
                statusBar
            }.background(WorkspaceColors.canvas)
        }
        .frame(minWidth: 900, minHeight: 650)
        .foregroundStyle(WorkspaceColors.ink)
        .tint(WorkspaceColors.blue)
        .preferredColorScheme(.light)
        .onAppear { model.reloadTrustedPeers(); startup.refresh() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in startup.refresh() }
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "rectangle.connected.to.line.below")
                    .font(.system(size: 22, weight: .medium)).foregroundStyle(.white)
                    .frame(width: 42, height: 42)
                    .background(WorkspaceColors.blue, in: RoundedRectangle(cornerRadius: 12))
                VStack(alignment: .leading, spacing: 2) {
                    Text("Harmony").font(.system(size: 17, weight: .bold))
                    Text("Remote").font(.system(size: 13, weight: .medium)).foregroundStyle(WorkspaceColors.muted)
                }
            }.padding(.horizontal, 20).padding(.top, 28).padding(.bottom, 38)
            VStack(spacing: 7) {
                ForEach(WorkspacePage.allCases) { item in
                    Button { selectPage(item) } label: {
                        HStack(spacing: 11) {
                            Image(systemName: item.symbol).font(.system(size: 16)).frame(width: 20)
                            Text(item.rawValue).font(.system(size: 14, weight: page == item ? .semibold : .medium))
                            Spacer(minLength: 0)
                            if item == .desktop && model.active {
                                Circle().fill(WorkspaceColors.blue).frame(width: 6, height: 6)
                            }
                        }
                        .foregroundStyle(page == item ? WorkspaceColors.blue : WorkspaceColors.muted)
                        .padding(.horizontal, 14).frame(height: 43)
                        .background(page == item ? WorkspaceColors.blue.opacity(0.09) : .clear,
                                    in: RoundedRectangle(cornerRadius: 9))
                        .contentShape(Rectangle())
                    }.buttonStyle(.plain).accessibilityAddTraits(page == item ? .isSelected : [])
                }
            }.padding(.horizontal, 12)
            Spacer()
            VStack(alignment: .leading, spacing: 10) {
                Label("局域网直连", systemImage: "network").font(.system(size: 12, weight: .medium))
                Text("macOS 控制端").font(.caption).foregroundStyle(WorkspaceColors.muted)
                Text("版本 \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—")")
                    .font(.caption2).foregroundStyle(WorkspaceColors.muted)
            }.padding(22)
        }.frame(width: 188).background(.white)
    }

    private func selectPage(_ next: WorkspacePage) {
        guard page != next else { return }
        // Resigning first responder sends the matching key/button ups first.
        // The following release also cancels a pending paste with no held keys.
        NSApp.keyWindow?.makeFirstResponder(nil)
        model.releaseInputs()
        page = next
    }

    private func pageHeader(_ title: String, subtitle: String) -> some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 7) {
                Text(title).font(.system(size: 27, weight: .bold))
                Text(subtitle).font(.system(size: 13)).foregroundStyle(WorkspaceColors.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 12)
            if page == .desktop {
                Label("同一局域网", systemImage: "wifi")
                    .font(.system(size: 11, weight: .medium)).foregroundStyle(WorkspaceColors.blue)
                    .padding(.horizontal, 12).padding(.vertical, 7)
                    .background(WorkspaceColors.blue.opacity(0.07), in: Capsule())
            }
        }.padding(.horizontal, 28).padding(.top, 27).padding(.bottom, 24)
    }

    private var desktopPage: some View {
        VStack(spacing: 0) {
            if page == .desktop {
                pageHeader("远程桌面", subtitle: "从这台 Mac 连接并操作你的鸿蒙 PC。")
            }
            ZStack {
                sessionWorkspace
                    .opacity(showsSession ? 1 : 0)
                    .allowsHitTesting(showsSession)
                    .accessibilityHidden(!showsSession)
                if page == .desktop && !showsSession { connectionWorkspace }
            }.padding(.horizontal, 28).padding(.bottom, 24)
        }
    }

    private var connectionWorkspace: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack(alignment: .top, spacing: 20) {
                    connectionCard.frame(maxWidth: .infinity)
                    VStack(alignment: .leading, spacing: 20) {
                        VStack(alignment: .leading, spacing: 17) {
                            Image(systemName: "laptopcomputer")
                                .font(.system(size: 28, weight: .light)).foregroundStyle(WorkspaceColors.blue)
                            Text("这台设备").font(.system(size: 16, weight: .semibold))
                            Text("Mac 控制端").font(.system(size: 14))
                            Text("在这里查看远端画面，使用键盘和鼠标操作鸿蒙 PC。")
                                .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted).lineSpacing(5)
                                .fixedSize(horizontal: false, vertical: true)
                        }.frame(maxWidth: .infinity, alignment: .leading).padding(22).workspaceCard()
                        VStack(alignment: .leading, spacing: 12) {
                            Label("由你掌控共享", systemImage: "hand.raised")
                                .font(.system(size: 14, weight: .semibold))
                            Text("屏幕共享和远程控制，都需要在鸿蒙端允许后才能开始。")
                                .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted).lineSpacing(5)
                                .fixedSize(horizontal: false, vertical: true)
                            Button("调整键盘与剪贴板设置") { selectPage(.settings) }
                                .buttonStyle(.plain).font(.system(size: 12, weight: .medium))
                                .foregroundStyle(WorkspaceColors.blue)
                        }.frame(maxWidth: .infinity, alignment: .leading).padding(22).workspaceCard()
                    }.frame(width: 226)
                }
                if !model.trustedPeers.isEmpty {
                    settingsCard("已配对设备", symbol: "checkmark.shield") {
                        ForEach(model.trustedPeers) { peer in
                            HStack {
                                VStack(alignment: .leading, spacing: 5) {
                                    Text(peer.host).font(.system(size: 14, design: .monospaced))
                                    Text("身份 \(peer.fingerprint)").font(.caption).foregroundStyle(WorkspaceColors.muted)
                                }
                                Spacer()
                                Button("连接") { model.connectTrusted(peer) }.disabled(model.active || model.reconnecting)
                            }
                        }
                    }
                }
                VStack(alignment: .leading, spacing: 17) {
                    Text("开始只需三步").font(.system(size: 14, weight: .semibold))
                    HStack(alignment: .top, spacing: 22) {
                        setupStep("1", title: "启动鸿蒙服务", detail: "在鸿蒙端获取设备地址和配对码。")
                        setupStep("2", title: "在此连接设备", detail: "输入地址和 6 位配对码。")
                        setupStep("3", title: "允许屏幕共享", detail: "在鸿蒙端开始共享，即可看到桌面。")
                    }
                }.padding(22).workspaceCard()
            }.frame(maxWidth: 1000, alignment: .topLeading).frame(maxWidth: .infinity)
        }
    }

    private var connectionCard: some View {
        VStack(alignment: .leading, spacing: 0) {
            Label("连接鸿蒙 PC", systemImage: "display.2")
                .font(.system(size: 19, weight: .semibold)).padding(.bottom, 9)
            Text("输入鸿蒙端显示的连接信息")
                .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted).padding(.bottom, 28)
            Text("设备地址").font(.system(size: 12, weight: .medium)).padding(.bottom, 8)
            TextField("例如 192.168.31.130", text: $model.host)
                .textFieldStyle(.plain).font(.system(size: 15, design: .monospaced))
                .padding(12).background(WorkspaceColors.canvas, in: RoundedRectangle(cornerRadius: 8))
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(WorkspaceColors.line, lineWidth: 1))
                .accessibilityLabel("鸿蒙 PC 地址").disabled(model.active || model.reconnecting)
                .padding(.bottom, 18)
            Text("配对码").font(.system(size: 12, weight: .medium)).padding(.bottom, 8)
            SecureField("6 位 PIN", text: $model.pin)
                .textFieldStyle(.plain).font(.system(size: 15, design: .monospaced))
                .padding(12).background(WorkspaceColors.canvas, in: RoundedRectangle(cornerRadius: 8))
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(WorkspaceColors.line, lineWidth: 1))
                .accessibilityLabel("配对 PIN").disabled(model.active || model.reconnecting)
                .onSubmit { model.connect() }.padding(.bottom, 14)
            Toggle("记住这台设备", isOn: $model.rememberDevice).toggleStyle(.checkbox).disabled(model.active || model.reconnecting)
                .font(.system(size: 12)).padding(.bottom, 7)
            Text("已保存配对时可留空；首次记住设备需鸿蒙端允许。")
                .font(.system(size: 11)).foregroundStyle(WorkspaceColors.muted).fixedSize(horizontal: false, vertical: true).padding(.bottom, 20)
            Button { model.connect() } label: {
                HStack(spacing: 9) {
                    Text("连接设备").font(.system(size: 14, weight: .semibold))
                    Image(systemName: "arrow.right")
                }.frame(maxWidth: .infinity).frame(height: 40)
            }.buttonStyle(.borderedProminent).controlSize(.large).disabled(model.active)
            if model.failed {
                Label(model.detail, systemImage: "exclamationmark.circle")
                    .font(.system(size: 12)).foregroundStyle(.red).padding(.top, 14)
                Button("查看详细原因") { selectPage(.development) }
                    .font(.caption).buttonStyle(.plain).foregroundStyle(WorkspaceColors.blue).padding(.top, 7)
            } else {
                Text("连接前，请确认两台设备处于同一网络。")
                    .font(.system(size: 11)).foregroundStyle(WorkspaceColors.muted)
                    .fixedSize(horizontal: false, vertical: true).padding(.top, 16)
            }
        }.padding(26).workspaceCard()
    }

    private func setupStep(_ number: String, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: 9) {
            Text(number).font(.system(size: 11, weight: .semibold)).foregroundStyle(WorkspaceColors.blue)
                .frame(width: 23, height: 23).background(WorkspaceColors.blue.opacity(0.08), in: Circle())
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.system(size: 12, weight: .semibold))
                Text(detail).font(.system(size: 11)).foregroundStyle(WorkspaceColors.muted).lineSpacing(3)
                    .fixedSize(horizontal: false, vertical: true)
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var sessionWorkspace: some View {
        VStack(spacing: 0) {
            // Remove hidden chrome from the accessibility tree while retaining
            // the single video surface at its stable sibling position.
            if page == .desktop && showsSession {
                HStack(spacing: 12) {
                    Image(systemName: "desktopcomputer").font(.system(size: 20)).foregroundStyle(WorkspaceColors.blue)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(model.replay ? "本地验证画面" : "鸿蒙 PC").font(.system(size: 14, weight: .semibold))
                        Text(model.replay ? "非实时连接" : model.host).font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(WorkspaceColors.muted)
                    }
                    Spacer(minLength: 8)
                    Button(model.inputEnabled ? "关闭控制" : "启用键鼠控制") { model.toggleInput() }
                        .disabled(!model.canControl)
                    Menu {
                        Picker("文字剪贴板", selection: clipboardSelection) {
                            ForEach(ClipboardMode.allCases) { Text($0.title).tag($0) }
                        }
                        Divider()
                        Button("复制远端文字到本机") { model.pullClipboard() }.disabled(!model.clipboardPullEnabled)
                        Button("更多设置…") { selectPage(.settings) }
                    } label: {
                        Label("剪贴板", systemImage: "doc.on.clipboard")
                    }.fixedSize().disabled(model.replay).help(model.clipboardStatus)
                    Button { model.setAudioMuted(!model.audioMuted) } label: {
                        Image(systemName: model.audioMuted ? "speaker.slash" : "speaker.wave.2")
                    }.help(model.audioMuted ? "取消静音" : "静音").disabled(model.replay || !model.active)
                    Button(model.reconnecting ? "取消重连" : model.active ? "断开" : "返回连接") {
                        if model.active || model.reconnecting { model.disconnect() }
                        else {
                            // Retain the last-frame model state; this view-only flag
                            // returns to the form without inventing a new session.
                            showConnectionForm = true
                        }
                    }
                }.padding(14).background(.white)
            }
            Rectangle().fill(WorkspaceColors.line).frame(height: 1)
            ZStack {
                Color(red: 0.055, green: 0.07, blue: 0.105)
                VideoSurface(mailbox: model.mailbox, presentationEnabled: page == .desktop && showsSession,
                    inputEnabled: page == .desktop && !showConnectionForm && model.canControl && model.inputEnabled,
                    keyboardMode: model.keyboardMode, onInput: model.sendInput, onRelease: model.releaseInputs,
                    clipboardPasteEnabled: model.clipboardPasteEnabled, onPaste: model.pasteClipboard,
                    onRemoteCopy: model.remoteCopyIntent)
                    .accessibilityHidden(page != .desktop || !showsSession)
                if page == .desktop && showsSession && model.decodedFrames == 0 {
                    VStack(spacing: 14) {
                        Image(systemName: "display.2").font(.system(size: 42, weight: .light))
                            .foregroundStyle(.white.opacity(0.72))
                        Text(model.reconnecting ? "连接中断，正在重连" : "等待鸿蒙端共享屏幕").font(.system(size: 18, weight: .medium))
                        Text(model.reconnecting ? model.detail : "连接建立后，请在鸿蒙端开始共享并允许系统授权。")
                            .font(.system(size: 12)).foregroundStyle(.white.opacity(0.55))
                    }.foregroundStyle(.white).padding(24).allowsHitTesting(false)
                }
                if page == .desktop && showsSession && (model.replay || (!model.active && model.decodedFrames > 0)) {
                    VStack {
                        HStack {
                            Text(model.replay ? "本地回放 · 非实时" : "共享已结束 · 最后一帧")
                                .font(.system(size: 11, weight: .medium)).foregroundStyle(.white)
                                .padding(.horizontal, 11).padding(.vertical, 7)
                                .background(.black.opacity(0.65), in: Capsule())
                            Spacer()
                        }
                        Spacer()
                    }.padding(16).allowsHitTesting(false)
                }
            }.frame(maxWidth: .infinity, maxHeight: .infinity).frame(minHeight: 280)
            if page == .desktop && showsSession {
                HStack(spacing: 8) {
                    Image(systemName: model.inputEnabled ? "cursorarrow" : "eye")
                    Text(model.inputEnabled ? "点击画面开始操作 · ⌘⇧Esc 释放焦点" : "仅查看 · 鸿蒙端允许后可启用键鼠控制")
                        .font(.system(size: 11))
                    Spacer(minLength: 0)
                    if model.active { Text("剪贴板：\(model.clipboardMode.title)").font(.system(size: 11)) }
                }.foregroundStyle(WorkspaceColors.muted).padding(.horizontal, 15).padding(.vertical, 12).background(.white)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(WorkspaceColors.line, lineWidth: 1))
        .onChange(of: model.active) { _, active in if active { showConnectionForm = false } }
    }
    @State private var showConnectionForm = false

    private var settingsPage: some View {
        VStack(spacing: 0) {
            pageHeader("设置", subtitle: "按你的习惯调整远程操作，切换页面不会断开连接。")
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    settingsCard("键盘与鼠标", symbol: "keyboard") {
                        HStack(alignment: .center, spacing: 20) {
                            VStack(alignment: .leading, spacing: 7) {
                                Text("快捷键映射").font(.system(size: 14, weight: .medium))
                                Text("将 Mac 的常用快捷键映射到鸿蒙。")
                                    .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            Spacer()
                            Picker("键盘映射", selection: keyboardSelection) {
                                Text("Mac 习惯 · ⌘ → Ctrl").tag(RemoteKeyboardMode.macFriendly)
                                Text("原始按键 · ⌘ → Meta").tag(RemoteKeyboardMode.raw)
                            }.labelsHidden().frame(width: 220)
                        }
                        Divider().padding(.vertical, 9)
                        Label("在远程画面按 ⌘⇧Esc，可随时释放键盘和鼠标焦点。", systemImage: "escape")
                            .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                    }
                    settingsCard("文字剪贴板", symbol: "doc.on.clipboard") {
                        HStack(alignment: .center, spacing: 20) {
                            VStack(alignment: .leading, spacing: 7) {
                                Text("同步方向").font(.system(size: 14, weight: .medium))
                                Text("启用后同步新复制的文字。")
                                    .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                            }
                            Spacer()
                            Picker("文字剪贴板", selection: clipboardSelection) {
                                ForEach(ClipboardMode.allCases) { Text($0.title).tag($0) }
                            }.labelsHidden().frame(width: 220).disabled(model.replay)
                        }
                        Divider().padding(.vertical, 9)
                        HStack(spacing: 12) {
                            Text(model.clipboardStatus).font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                                .fixedSize(horizontal: false, vertical: true)
                            Spacer()
                            Button("复制远端文字到本机") { model.pullClipboard() }.disabled(!model.clipboardPullEnabled)
                        }
                        Text("需要在鸿蒙端允许剪贴板同步。当前仅支持纯文字，复制时不保留排版；文件与图片暂不支持。")
                            .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted).lineSpacing(4)
                            .fixedSize(horizontal: false, vertical: true).padding(.top, 12)
                    }
                    settingsCard("连接与配对", symbol: "checkmark.shield") {
                        Toggle("断线后自动重连", isOn: Binding(get: { model.automaticReconnect }, set: { model.setAutomaticReconnect($0) }))
                        Text("仅恢复已校验设备的连接；不回放旧按键或剪贴板。共享授权已结束时，需在鸿蒙端重新允许共享。")
                            .font(.caption).foregroundStyle(WorkspaceColors.muted).fixedSize(horizontal: false, vertical: true)
                        Text(model.pairingStatus).font(.caption).foregroundStyle(WorkspaceColors.muted)
                        ForEach(model.trustedPeers) { peer in
                            HStack { Text(peer.host); Spacer(); Text(peer.fingerprint).font(.caption.monospaced()); Button("移除配对") { model.forgetPeer(peer) } }
                        }
                        Button("刷新已配对设备") { model.reloadTrustedPeers() }
                    }
                    settingsCard("声音与画面", symbol: "speaker.wave.2") {
                        Toggle("静音远端声音", isOn: Binding(get: { model.audioMuted }, set: { model.setAudioMuted($0) }))
                        Text(model.audioStatus).font(.caption).foregroundStyle(WorkspaceColors.muted)
                        Text("在鸿蒙端设置 30 / 60 帧和共享系统声音，于下一次共享生效。60 帧须设备编码器支持；不采集麦克风。")
                            .font(.caption).foregroundStyle(WorkspaceColors.muted).fixedSize(horizontal: false, vertical: true)
                    }
                    settingsCard("启动", symbol: "power") {
                        Toggle("登录 Mac 后启动", isOn: Binding(get: { startup.enabled }, set: { startup.setEnabled($0) })).disabled(startup.busy)
                        Text(startup.status.summary).font(.system(size: 13, weight: .medium))
                        Text(startup.status.detail).font(.caption).foregroundStyle(WorkspaceColors.muted).fixedSize(horizontal: false, vertical: true)
                        if let error = startup.lastError { Text(error).font(.caption).foregroundStyle(.red) }
                        HStack {
                            Button("打开系统登录项") { startup.openSystemSettings() }
                            Button("刷新状态") { startup.refresh() }
                            if startup.status == .requiresApproval { Button("取消登录启动请求") { startup.setEnabled(false) } }
                        }
                    }
                    settingsCard("连接方式", symbol: "network") {
                        HStack {
                            VStack(alignment: .leading, spacing: 7) {
                                Text("同一局域网直连").font(.system(size: 14, weight: .medium))
                                Text("使用鸿蒙端显示的 IPv4 地址和一次性配对码连接。")
                                    .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            Spacer()
                            Text("Mac → 鸿蒙 PC").font(.system(size: 12, weight: .medium)).foregroundStyle(WorkspaceColors.blue)
                        }
                        Text("仅用于可信局域网；当前连接未加密。")
                            .font(.system(size: 11)).foregroundStyle(WorkspaceColors.muted)
                    }
                }.padding(.horizontal, 28).padding(.bottom, 28)
            }
        }.background(WorkspaceColors.canvas)
    }

    private func settingsCard<Content: View>(_ title: String, symbol: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 18) {
            Label(title, systemImage: symbol).font(.system(size: 16, weight: .semibold))
                .foregroundStyle(WorkspaceColors.ink)
            content()
        }.padding(24).workspaceCard()
    }

    private var developmentPage: some View {
        VStack(spacing: 0) {
            pageHeader("开发测试", subtitle: "查看连接详情、性能计数与诊断信息。")
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    settingsCard("会话诊断", symbol: "waveform.path.ecg") {
                        HStack(alignment: .top) {
                            VStack(alignment: .leading, spacing: 8) {
                                Text(model.state).font(.system(size: 15, weight: .semibold))
                                Text(model.detail).font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                                    .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                            }
                            Spacer(minLength: 20)
                            Button { model.exportDiagnostics() } label: {
                                Label("导出诊断", systemImage: "square.and.arrow.down")
                            }
                        }
                        Text("诊断不包含配对码和剪贴板正文。")
                            .font(.system(size: 11)).foregroundStyle(WorkspaceColors.muted)
                    }
                    settingsCard("网络状态", symbol: "network") {
                        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), alignment: .leading), count: 3), alignment: .leading, spacing: 24) {
                            diagnosticMetric("连接网络", model.networkType)
                            diagnosticMetric("网络 RTT", milliseconds(model.networkRTT))
                            diagnosticMetric("RTT 波动", milliseconds(model.networkJitter))
                            diagnosticMetric("视频码率", String(format: "%.2f Mbps", model.videoMbps))
                            diagnosticMetric("接收帧率", String(format: "%.1f FPS", model.receivedFPS))
                            diagnosticMetric("声音", model.audioStatus)
                        }
                        Text("RTT 是控制消息的往返耗时，不代表画面端到端延迟。视频码率只统计视频有效载荷；未估算 TCP 丢包率。")
                            .font(.caption).foregroundStyle(WorkspaceColors.muted).fixedSize(horizontal: false, vertical: true)
                    }
                    settingsCard("画面与解码", symbol: "chart.bar.xaxis") {
                        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), alignment: .leading), count: 3), alignment: .leading, spacing: 24) {
                            diagnosticMetric("接收帧数", "\(model.receivedFrames)")
                            diagnosticMetric("解码帧数", "\(model.decodedFrames)")
                            diagnosticMetric("显示帧数", "\(model.renderedFrames)")
                            diagnosticMetric("待显示帧替换", "\(model.displayReplacements)")
                            diagnosticMetric("画面尺寸", model.dimensions)
                            diagnosticMetric("解码方式", model.hardware)
                        }
                    }
                    settingsCard("输入与剪贴板", symbol: "keyboard.badge.ellipsis") {
                        HStack(alignment: .top, spacing: 24) {
                            diagnosticMetric("远程控制", model.inputEnabled ? "已启用" : "未启用")
                            diagnosticMetric("剪贴板通道", model.clipboardConnected ? "已连接" : "未连接")
                            diagnosticMetric("同步方向", model.clipboardMode.title)
                        }
                        Text(model.clipboardStatus).font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                            .textSelection(.enabled)
                    }
                    settingsCard("本地验证回放", symbol: "play.rectangle") {
                        HStack(alignment: .top, spacing: 20) {
                            VStack(alignment: .leading, spacing: 8) {
                                Text(model.replay ? "当前显示本地回放，不是实时连接。" : "当前未使用本地回放。")
                                    .font(.system(size: 13, weight: .medium))
                                Text("开发时可通过 --replay 参数加载已采集的画面。回放不提供远程控制。")
                                    .font(.system(size: 12)).foregroundStyle(WorkspaceColors.muted)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            Spacer()
                            if model.replay { Button("查看回放画面") { selectPage(.desktop) } }
                        }
                    }
                }.padding(.horizontal, 28).padding(.bottom, 28)
            }
        }.background(WorkspaceColors.canvas)
    }

    private func diagnosticMetric(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(label).font(.system(size: 11)).foregroundStyle(WorkspaceColors.muted)
            Text(value).font(.system(size: 15, weight: .medium, design: .monospaced))
        }.frame(maxWidth: .infinity, alignment: .leading)
    }

    private func milliseconds(_ value: Double?) -> String { value.map { String(format: "%.1f ms", $0) } ?? "—" }

    private var statusBar: some View {
        HStack(spacing: 8) {
            Circle().fill(model.failed ? .red : model.active ? .green : WorkspaceColors.muted.opacity(0.6))
                .frame(width: 6, height: 6)
            Text(sessionTitle).font(.system(size: 11))
            if model.active && !model.replay {
                Text("· \(milliseconds(model.networkRTT)) · \(String(format: "%.1f", model.receivedFPS)) FPS · \(String(format: "%.1f", model.videoMbps)) Mbps")
                    .font(.system(size: 10, design: .monospaced))
            }
            Spacer()
            if page != .desktop && model.active {
                Button("返回远程画面") { selectPage(.desktop) }.buttonStyle(.plain)
                    .font(.system(size: 11, weight: .medium)).foregroundStyle(WorkspaceColors.blue)
            } else {
                Text("Harmony Remote").font(.system(size: 10)).foregroundStyle(WorkspaceColors.muted)
            }
        }.foregroundStyle(WorkspaceColors.muted).padding(.horizontal, 22).frame(height: 35)
            .background(.white).overlay(alignment: .top) { Rectangle().fill(WorkspaceColors.line).frame(height: 1) }
    }
}

private extension View {
    func workspaceCard() -> some View {
        background(.white, in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(WorkspaceColors.line, lineWidth: 1))
    }
}
