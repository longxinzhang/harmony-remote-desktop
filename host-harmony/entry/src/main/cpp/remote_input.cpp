#include "remote_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include <time.h>
#include <multimodalinput/oh_input_manager.h>
#include <window_manager/oh_display_manager.h>

namespace {
constexpr int ALLOCATION_FAILED = 9100001;
constexpr int INVALID_DISPLAY = 9100002;
constexpr int CLOCK_FAILED = 9100003;
constexpr int INVALID_EVENT = 9100004;
constexpr int QUEUE_OVERFLOW = 9100005;
constexpr int DISPLAY_CHANGED = 9100006;
constexpr int DUPLICATE_EDGE = 9100007;
constexpr size_t QUEUE_LIMIT = 128;

int64_t NowUs()
{
    timespec now {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

const std::map<std::string, int32_t>& KeyCodes()
{
    static const std::map<std::string, int32_t> codes = [] {
        std::map<std::string, int32_t> result;
        // SDK oh_key_code.h defines each of these ranges contiguously.
        for (int i = 0; i < 26; ++i) result[std::string("KEY_") + char('A' + i)] = KEYCODE_A + i;
        for (int i = 0; i < 10; ++i) result[std::string("KEY_") + char('0' + i)] = KEYCODE_0 + i;
        for (int i = 0; i < 12; ++i) result["KEY_F" + std::to_string(i + 1)] = KEYCODE_F1 + i;
        const std::pair<const char*, int32_t> extra[] = {
            {"KEY_MINUS", KEYCODE_MINUS}, {"KEY_EQUALS", KEYCODE_EQUALS},
            {"KEY_LEFT_BRACKET", KEYCODE_LEFT_BRACKET}, {"KEY_RIGHT_BRACKET", KEYCODE_RIGHT_BRACKET},
            {"KEY_BACKSLASH", KEYCODE_BACKSLASH}, {"KEY_SEMICOLON", KEYCODE_SEMICOLON},
            {"KEY_APOSTROPHE", KEYCODE_APOSTROPHE}, {"KEY_GRAVE", KEYCODE_GRAVE},
            {"KEY_COMMA", KEYCODE_COMMA}, {"KEY_PERIOD", KEYCODE_PERIOD}, {"KEY_SLASH", KEYCODE_SLASH},
            {"KEY_ENTER", KEYCODE_ENTER}, {"KEY_ESCAPE", KEYCODE_ESCAPE}, {"KEY_TAB", KEYCODE_TAB},
            {"KEY_SPACE", KEYCODE_SPACE}, {"KEY_BACKSPACE", KEYCODE_DEL}, {"KEY_DELETE", KEYCODE_FORWARD_DEL},
            {"KEY_UP", KEYCODE_DPAD_UP}, {"KEY_DOWN", KEYCODE_DPAD_DOWN},
            {"KEY_LEFT", KEYCODE_DPAD_LEFT}, {"KEY_RIGHT", KEYCODE_DPAD_RIGHT},
            {"KEY_HOME", KEYCODE_MOVE_HOME}, {"KEY_END", KEYCODE_MOVE_END},
            {"KEY_PAGE_UP", KEYCODE_PAGE_UP}, {"KEY_PAGE_DOWN", KEYCODE_PAGE_DOWN},
            {"KEY_SHIFT_LEFT", KEYCODE_SHIFT_LEFT}, {"KEY_SHIFT_RIGHT", KEYCODE_SHIFT_RIGHT},
            {"KEY_CTRL_LEFT", KEYCODE_CTRL_LEFT}, {"KEY_CTRL_RIGHT", KEYCODE_CTRL_RIGHT},
            {"KEY_ALT_LEFT", KEYCODE_ALT_LEFT}, {"KEY_ALT_RIGHT", KEYCODE_ALT_RIGHT},
            {"KEY_META_LEFT", KEYCODE_META_LEFT}, {"KEY_META_RIGHT", KEYCODE_META_RIGHT},
            {"KEY_CAPS_LOCK", KEYCODE_CAPS_LOCK}
        };
        for (const auto& entry : extra) result.emplace(entry.first, entry.second);
        return result;
    }();
    return codes;
}

int32_t ButtonCode(const std::string& button)
{
    if (button == "left") return MOUSE_BUTTON_LEFT;
    if (button == "right") return MOUSE_BUTTON_RIGHT;
    if (button == "middle") return MOUSE_BUTTON_MIDDLE;
    return MOUSE_BUTTON_NONE;
}

bool Valid(const RemoteInputEvent& event)
{
    if (event.code.size() > 24 || event.button.size() > 6) return false;
    if (event.kind == RemoteInputEvent::Kind::Key) return KeyCodes().count(event.code) != 0;
    switch (event.kind) {
        case RemoteInputEvent::Kind::Move:
            return std::isfinite(event.x) && std::isfinite(event.y) && event.x >= 0 && event.x <= 1 && event.y >= 0 && event.y <= 1;
        case RemoteInputEvent::Kind::Button: return ButtonCode(event.button) != MOUSE_BUTTON_NONE;
        case RemoteInputEvent::Kind::Scroll:
            return std::isfinite(event.dx) && std::isfinite(event.dy) && std::abs(event.dx) <= 120 &&
                std::abs(event.dy) <= 120 && (event.dx != 0 || event.dy != 0);
        default: return false;
    }
}
}

class RemoteInput::Impl {
public:
    Impl() : worker_([this] { Run(); }) {}
    ~Impl()
    {
        // Destruction does not allocate a promise/command. Stop production first;
        // the worker performs its final best-effort release before join returns.
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; enabled_ = false; }
        condition_.notify_one();
        worker_.join();
    }

    enum class TaskKind { Event, Allow, Enable, Release };
    bool Command(TaskKind kind, bool value = false)
    {
        auto answer = std::make_shared<std::promise<bool>>();
        auto future = answer->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (kind == TaskKind::Release || !value) ClearEventsLocked();
            if (kind == TaskKind::Enable && !value) enabled_ = false;
            if (kind == TaskKind::Allow && !value) { allowed_ = false; enabled_ = false; }
            tasks_.push_back({kind, {}, value, answer});
        }
        condition_.notify_one();
        return future.get();
    }

