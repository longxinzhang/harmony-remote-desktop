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
        check(mailbox.statistics.lastSubmittedFrameAt == nil, "unused mailbox does not claim a display submission")
        mailbox.displayed(error: "synthetic diagnostic failure")
        check(mailbox.statistics.lastSubmittedFrameAt == nil && mailbox.statistics.submitted == 0,
              "display failure cannot create a successful submission timestamp")
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
        // Exercise the actual view boundary used by workspace navigation. The
        // surface has no window, and uses synthetic pixels instead of a capture.
        let gatedMailbox = FrameMailbox()
        let surface = VideoSurfaceView(mailbox: gatedMailbox)
        gatedMailbox.put(pixels, ptsUs: 10) // queues an observer callback on main
        surface.setPresentationEnabled(false)
        gatedMailbox.put(pixels, ptsUs: 11)
        run(0.08)
        check(gatedMailbox.statistics.submitted == 0 && gatedMailbox.take()?.ptsUs == 11,
              "hiding rejects an already queued callback and preserves the newest frame")
        check(!gatedMailbox.statistics.presentationEnabled && gatedMailbox.statistics.lastSubmittedFrameAt == nil,
              "hidden presentation is explicitly distinguishable from an idle or failed display")

        gatedMailbox.put(pixels, ptsUs: 12); gatedMailbox.put(pixels, ptsUs: 13)
        run(0.08)
        check(gatedMailbox.statistics.submitted == 0,
              "hidden surface does not submit newly arriving frames")
        surface.setPresentationEnabled(true)
        run(0.08)
        check(gatedMailbox.statistics.submitted == 1 && gatedMailbox.take() == nil,
              "resuming a hidden surface consumes its single latest pending frame")
        check(gatedMailbox.statistics.presentationEnabled && gatedMailbox.statistics.lastSubmittedFrameAt.map {
            ProcessInfo.processInfo.systemUptime - $0 < 0.5
        } == true, "successful display enqueue records local monotonic time")
        let lastSubmittedAt = gatedMailbox.statistics.lastSubmittedFrameAt
        run(0.06)
        check(gatedMailbox.statistics.submitted == 1,
              "resumed surface does not repeatedly submit the retained display image")
        check(gatedMailbox.statistics.lastSubmittedFrameAt == lastSubmittedAt,
              "idle display does not refresh the submission timestamp")

        surface.setPresentationEnabled(false)
        let replacementMailbox = FrameMailbox()
        replacementMailbox.put(pixels, ptsUs: 20)
        surface.replaceMailbox(replacementMailbox)
        gatedMailbox.put(pixels, ptsUs: 14)
        replacementMailbox.put(pixels, ptsUs: 21)
        run(0.08)
        check(replacementMailbox.statistics.submitted == 0 && replacementMailbox.take()?.ptsUs == 21,
              "replacing a hidden mailbox remains inactive and retains its latest frame")
        check(!replacementMailbox.statistics.presentationEnabled && !gatedMailbox.statistics.presentationEnabled,
              "replacement records disabled presentation on both hidden and detached sources")
        check(gatedMailbox.statistics.submitted == 1 && gatedMailbox.take()?.ptsUs == 14,
              "mailbox replacement detaches the old source from presentation")
        replacementMailbox.put(pixels, ptsUs: 22)
        surface.setPresentationEnabled(true)
        run(0.08)
        check(replacementMailbox.statistics.submitted == 1 && replacementMailbox.take() == nil,
              "reenabling after mailbox replacement consumes the new source")

        surface.setPresentationEnabled(false)
        surface.stop()
        surface.setPresentationEnabled(true)
        replacementMailbox.put(pixels, ptsUs: 23)
        run(0.08)
        check(replacementMailbox.statistics.submitted == 1 && replacementMailbox.take()?.ptsUs == 23,
              "stopped view cannot be reactivated by a later visible-page update")
        check(!replacementMailbox.statistics.presentationEnabled, "stopped display diagnostics remain disabled")
        print("\(count) presentation lifecycle checks passed; no app window, capture, or system clipboard used.")
    }
}
