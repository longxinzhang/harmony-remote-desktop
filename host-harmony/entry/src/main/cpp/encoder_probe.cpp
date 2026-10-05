#include "encoder_probe.h"
#include "encoder_timing.h"
#include "capture_state_policy.h"

#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avscreen_capture.h>
#include <native_window/external_window.h>
#include <window_manager/oh_display_manager.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int WIDTH_LIMIT = 1920;
constexpr int HEIGHT_LIMIT = 1080;
constexpr int64_t BITRATE = 8000000;
constexpr size_t QUEUE_PACKETS = 64;
constexpr size_t QUEUE_BYTES = 16 * 1024 * 1024;
constexpr size_t PACKET_BYTES = 8 * 1024 * 1024;
constexpr uint64_t FILE_BYTES = 64 * 1024 * 1024;
constexpr int FAILURE = -9100;
constexpr int NO_HARDWARE = -9101;
constexpr int BAD_BUFFER = -9102;
constexpr int QUEUE_FULL = -9103;
constexpr int WRITE_ERROR = -9104;
constexpr int TIMEOUT = -9105;
constexpr int STREAM_REJECTED = -9110;
constexpr int STREAM_CANCELLED = -9111;
constexpr int STREAM_EXCEPTION = -9112;

void RejectStreamStart(const EncoderStreamHooks& hooks) noexcept
{
    if (hooks.finished) { try { hooks.finished(false); } catch (...) {} }
}

double Seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}
int64_t SteadyNs(Clock::time_point point)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
}
int64_t UnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Quote(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}
struct FormatDeleter { void operator()(OH_AVFormat* p) const { if (p) OH_AVFormat_Destroy(p); } };
using FormatPtr = std::unique_ptr<OH_AVFormat, FormatDeleter>;

struct FormatInfo {
    bool present = false;
    int32_t width = 0, height = 0, pixelFormat = -1, iFrameIntervalMs = -1, bitrateMode = -1;
    int64_t bitrate = -1;
    double fps = -1;
    std::string dump;
};
FormatInfo ReadFormat(OH_AVFormat* format)
{
    FormatInfo value;
    if (!format) return value;
    value.present = true;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &value.width);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &value.height);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, &value.pixelFormat);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, &value.iFrameIntervalMs);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, &value.bitrateMode);
    OH_AVFormat_GetLongValue(format, OH_MD_KEY_BITRATE, &value.bitrate);
    OH_AVFormat_GetDoubleValue(format, OH_MD_KEY_FRAME_RATE, &value.fps);
    if (!std::isfinite(value.fps)) value.fps = -1;
    if (const char* dump = OH_AVFormat_DumpInfo(format)) value.dump.assign(dump, std::min<size_t>(4096, std::char_traits<char>::length(dump)));
    return value;
}
void FormatJson(std::ostream& out, const FormatInfo& f)
{
    out << "{\"present\":" << (f.present ? "true" : "false") << ",\"width\":" << f.width
        << ",\"height\":" << f.height << ",\"pixelFormat\":" << f.pixelFormat << ",\"fps\":" << f.fps
        << ",\"bitrate\":" << f.bitrate << ",\"bitrateMode\":" << f.bitrateMode
        << ",\"iFrameIntervalMs\":" << f.iFrameIntervalMs << ",\"dump\":" << Quote(f.dump) << '}';
}
void CadenceJson(std::ostream& out, const encoder_timing::Cadence& cadence)
{
    out << "{\"observations\":" << cadence.observations
        << ",\"firstSteadyNs\":" << Quote(std::to_string(cadence.firstSteadyNs))
        << ",\"lastSteadyNs\":" << Quote(std::to_string(cadence.lastSteadyNs))
        << ",\"firstToLastWallSeconds\":" << cadence.SpanSeconds()
        << ",\"maxCallbackGapMs\":" << double(cadence.maxGapNs) / 1000000.0
        << ",\"clockRegressions\":" << cadence.clockRegressions
        << ",\"bucketDurationSeconds\":1,\"bucketOrigin\":\"first callback in this series\""
        << ",\"bucketOverflowObservations\":" << cadence.overflowObservations << ",\"perSecond\":[";
    for (size_t i = 0; i < cadence.perSecond.size(); ++i) {
        if (i) out << ',';
        out << cadence.perSecond[i];
    }
    out << "]}";
}
} // namespace

struct EncoderProbe::Impl {
    struct Api { std::string name; uint64_t calls = 0; int last = 0; int firstError = 0; };
    struct Event { std::string source; int code; int64_t at; };
    struct CaptureEvent {
        int code = -1;
        int64_t atUnixMs = 0, sinceStartMs = 0, lastOutputAgeMs = -1, lastFrameAgeMs = -1;
        bool started = false, paused = false, stopping = false, cancelled = false, privateScene = false;
        uint64_t frames = 0, outputBuffers = 0, acceptedPackets = 0, acceptedBytes = 0;
    };
    struct Packet {
        std::vector<uint8_t> bytes;
        int64_t pts = 0, callbackSteadyNs = 0;
        uint32_t flags = 0;
        bool frame = false;
    };
    struct State {
        bool running = false, cancel = false, captureStarted = false, captureStopping = false;
        bool windowEnded = false, timerFinished = false, eos = false, eosRequested = false, eosTimedOut = false;
        bool accepting = true, hardware = false, selectedKnown = false, timestampMonotonic = true;
        bool cbrSupported = false, vbrSupported = false;
        bool fileSaved = false, reportSaved = false;
        bool recordLocally = true;
        int durationSeconds = 10, requestedFps = 30;
        bool captureSystemAudio = false, capturePaused = false;
        int privacyMaskMode = 1, lastCaptureState = -1;
        bool privacyStrategyApplied = false, privateScene = false;
        uint64_t privateSceneEntries = 0, privateSceneExits = 0, captureEventOverflow = 0;
        uint64_t framesAtLastPrivacyExit = 0;
        int64_t lastPrivacyExitSteadyNs = 0, firstFrameAfterLastPrivacyExitMs = -1;
        uint64_t audioBuffers = 0, audioBytes = 0, audioDroppedBuffers = 0, audioInvalidBuffers = 0;
        bool streamEnabled = false, streamCancelled = false, streamFailed = false;
        bool streamEosAccepted = false, streamFinishedCalled = false, streamFinishedSuccess = false;
        uint64_t streamAcceptedPackets = 0, streamAcceptedBytes = 0, streamRejectedPackets = 0;
        int errorCode = 0, displayWidth = 0, displayHeight = 0, width = 0, height = 0;
        int widthAlignment = 0, heightAlignment = 0, bitrateMode = -1;
        uint64_t displayId = 0, selectedId = 0;
        uint64_t outputBuffers = 0, frames = 0, framesAtStop = 0, bytes = 0, bytesReceived = 0;
        uint64_t writtenPackets = 0, writtenFrames = 0, codecDataBuffers = 0, syncFrames = 0;
        uint64_t incompleteBuffers = 0, discardFlags = 0, emptyBuffers = 0;
        uint64_t droppedPackets = 0, droppedBytes = 0, queueFailures = 0, invalidBuffers = 0;
        uint64_t outputFreeFailures = 0, unexpectedInputBuffers = 0, latePackets = 0;
        uint64_t activeOutputCallbacks = 0, highWaterPackets = 0, highWaterBytes = 0;
        uint64_t spsNals = 0, ppsNals = 0, idrNals = 0, vclNals = 0, annexBStartCodes = 0;
        uint64_t eventOverflow = 0;
        int64_t requestedAtUnixMs = 0, finishedAtUnixMs = 0, firstPts = 0, lastPts = 0;
        Clock::time_point requestedAt, firstOutputAt, windowEnd;
        std::string status = "idle", error, codecName, filePath, reportPath, fileError;
        OH_AVRange bitrateRange {}, fpsRange {};
        FormatInfo configuredFormat, streamFormat;
        encoder_timing::Cadence outputCallbackCadence, frameOutputCadence;
        std::vector<Api> apis;
        std::vector<Event> events;
        std::deque<CaptureEvent> captureEvents; // Latest 64 callbacks; counters only, never pixels or input.
    } data;
    std::mutex lifecycle;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::deque<Packet> packets;
    size_t queuedBytes = 0;
    unsigned zeroRun = 0;
    bool expectNalHeader = false;
    EncoderStreamHooks hooks; // Immutable during a run; audioPCM alone is invoked from capture callback.

