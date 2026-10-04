#include "input_probe.h"

#include <limits>
#include <sstream>
#include <time.h>
#include <multimodalinput/oh_input_manager.h>
#include <window_manager/oh_display_manager.h>

namespace {
int FirstError(int current, int next)
{
    return current == INPUT_SUCCESS ? next : current;
}

int64_t MonotonicMicroseconds()
{
    timespec now {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

int64_t UnixMilliseconds()
{
    timespec now {};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

const char* StatusName(int status)
{
    switch (status) {
        case UNAUTHORIZED: return "UNAUTHORIZED";
        case AUTHORIZING: return "AUTHORIZING";
        case AUTHORIZED: return "AUTHORIZED";
        default: return "UNKNOWN";
    }
}
}

InputProbe& GetInputProbe()
{
    static InputProbe probe;
    return probe;
}

void InputProbe::Begin(const char* operation)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastOperation_ = operation;
    lastCode_ = 0;
    operationStartedUnixMs_ = UnixMilliseconds();
    steps_.clear();
}

void InputProbe::Record(const char* step, int code)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (steps_.size() >= 64) {
        steps_.erase(steps_.begin());
    }
    steps_.push_back({step, code});
}

int InputProbe::Finish(int code)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastCode_ = code;
    if (history_.size() >= HISTORY_LIMIT) {
        history_.erase(history_.begin());
    }
    // The caller still owns operationMutex_, so pending input state cannot
    // change while the finished operation is copied into the history.
    history_.push_back({lastOperation_, operationStartedUnixMs_, UnixMilliseconds(),
        code, steps_, pendingKeys_, pendingMouseLeft_});
    return code;
}

void InputProbe::OnAuthorization(int status)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    callbackStatus_ = status;
    ++callbackCount_;
    dialogStatus_ = status;
}

// Intentionally not used as an SDK callback directly: the SDK accepts the enum
// Input_InjectionStatus, not an int-typed callback.
void InputProbe::AuthorizationCallback(int status)
{
    GetInputProbe().OnAuthorization(status);
}

int InputProbe::RequestAuthorization()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    Begin("requestAuthorization");
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        cancelRequested_ = false;
    }
    // A non-capturing lambda preserves the exact Input_InjectAuthorizeCallback
    // signature and does not touch N-API or ArkTS from the SDK callback thread.
    int result = OH_Input_RequestInjection([](Input_InjectionStatus status) {
        InputProbe::AuthorizationCallback(static_cast<int>(status));
    });
    Record("requestInjection", result);
    // Success means the request was accepted, not that consent was granted.
    int queryResult = QueryAuthorization();
    Record("queryAuthorization", queryResult);
    return Finish(FirstError(result, queryResult));
}

int InputProbe::QueryAuthorization()
{
    Input_InjectionStatus status = UNAUTHORIZED;
    int result = OH_Input_QueryAuthorizedStatus(&status);
    std::lock_guard<std::mutex> lock(stateMutex_);
    queryCode_ = result;
    dialogStatus_ = result == INPUT_SUCCESS ? static_cast<int>(status) : -1;
    return result;
}

int InputProbe::RequireAuthorization()
{
    int result = QueryAuthorization();
    Record("queryAuthorization", result);
    if (result != INPUT_SUCCESS) {
        return result;
    }
    int status;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status = dialogStatus_;
    }
    // The CONTROL_DEVICE_PERMISSION mode is reserved, never inferred from a
    // successful query or enabled in this MVP.
    if (mode_ != InputAuthorizationMode::USER_GRANTED || status != AUTHORIZED) {
        Record("userAuthorizationRequired", INPUT_PERMISSION_DENIED);
        return INPUT_PERMISSION_DENIED;
    }
    // Do not begin a new gesture while a previous up event is still uncertain.
    // A retry is explicit in the returned cleanup steps and its error code.
    return ReleasePendingInputs();
}

int InputProbe::ReadDefaultDisplay(DisplayInfo& display)
{
    display.valid = false;
    int result = OH_NativeDisplayManager_GetDefaultDisplayId(&display.id);
    Record("getDefaultDisplayId", result);
    if (result != DISPLAY_MANAGER_OK) {
        return result;
    }
    result = OH_NativeDisplayManager_GetDefaultDisplayWidth(&display.width);
    Record("getDefaultDisplayWidth", result);
    if (result != DISPLAY_MANAGER_OK) {
        return result;
    }
    result = OH_NativeDisplayManager_GetDefaultDisplayHeight(&display.height);
    Record("getDefaultDisplayHeight", result);
    if (result != DISPLAY_MANAGER_OK) {
        return result;
    }
    // The display API exposes uint64_t IDs; the input API accepts int32_t.
    if (display.id > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        display.width <= 0 || display.height <= 0) {
        Record("validateDisplay", INVALID_DISPLAY);
        return INVALID_DISPLAY;
    }
    display.valid = true;
    return INPUT_SUCCESS;
}

