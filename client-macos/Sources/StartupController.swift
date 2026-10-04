import Combine
import Foundation
import ServiceManagement

enum StartupStatus: String {
    case notRegistered, enabled, requiresApproval, notFound, unknown

    var summary: String {
        switch self {
        case .notRegistered: return "未启用"
        case .enabled: return "已启用"
        case .requiresApproval: return "等待系统设置批准"
        case .notFound: return "系统未找到此应用"
        case .unknown: return "无法识别系统状态"
        }
    }

    var detail: String {
        switch self {
        case .enabled: return "登录当前 Mac 用户后启动 Harmony Remote。不会自动连接或开启远程控制。"
        case .requiresApproval: return "请在系统设置的登录项中允许 Harmony Remote，然后刷新状态。"
        case .notFound: return "请将应用放在固定位置后重新设置；移动或替换应用后需核对登录项。"
        case .notRegistered: return "启用后，在登录当前 Mac 用户时打开应用。"
        case .unknown: return "请在系统设置中核对登录项。"
        }
    }
}

/// Tests use an in-memory backend. Production never caches an enabled flag:
/// System Settings can change consent independently of this application.
@MainActor
protocol StartupBackend: AnyObject {
    var status: StartupStatus { get }
    func register() throws
    func unregister() throws
    func openSystemSettings()
}

@MainActor
private final class SystemStartupBackend: StartupBackend {
    var status: StartupStatus {
        switch SMAppService.mainApp.status {
        case .notRegistered: return .notRegistered
        case .enabled: return .enabled
        case .requiresApproval: return .requiresApproval
        case .notFound: return .notFound
        @unknown default: return .unknown
        }
    }
    func register() throws { try SMAppService.mainApp.register() }
    func unregister() throws { try SMAppService.mainApp.unregister() }
    func openSystemSettings() { SMAppService.openSystemSettingsLoginItems() }
}

@MainActor
final class StartupController: ObservableObject {
    @Published private(set) var status: StartupStatus
    @Published private(set) var lastError: String?
    @Published private(set) var busy = false
    private let backend: StartupBackend

    var enabled: Bool { status == .enabled }

    init(backend: StartupBackend? = nil) {
        let selected = backend ?? SystemStartupBackend()
        self.backend = selected
        self.status = selected.status
    }

    func refresh() {
        status = backend.status
        lastError = nil
    }

    /// Call only in response to the user's setting change, never at launch.
    func setEnabled(_ requested: Bool) {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        lastError = nil
        status = backend.status
        // A pending approval is already registered. Do not repeatedly request it.
        if requested && (status == .enabled || status == .requiresApproval) { return }
        if !requested && status == .notRegistered { return }
        do {
            if requested { try backend.register() }
            else { try backend.unregister() }
        } catch {
            let failure = error as NSError
            lastError = "设置未完成（\(failure.code)）：\(failure.localizedDescription)"
        }
        // Even a successful register() may still require the user's approval.
        status = backend.status
    }

    /// Opening Settings does not grant consent or change the displayed state.
    func openSystemSettings() { backend.openSystemSettings() }
}
