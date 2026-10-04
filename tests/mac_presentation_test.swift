import Foundation
import CoreVideo

private final class WeakPresenter {
    weak var value: FramePresentationScheduler?
    init(_ value: FramePresentationScheduler?) { self.value = value }
}

@main
struct PresentationTests {
    static func main() throws {
        var count = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            count += 1; print("PASS: \(name)")
        }
        func run(_ seconds: TimeInterval) { RunLoop.main.run(until: Date(timeIntervalSinceNow: seconds)) }

        var calls = 0
        var arrivalTimes: [TimeInterval] = []
        let scheduler = FramePresentationScheduler(interval: 0.05) {
            calls += 1; arrivalTimes.append(ProcessInfo.processInfo.systemUptime)
        }
        run(0.03)
        check(!scheduler.isScheduled && calls == 0, "idle scheduler has no timer or callbacks")
        scheduler.request(); scheduler.request(); scheduler.request()
        check(scheduler.isScheduled, "pending work arms one timer")
        run(0.03)
        check(calls == 1 && !scheduler.isScheduled, "burst coalesces into one presentation and disarms")
        scheduler.request(); scheduler.request()
        run(0.07)
        check(calls == 2 && arrivalTimes[1] - arrivalTimes[0] >= 0.045,
              "subsequent presentation respects the configured rate limit")
        run(0.08)
        check(calls == 2 && !scheduler.isScheduled, "no recurring wakeups after consuming work")
        scheduler.request(); scheduler.cancel(); run(0.07)
        check(calls == 2 && !scheduler.isScheduled, "cancellation prevents stale presentation")
        scheduler.request(); scheduler.stop(); scheduler.request(); run(0.07)
        check(calls == 2 && !scheduler.isScheduled, "stopped presenter rejects queued and future work")

        var releasedCalls = 0
        var transient: FramePresentationScheduler? = FramePresentationScheduler { releasedCalls += 1 }
        let released = WeakPresenter(transient)
        transient?.request(); transient = nil; run(0.03)
        check(released.value == nil && releasedCalls == 0, "pending timer does not retain destroyed presenter")

        var pixels: CVPixelBuffer?
        guard CVPixelBufferCreate(kCFAllocatorDefault, 16, 16, kCVPixelFormatType_32BGRA, nil, &pixels) == kCVReturnSuccess,
              let pixels else { fatalError("cannot allocate test pixels") }
        let mailbox = FrameMailbox()
        var notices = 0
        mailbox.observePendingFrames { notices += 1 }
        mailbox.put(pixels, ptsUs: 1); mailbox.put(pixels, ptsUs: 2)
        check(notices == 1 && mailbox.statistics.replaced == 1,
              "only empty-to-pending transition notifies; replacement accounting is preserved")
        check(mailbox.take()?.ptsUs == 2 && mailbox.take() == nil, "mailbox consumes the latest frame once")
        mailbox.put(pixels, ptsUs: 3)
        check(notices == 2, "new frame after consumption schedules another presentation")
        mailbox.observePendingFrames(nil)
        _ = mailbox.take(); mailbox.put(pixels, ptsUs: 4)
        check(notices == 2, "detached mailbox observer receives no new frame notices")
        mailbox.observePendingFrames { notices += 1 }
        check(notices == 3, "attaching to a preloaded mailbox schedules its pending frame")
        print("\(count) presentation lifecycle checks passed; no app window, capture, or system clipboard used.")
    }
}