int InputProbe::InjectKey(int32_t code, bool down, const char* step)
{
    auto* event = OH_Input_CreateKeyEvent();
    if (event == nullptr) {
        Record(step, ALLOCATION_FAILED);
        return ALLOCATION_FAILED;
    }
    int64_t timestamp = MonotonicMicroseconds();
    if (timestamp < 0) {
        OH_Input_DestroyKeyEvent(&event);
        Record(step, CLOCK_UNAVAILABLE);
        return CLOCK_UNAVAILABLE;
    }
    OH_Input_SetKeyEventKeyCode(event, code);
    OH_Input_SetKeyEventAction(event, down ? KEY_ACTION_DOWN : KEY_ACTION_UP);
    OH_Input_SetKeyEventActionTime(event, timestamp);
    if (down) {
        // Even a failed down call is followed by an up attempt: the caller
        // cannot prove a service error had no effect on the receiver.
        pendingKeys_.insert(code);
    }
    int result = OH_Input_InjectKeyEvent(event);
    OH_Input_DestroyKeyEvent(&event);
    if (!down && result == INPUT_SUCCESS) {
        pendingKeys_.erase(code);
    }
    Record(step, result);
    return result;
}

int InputProbe::InjectMouse(const DisplayInfo& display, int32_t action, int32_t button, const char* step)
{
    if (!display.valid) {
        Record(step, INVALID_DISPLAY);
        return INVALID_DISPLAY;
    }
    auto* event = OH_Input_CreateMouseEvent();
    if (event == nullptr) {
        Record(step, ALLOCATION_FAILED);
        return ALLOCATION_FAILED;
    }
    int64_t timestamp = MonotonicMicroseconds();
    if (timestamp < 0) {
        OH_Input_DestroyMouseEvent(&event);
        Record(step, CLOCK_UNAVAILABLE);
        return CLOCK_UNAVAILABLE;
    }
    OH_Input_SetMouseEventDisplayId(event, static_cast<int32_t>(display.id));
    OH_Input_SetMouseEventDisplayX(event, display.width / 2);
    OH_Input_SetMouseEventDisplayY(event, display.height / 2);
    OH_Input_SetMouseEventAction(event, action);
    OH_Input_SetMouseEventButton(event, button);
    OH_Input_SetMouseEventActionTime(event, timestamp);
    if (action == MOUSE_ACTION_BUTTON_DOWN && button == MOUSE_BUTTON_LEFT) {
        pendingMouseLeft_ = true;
    }
    int result = OH_Input_InjectMouseEvent(event);
    OH_Input_DestroyMouseEvent(&event);
    if (action == MOUSE_ACTION_BUTTON_UP && button == MOUSE_BUTTON_LEFT && result == INPUT_SUCCESS) {
        pendingMouseLeft_ = false;
    }
    Record(step, result);
    return result;
}

int InputProbe::MoveMouseToCenter()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    Begin("moveMouseToCenter");
    int result = RequireAuthorization();
    if (result == INPUT_SUCCESS) {
        result = ReadDefaultDisplay(display_);
    }
    if (result == INPUT_SUCCESS) {
        result = InjectMouse(display_, MOUSE_ACTION_MOVE, MOUSE_BUTTON_NONE, "mouse.move");
    }
    return Finish(result);
}

int InputProbe::InjectA()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    Begin("injectA");
    int result = RequireAuthorization();
    if (result != INPUT_SUCCESS) {
        return Finish(result);
    }
    result = InjectKey(KEYCODE_A, true, "a.down");
    result = FirstError(result, InjectKey(KEYCODE_A, false, "a.up"));
    return Finish(result);
}

int InputProbe::ClickLeft()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    Begin("clickLeftAtCenter");
    int result = RequireAuthorization();
    if (result == INPUT_SUCCESS) {
        result = ReadDefaultDisplay(display_);
    }
    if (result == INPUT_SUCCESS) {
        result = InjectMouse(display_, MOUSE_ACTION_MOVE, MOUSE_BUTTON_NONE, "mouse.move");
    }
    if (result == INPUT_SUCCESS) {
        result = InjectMouse(display_, MOUSE_ACTION_BUTTON_DOWN, MOUSE_BUTTON_LEFT, "left.down");
        result = FirstError(result,
            InjectMouse(display_, MOUSE_ACTION_BUTTON_UP, MOUSE_BUTTON_LEFT, "left.up"));
    }
    return Finish(result);
}