    bool PollStreamCancellation()
    {
        if (!hooks.cancelled) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (data.streamCancelled || data.streamFailed) return true;
            // A receiver may close immediately after consuming EOS. Do not turn
            // successful end-of-stream acknowledgement into a cancellation.
            if (data.timerFinished && data.streamEosAccepted) return false;
        }
        bool canceled = false;
        try { canceled = hooks.cancelled(); }
        catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            data.streamFailed = true;
            FailLocked(STREAM_EXCEPTION, "network cancellation hook threw");
            return true;
        }
        if (canceled) {
            std::lock_guard<std::mutex> lock(mutex);
            data.streamCancelled = true;
            FailLocked(STREAM_CANCELLED, "network stream cancelled");
        }
        return canceled;
    }

    bool ForwardPacket(const Packet& packet)
    {
        if (!hooks.packet) return true;
        int64_t origin;
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Once failed, preserve remaining local evidence without calling a dead sink.
            if (data.streamCancelled || data.streamFailed) return true;
            origin = data.outputCallbackCadence.firstSteadyNs;
        }
        if (PollStreamCancellation()) return false;
        uint64_t ptsUs = 0;
        if ((packet.flags & (AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME | AVCODEC_BUFFER_FLAGS_DISCARD)) ||
            !encoder_timing::RelativeCallbackPtsUs(origin, packet.callbackSteadyNs, ptsUs)) {
            std::lock_guard<std::mutex> lock(mutex);
            data.streamFailed = true; ++data.streamRejectedPackets;
            FailLocked(STREAM_REJECTED, "network requires complete AU and nonnegative callback timestamp");
            return false;
        }
        bool accepted = false;
        bool threw = false;
        try {
            accepted = hooks.packet(packet.bytes.empty() ? nullptr : packet.bytes.data(), packet.bytes.size(), ptsUs,
                (packet.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0,
                (packet.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0,
                (packet.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0);
        } catch (...) { threw = true; }
        std::lock_guard<std::mutex> lock(mutex);
        if (!accepted) {
            data.streamFailed = true; ++data.streamRejectedPackets;
            FailLocked(threw ? STREAM_EXCEPTION : STREAM_REJECTED,
                threw ? "network packet hook threw" : "network packet sink rejected output");
            return false;
        }
        ++data.streamAcceptedPackets;
        data.streamAcceptedBytes += packet.bytes.size();
        if (packet.flags & AVCODEC_BUFFER_FLAGS_EOS) data.streamEosAccepted = true;
        return true;
    }

    void FinishStream() noexcept
    {
        bool success = false;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (data.streamFinishedCalled) return;
                data.streamFinishedCalled = true;
                success = (data.status == "completed" || data.status == "stopped") && data.frames > 0 && data.eos && data.eosRequested && !data.eosTimedOut &&
                    !data.errorCode && !data.streamFailed && !data.streamCancelled &&
                    (!data.streamEnabled || data.streamEosAccepted);
                data.streamFinishedSuccess = success;
            }
            if (hooks.finished) hooks.finished(success);
        } catch (...) {
            try {
                std::lock_guard<std::mutex> lock(mutex);
                data.streamFailed = true; data.streamFinishedSuccess = false;
                FailLocked(STREAM_EXCEPTION, "network finished hook threw"); data.status = "network_failed";
            } catch (...) {}
        }
    }

    void EventLocked(const char* source, int code)
    {
        if (data.events.size() < 64) data.events.push_back({source, code, UnixMs()});
        else ++data.eventOverflow;
    }
    void RecordLocked(const char* name, int code)
    {
        auto it = std::find_if(data.apis.begin(), data.apis.end(), [&](const Api& a) { return a.name == name; });
        if (it == data.apis.end()) {
            if (data.apis.size() >= 128) { ++data.eventOverflow; return; }
            data.apis.push_back({name});
            it = data.apis.end() - 1;
        }
        ++it->calls;
        it->last = code;
        if (code && !it->firstError) it->firstError = code;
        if (code) EventLocked(name, code);
    }
    void Record(const char* name, int code)
    {
        std::lock_guard<std::mutex> lock(mutex);
        RecordLocked(name, code);
    }
    void FreezeLocked()
    {
        if (!data.windowEnded && data.frames) {
            data.windowEnd = Clock::now();
            data.windowEnded = true;
            data.framesAtStop = data.frames;
        }
    }
    void FailLocked(int code, const char* message)
    {
        if (!data.errorCode) { data.errorCode = code ? code : FAILURE; data.error = message; }
        data.cancel = true;
        FreezeLocked();
        wake.notify_all();
    }
    void Fail(int code, const char* message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        EventLocked(message, code);
        FailLocked(code, message);
    }
    bool Checked(const char* api, int code)
    {
        std::lock_guard<std::mutex> lock(mutex);
        RecordLocked(api, code);
        if (!code) return true;
        FailLocked(code, api);
        return false;
    }
    void CallbackException() noexcept { try { Fail(FAILURE, "C++ exception in callback"); } catch (...) {} }

    static void OnCodecError(OH_AVCodec*, int32_t code, void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        try { self->Fail(code, "encoder error callback"); } catch (...) { self->CallbackException(); }
    }
    static void OnStream(OH_AVCodec*, OH_AVFormat* format, void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        try {
            auto value = ReadFormat(format); // Callback format is borrowed; never destroy it.
            std::lock_guard<std::mutex> lock(self->mutex);
            self->data.streamFormat = std::move(value);
            self->EventLocked("encoder stream changed", 0);
        } catch (...) { self->CallbackException(); }
    }
    static void OnInput(OH_AVCodec*, uint32_t, OH_AVBuffer*, void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        try {
            std::lock_guard<std::mutex> lock(self->mutex);
            ++self->data.unexpectedInputBuffers;
            self->FailLocked(FAILURE, "unexpected input buffer callback in surface mode");
        } catch (...) { self->CallbackException(); }
    }
    static void OnOutput(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* context) noexcept
    {
        auto callbackArrival = Clock::now();
        auto* self = static_cast<Impl*>(context);
        bool counted = false;
        try {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                ++self->data.activeOutputCallbacks;
                self->data.outputCallbackCadence.Observe(SteadyNs(callbackArrival));
                counted = true;
            }
            self->Receive(buffer, callbackArrival);
        } catch (...) {
            try {
                std::lock_guard<std::mutex> lock(self->mutex);
                ++self->data.droppedPackets;
                self->FailLocked(FAILURE, "exception copying encoded packet");
            } catch (...) {}
        }
        // Return every output index, including malformed/queue-full/EOS paths.
        // No state mutex is held over this SDK call. Stop/Destroy remain worker-only.
        int code = OH_VideoEncoder_FreeOutputBuffer(codec, index);
        try {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->RecordLocked("OH_VideoEncoder_FreeOutputBuffer", code);
            if (code) { ++self->data.outputFreeFailures; self->FailLocked(code, "output buffer release failed"); }
            if (counted) --self->data.activeOutputCallbacks;
            self->wake.notify_all();
        } catch (...) { self->CallbackException(); }
    }
    void Receive(OH_AVBuffer* buffer, Clock::time_point callbackArrival)
    {
        OH_AVCodecBufferAttr attr {};
        int result = buffer ? OH_AVBuffer_GetBufferAttr(buffer, &attr) : BAD_BUFFER;
        int capacity = buffer ? OH_AVBuffer_GetCapacity(buffer) : -1;
        auto* address = buffer && attr.size > 0 ? OH_AVBuffer_GetAddr(buffer) : nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        ++data.outputBuffers;
        RecordLocked("OH_AVBuffer_GetBufferAttr", result);
        if (result || capacity < 0 || attr.offset < 0 || attr.size < 0 || attr.offset > capacity ||
            attr.size > capacity - attr.offset || static_cast<size_t>(attr.size) > PACKET_BYTES ||
            (attr.size > 0 && !address)) {
            ++data.invalidBuffers;
            ++data.droppedPackets;
            if (attr.size > 0) data.droppedBytes += attr.size;
            FailLocked(BAD_BUFFER, "invalid encoded buffer bounds/address or oversized packet");
            return;
        }
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) data.eos = true;
        if (!attr.size) {
            ++data.emptyBuffers;
            if (!(data.streamEnabled && (attr.flags & AVCODEC_BUFFER_FLAGS_EOS))) { wake.notify_all(); return; }
        }
        data.bytesReceived += attr.size;
        bool codecData = (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0;
        bool incomplete = (attr.flags & AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME) != 0;
        bool frame = attr.size > 0 && !codecData && !incomplete &&
            (!(attr.flags & AVCODEC_BUFFER_FLAGS_EOS) || data.streamEnabled);
        if (codecData) ++data.codecDataBuffers;
        if (incomplete) ++data.incompleteBuffers;
        if (attr.flags & AVCODEC_BUFFER_FLAGS_DISCARD) ++data.discardFlags;
        if (frame) {
            data.frameOutputCadence.Observe(SteadyNs(callbackArrival));
            if (data.lastPrivacyExitSteadyNs > 0 && data.firstFrameAfterLastPrivacyExitMs < 0 &&
                SteadyNs(callbackArrival) >= data.lastPrivacyExitSteadyNs) {
                data.firstFrameAfterLastPrivacyExitMs = (SteadyNs(callbackArrival) - data.lastPrivacyExitSteadyNs) / 1000000;
            }
            if (!data.frames) { data.firstOutputAt = Clock::now(); data.firstPts = attr.pts; }
            else if (attr.pts < data.lastPts) data.timestampMonotonic = false;
            ++data.frames;
            data.lastPts = attr.pts;
            if (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) ++data.syncFrames;
        }
        if (!data.accepting || packets.size() >= QUEUE_PACKETS || static_cast<size_t>(attr.size) > QUEUE_BYTES - queuedBytes) {
            ++data.droppedPackets;
            data.droppedBytes += attr.size;
            if (!data.accepting) { ++data.latePackets; FailLocked(QUEUE_FULL, "encoded output arrived after drain closed"); }
            else { ++data.queueFailures; FailLocked(QUEUE_FULL, "encoded packet queue capacity exceeded"); }
            wake.notify_all();
            return;
        }
        Packet packet;
        packet.pts = attr.pts;
        packet.callbackSteadyNs = SteadyNs(callbackArrival);
        packet.flags = attr.flags;
        packet.frame = frame;
        if (attr.size) packet.bytes.assign(address + attr.offset, address + attr.offset + attr.size);
        packets.push_back(std::move(packet));
        queuedBytes += attr.size;
        data.highWaterPackets = std::max<uint64_t>(data.highWaterPackets, packets.size());
        data.highWaterBytes = std::max<uint64_t>(data.highWaterBytes, queuedBytes);
        wake.notify_all();
    }
    static void OnCaptureData(OH_AVScreenCapture*, OH_AVBuffer* buffer,
        OH_AVScreenCaptureBufferType type, int64_t timestamp, void* context) noexcept
    {
        if (type != OH_SCREEN_CAPTURE_BUFFERTYPE_AUDIO_INNER) return;
        auto* self = static_cast<Impl*>(context);
        try {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (!self->data.captureSystemAudio || !self->data.captureStarted || self->data.capturePaused || self->data.captureStopping || self->data.cancel) return;
                ++self->data.audioBuffers;
            }
            OH_AVCodecBufferAttr attr {};
            const auto capacity = buffer ? OH_AVBuffer_GetCapacity(buffer) : 0;
            const auto* address = buffer ? OH_AVBuffer_GetAddr(buffer) : nullptr;
            if (!buffer || OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK || !address ||
                attr.offset < 0 || attr.size <= 0 || attr.size > 192000 || attr.size % 4 != 0 ||
                capacity <= 0 || attr.offset > capacity || attr.size > capacity - attr.offset || timestamp < 0) {
                std::lock_guard<std::mutex> lock(self->mutex); ++self->data.audioInvalidBuffers; return;
            }
            bool accepted = self->hooks.audioPCM && self->hooks.audioPCM(address + attr.offset, size_t(attr.size), uint64_t(timestamp) / 1000);
            std::lock_guard<std::mutex> lock(self->mutex);
            if (accepted) self->data.audioBytes += uint64_t(attr.size); else ++self->data.audioDroppedBuffers;
        } catch (...) {
            try { std::lock_guard<std::mutex> lock(self->mutex); ++self->data.audioInvalidBuffers; } catch (...) {}
        }
        // Borrowed callback buffer belongs to the SDK; never release or record it.
    }

    static void OnCaptureState(OH_AVScreenCapture*, OH_AVScreenCaptureStateCode code, void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        try {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->EventLocked("capture state", static_cast<int>(code));
            auto& state = self->data;
            state.lastCaptureState = static_cast<int>(code);
            const int64_t nowNs = SteadyNs(Clock::now());
            if (code == OH_SCREEN_CAPTURE_STATE_ENTER_PRIVATE_SCENE) ++state.privateSceneEntries;
            if (code == OH_SCREEN_CAPTURE_STATE_EXIT_PRIVATE_SCENE) {
                ++state.privateSceneExits;
                state.framesAtLastPrivacyExit = state.frames;
                state.lastPrivacyExitSteadyNs = nowNs;
                state.firstFrameAfterLastPrivacyExitMs = -1;
            }
            if (capture_state::Apply(code, state.captureStopping, state.captureStarted,
                state.capturePaused, state.privateScene)) {
                self->data.cancel = true;
                self->FreezeLocked();
                self->data.status = "canceled";
            }
            const auto ageMs = [nowNs](int64_t lastNs) -> int64_t {
                return lastNs > 0 && nowNs >= lastNs ? (nowNs - lastNs) / 1000000 : -1;
            };
            if (state.captureEvents.size() == 64) { state.captureEvents.pop_front(); ++state.captureEventOverflow; }
            state.captureEvents.push_back({static_cast<int>(code), UnixMs(),
                (nowNs - SteadyNs(state.requestedAt)) / 1000000,
                ageMs(state.outputCallbackCadence.lastSteadyNs), ageMs(state.frameOutputCadence.lastSteadyNs),
                state.captureStarted, state.capturePaused, state.captureStopping, state.cancel, state.privateScene,
                state.frames, state.outputBuffers, state.streamAcceptedPackets, state.streamAcceptedBytes});
            self->wake.notify_all();
        } catch (...) { self->CallbackException(); }
    }
    static void OnCaptureError(OH_AVScreenCapture*, int32_t code, void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        try { self->Fail(code, "screen capture error callback"); } catch (...) { self->CallbackException(); }
    }
    static void OnDisplay(OH_AVScreenCapture*, uint64_t id, void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        try {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->data.selectedKnown = true;
            self->data.selectedId = id;
            if (id != self->data.displayId) self->FailLocked(FAILURE, "selected screen differs from default display");
        } catch (...) { self->CallbackException(); }
    }

    struct Resources {
        Impl& owner;
        OH_AVCodec* encoder = nullptr;
        OHNativeWindow* surface = nullptr;
        OH_AVScreenCapture* capture = nullptr;
        OH_AVScreenCapture_CaptureStrategy* strategy = nullptr;
        bool encoderStarted = false, captureStartCalled = false;
        void StopProducer()
        {
            if (!captureStartCalled) return;
            captureStartCalled = false;
            {
                std::lock_guard<std::mutex> lock(owner.mutex);
                owner.data.captureStopping = true;
                owner.FreezeLocked();
            }
            owner.Checked("OH_AVScreenCapture_StopScreenCapture", OH_AVScreenCapture_StopScreenCapture(capture));
        }
        ~Resources()
        {
            // Keep the callback context and surface alive until both SDK owners are stopped.
            try { StopProducer(); } catch (...) {}
            if (strategy) {
                const int code = OH_AVScreenCapture_ReleaseCaptureStrategy(strategy);
                try { owner.Checked("OH_AVScreenCapture_ReleaseCaptureStrategy", code); } catch (...) {}
            }
            if (capture) {
                int code = OH_AVScreenCapture_Release(capture);
                try { owner.Checked("OH_AVScreenCapture_Release", code); } catch (...) {}
            }
            if (encoderStarted) {
                int code = OH_VideoEncoder_Stop(encoder);
                try { owner.Checked("OH_VideoEncoder_Stop", code); } catch (...) {}
            }
            if (encoder) {
                int code = OH_VideoEncoder_Destroy(encoder);
                try { owner.Checked("OH_VideoEncoder_Destroy", code); } catch (...) {}
            }
            if (surface) OH_NativeWindow_DestroyNativeWindow(surface);
        }
    };

    bool Configure(Resources& r)
    {
        uint64_t display = 0;
        int32_t displayW = 0, displayH = 0;
        if (!Checked("OH_NativeDisplayManager_GetDefaultDisplayId", OH_NativeDisplayManager_GetDefaultDisplayId(&display)) ||
            !Checked("OH_NativeDisplayManager_GetDefaultDisplayWidth", OH_NativeDisplayManager_GetDefaultDisplayWidth(&displayW)) ||
            !Checked("OH_NativeDisplayManager_GetDefaultDisplayHeight", OH_NativeDisplayManager_GetDefaultDisplayHeight(&displayH))) return false;
        if (displayW < 2 || displayH < 2) { Fail(FAILURE, "invalid default display size"); return false; }
        double scale = std::min({1.0, double(WIDTH_LIMIT) / displayW, double(HEIGHT_LIMIT) / displayH});
        int width = std::max(2, int(std::floor(displayW * scale)) & ~1);
        int height = std::max(2, int(std::floor(displayH * scale)) & ~1);
        {
            std::lock_guard<std::mutex> lock(mutex);
            data.displayId = display; data.displayWidth = displayW; data.displayHeight = displayH;
            data.width = width; data.height = height;
            if (data.cancel) return false;
        }
        OH_AVCapability* capability = OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_VIDEO_AVC, true, HARDWARE);
        if (!Checked("OH_AVCodec_GetCapabilityByCategory(HARDWARE)", capability ? 0 : NO_HARDWARE)) return false;
        bool hardware = OH_AVCapability_IsHardware(capability);
        if (!Checked("OH_AVCapability_IsHardware", hardware ? 0 : NO_HARDWARE)) return false;
        const char* name = OH_AVCapability_GetName(capability);
        if (!Checked("OH_AVCapability_GetName", name && *name ? 0 : NO_HARDWARE)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            data.hardware = hardware; data.codecName = name;
        }
        OH_AVRange bitrateRange {}, fpsRange {};
        int32_t widthAlignment = 0, heightAlignment = 0;
        if (!Checked("OH_AVCapability_GetEncoderBitrateRange", OH_AVCapability_GetEncoderBitrateRange(capability, &bitrateRange)) ||
            !Checked("OH_AVCapability_GetVideoWidthAlignment", OH_AVCapability_GetVideoWidthAlignment(capability, &widthAlignment)) ||
            !Checked("OH_AVCapability_GetVideoHeightAlignment", OH_AVCapability_GetVideoHeightAlignment(capability, &heightAlignment))) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            data.bitrateRange = bitrateRange;
            data.widthAlignment = widthAlignment; data.heightAlignment = heightAlignment;
        }
        if (!Checked("OH_AVCapability_IsVideoSizeSupported", OH_AVCapability_IsVideoSizeSupported(capability, width, height) ? 0 : NO_HARDWARE) ||
            !Checked("OH_AVCapability_GetVideoFrameRateRangeForSize", OH_AVCapability_GetVideoFrameRateRangeForSize(capability, width, height, &fpsRange))) return false;
        { std::lock_guard<std::mutex> lock(mutex); data.fpsRange = fpsRange; }
        bool supported = OH_AVCapability_AreVideoSizeAndFrameRateSupported(capability, width, height, data.requestedFps);
        if (!Checked("OH_AVCapability_AreVideoSizeAndFrameRateSupported", supported ? 0 : NO_HARDWARE)) return false;
        if (BITRATE < bitrateRange.minVal || BITRATE > bitrateRange.maxVal) { Fail(NO_HARDWARE, "8 Mbps outside hardware bitrate capability"); return false; }
        bool cbr = OH_AVCapability_IsEncoderBitrateModeSupported(capability, BITRATE_MODE_CBR);
        bool vbr = OH_AVCapability_IsEncoderBitrateModeSupported(capability, BITRATE_MODE_VBR);
        {
            std::lock_guard<std::mutex> lock(mutex);
            data.cbrSupported = cbr; data.vbrSupported = vbr;
        }
        if (!cbr && !vbr) { Fail(NO_HARDWARE, "neither CBR nor VBR is supported"); return false; }
        int mode = cbr ? BITRATE_MODE_CBR : BITRATE_MODE_VBR;
        {
            std::lock_guard<std::mutex> lock(mutex);
            data.bitrateMode = mode;
            if (data.cancel) return false;
        }
        r.encoder = OH_VideoEncoder_CreateByName(name);
        if (!Checked("OH_VideoEncoder_CreateByName", r.encoder ? 0 : NO_HARDWARE)) return false;
        OH_AVCodecCallback callbacks {OnCodecError, OnStream, OnInput, OnOutput};
        if (!Checked("OH_VideoEncoder_RegisterCallback", OH_VideoEncoder_RegisterCallback(r.encoder, callbacks, this))) return false;
        FormatPtr format(OH_AVFormat_Create());
        if (!Checked("OH_AVFormat_Create", format ? 0 : FAILURE)) return false;
        if (!Checked("OH_AVFormat_SetIntValue(width)", OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_WIDTH, width) ? 0 : FAILURE) ||
            !Checked("OH_AVFormat_SetIntValue(height)", OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_HEIGHT, height) ? 0 : FAILURE) ||
            !Checked("OH_AVFormat_SetIntValue(surface)", OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_SURFACE_FORMAT) ? 0 : FAILURE) ||
            !Checked("OH_AVFormat_SetDoubleValue(frame_rate)", OH_AVFormat_SetDoubleValue(format.get(), OH_MD_KEY_FRAME_RATE, data.requestedFps) ? 0 : FAILURE) ||
            !Checked("OH_AVFormat_SetLongValue(bitrate)", OH_AVFormat_SetLongValue(format.get(), OH_MD_KEY_BITRATE, BITRATE) ? 0 : FAILURE) ||
            !Checked("OH_AVFormat_SetIntValue(bitrate_mode)", OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, mode) ? 0 : FAILURE) ||
            !Checked("OH_AVFormat_SetIntValue(i_frame_interval)", OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_I_FRAME_INTERVAL, 1000) ? 0 : FAILURE)) return false;
        if (!Checked("OH_VideoEncoder_Configure", OH_VideoEncoder_Configure(r.encoder, format.get())) ||
            !Checked("OH_VideoEncoder_GetSurface", OH_VideoEncoder_GetSurface(r.encoder, &r.surface)) ||
            !Checked("OH_VideoEncoder_GetSurface(non-null)", r.surface ? 0 : FAILURE) ||
            !Checked("OH_VideoEncoder_Prepare", OH_VideoEncoder_Prepare(r.encoder))) return false;
        FormatPtr actual(OH_VideoEncoder_GetOutputDescription(r.encoder));
        Record("OH_VideoEncoder_GetOutputDescription", actual ? 0 : FAILURE);
        if (actual) {
            auto info = ReadFormat(actual.get());
            std::lock_guard<std::mutex> lock(mutex);
            data.configuredFormat = std::move(info);
        }
        if (!Checked("OH_VideoEncoder_Start", OH_VideoEncoder_Start(r.encoder))) return false;
        r.encoderStarted = true;
        r.capture = OH_AVScreenCapture_Create();
        if (!Checked("OH_AVScreenCapture_Create", r.capture ? 0 : FAILURE) ||
            !Checked("OH_AVScreenCapture_SetStateCallback", OH_AVScreenCapture_SetStateCallback(r.capture, OnCaptureState, this)) ||
            !Checked("OH_AVScreenCapture_SetErrorCallback", OH_AVScreenCapture_SetErrorCallback(r.capture, OnCaptureError, this)) ||
            !Checked("OH_AVScreenCapture_SetDisplayCallback", OH_AVScreenCapture_SetDisplayCallback(r.capture, OnDisplay, this))) return false;
        OH_AVScreenCaptureConfig config {};
        config.captureMode = OH_CAPTURE_HOME_SCREEN;
        config.dataType = OH_ORIGINAL_STREAM;
        config.videoInfo.videoCapInfo.videoFrameWidth = width;
        config.videoInfo.videoCapInfo.videoFrameHeight = height;
        config.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
        // Only system playback is opted in. A zero microphone format plus explicit disable
        // prevents microphone capture. Surface video still flows directly to the encoder.
        if (data.captureSystemAudio) {
            config.audioInfo.innerCapInfo.audioSampleRate = 48000;
            config.audioInfo.innerCapInfo.audioChannels = 2;
            config.audioInfo.innerCapInfo.audioSource = OH_ALL_PLAYBACK;
            if (!Checked("OH_AVScreenCapture_SetDataCallback", OH_AVScreenCapture_SetDataCallback(r.capture, OnCaptureData, this))) return false;
        }
        if (!Checked("OH_AVScreenCapture_Init", OH_AVScreenCapture_Init(r.capture, config)) ||
            !Checked("OH_AVScreenCapture_SetMicrophoneEnabled", OH_AVScreenCapture_SetMicrophoneEnabled(r.capture, false)) ||
            !Checked("OH_AVScreenCapture_ShowCursor", OH_AVScreenCapture_ShowCursor(r.capture, true))) return false;
        // Configure one strategy before Start. Only the requested mask policy is
        // changed; all other capture/encoder/audio options above remain intact.
        // Keep this instance if further strategy options are added in the future.
        r.strategy = OH_AVScreenCapture_CreateCaptureStrategy();
        if (!Checked("OH_AVScreenCapture_CreateCaptureStrategy(non-null)", r.strategy ? 0 : FAILURE) ||
            !Checked("OH_AVScreenCapture_StrategyForPrivacyMaskMode",
                OH_AVScreenCapture_StrategyForPrivacyMaskMode(r.strategy, data.privacyMaskMode)) ||
            !Checked("OH_AVScreenCapture_SetCaptureStrategy", OH_AVScreenCapture_SetCaptureStrategy(r.capture, r.strategy))) return false;
        { std::lock_guard<std::mutex> lock(mutex); data.privacyStrategyApplied = true; }
        auto* appliedStrategy = r.strategy;
        r.strategy = nullptr; // Release is attempted exactly once, including failure paths.
        if (!Checked("OH_AVScreenCapture_ReleaseCaptureStrategy", OH_AVScreenCapture_ReleaseCaptureStrategy(appliedStrategy))) return false;
        if (PollStreamCancellation()) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (data.cancel) return false;
            data.status = "awaiting_consent";
        }
        r.captureStartCalled = true;
        return Checked("OH_AVScreenCapture_StartScreenCaptureWithSurface",
            OH_AVScreenCapture_StartScreenCaptureWithSurface(r.capture, r.surface));
    }

    // Streaming start-code scan is diagnostic only, not a claim that decoding succeeded.
    void ScanAnnexB(const std::vector<uint8_t>& bytes, uint64_t& starts, uint64_t& sps, uint64_t& pps,
        uint64_t& idr, uint64_t& vcl)
    {
        for (uint8_t b : bytes) {
            if (expectNalHeader) {
                unsigned type = b & 31;
                if (type == 7) ++sps;
                if (type == 8) ++pps;
                if (type == 5) ++idr;
                if (type >= 1 && type <= 5) ++vcl;
                expectNalHeader = false;
            }
            if (b == 1 && zeroRun >= 2) { ++starts; expectNalHeader = true; }
            zeroRun = b == 0 ? std::min(zeroRun + 1, 3u) : 0;
        }
    }
    void Drain(std::ofstream& output)
    {
        // Bound work per call, so a busy producer cannot starve the stop deadline.
        for (size_t i = 0; i < QUEUE_PACKETS; ++i) {
            Packet packet;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (packets.empty()) break;
                packet = std::move(packets.front());
                packets.pop_front();
                queuedBytes -= packet.bytes.size();
                if (data.recordLocally && (data.bytes + packet.bytes.size() > FILE_BYTES || !output.good())) {
                    ++data.droppedPackets; data.droppedBytes += packet.bytes.size();
                    FailLocked(WRITE_ERROR, "output file limit exceeded or stream write failed");
                    continue;
                }
            }
            if (data.recordLocally && !packet.bytes.empty()) {
                output.write(reinterpret_cast<const char*>(packet.bytes.data()), static_cast<std::streamsize>(packet.bytes.size()));
            }
            uint64_t starts = 0, sps = 0, pps = 0, idr = 0, vcl = 0;
            if (output.good()) ScanAnnexB(packet.bytes, starts, sps, pps, idr, vcl);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!output.good()) {
                    ++data.droppedPackets; data.droppedBytes += packet.bytes.size();
                    data.fileError = "writing capture.h264 failed; partial packet bytes may exist";
                    FailLocked(WRITE_ERROR, "writing capture.h264 failed");
                } else {
                    data.bytes += packet.bytes.size();
                    if (data.recordLocally) {
                        ++data.writtenPackets;
                        if (packet.frame) ++data.writtenFrames;
                    }
                    data.annexBStartCodes += starts; data.spsNals += sps; data.ppsNals += pps;
                    data.idrNals += idr; data.vclNals += vcl;
                }
            }
            // Optional short-trial disk evidence first; sink only queues borrowed bytes.
            // In particular, no encoder mutex is held during any external hook.
            if (output.good() && !ForwardPacket(packet)) return;
        }
    }
    std::string Json()
    {
        std::lock_guard<std::mutex> lock(mutex);
        double elapsed = data.frames ? Seconds(data.firstOutputAt, data.windowEnded ? data.windowEnd : Clock::now()) : 0;
        uint64_t windowFrames = data.windowEnded ? data.framesAtStop : data.frames;
        double fps = elapsed > 0 && windowFrames > 1 ? double(windowFrames - 1) / elapsed : 0;
        auto pts = encoder_timing::AssessPts(data.firstPts, data.lastPts, data.frames,
            data.frameOutputCadence.SpanSeconds(), data.timestampMonotonic);
        const int64_t nowNs = SteadyNs(Clock::now());
        const auto ageMs = [nowNs](int64_t lastNs) -> int64_t {
            return lastNs > 0 && nowNs >= lastNs ? (nowNs - lastNs) / 1000000 : -1;
        };
        const char* verdict = data.running ? "RUNNING" : data.status == "idle" ? "NOT_RUN" :
            data.errorCode ? "FAILED" : data.timerFinished ? "NEEDS_MAC_DECODE_AND_FPS_REVIEW" : "INCOMPLETE";
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(3) << "{\"schemaVersion\":2,\"probe\":\"harmony-h264-phase0c\",\"status\":"
            << Quote(data.status) << ",\"running\":" << (data.running ? "true" : "false")
            << ",\"requestedFps\":" << data.requestedFps
            << ",\"privacy\":{\"requestedMaskMode\":" << data.privacyMaskMode
            << ",\"strategyApplied\":" << (data.privacyStrategyApplied ? "true" : "false")
            << ",\"privateSceneActive\":" << (data.privateScene ? "true" : "false")
            << ",\"enterEvents\":" << data.privateSceneEntries << ",\"exitEvents\":" << data.privateSceneExits
            << ",\"framesAtLastExit\":" << data.framesAtLastPrivacyExit
            << ",\"framesSinceLastExit\":" << (data.privateSceneExits ? data.frames - data.framesAtLastPrivacyExit : 0)
            << ",\"lastExitAgeMs\":" << ageMs(data.lastPrivacyExitSteadyNs)
            << ",\"firstFrameAfterLastExitMs\":" << data.firstFrameAfterLastPrivacyExitMs
            << ",\"interpretation\":\"SDK scene notifications and encoder output only; not pixel classification or proof that protected content is visible\"}"
            << ",\"captureState\":{\"lastCode\":" << data.lastCaptureState
            << ",\"lastName\":" << Quote(capture_state::Name(data.lastCaptureState))
            << ",\"started\":" << (data.captureStarted ? "true" : "false")
            << ",\"paused\":" << (data.capturePaused ? "true" : "false")
            << ",\"stopping\":" << (data.captureStopping ? "true" : "false")
            << ",\"cancelRequested\":" << (data.cancel ? "true" : "false")
            << ",\"captureFrameCountAvailable\":false,\"lastOutputAgeMs\":" << ageMs(data.outputCallbackCadence.lastSteadyNs)
            << ",\"lastFrameAgeMs\":" << ageMs(data.frameOutputCadence.lastSteadyNs) << '}'
            << ",\"systemAudio\":{\"enabled\":" << (data.captureSystemAudio ? "true" : "false")
            << ",\"paused\":" << (data.capturePaused ? "true" : "false")
            << ",\"buffers\":" << data.audioBuffers << ",\"bytes\":" << data.audioBytes
            << ",\"droppedBuffers\":" << data.audioDroppedBuffers << ",\"invalidBuffers\":" << data.audioInvalidBuffers << ",\"microphone\":false}"
            << ",\"frames\":" << data.frames << ",\"elapsedSeconds\":" << elapsed << ",\"averageFps\":" << fps
            << ",\"bytes\":" << data.bytes << ",\"bytesReceived\":" << data.bytesReceived
            << ",\"stream\":{\"enabled\":" << (data.streamEnabled ? "true" : "false")
            << ",\"acceptedPackets\":" << data.streamAcceptedPackets << ",\"acceptedBytes\":" << data.streamAcceptedBytes
            << ",\"rejectedPackets\":" << data.streamRejectedPackets << ",\"cancelled\":" << (data.streamCancelled ? "true" : "false")
            << ",\"failed\":" << (data.streamFailed ? "true" : "false") << ",\"eosAccepted\":" << (data.streamEosAccepted ? "true" : "false")
            << ",\"finishedCalled\":" << (data.streamFinishedCalled ? "true" : "false")
            << ",\"finishedSuccess\":" << (data.streamFinishedSuccess ? "true" : "false")
            << ",\"timestampUnit\":\"microseconds\",\"timestampSource\":\"encoder_callback_monotonic\""
            << ",\"timestampOrigin\":\"first output callback in this session\",\"timestampIsCaptureTimeOrLatency\":false}"
            << ",\"framesAtCaptureStop\":" << data.framesAtStop << ",\"writtenFrames\":" << data.writtenFrames
            << ",\"frameCountMethod\":\"complete non-codec-data output buffers; verify by offline decode\""
            << ",\"requestedAtUnixMs\":" << data.requestedAtUnixMs << ",\"finishedAtUnixMs\":" << data.finishedAtUnixMs
            << ",\"firstPtsRaw\":" << Quote(std::to_string(data.firstPts)) << ",\"lastPtsRaw\":" << Quote(std::to_string(data.lastPts))
            << ",\"timestampMonotonic\":" << (data.timestampMonotonic ? "true" : "false")
            << ",\"ptsDiagnostics\":{\"sdkDeclaredUnit\":\"microseconds\",\"rawUnitVerified\":false,\"rawSpan\":" << Quote(pts.rawSpan)
            << ",\"sdkDeclarationSource\":\"native_avbuffer_info.h OH_AVCodecBufferAttr.pts\""
            << ",\"sdkDeclaredSpanSeconds\":" << pts.sdkDeclaredSpanSeconds
            << ",\"nanosecondCandidateSpanSeconds\":" << pts.nanosecondCandidateSpanSeconds
            << ",\"outputWallSpanSeconds\":" << pts.outputWallSpanSeconds
            << ",\"sdkSpanToWallRatio\":" << pts.sdkSpanToWallRatio
            << ",\"comparisonToleranceSeconds\":" << pts.toleranceSeconds
            << ",\"sdkDeclaredUnitConsistent\":" << (pts.sdkDeclaredUnitConsistent ? "true" : "false")
            << ",\"observedNanosecondCandidate\":" << (pts.observedNanosecondCandidate ? "true" : "false")
            << ",\"reviewRequired\":" << (pts.reviewRequired ? "true" : "false") << ",\"status\":" << Quote(pts.status)
            << ",\"interpretation\":\"wall comparison only; no automatic unit normalization or PTS-based FPS\"}"
            << ",\"frameOutputCadence\":";
        CadenceJson(out, data.frameOutputCadence);
        out << ",\"outputCallbackCadence\":";
        CadenceJson(out, data.outputCallbackCadence);
        out
            << ",\"display\":{\"id\":" << Quote(std::to_string(data.displayId)) << ",\"width\":" << data.displayWidth
            << ",\"height\":" << data.displayHeight << ",\"selectedId\":" << (data.selectedKnown ? Quote(std::to_string(data.selectedId)) : "null") << '}'
            << ",\"requested\":{\"width\":" << data.width << ",\"height\":" << data.height
            << ",\"fps\":" << data.requestedFps << ",\"bitrate\":" << BITRATE << ",\"iFrameIntervalMs\":1000,\"durationSeconds\":" << data.durationSeconds
            << ",\"durationLimitEnabled\":" << (data.durationSeconds > 0 ? "true" : "false")
            << ",\"audio\":" << (data.captureSystemAudio ? "true" : "false") << ",\"microphone\":false,\"cursor\":true,\"pixelFormat\":4}"
            << ",\"hardware\":{\"verified\":" << (data.hardware ? "true" : "false") << ",\"codecName\":" << Quote(data.codecName)
            << ",\"selection\":\"GetCapabilityByCategory(HARDWARE), IsHardware, CreateByName\",\"bitrateMode\":" << data.bitrateMode
            << ",\"cbrSupported\":" << (data.cbrSupported ? "true" : "false") << ",\"vbrSupported\":" << (data.vbrSupported ? "true" : "false")
            << ",\"widthAlignment\":" << data.widthAlignment << ",\"heightAlignment\":" << data.heightAlignment
            << ",\"bitrateMin\":" << data.bitrateRange.minVal << ",\"bitrateMax\":" << data.bitrateRange.maxVal
            << ",\"fpsMinForSize\":" << data.fpsRange.minVal << ",\"fpsMaxForSize\":" << data.fpsRange.maxVal << '}'
            << ",\"configuredOutputFormat\":";
        FormatJson(out, data.configuredFormat);
        out << ",\"streamOutputFormat\":"; FormatJson(out, data.streamFormat);
        out << ",\"path\":\"AVScreenCapture to hardware encoder input Surface; no CPU RGBA conversion\""
            << ",\"output\":{\"path\":" << Quote(data.filePath) << ",\"localRecordingEnabled\":" << (data.recordLocally ? "true" : "false") << ",\"saved\":" << (data.fileSaved ? "true" : "false")
            << ",\"error\":" << Quote(data.fileError) << ",\"buffers\":" << data.outputBuffers << ",\"writtenPackets\":" << data.writtenPackets
            << ",\"codecDataBuffers\":" << data.codecDataBuffers << ",\"syncFrames\":" << data.syncFrames
            << ",\"incompleteBuffers\":" << data.incompleteBuffers << ",\"discardFlagBuffers\":" << data.discardFlags
            << ",\"emptyBuffers\":" << data.emptyBuffers << ",\"annexBStartCodes\":" << data.annexBStartCodes
            << ",\"spsNals\":" << data.spsNals << ",\"ppsNals\":" << data.ppsNals << ",\"idrNals\":" << data.idrNals << ",\"vclNals\":" << data.vclNals << '}'
            << ",\"queue\":{\"maxPackets\":64,\"maxBytes\":16777216,\"maxPacketBytes\":8388608,\"highWaterPackets\":" << data.highWaterPackets
            << ",\"highWaterBytes\":" << data.highWaterBytes << ",\"failures\":" << data.queueFailures << ",\"droppedPackets\":" << data.droppedPackets
            << ",\"droppedBytes\":" << data.droppedBytes << ",\"invalidBuffers\":" << data.invalidBuffers << ",\"latePackets\":" << data.latePackets
            << ",\"freeFailures\":" << data.outputFreeFailures << ",\"unexpectedInputBuffers\":" << data.unexpectedInputBuffers << '}'
            << ",\"dropScope\":\"application output queue only; upstream capture/codec losses are unknown\""
            << ",\"eosRequested\":" << (data.eosRequested ? "true" : "false") << ",\"eosReceived\":" << (data.eos ? "true" : "false")
            << ",\"eosTimedOut\":" << (data.eosTimedOut ? "true" : "false") << ",\"timerFinished\":" << (data.timerFinished ? "true" : "false")
            << ",\"reportPath\":" << Quote(data.reportPath) << ",\"reportSaved\":" << (data.reportSaved ? "true" : "false")
            << ",\"errorCode\":" << data.errorCode << ",\"error\":" << Quote(data.error)
            << ",\"macDecodeVerified\":false,\"verdict\":" << Quote(verdict) << ",\"apiCodes\":{";
        for (size_t i = 0; i < data.apis.size(); ++i) {
            if (i) out << ',';
            const auto& a = data.apis[i];
            out << Quote(a.name) << ":{\"calls\":" << a.calls << ",\"lastCode\":" << a.last << ",\"firstError\":" << a.firstError << '}';
        }
        out << "},\"events\":[";
        for (size_t i = 0; i < data.events.size(); ++i) {
            if (i) out << ',';
            const auto& e = data.events[i];
            out << "{\"source\":" << Quote(e.source) << ",\"code\":" << e.code << ",\"atUnixMs\":" << e.at << '}';
        }
        out << "],\"eventOverflow\":" << data.eventOverflow << ",\"captureStateEvents\":[";
        for (size_t i = 0; i < data.captureEvents.size(); ++i) {
            if (i) out << ',';
            const auto& e = data.captureEvents[i];
            out << "{\"code\":" << e.code
                << ",\"name\":" << Quote(capture_state::Name(e.code))
                << ",\"atUnixMs\":" << e.atUnixMs << ",\"sinceStartMs\":" << e.sinceStartMs
                << ",\"started\":" << (e.started ? "true" : "false") << ",\"paused\":" << (e.paused ? "true" : "false")
                << ",\"stopping\":" << (e.stopping ? "true" : "false") << ",\"cancelRequested\":" << (e.cancelled ? "true" : "false")
                << ",\"privateSceneActive\":" << (e.privateScene ? "true" : "false")
                << ",\"encodedFrames\":" << e.frames << ",\"outputBuffers\":" << e.outputBuffers
                << ",\"streamAcceptedPackets\":" << e.acceptedPackets << ",\"streamAcceptedBytes\":" << e.acceptedBytes
                << ",\"lastOutputAgeMs\":" << e.lastOutputAgeMs << ",\"lastFrameAgeMs\":" << e.lastFrameAgeMs << '}';
        }
        out << "],\"captureStateEventOverflow\":" << data.captureEventOverflow << '}';
        return out.str();
    }
    void Report()
    {
        std::string path;
        { std::lock_guard<std::mutex> lock(mutex); path = data.reportPath; }
        std::ofstream output(path + ".tmp", std::ios::binary | std::ios::trunc);
        output << Json() << '\n';
        output.close();
        bool saved = output.good() && std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
        std::lock_guard<std::mutex> lock(mutex);
        data.reportSaved = saved;
        if (!saved) { FailLocked(WRITE_ERROR, "encoder JSON report write failed"); data.status = "failed"; }
    }
    void Run()
    {
        std::string path;
        { std::lock_guard<std::mutex> lock(mutex); path = data.filePath; }
        std::ofstream output;
        if (data.recordLocally) output.open(path + ".partial", std::ios::binary | std::ios::trunc);
        {
            Resources r {*this};
            try {
                if (data.recordLocally) std::remove(path.c_str());
                if (!output.good()) Fail(WRITE_ERROR, "cannot open capture.h264.partial");
                Report();
                PollStreamCancellation();
                bool okay;
                { std::lock_guard<std::mutex> lock(mutex); okay = !data.cancel; }
                if (okay && Configure(r)) {
                    bool rateSet = false;
                    auto nextReport = Clock::now();
                    for (;;) {
                        if (PollStreamCancellation()) break;
                        if (hooks.requestKeyframe && hooks.requestKeyframe()) {
                            FormatPtr keyframe(OH_AVFormat_Create());
                            if (keyframe && OH_AVFormat_SetIntValue(keyframe.get(), OH_MD_KEY_REQUEST_I_FRAME, 1))
                                Record("OH_VideoEncoder_SetParameter(request_i_frame)", OH_VideoEncoder_SetParameter(r.encoder, keyframe.get()));
                        }
                        bool setRate = false;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            auto now = Clock::now();
                            if (data.cancel) break;
                            if (data.frames && encoder_session::DurationExpired(data.durationSeconds, Seconds(data.firstOutputAt, now))) {
                                data.timerFinished = true; FreezeLocked(); break;
                            }
                            if (!data.frames && Seconds(data.requestedAt, now) >= 60) {
                                FailLocked(TIMEOUT, "no encoded frame within 60 seconds including consent wait"); break;
                            }
                            setRate = (data.captureStarted || data.frames) && !rateSet;
                            if (data.frames) data.status = "encoding";
                        }
                        if (setRate) {
                            rateSet = true;
                            if (!Checked("OH_AVScreenCapture_SetMaxVideoFrameRate", OH_AVScreenCapture_SetMaxVideoFrameRate(r.capture, data.requestedFps))) break;
                        }
                        Drain(output);
                        if (Clock::now() >= nextReport) { Report(); nextReport = Clock::now() + std::chrono::seconds(1); }
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return data.cancel || !packets.empty(); });
                    }
                }
                r.StopProducer();
                if (r.encoderStarted) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        data.eosRequested = true; data.status = "draining";
                    }
                    bool notified = Checked("OH_VideoEncoder_NotifyEndOfStream", OH_VideoEncoder_NotifyEndOfStream(r.encoder));
                    auto deadline = Clock::now() + std::chrono::seconds(3);
                    while (notified) {
                        PollStreamCancellation();
                        Drain(output);
                        std::unique_lock<std::mutex> lock(mutex);
                        if (data.eos && packets.empty() && data.activeOutputCallbacks == 0) break;
                        if (Clock::now() >= deadline) {
                            data.eosTimedOut = true; FailLocked(TIMEOUT, "encoder EOS was not fully drained within 3 seconds"); break;
                        }
                        wake.wait_for(lock, std::chrono::milliseconds(50));
                    }
                }
            } catch (...) { try { Fail(FAILURE, "exception in encoder worker"); } catch (...) {} }
            {
                std::lock_guard<std::mutex> lock(mutex);
                FreezeLocked(); data.accepting = false;
            }
        } // Stop/Destroy complete before the final queued packets and files are finalized.
        try {
            Drain(output);
            if (data.recordLocally) output.close();
            bool saved = data.recordLocally && output.good() && std::rename((path + ".partial").c_str(), path.c_str()) == 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                data.fileSaved = saved;
                if (data.recordLocally && !saved) { data.fileError = "file close or rename failed"; FailLocked(WRITE_ERROR, "capture.h264 finalization failed"); }
                data.finishedAtUnixMs = UnixMs();
                data.status = data.streamFailed ? "network_failed" : data.streamCancelled ? "network_cancelled" :
                    data.errorCode ? "failed" : data.timerFinished ? "completed" : "stopped";
                data.running = false;
            }
            Report();
        } catch (...) {
            try {
                std::lock_guard<std::mutex> lock(mutex);
                FailLocked(FAILURE, "exception finalizing encoder evidence"); data.running = false; data.status = "failed";
            } catch (...) {}
        }
        FinishStream();
        if (hooks.packet || hooks.cancelled || hooks.finished) {
            try { Report(); } catch (...) {}
        }
    }
};

