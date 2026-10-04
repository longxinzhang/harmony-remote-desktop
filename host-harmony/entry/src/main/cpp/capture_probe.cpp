#include "capture_probe.h"

#include <multimedia/player_framework/native_avscreen_capture.h>
#include <native_buffer/native_buffer.h>
#include <window_manager/oh_display_manager.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int MAX_WIDTH = 1920;
constexpr int MAX_HEIGHT = 1080;
constexpr int TARGET_FPS = 30;
constexpr int PROBE_SECONDS = 30;
constexpr int SAMPLE_FRAME = 150;
constexpr int SAMPLE_SECONDS = 5;
constexpr int START_TIMEOUT_SECONDS = 60;
constexpr size_t MAX_EVENTS = 64;
constexpr int INTERNAL_ERROR = -9001;
constexpr int OUTPUT_ERROR = -9002;
constexpr int START_TIMEOUT = -9003;
constexpr int CALLBACK_ERROR = -9004;
constexpr int WRONG_DISPLAY = -9005;

int64_t UnixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

double Seconds(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double>(to - from).count();
}

std::string Quote(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    out << c;
                }
        }
    }
    out << '"';
    return out.str();
}

struct NativeFrame {
    OH_NativeBuffer* buffer = nullptr;
    bool mapped = false;
    ~NativeFrame()
    {
        if (mapped) OH_NativeBuffer_Unmap(buffer);
        // GetNativeBuffer returns an owned reference, unlike the callback AVBuffer.
        if (buffer) OH_NativeBuffer_Unreference(buffer);
    }
};

struct Sample {
    int width = 0;
    int height = 0;
    int stride = 0;
    int format = 0;
    uint64_t frameNumber = 0;
    int64_t timestamp = 0;
    // Exactly one owned, tightly packed four-byte pixel frame; no frame queue.
    std::vector<uint8_t> pixels;
};
} // namespace

struct CaptureProbe::Impl {
    struct ApiResult { std::string name; int code; };
    struct StateEvent { int code; int64_t atUnixMs; };

    std::mutex lifecycle;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool running = false;
    bool stopRequested = false;
    bool captureStarted = false;
    bool timerFinished = false;
    bool imageAttempted = false;
    bool imageSaved = false;
    bool selectedDisplayKnown = false;
    bool reportSaved = false;
    bool timestampMonotonic = true;
    bool captureEnded = false;
    uint64_t frames = 0;
    uint64_t audioCallbacks = 0;
    uint64_t selectedDisplayId = 0;
    uint64_t displayId = 0;
    int displayWidth = 0;
    int displayHeight = 0;
    int width = 0;
    int height = 0;
    int errorCode = 0;
    int64_t requestedAtUnixMs = 0;
    int64_t finishedAtUnixMs = 0;
    int64_t firstTimestamp = 0;
    int64_t lastTimestamp = 0;
    Clock::time_point requestedAt;
    Clock::time_point firstFrameAt;
    Clock::time_point captureEndedAt;
    Clock::time_point finishedAt;
    std::string status = "idle";
    std::string error;
    std::string imageError;
    std::string reportError;
    std::string filesDir;
    std::string framePath;
    std::string reportPath;
    Sample sample;
    std::vector<ApiResult> apiCodes;
    std::vector<StateEvent> states;

    void FreezeCaptureLocked()
    {
        if (frames && !captureEnded) {
            captureEndedAt = Clock::now();
            captureEnded = true;
        }
    }

