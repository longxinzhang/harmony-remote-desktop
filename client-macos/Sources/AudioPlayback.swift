import Foundation
import AVFoundation

struct AudioPlaybackSnapshot {
    var receivedFrames: UInt64 = 0
    var playedFrames: UInt64 = 0
    var droppedFrames: UInt64 = 0
    var queuedFrames = 0
    var muted = false
    var running = false
    var error = ""
}
// Work submitted from the transport queue never accumulates unbounded GCD blocks.
// A single draining block and a 120 ms mailbox feed a separately bounded player.
final class AudioPlayback {
    private let queue = DispatchQueue(label: "HarmonyRemote.Audio.Playback")
    private let lock = NSLock()
    private var pending: [AudioPacket] = []
    private var pendingFrames = 0
    private var draining = false
    private var stopped = false
    private var muted = false
    private var stats = AudioPlaybackSnapshot()
    private var engine: AVAudioEngine?
    private var player: AVAudioPlayerNode?
    private var scheduledFrames = 0
    private var generation: UInt64 = 0
    private var retryAfter: TimeInterval = 0
    private var streamID: UInt64?
    private let format = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
    func snapshot() -> AudioPlaybackSnapshot { lock.lock(); defer { lock.unlock() }; return stats }
    func append(_ packet: AudioPacket) {
        lock.lock()
        guard !stopped else { lock.unlock(); return }
        if packet.kind == .reset { pending.removeAll(); pendingFrames = 0 }
        else {
            stats.receivedFrames += UInt64(packet.frames)
            if muted { stats.droppedFrames += UInt64(packet.frames); lock.unlock(); return }
            while pendingFrames + packet.frames > AudioWire.maxQueuedFrames, !pending.isEmpty {
                let old = pending.removeFirst(); pendingFrames -= old.frames; stats.droppedFrames += UInt64(old.frames)
            }
        }
        pending.append(packet); pendingFrames += packet.frames
        let start = !draining; draining = true; lock.unlock()
        if start { queue.async { [weak self] in self?.drain() } }
    }
    func setMuted(_ value: Bool) {
        lock.lock(); muted = value; stats.muted = value; pending.removeAll(); pendingFrames = 0; lock.unlock()
        queue.async { [weak self] in self?.resetPlayer(); self?.engine?.pause() }
    }
    func stop() {
        lock.lock(); stopped = true; pending.removeAll(); pendingFrames = 0; lock.unlock()
        // Keep this object alive until its audio queue has stopped native resources;
        // the owner may release the session immediately after calling stop().
        queue.async { [self] in resetPlayer(); engine?.stop(); engine = nil; player = nil }
    }
    private func drain() {
        while true {
            lock.lock()
            guard !stopped, !pending.isEmpty else { draining = false; lock.unlock(); return }
            let packet = pending.removeFirst(); pendingFrames -= packet.frames; let suppress = muted; lock.unlock()
            if packet.kind == .reset || streamID != packet.streamID { resetPlayer(); streamID = packet.streamID }
            guard packet.kind == .pcm, !suppress else { engine?.pause(); continue }
            guard ProcessInfo.processInfo.systemUptime >= retryAfter else {
                lock.lock(); stats.droppedFrames += UInt64(packet.frames); lock.unlock(); continue
            }
            do {
                if scheduledFrames + packet.frames > AudioWire.maxQueuedFrames {
                    lock.lock(); stats.droppedFrames += UInt64(scheduledFrames); lock.unlock(); resetPlayer()
                }
                try ensureEngine()
                guard let player, let buffer = Self.buffer(packet.pcm, format: format) else { continue }
                let gen = generation; scheduledFrames += packet.frames; updateQueued()
                player.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { [weak self] _ in
                    self?.queue.async { [weak self] in
                        guard let self, self.generation == gen else { return }
                        self.scheduledFrames = max(0, self.scheduledFrames - packet.frames)
                        self.lock.lock(); self.stats.playedFrames += UInt64(packet.frames); self.lock.unlock(); self.updateQueued()
                    }
                }
                if !player.isPlaying { player.play() }
            } catch {
                retryAfter = ProcessInfo.processInfo.systemUptime + 2
                lock.lock(); stats.error = "Mac 音频输出不可用"; lock.unlock(); resetPlayer(); engine?.stop(); engine = nil; player = nil
                // No synthetic success: incoming audio may retry when the output recovers.
            }
        }
    }
    private func ensureEngine() throws {
        if engine == nil {
            let engine = AVAudioEngine(), player = AVAudioPlayerNode()
            engine.attach(player); engine.connect(player, to: engine.mainMixerNode, format: format)
            self.engine = engine; self.player = player
        }
        if let engine, !engine.isRunning { try engine.start() }
        lock.lock(); stats.running = true; stats.error = ""; lock.unlock()
    }
    private func updateQueued() { lock.lock(); stats.queuedFrames = scheduledFrames + pendingFrames; lock.unlock() }
    private func resetPlayer() {
        generation &+= 1; player?.stop(); scheduledFrames = 0
        lock.lock(); stats.queuedFrames = pendingFrames; stats.running = false; lock.unlock()
    }
    static func buffer(_ pcm: Data, format: AVAudioFormat) -> AVAudioPCMBuffer? {
        guard !pcm.isEmpty, pcm.count <= AudioWire.maxPayload, pcm.count % 4 == 0,
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(pcm.count / 4)),
              let channels = buffer.floatChannelData else { return nil }
        buffer.frameLength = buffer.frameCapacity
        pcm.withUnsafeBytes { bytes in
            for frame in 0..<Int(buffer.frameLength) {
                channels[0][frame] = AudioWire.sample(bytes, offset: frame * 4)
                channels[1][frame] = AudioWire.sample(bytes, offset: frame * 4 + 2)
            }
        }
        return buffer
    }
}
