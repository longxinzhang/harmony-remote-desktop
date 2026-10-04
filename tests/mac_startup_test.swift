import Foundation

@MainActor
private final class FakeStartupBackend: StartupBackend {
    var status: StartupStatus = .notRegistered
    var registeredStatus: StartupStatus = .enabled
    var failure: Error?
    var registerCount = 0
    var unregisterCount = 0
    var settingsCount = 0

    func register() throws {
        registerCount += 1
        if let failure { throw failure }
        status = registeredStatus
    }
    func unregister() throws {
        unregisterCount += 1
        if let failure { throw failure }
        status = .notRegistered
    }
    func openSystemSettings() { settingsCount += 1 }
}

@main
struct StartupTests {
    @MainActor
    static func main() {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ title: String) {
            guard condition() else { fatalError("FAIL: \(title)") }
            checks += 1
            print("PASS: \(title)")
        }
        let fake = FakeStartupBackend()
        let controller = StartupController(backend: fake)
        check(!controller.enabled && fake.registerCount == 0 && fake.unregisterCount == 0,
              "constructing the settings controller only reads state")
        controller.setEnabled(true)
        check(controller.enabled && fake.registerCount == 1 && !controller.busy,
              "enable reflects system-confirmed registration")
        controller.setEnabled(true)
        check(fake.registerCount == 1, "enabling an already enabled item is idempotent")
        controller.setEnabled(false)
        check(!controller.enabled && fake.unregisterCount == 1, "disable reflects system unregister result")
        controller.setEnabled(false)
        check(fake.unregisterCount == 1, "already disabled does not attempt unregister")

        fake.registeredStatus = .requiresApproval
        controller.setEnabled(true)
        check(controller.status == .requiresApproval && !controller.enabled,
              "successful registration awaiting approval is not shown as enabled")
        let previousRegisters = fake.registerCount
        controller.setEnabled(true)
        check(fake.registerCount == previousRegisters && fake.settingsCount == 0,
              "pending approval does not re-register or automatically open settings")
        controller.openSystemSettings()
        check(fake.settingsCount == 1 && !controller.enabled && controller.status == .requiresApproval,
              "opening settings is not interpreted as consent")
        fake.status = .enabled
        controller.refresh()
        check(controller.enabled, "refresh observes externally granted approval")
        fake.status = .requiresApproval
        controller.refresh()
        check(!controller.enabled, "refresh observes externally revoked approval")
        controller.setEnabled(false)
        check(controller.status == .notRegistered,
              "a pending approval can be unregistered explicitly")

        fake.failure = NSError(domain: "StartupTest", code: 12,
                               userInfo: [NSLocalizedDescriptionKey: "Denied"])
        controller.setEnabled(true)
        check(!controller.enabled && controller.lastError?.contains("12") == true && !controller.busy,
              "registration failure keeps truthful state and an actionable error")
        fake.status = .enabled
        controller.refresh()
        controller.setEnabled(false)
        check(controller.enabled && controller.lastError != nil,
              "failed unregister does not optimistically disable the setting")
        fake.failure = nil
        fake.status = .notFound
        controller.refresh()
        check(controller.status == .notFound && !controller.enabled && controller.lastError == nil,
              "missing application is distinct from disabled and clears stale operation errors")
        fake.status = .unknown
        controller.refresh()
        check(!controller.enabled && controller.status == .unknown,
              "future unrecognized status never implies consent")
        fake.status = .enabled
        let countBeforeDrift = fake.registerCount
        controller.setEnabled(true)
        check(controller.enabled && fake.registerCount == countBeforeDrift,
              "setting action reconciles external state before deciding to register")
        print("\(checks) startup checks passed; no real login items or system settings were changed.")
    }
}