    void Record(const char* name, int code)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (apiCodes.size() < MAX_EVENTS) apiCodes.push_back({name, code});
    }

    void Fail(int code, const char* message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!errorCode) {
                errorCode = code == 0 ? INTERNAL_ERROR : code;
                error = message;
            }
            stopRequested = true;
            FreezeCaptureLocked();
        }
        wake.notify_all();
    }

    void CallbackFailed() noexcept
    {
        // Exceptions must never propagate into the C SDK callback boundary.
        try { Fail(CALLBACK_ERROR, "exception in screen capture callback"); } catch (...) {}
    }

    static void OnState(OH_AVScreenCapture*, OH_AVScreenCaptureStateCode code, void* data) noexcept
    {
        auto* self = static_cast<Impl*>(data);
        try {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (self->states.size() < MAX_EVENTS) {
                    self->states.push_back({static_cast<int>(code), UnixMilliseconds()});
                }
                if (code == OH_SCREEN_CAPTURE_STATE_STARTED) self->captureStarted = true;
                if (code == OH_SCREEN_CAPTURE_STATE_CANCELED || code == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER ||
                    code == OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER ||
                    code == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL ||
                    code == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES) {
                    self->stopRequested = true;
                    self->FreezeCaptureLocked();
                    if (!self->timerFinished && self->error.empty()) self->error = "capture canceled or interrupted";
                }
            }
            self->wake.notify_all();
        } catch (...) { self->CallbackFailed(); }
    }

    static void OnError(OH_AVScreenCapture*, int32_t code, void* data) noexcept
    {
        auto* self = static_cast<Impl*>(data);
        try { self->Fail(code, "SDK screen capture error callback"); }
        catch (...) { self->CallbackFailed(); }
    }

    static void OnDisplay(OH_AVScreenCapture*, uint64_t id, void* data) noexcept
    {
        auto* self = static_cast<Impl*>(data);
        try {
            bool mismatch;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->selectedDisplayKnown = true;
                self->selectedDisplayId = id;
                mismatch = id != self->displayId;
            }
            if (mismatch) self->Fail(WRONG_DISPLAY, "selected display differs from requested default display");
        } catch (...) { self->CallbackFailed(); }
    }

    static void OnData(OH_AVScreenCapture*, OH_AVBuffer* buffer, OH_AVScreenCaptureBufferType type,
        int64_t timestamp, void* data) noexcept
    {
        auto* self = static_cast<Impl*>(data);
        try { self->ReceiveFrame(buffer, type, timestamp); }
        catch (...) { self->CallbackFailed(); }
    }

    void ReceiveFrame(OH_AVBuffer* buffer, OH_AVScreenCaptureBufferType type, int64_t timestamp)
    {
        uint64_t frameNumber;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || stopRequested) return;
            if (type != OH_SCREEN_CAPTURE_BUFFERTYPE_VIDEO) {
                ++audioCallbacks;
                return;
            }
            if (!buffer) return;
            auto now = Clock::now();
            if (frames == 0) {
                firstFrameAt = now;
                firstTimestamp = timestamp;
                captureStarted = true;
            } else if (timestamp < lastTimestamp) {
                timestampMonotonic = false;
            }
            lastTimestamp = timestamp;
            frameNumber = ++frames;
            wake.notify_all();
            if (imageAttempted || (frames < SAMPLE_FRAME && Seconds(firstFrameAt, now) < SAMPLE_SECONDS)) return;
            // Mark before mapping; even concurrent callbacks cannot copy another frame.
            imageAttempted = true;
        }

        Sample frame;
        frame.frameNumber = frameNumber;
        frame.timestamp = timestamp;
        NativeFrame native;
        native.buffer = OH_AVBuffer_GetNativeBuffer(buffer);
        Record("OH_AVBuffer_GetNativeBuffer", native.buffer ? 0 : INTERNAL_ERROR);
        if (!native.buffer) {
            std::lock_guard<std::mutex> lock(mutex);
            imageError = "OH_AVBuffer_GetNativeBuffer returned null";
            return;
        }
        OH_NativeBuffer_Config config {};
        OH_NativeBuffer_GetConfig(native.buffer, &config);
        frame.width = config.width;
        frame.height = config.height;
        frame.stride = config.stride;
        frame.format = config.format;
        std::string problem;
        if (config.width <= 0 || config.height <= 0 || config.width > MAX_WIDTH || config.height > MAX_HEIGHT ||
            config.stride < config.width * 4 || config.stride > 16 * MAX_WIDTH) {
            problem = "invalid or out-of-bounds native buffer dimensions/byte stride";
        } else if (config.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888 &&
            config.format != NATIVEBUFFER_PIXEL_FMT_RGBX_8888 && config.format != NATIVEBUFFER_PIXEL_FMT_BGRA_8888) {
            problem = "unsupported native buffer pixel format; image was not guessed";
        } else {
            void* address = nullptr;
            int code = OH_NativeBuffer_Map(native.buffer, &address);
            native.mapped = code == 0;
            Record("OH_NativeBuffer_Map", code);
            if (code != 0 || !address) {
                problem = "OH_NativeBuffer_Map failed or returned null";
            } else {
                const size_t rowBytes = static_cast<size_t>(config.width) * 4;
                frame.pixels.resize(rowBytes * static_cast<size_t>(config.height));
                for (int y = 0; y < config.height; ++y) {
                    std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * rowBytes,
                        static_cast<const uint8_t*>(address) + static_cast<size_t>(y) * config.stride, rowBytes);
                }
            }
        }
        if (native.mapped) {
            int code = OH_NativeBuffer_Unmap(native.buffer);
            native.mapped = false;
            Record("OH_NativeBuffer_Unmap", code);
        }
        int releaseCode = OH_NativeBuffer_Unreference(native.buffer);
        native.buffer = nullptr;
        Record("OH_NativeBuffer_Unreference", releaseCode);
        {
            std::lock_guard<std::mutex> lock(mutex);
            sample = std::move(frame);
            imageError = std::move(problem);
        }
        wake.notify_all();
    }

    struct CaptureOwner {
        Impl& owner;
        OH_AVScreenCapture* capture = nullptr;
        bool startCalled = false;
        ~CaptureOwner()
        {
            if (!capture) return;
            // Never hold the snapshot mutex across SDK calls: they can invoke callbacks.
            if (startCalled) {
                int code = OH_AVScreenCapture_StopScreenCapture(capture);
                try { owner.Record("OH_AVScreenCapture_StopScreenCapture", code); } catch (...) {}
            }
            int code = OH_AVScreenCapture_Release(capture);
            try { owner.Checked("OH_AVScreenCapture_Release", code); } catch (...) {}
        }
    };

    bool Checked(const char* api, int code)
    {
        Record(api, code);
        if (code == 0) return true;
        Fail(code, api);
        return false;
    }

    bool Configure(CaptureOwner& owner)
    {
        uint64_t id = 0;
        int32_t displayW = 0, displayH = 0;
        if (!Checked("OH_NativeDisplayManager_GetDefaultDisplayId", OH_NativeDisplayManager_GetDefaultDisplayId(&id)) ||
            !Checked("OH_NativeDisplayManager_GetDefaultDisplayWidth", OH_NativeDisplayManager_GetDefaultDisplayWidth(&displayW)) ||
            !Checked("OH_NativeDisplayManager_GetDefaultDisplayHeight", OH_NativeDisplayManager_GetDefaultDisplayHeight(&displayH))) {
            return false;
        }
        if (displayW < 2 || displayH < 2) {
            Fail(INTERNAL_ERROR, "default display dimensions are invalid");
            return false;
        }
        double scale = std::min({1.0, double(MAX_WIDTH) / displayW, double(MAX_HEIGHT) / displayH});
        // Even dimensions preserve the aspect ratio within one output pixel per axis.
        int outputW = std::max(2, static_cast<int>(std::floor(displayW * scale)) & ~1);
        int outputH = std::max(2, static_cast<int>(std::floor(displayH * scale)) & ~1);
        {
            std::lock_guard<std::mutex> lock(mutex);
            displayId = id;
            displayWidth = displayW;
            displayHeight = displayH;
            width = outputW;
            height = outputH;
            if (stopRequested) return false;
        }
        owner.capture = OH_AVScreenCapture_Create();
        if (!Checked("OH_AVScreenCapture_Create", owner.capture ? 0 : INTERNAL_ERROR)) return false;
        auto* capture = owner.capture;
        if (!Checked("OH_AVScreenCapture_SetStateCallback", OH_AVScreenCapture_SetStateCallback(capture, OnState, this)) ||
            !Checked("OH_AVScreenCapture_SetErrorCallback", OH_AVScreenCapture_SetErrorCallback(capture, OnError, this)) ||
            !Checked("OH_AVScreenCapture_SetDataCallback", OH_AVScreenCapture_SetDataCallback(capture, OnData, this)) ||
            !Checked("OH_AVScreenCapture_SetDisplayCallback", OH_AVScreenCapture_SetDisplayCallback(capture, OnDisplay, this))) {
            return false;
        }
        OH_AVScreenCaptureConfig config {};
        config.captureMode = OH_CAPTURE_HOME_SCREEN;
        config.dataType = OH_ORIGINAL_STREAM;
        config.videoInfo.videoCapInfo.videoFrameWidth = outputW;
        config.videoInfo.videoCapInfo.videoFrameHeight = outputH;
        config.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
        // Both audio sample rates and channel counts remain zero: no audio is captured.
        if (!Checked("OH_AVScreenCapture_Init", OH_AVScreenCapture_Init(capture, config)) ||
            !Checked("OH_AVScreenCapture_SetMicrophoneEnabled", OH_AVScreenCapture_SetMicrophoneEnabled(capture, false)) ||
            !Checked("OH_AVScreenCapture_ShowCursor", OH_AVScreenCapture_ShowCursor(capture, true))) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopRequested) return false;
            status = "awaiting_consent";
        }
        owner.startCalled = true;
        return Checked("OH_AVScreenCapture_StartScreenCapture", OH_AVScreenCapture_StartScreenCapture(capture));
    }

    void WriteSample()
    {
        Sample frame;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (sample.pixels.empty()) return;
            frame.width = sample.width;
            frame.height = sample.height;
            frame.format = sample.format;
            frame.pixels.swap(sample.pixels);
        }
        std::string temporary = framePath + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
        std::vector<uint8_t> row(static_cast<size_t>(frame.width) * 3);
        const bool bgra = frame.format == NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
        for (int y = 0; y < frame.height && output; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                size_t offset = (static_cast<size_t>(y) * frame.width + x) * 4;
                row[static_cast<size_t>(x) * 3] = frame.pixels[offset + (bgra ? 2 : 0)];
                row[static_cast<size_t>(x) * 3 + 1] = frame.pixels[offset + 1];
                row[static_cast<size_t>(x) * 3 + 2] = frame.pixels[offset + (bgra ? 0 : 2)];
            }
            output.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        output.close();
        bool saved = output.good() && std::rename(temporary.c_str(), framePath.c_str()) == 0;
        if (!saved) std::remove(temporary.c_str());
        std::lock_guard<std::mutex> lock(mutex);
        imageSaved = saved;
        if (!saved) imageError = "failed to write/rename capture-frame.ppm";
    }

    std::string Json()
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto end = captureEnded ? captureEndedAt : (running ? Clock::now() : finishedAt);
        double elapsed = frames ? std::max(0.0, Seconds(firstFrameAt, end)) : 0.0;
        double fps = elapsed > 0.0 && frames > 1 ? static_cast<double>(frames - 1) / elapsed : 0.0;
        bool frameDurationGate = timerFinished && frames >= 300 && elapsed >= PROBE_SECONDS;
        bool gate = frameDurationGate && !errorCode && timestampMonotonic && audioCallbacks == 0;
        const char* verdict = "NOT_RUN";
        if (running) verdict = "RUNNING";
        else if (status == "failed") verdict = "FAILED";
        else if (status != "idle") {
            verdict = gate ? (imageSaved ? "NEEDS_MANUAL_DESKTOP_REVIEW" : "INCOMPLETE_IMAGE_EVIDENCE")
                           : "NUMERICAL_GATE_NOT_MET";
        }
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(3)
            << "{\"schemaVersion\":1,\"probe\":\"harmony-screen-capture-phase0\",\"status\":" << Quote(status)
            << ",\"running\":" << (running ? "true" : "false")
            << ",\"requestedAtUnixMs\":" << requestedAtUnixMs << ",\"finishedAtUnixMs\":" << finishedAtUnixMs
            << ",\"frames\":" << frames << ",\"elapsedSeconds\":" << elapsed << ",\"averageFps\":" << fps
            << ",\"firstTimestampNs\":" << Quote(std::to_string(firstTimestamp))
            << ",\"lastTimestampNs\":" << Quote(std::to_string(lastTimestamp))
            << ",\"timestampMonotonic\":" << (timestampMonotonic ? "true" : "false")
            << ",\"unexpectedAudioCallbacks\":" << audioCallbacks
            << ",\"display\":{\"id\":" << Quote(std::to_string(displayId)) << ",\"width\":" << displayWidth
            << ",\"height\":" << displayHeight << ",\"selectedId\":"
            << (selectedDisplayKnown ? Quote(std::to_string(selectedDisplayId)) : "null") << '}'
            << ",\"requested\":{\"width\":" << width << ",\"height\":" << height
            << ",\"fps\":30,\"durationSeconds\":30,\"captureMode\":\"OH_CAPTURE_HOME_SCREEN\",\"cursor\":true,\"audio\":false}"
            << ",\"image\":{\"path\":" << Quote(framePath) << ",\"saved\":" << (imageSaved ? "true" : "false")
            << ",\"attempted\":" << (imageAttempted ? "true" : "false") << ",\"width\":" << sample.width
            << ",\"height\":" << sample.height << ",\"stride\":" << sample.stride << ",\"format\":" << sample.format
            << ",\"frameNumber\":" << sample.frameNumber << ",\"timestampNs\":" << Quote(std::to_string(sample.timestamp))
            << ",\"sampleFrameThreshold\":150,\"sampleSecondsThreshold\":5"
            << ",\"error\":" << Quote(imageError) << '}'
            << ",\"report\":{\"path\":" << Quote(reportPath) << ",\"saved\":" << (reportSaved ? "true" : "false")
            << ",\"error\":" << Quote(reportError) << '}'
            << ",\"errorCode\":" << errorCode << ",\"error\":" << Quote(error)
            << ",\"frameDurationGateMet\":" << (frameDurationGate ? "true" : "false")
            << ",\"numericalGateMet\":" << (gate ? "true" : "false")
            << ",\"desktopVerified\":false,\"verdict\":" << Quote(verdict) << ",\"apiCodes\":{";
        for (size_t i = 0; i < apiCodes.size(); ++i) {
            if (i) out << ',';
            out << Quote(apiCodes[i].name) << ':' << apiCodes[i].code;
        }
        out << "},\"stateHistory\":[";
        for (size_t i = 0; i < states.size(); ++i) {
            if (i) out << ',';
            out << "{\"code\":" << states[i].code << ",\"atUnixMs\":" << states[i].atUnixMs << '}';
        }
        out << "]}";
        return out.str();
    }

    void WriteReport()
    {
        // Worker only. Report persists once per second and after SDK release.
        std::string temporary = reportPath + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << Json() << '\n';
        output.close();
        bool saved = output.good() && std::rename(temporary.c_str(), reportPath.c_str()) == 0;
        if (!saved) std::remove(temporary.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex);
            reportSaved = saved;
            reportError = saved ? "" : "failed to write/rename capture-probe.json";
        }
        if (!saved) {
            Fail(OUTPUT_ERROR, "probe report cannot be saved to filesDir");
            std::lock_guard<std::mutex> lock(mutex);
            status = "failed";
        }
    }

    void Run() noexcept
    {
        {
            CaptureOwner owner {*this};
            try {
                // A new run must never present the previous run's image as new evidence.
                std::remove(framePath.c_str());
                WriteReport();
                if (Configure(owner)) {
                    bool frameRateSet = false;
                    auto nextReport = Clock::now();
                    for (;;) {
                        bool shouldSetRate = false;
                        {
                            std::unique_lock<std::mutex> lock(mutex);
                            auto now = Clock::now();
                            if (stopRequested) break;
                            if (frames && Seconds(firstFrameAt, now) >= PROBE_SECONDS) {
                                timerFinished = true;
                                stopRequested = true;
                                FreezeCaptureLocked();
                                break;
                            }
                            if (!frames && Seconds(requestedAt, now) >= START_TIMEOUT_SECONDS) {
                                lock.unlock();
                                Fail(START_TIMEOUT, "no video frame within 60 seconds, including consent wait");
                                break;
                            }
                            shouldSetRate = captureStarted && !frameRateSet;
                            if (captureStarted) status = "capturing";
                        }
                        if (shouldSetRate) {
                            frameRateSet = true;
                            if (!Checked("OH_AVScreenCapture_SetMaxVideoFrameRate",
                                OH_AVScreenCapture_SetMaxVideoFrameRate(owner.capture, TARGET_FPS))) break;
                        }
                        WriteSample();
                        if (Clock::now() >= nextReport) {
                            WriteReport();
                            nextReport = Clock::now() + std::chrono::seconds(1);
                        }
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait_for(lock, std::chrono::milliseconds(100));
                    }
                }
            } catch (...) {
                try { Fail(INTERNAL_ERROR, "exception in capture worker"); } catch (...) {}
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                FreezeCaptureLocked();
            }
        } // SDK is stopped/released on this worker before final evidence is written.
        try {
            WriteSample();
            {
                std::lock_guard<std::mutex> lock(mutex);
                finishedAt = Clock::now();
                finishedAtUnixMs = UnixMilliseconds();
                status = errorCode ? "failed" : (timerFinished ? "completed" : "stopped");
                running = false;
            }
            WriteReport();
        } catch (...) {
            try {
                Fail(INTERNAL_ERROR, "exception while saving final capture evidence");
                std::lock_guard<std::mutex> lock(mutex);
                status = "failed";
                running = false;
            } catch (...) {}
        }
    }
};