    bool Enabled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }

    bool Submit(const RemoteInputEvent& event)
    {
        try {
            const bool valid = Valid(event);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!allowed_ || !enabled_ || stopping_) return false;
            if (!valid) { ScheduleFailureLocked(INVALID_EVENT); return false; }
            // Project queued edges as well as the current hold state, so a
            // duplicate down is rejected by Submit itself, even before the
            // worker reaches the earlier down. Release barriers clear this view.
            if (event.kind == RemoteInputEvent::Kind::Key || event.kind == RemoteInputEvent::Kind::Button) {
                auto& projected = event.kind == RemoteInputEvent::Kind::Key ? projectedKeys_ : projectedButtons_;
                const int32_t code = event.kind == RemoteInputEvent::Kind::Key ? KeyCodes().at(event.code) : ButtonCode(event.button);
                if ((projected.count(code) != 0) == event.down) { ScheduleFailureLocked(DUPLICATE_EDGE); return false; }
                if (event.down) projected.insert(code); else projected.erase(code);
            }
            if (!tasks_.empty() && tasks_.back().kind == TaskKind::Event &&
                event.kind == RemoteInputEvent::Kind::Move && tasks_.back().event.kind == RemoteInputEvent::Kind::Move) {
                tasks_.back().event = event;
                ++coalesced_;
                ++accepted_;
                return true;
            }
            if (queuedEvents_ >= QUEUE_LIMIT) { ScheduleFailureLocked(QUEUE_OVERFLOW); return false; }
            tasks_.push_back({TaskKind::Event, event, false, nullptr});
            ++queuedEvents_;
            ++accepted_;
            condition_.notify_one();
            return true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            ScheduleFailureLocked(ALLOCATION_FAILED);
            return false;
        }
    }

    std::string Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        out << "{\"allowed\":" << (allowed_ ? "true" : "false")
            << ",\"sessionEnabled\":" << (enabled_ ? "true" : "false")
            << ",\"authorized\":" << (queryCode_ == INPUT_SUCCESS && authorization_ == AUTHORIZED ? "true" : "false")
            << ",\"authorizationStatus\":" << authorization_ << ",\"queryCode\":" << queryCode_
            << ",\"queuedEvents\":" << queuedEvents_ << ",\"queueLimit\":" << QUEUE_LIMIT
            << ",\"busy\":" << (busy_ ? "true" : "false")
            << ",\"acceptedEvents\":" << accepted_ << ",\"processedEvents\":" << processed_
            << ",\"coalescedMoves\":" << coalesced_ << ",\"failures\":" << failures_
            << ",\"errorCode\":" << lastCode_ << ",\"lastReleaseCode\":" << releaseCode_
            << ",\"pendingKeysCount\":" << pendingKeyCount_ << ",\"pendingButtonsCount\":" << pendingButtonCount_
            << ",\"pendingAxesCount\":" << pendingAxisCount_ << '}';
        return out.str();
    }