int InputProbe::InjectCtrlL()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    Begin("injectCtrlL");
    int result = RequireAuthorization();
    if (result != INPUT_SUCCESS) {
        return Finish(result);
    }
    result = InjectKey(KEYCODE_CTRL_LEFT, true, "ctrl.down");
    if (result == INPUT_SUCCESS) {
        result = InjectKey(KEYCODE_L, true, "l.down");
        result = FirstError(result, InjectKey(KEYCODE_L, false, "l.up"));
    }
    result = FirstError(result, InjectKey(KEYCODE_CTRL_LEFT, false, "ctrl.up"));
    return Finish(result);
}

int InputProbe::ReleasePendingInputs()
{
    int result = INPUT_SUCCESS;
    // Copy because successful release removes elements from pendingKeys_.
    const auto keys = pendingKeys_;
    for (auto code : keys) {
        result = FirstError(result, InjectKey(code, false, "cleanup.keyUp"));
    }
    if (pendingMouseLeft_) {
        result = FirstError(result,
            InjectMouse(display_, MOUSE_ACTION_BUTTON_UP, MOUSE_BUTTON_LEFT, "cleanup.leftUp"));
    }
    return result;
}

int InputProbe::CancelAndRelease()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    Begin("cancelAndRelease");
    // Attempt releases even if authorization was revoked between events. Expose
    // any failure; never silently clear the pending release state.
    int result = ReleasePendingInputs();
    OH_Input_CancelInjection(); // void API: there is no SDK success code to log.
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        cancelRequested_ = true;
    }
    int queryResult = QueryAuthorization();
    Record("queryAuthorizationAfterCancel", queryResult);
    return Finish(FirstError(result, queryResult));
}

std::string InputProbe::QueryJson()
{
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    QueryAuthorization();
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    std::ostringstream json;
    // All string fields originate from this source, not from external input.
    json << "{\"mode\":\"USER_GRANTED\",\"controlDevicePermissionEnabled\":false"
         << ",\"dialogStatus\":" << dialogStatus_
         << ",\"dialogStatusName\":\"" << StatusName(dialogStatus_) << "\""
         << ",\"queryCode\":" << queryCode_
         << ",\"authorized\":" << (queryCode_ == 0 && dialogStatus_ == AUTHORIZED ? "true" : "false")
         << ",\"callbackStatus\":" << callbackStatus_
         << ",\"callbackCount\":" << callbackCount_
         << ",\"lastOperation\":\"" << lastOperation_ << "\""
         << ",\"lastCode\":" << lastCode_
         << ",\"cancelRequested\":" << (cancelRequested_ ? "true" : "false")
         << ",\"display\":{\"valid\":" << (display_.valid ? "true" : "false")
         << ",\"id\":" << display_.id << ",\"width\":" << display_.width
         << ",\"height\":" << display_.height << "}"
         << ",\"pendingKeys\":[";
    bool first = true;
    for (auto key : pendingKeys_) {
        if (!first) json << ',';
        json << key;
        first = false;
    }
    json << "],\"pendingMouseLeft\":" << (pendingMouseLeft_ ? "true" : "false") << ",\"steps\":[";
    first = true;
    for (const auto& step : steps_) {
        if (!first) json << ',';
        json << "{\"name\":\"" << step.name << "\",\"code\":" << step.code << '}';
        first = false;
    }
    json << "],\"historyLimit\":" << HISTORY_LIMIT << ",\"history\":[";
    first = true;
    for (const auto& operation : history_) {
        if (!first) json << ',';
        first = false;
        json << "{\"operation\":\"" << operation.operation << "\",\"startedUnixMs\":";
        // A failed wall-clock read is unknown, never silently substituted with
        // monotonic time or a manufactured timestamp. Input results are intact.
        if (operation.startedUnixMs >= 0) json << operation.startedUnixMs;
        else json << "null";
        json << ",\"finishedUnixMs\":";
        if (operation.finishedUnixMs >= 0) json << operation.finishedUnixMs;
        else json << "null";
        json << ",\"code\":" << operation.code << ",\"steps\":[";
        bool firstStep = true;
        for (const auto& step : operation.steps) {
            if (!firstStep) json << ',';
            json << "{\"name\":\"" << step.name << "\",\"code\":" << step.code << '}';
            firstStep = false;
        }
        json << "],\"pendingKeys\":[";
        bool firstKey = true;
        for (auto key : operation.pendingKeys) {
            if (!firstKey) json << ',';
            json << key;
            firstKey = false;
        }
        json << "],\"pendingMouseLeft\":" << (operation.pendingMouseLeft ? "true" : "false") << '}';
    }
    json << "]}";
    return json.str();
}
