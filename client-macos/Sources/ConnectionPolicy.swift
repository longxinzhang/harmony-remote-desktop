import Foundation

struct ReconnectPolicy {
    private(set) var attempts = 0
    mutating func reset() { attempts = 0 }
    mutating func nextDelay(for error: Error, enabled: Bool, hasIdentity: Bool) -> TimeInterval? {
        guard enabled, hasIdentity, let reason = error as? LANError else { return nil }
        switch reason {
        case .connectionFailed, .connectionClosed, .heartbeatTimeout, .deadline: break
        default: return nil
        }
        attempts = min(attempts + 1, 1000000)
        return [1.0, 2, 4, 8, 15, 30][min(attempts - 1, 5)]
    }
}

struct NetworkMeasurements {
    private var probes: [String: TimeInterval] = [:]
    private var latencies: [Double] = []
    private var rateTime: TimeInterval?
    private var rateBytes = 0
    private var rateFrames = 0
    private(set) var rttMilliseconds: Double?
    private(set) var jitterMilliseconds: Double?
    private(set) var rttSamples = 0
    private(set) var videoMbps = 0.0
    private(set) var receivedFPS = 0.0
    mutating func sentProbe(_ id: String, at now: TimeInterval) {
        probes = probes.filter { now - $0.value < 6 }
        if probes.count < 4 { probes[id] = now }
    }
    mutating func receivedPong(_ id: String, at now: TimeInterval) {
        guard let start = probes.removeValue(forKey: id), now >= start, now - start <= 6 else { return }
        let value = (now - start) * 1000
        latencies.append(value); if latencies.count > 30 { latencies.removeFirst() }
        rttMilliseconds = value; rttSamples += 1
        if latencies.count > 1 {
            jitterMilliseconds = zip(latencies.dropFirst(), latencies).map { abs($0 - $1) }.reduce(0, +) / Double(latencies.count - 1)
        }
    }
    mutating func sample(bytes: Int, frames: Int, at now: TimeInterval) {
        guard let previous = rateTime else { rateTime = now; rateBytes = bytes; rateFrames = frames; return }
        let delta = now - previous
        guard delta >= 1 else { return }
        videoMbps = Double(max(0, bytes - rateBytes)) * 8 / delta / 1_000_000
        receivedFPS = Double(max(0, frames - rateFrames)) / delta
        rateTime = now; rateBytes = bytes; rateFrames = frames
    }
}
