#ifndef HARMONY_REMOTE_INPUT_PROBE_H
#define HARMONY_REMOTE_INPUT_PROBE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

enum class InputAuthorizationMode {
    USER_GRANTED,
    CONTROL_DEVICE_PERMISSION
};

// Local, explicitly triggered capability probe. Network code must not call this
// until the product's separate session consent and authentication checks pass.
class InputProbe {
public:
    int RequestAuthorization();
    std::string QueryJson();
    int MoveMouseToCenter();
    int InjectA();
    int ClickLeft();
    int InjectCtrlL();
    int CancelAndRelease();

    // Application errors; all SDK errors retain their original numeric values.
    static constexpr int ALLOCATION_FAILED = 9000001;
    static constexpr int INVALID_DISPLAY = 9000002;
    static constexpr int CLOCK_UNAVAILABLE = 9000003;

private:
    friend InputProbe& GetInputProbe();
    InputProbe() = default;

    struct StepResult {
        std::string name;
        int code;
    };
    struct DisplayInfo {
        uint64_t id = 0;
        int32_t width = 0;
        int32_t height = 0;
        bool valid = false;
    };
    struct CompletedOperation {
        std::string operation;
        int64_t startedUnixMs = -1;
        int64_t finishedUnixMs = -1;
        int code = 0;
        std::vector<StepResult> steps;
        std::set<int32_t> pendingKeys;
        bool pendingMouseLeft = false;
    };
    static constexpr std::size_t HISTORY_LIMIT = 32;

    void Begin(const char* operation);
    void Record(const char* step, int code);
    int Finish(int code);
    int QueryAuthorization();
    int RequireAuthorization();
    int ReadDefaultDisplay(DisplayInfo& display);
    int InjectKey(int32_t code, bool down, const char* step);
    int InjectMouse(const DisplayInfo& display, int32_t action, int32_t button, const char* step);
    int ReleasePendingInputs();
    void OnAuthorization(int status);
    static void AuthorizationCallback(int status);

    // SDK operations serialize separately from callbacks, which may be invoked
    // synchronously or on an SDK thread while a request is in progress.
    std::mutex operationMutex_;
    std::mutex stateMutex_;
    const InputAuthorizationMode mode_ = InputAuthorizationMode::USER_GRANTED;
    int dialogStatus_ = 0;
    int queryCode_ = -1;
    int callbackStatus_ = -1;
    uint64_t callbackCount_ = 0;
    std::string lastOperation_ = "idle";
    int lastCode_ = 0;
    int64_t operationStartedUnixMs_ = -1;
    bool cancelRequested_ = false;
    std::vector<StepResult> steps_;
    std::set<int32_t> pendingKeys_;
    bool pendingMouseLeft_ = false;
    DisplayInfo display_;
    // Completed operation snapshots only. Read/write while holding both
    // operationMutex_ and stateMutex_; authorization callbacks never append.
    std::vector<CompletedOperation> history_;
};

InputProbe& GetInputProbe();

#endif