CaptureProbe::CaptureProbe() : impl_(std::make_unique<Impl>()) {}
CaptureProbe::~CaptureProbe() { Stop(); }

int CaptureProbe::Start(const std::string& filesDir)
{
    struct stat info {};
    if (filesDir.empty() || filesDir.front() != '/' || filesDir.find('\0') != std::string::npos ||
        stat(filesDir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return -1;
    auto& self = *impl_;
    std::lock_guard<std::mutex> operation(self.lifecycle);
    {
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.running) return -2;
    }
    if (self.worker.joinable()) self.worker.join();
    {
        std::lock_guard<std::mutex> lock(self.mutex);
        self.stopRequested = self.captureStarted = self.timerFinished = self.imageAttempted = false;
        self.imageSaved = self.selectedDisplayKnown = self.reportSaved = false;
        self.timestampMonotonic = true;
        self.captureEnded = false;
        self.frames = self.audioCallbacks = self.selectedDisplayId = self.displayId = 0;
        self.displayWidth = self.displayHeight = self.width = self.height = self.errorCode = 0;
        self.firstTimestamp = self.lastTimestamp = self.finishedAtUnixMs = 0;
        self.requestedAtUnixMs = UnixMilliseconds();
        self.requestedAt = Clock::now();
        self.firstFrameAt = self.captureEndedAt = self.finishedAt = {};
        self.status = "starting";
        self.error.clear();
        self.imageError.clear();
        self.reportError.clear();
        self.filesDir = filesDir;
        self.framePath = filesDir + "/capture-frame.ppm";
        self.reportPath = filesDir + "/capture-probe.json";
        self.sample = {};
        self.apiCodes.clear();
        self.states.clear();
        self.apiCodes.reserve(MAX_EVENTS);
        self.states.reserve(MAX_EVENTS);
        self.running = true;
    }
    try { self.worker = std::thread([impl = &self] { impl->Run(); }); }
    catch (...) {
        std::lock_guard<std::mutex> lock(self.mutex);
        self.running = false;
        self.status = "failed";
        self.errorCode = INTERNAL_ERROR;
        self.error = "capture worker could not be created";
        self.finishedAt = Clock::now();
        self.finishedAtUnixMs = UnixMilliseconds();
        return -3;
    }
    return 0;
}

void CaptureProbe::Stop()
{
    auto& self = *impl_;
    std::lock_guard<std::mutex> operation(self.lifecycle);
    {
        std::lock_guard<std::mutex> lock(self.mutex);
        self.stopRequested = true;
        self.FreezeCaptureLocked();
    }
    self.wake.notify_all();
    if (self.worker.joinable()) self.worker.join();
}

std::string CaptureProbe::SnapshotJson() { return impl_->Json(); }

CaptureProbe& GetCaptureProbe()
{
    static CaptureProbe probe;
    return probe;
}