private:
    struct Task {
        TaskKind kind;
        RemoteInputEvent event;
        bool value;
        std::shared_ptr<std::promise<bool>> answer;
    };
    struct Display {
        uint64_t id = 0;
        int32_t width = 0;
        int32_t height = 0;
        NativeDisplayManager_Rotation rotation = DISPLAY_MANAGER_ROTATION_0;
        bool operator==(const Display& other) const
        {
            return id == other.id && width == other.width && height == other.height && rotation == other.rotation;
        }
    };

    void ClearEventsLocked()
    {
        tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [](const Task& task) {
            return task.kind == TaskKind::Event;
        }), tasks_.end());
        queuedEvents_ = 0;
        projectedKeys_.clear();
        projectedButtons_.clear();
    }

    void ScheduleFailureLocked(int code)
    {
        enabled_ = false;
        ClearEventsLocked();
        if (pendingFailure_ == 0) pendingFailure_ = code;
        lastCode_ = code;
        condition_.notify_one();
    }

    int Authorization()
    {
        Input_InjectionStatus status = UNAUTHORIZED;
        const int result = OH_Input_QueryAuthorizedStatus(&status);
        std::lock_guard<std::mutex> lock(mutex_);
        queryCode_ = result;
        authorization_ = result == INPUT_SUCCESS ? static_cast<int>(status) : -1;
        if (result != INPUT_SUCCESS || status != AUTHORIZED) {
            allowed_ = false;
            return result != INPUT_SUCCESS ? result : INPUT_PERMISSION_DENIED;
        }
        return INPUT_SUCCESS;
    }

    int ReadDisplay(Display& display)
    {
        int result = OH_NativeDisplayManager_GetDefaultDisplayId(&display.id);
        if (result == DISPLAY_MANAGER_OK) result = OH_NativeDisplayManager_GetDefaultDisplayWidth(&display.width);
        if (result == DISPLAY_MANAGER_OK) result = OH_NativeDisplayManager_GetDefaultDisplayHeight(&display.height);
        if (result == DISPLAY_MANAGER_OK) result = OH_NativeDisplayManager_GetDefaultDisplayRotation(&display.rotation);
        if (result != DISPLAY_MANAGER_OK) return result;
        if (display.id > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || display.width <= 0 || display.height <= 0 ||
            display.rotation < DISPLAY_MANAGER_ROTATION_0 || display.rotation > DISPLAY_MANAGER_ROTATION_270) return INVALID_DISPLAY;
        return INPUT_SUCCESS;
    }

    int CheckSession()
    {
        int result = Authorization();
        if (result != INPUT_SUCCESS) return result;
        Display current;
        result = ReadDisplay(current);
        if (result != INPUT_SUCCESS) return result;
        return current == display_ ? INPUT_SUCCESS : DISPLAY_CHANGED;
    }

    void PublishPending()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingKeyCount_ = keys_.size();
        pendingButtonCount_ = buttons_.size();
        pendingAxisCount_ = axes_.size();
    }

    int Key(int32_t code, bool down)
    {
        auto* event = OH_Input_CreateKeyEvent();
        if (event == nullptr) return ALLOCATION_FAILED;
        const auto cleanup = std::unique_ptr<Input_KeyEvent, void (*)(Input_KeyEvent*)>(event,
            [](Input_KeyEvent* value) { OH_Input_DestroyKeyEvent(&value); });
        const int64_t time = NowUs();
        if (time < 0) return CLOCK_FAILED;
        OH_Input_SetKeyEventKeyCode(event, code);
        OH_Input_SetKeyEventAction(event, down ? KEY_ACTION_DOWN : KEY_ACTION_UP);
        OH_Input_SetKeyEventActionTime(event, time);
        if (down) { keyOrder_.push_back(code); keys_.insert(code); }
        const int result = OH_Input_InjectKeyEvent(event);
        if (!down && result == INPUT_SUCCESS) {
            keys_.erase(code);
            keyOrder_.erase(std::remove(keyOrder_.begin(), keyOrder_.end(), code), keyOrder_.end());
        }
        PublishPending();
        return result;
    }

    int Mouse(int32_t action, int32_t button = MOUSE_BUTTON_NONE, int32_t axis = MOUSE_AXIS_SCROLL_VERTICAL, float value = 0)
    {
        auto* event = OH_Input_CreateMouseEvent();
        if (event == nullptr) return ALLOCATION_FAILED;
        const auto cleanup = std::unique_ptr<Input_MouseEvent, void (*)(Input_MouseEvent*)>(event,
            [](Input_MouseEvent* value) { OH_Input_DestroyMouseEvent(&value); });
        const int64_t time = NowUs();
        if (time < 0) return CLOCK_FAILED;
        OH_Input_SetMouseEventDisplayId(event, static_cast<int32_t>(display_.id));
        OH_Input_SetMouseEventDisplayX(event, mouseX_);
        OH_Input_SetMouseEventDisplayY(event, mouseY_);
        OH_Input_SetMouseEventAction(event, action);
        OH_Input_SetMouseEventButton(event, button);
        OH_Input_SetMouseEventActionTime(event, time);
        if (action >= MOUSE_ACTION_AXIS_BEGIN && action <= MOUSE_ACTION_AXIS_END) {
            OH_Input_SetMouseEventAxisType(event, axis);
            OH_Input_SetMouseEventAxisValue(event, value);
        }
        if (action == MOUSE_ACTION_BUTTON_DOWN) buttons_.insert(button);
        if (action == MOUSE_ACTION_AXIS_BEGIN) axes_.insert(axis);
        const int result = OH_Input_InjectMouseEvent(event);
        if (result == INPUT_SUCCESS) {
            if (action == MOUSE_ACTION_BUTTON_UP) buttons_.erase(button);
            if (action == MOUSE_ACTION_AXIS_END) axes_.erase(axis);
        }
        PublishPending();
        return result;
    }

    int Release()
    {
        int firstError = INPUT_SUCCESS;
        const auto axes = axes_;
        const auto buttons = buttons_;
        const auto keys = keyOrder_;
        auto record = [&](int code) { if (firstError == INPUT_SUCCESS) firstError = code; };
        // Cleanup intentionally bypasses authorization: even a denied down may
        // have reached the service, and a failed up must remain pending.
        for (int32_t axis : axes) record(Mouse(MOUSE_ACTION_AXIS_END, MOUSE_BUTTON_NONE, axis, 0));
        for (int32_t button : buttons) record(Mouse(MOUSE_ACTION_BUTTON_UP, button));
        for (auto key = keys.rbegin(); key != keys.rend(); ++key) record(Key(*key, false));
        { std::lock_guard<std::mutex> lock(mutex_); releaseCode_ = firstError; }
        PublishPending();
        return firstError;
    }

    void Fail(int code, bool release = true)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            enabled_ = false;
            ClearEventsLocked();
            lastCode_ = code;
            ++failures_;
        }
        if (release) Release();
    }

    int ScrollAxis(int32_t axis, double amount)
    {
        if (amount == 0) return INPUT_SUCCESS;
        int result = Mouse(MOUSE_ACTION_AXIS_BEGIN, MOUSE_BUTTON_NONE, axis, 0);
        if (result == INPUT_SUCCESS) result = Mouse(MOUSE_ACTION_AXIS_UPDATE, MOUSE_BUTTON_NONE, axis, static_cast<float>(amount));
        // Always attempt END, including after an uncertain BEGIN/UPDATE failure.
        const int end = Mouse(MOUSE_ACTION_AXIS_END, MOUSE_BUTTON_NONE, axis, 0);
        return result == INPUT_SUCCESS ? end : result;
    }

    int Process(const RemoteInputEvent& event)
    {
        int result = CheckSession();
        if (result != INPUT_SUCCESS) return result;
        // A concurrent disable can arrive while the SDK query is in flight.
        if (!Enabled()) return INPUT_SUCCESS;
        if (event.kind == RemoteInputEvent::Kind::Key) {
            const int32_t code = KeyCodes().at(event.code);
            if ((keys_.count(code) != 0) == event.down) return DUPLICATE_EDGE;
            return Key(code, event.down);
        }
        if (event.kind == RemoteInputEvent::Kind::Move) {
            mouseX_ = static_cast<int32_t>(std::llround(event.x * (display_.width - 1)));
            mouseY_ = static_cast<int32_t>(std::llround(event.y * (display_.height - 1)));
            // InputKit's button field belongs to each event, including MOVE.
            // Preserve a held button's identity throughout a drag; passing NONE
            // here turns the move into an event with no active button identity.
            // The SDK exposes one button ID, not a pressed-buttons array; prefer
            // left, then middle/right if more than one button is held.
            const int32_t button = buttons_.empty() ? MOUSE_BUTTON_NONE : *buttons_.begin();
            result = Mouse(MOUSE_ACTION_MOVE, button);
            if (result == INPUT_SUCCESS) positionKnown_ = true;
            return result;
        }
        // Button/Scroll wire messages intentionally carry no coordinates.
        // Never interpret their default x/y values as a request to jump to (0,0).
        if (!positionKnown_) return INVALID_EVENT;
        if (event.kind == RemoteInputEvent::Kind::Button) {
            const int32_t button = ButtonCode(event.button);
            if ((buttons_.count(button) != 0) == event.down) return DUPLICATE_EDGE;
            return Mouse(event.down ? MOUSE_ACTION_BUTTON_DOWN : MOUSE_ACTION_BUTTON_UP, button);
        }
        result = ScrollAxis(MOUSE_AXIS_SCROLL_HORIZONTAL, event.dx);
        if (result == INPUT_SUCCESS) result = ScrollAxis(MOUSE_AXIS_SCROLL_VERTICAL, event.dy);
        return result;
    }

    bool HandleCommand(const Task& task)
    {
        if (task.kind == TaskKind::Release || !task.value) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Reassert at execution as another already-running command may
                // have completed since the caller's immediate disable barrier.
                if (task.kind == TaskKind::Enable) enabled_ = false;
                if (task.kind == TaskKind::Allow) { allowed_ = false; enabled_ = false; }
            }
            const int result = Release();
            if (result != INPUT_SUCCESS) Fail(result, false);
            return result == INPUT_SUCCESS;
        }
        if (task.kind == TaskKind::Allow) {
            const int result = Authorization();
            if (result != INPUT_SUCCESS) { Fail(result); return false; }
            std::lock_guard<std::mutex> lock(mutex_);
            allowed_ = true;
            lastCode_ = INPUT_SUCCESS;
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!allowed_) { lastCode_ = INPUT_PERMISSION_DENIED; return false; }
        }
        if (Enabled()) {
            const int result = CheckSession();
            if (result != INPUT_SUCCESS) Fail(result);
            return result == INPUT_SUCCESS;
        }
        int result = Authorization();
        if (result == INPUT_SUCCESS) result = Release();
        Display display;
        if (result == INPUT_SUCCESS) result = ReadDisplay(display);
        if (result != INPUT_SUCCESS) { Fail(result); return false; }
        display_ = display;
        mouseX_ = 0;
        mouseY_ = 0;
        positionKnown_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!allowed_) return false;
        enabled_ = true;
        lastCode_ = INPUT_SUCCESS;
        return true;
    }

    void Run()
    {
        auto nextCheck = std::chrono::steady_clock::now();
        for (;;) {
            Task task {};
            bool haveTask = false;
            bool check = false;
            int failure = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait_until(lock, nextCheck, [&] { return stopping_ || pendingFailure_ != 0 || !tasks_.empty(); });
                if (stopping_) {
                    lock.unlock();
                    try { Release(); }
                    catch (...) {
                        std::lock_guard<std::mutex> stateLock(mutex_);
                        lastCode_ = ALLOCATION_FAILED;
                        releaseCode_ = ALLOCATION_FAILED;
                    }
                    return;
                }
                failure = pendingFailure_;
                pendingFailure_ = 0;
                if (failure == 0 && !tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                    haveTask = true;
                    if (task.kind == TaskKind::Event) --queuedEvents_;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextCheck) { check = enabled_; nextCheck = now + std::chrono::milliseconds(250); }
                busy_ = failure != 0 || haveTask || check;
            }
            bool answer = false;
            try {
                if (failure != 0) Fail(failure);
                if (check && Enabled()) { const int result = CheckSession(); if (result != INPUT_SUCCESS) Fail(result); }
                if (haveTask) {
                    if (task.kind == TaskKind::Event) {
                        if (Enabled()) {
                            const int result = Process(task.event);
                            if (result != INPUT_SUCCESS) Fail(result);
                            else { std::lock_guard<std::mutex> lock(mutex_); ++processed_; }
                        }
                    } else {
                        answer = HandleCommand(task);
                    }
                }
            } catch (...) {
                // No exception may abandon a waiting UI/network command or kill
                // the worker with possibly held inputs. Keep uncertainty visible.
                try { Fail(ALLOCATION_FAILED); }
                catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    enabled_ = false;
                    lastCode_ = ALLOCATION_FAILED;
                    releaseCode_ = ALLOCATION_FAILED;
                    pendingKeyCount_ = keyOrder_.size();
                    pendingButtonCount_ = buttons_.size();
                    pendingAxisCount_ = axes_.size();
                }
            }
            if (task.answer) task.answer->set_value(answer);
            { std::lock_guard<std::mutex> lock(mutex_); busy_ = false; }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> tasks_;
    bool stopping_ = false;
    bool allowed_ = false;
    bool enabled_ = false;
    bool busy_ = false;
    int pendingFailure_ = 0;
    int authorization_ = UNAUTHORIZED;
    int queryCode_ = -1;
    int lastCode_ = 0;
    int releaseCode_ = 0;
    size_t queuedEvents_ = 0;
    std::set<int32_t> projectedKeys_;
    std::set<int32_t> projectedButtons_;
    size_t pendingKeyCount_ = 0;
    size_t pendingButtonCount_ = 0;
    size_t pendingAxisCount_ = 0;
    uint64_t accepted_ = 0;
    uint64_t processed_ = 0;
    uint64_t coalesced_ = 0;
    uint64_t failures_ = 0;
    // Worker-only input state. Snapshot exposes counts, never key codes, mouse
    // positions, typed content, or an input history.
    Display display_;
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    bool positionKnown_ = false;
    std::set<int32_t> keys_;
    std::vector<int32_t> keyOrder_;
    std::set<int32_t> buttons_;
    std::set<int32_t> axes_;
    std::thread worker_;
};

RemoteInput::RemoteInput() : impl_(std::make_unique<Impl>()) {}
RemoteInput::~RemoteInput() = default;
void RemoteInput::SetAllowed(bool allowed) { impl_->Command(Impl::TaskKind::Allow, allowed); }
bool RemoteInput::EnableSession(bool enabled) { return impl_->Command(Impl::TaskKind::Enable, enabled); }
bool RemoteInput::IsSessionEnabled() const { return impl_->Enabled(); }
bool RemoteInput::Submit(const RemoteInputEvent& event) { return impl_->Submit(event); }
void RemoteInput::ReleaseAll() { impl_->Command(Impl::TaskKind::Release); }
std::string RemoteInput::SnapshotJson() const { return impl_->Snapshot(); }
RemoteInput& GetRemoteInput() { static RemoteInput input; return input; }