EncoderProbe::EncoderProbe() : impl_(std::make_unique<Impl>()) {}
EncoderProbe::~EncoderProbe() { Stop(); }
int EncoderProbe::Start(const std::string& filesDir, EncoderStreamHooks hooks, EncoderSessionOptions options)
{
    struct stat info {};
    const bool hasPacketSink = static_cast<bool>(hooks.packet);
    const bool hasAnyStreamHook = hasPacketSink || hooks.cancelled || hooks.finished || hooks.audioPCM || hooks.requestKeyframe;
    if ((options.frameRate != 30 && options.frameRate != 60) ||
        !capture_state::ValidPrivacyMaskMode(options.privacyMaskMode) ||
        (options.captureSystemAudio && (!hasPacketSink || !hooks.audioPCM || options.recordLocally)) ||
        !encoder_session::ValidOptions(options.durationSeconds, options.recordLocally, hasPacketSink, hasAnyStreamHook)) {
        RejectStreamStart(hooks); return -1;
    }
    if (filesDir.empty() || filesDir[0] != '/' || filesDir.find('\0') != std::string::npos ||
        stat(filesDir.c_str(), &info) || !S_ISDIR(info.st_mode)) { RejectStreamStart(hooks); return -1; }
    auto& p = *impl_;
    std::unique_lock<std::mutex> operation(p.lifecycle);
    bool busy;
    { std::lock_guard<std::mutex> lock(p.mutex); busy = p.data.running; }
    if (busy) { operation.unlock(); RejectStreamStart(hooks); return -2; }
    if (p.worker.joinable()) p.worker.join();
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.data = {};
        p.data.durationSeconds = options.durationSeconds;
        p.data.recordLocally = options.recordLocally;
        p.data.requestedFps = options.frameRate;
        p.data.captureSystemAudio = options.captureSystemAudio;
        p.data.privacyMaskMode = options.privacyMaskMode;
        p.hooks = std::move(hooks);
        p.data.streamEnabled = static_cast<bool>(p.hooks.packet);
        p.packets.clear(); p.queuedBytes = 0; p.zeroRun = 0; p.expectNalHeader = false;
        p.data.filePath = options.recordLocally ? filesDir + "/capture.h264" : "";
        p.data.reportPath = filesDir + "/encoder-probe.json";
        p.data.apis.reserve(128); p.data.events.reserve(64);
        p.data.requestedAt = Clock::now(); p.data.requestedAtUnixMs = UnixMs();
        p.data.running = true; p.data.status = "starting";
    }
    try {
        p.worker = std::thread([impl = &p] {
            try { impl->Run(); }
            catch (...) {
                try {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->FailLocked(FAILURE, "uncaught encoder worker exception");
                    impl->data.running = false; impl->data.status = "failed";
                } catch (...) {}
                impl->FinishStream();
                try { impl->Report(); } catch (...) {}
            }
        });
    }
    catch (...) {
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            p.FailLocked(FAILURE, "encoder worker creation failed"); p.data.running = false; p.data.status = "failed";
        }
        operation.unlock();
        p.FinishStream();
        return -3;
    }
    return 0;
}
void EncoderProbe::Stop()
{
    auto& p = *impl_;
    std::lock_guard<std::mutex> operation(p.lifecycle);
    { std::lock_guard<std::mutex> lock(p.mutex); p.data.cancel = true; p.FreezeLocked(); }
    p.wake.notify_all();
    if (p.worker.joinable()) p.worker.join();
}
bool EncoderProbe::IsRunning() { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->data.running; }
bool EncoderProbe::IsSystemAudioRunning()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->data.running && impl_->data.captureStarted && impl_->data.captureSystemAudio &&
        !impl_->data.cancel && !impl_->data.captureStopping;
}
std::string EncoderProbe::SnapshotJson() { return impl_->Json(); }
EncoderProbe& GetEncoderProbe() { static EncoderProbe probe; return probe; }
