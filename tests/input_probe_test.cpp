#include "input_probe.h"
#include <multimodalinput/oh_input_manager.h>
#include <window_manager/oh_display_manager.h>

#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

struct Input_KeyEvent {
    int32_t code = 0;
    int32_t action = 0;
    int64_t timestamp = 0;
};
struct Input_MouseEvent {
    int32_t displayId = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t action = 0;
    int32_t button = 0;
    int64_t timestamp = 0;
};

namespace {
struct FakeSdk {
    Input_InjectionStatus status = AUTHORIZED;
    Input_Result queryResult = INPUT_SUCCESS;
    Input_Result requestResult = INPUT_SUCCESS;
    bool synchronousCallback = false;
    int cancelCount = 0;
    int liveObjects = 0;
    uint64_t displayId = 7;
    int32_t width = 1920;
    int32_t height = 1080;
    NativeDisplayManager_ErrorCode displayResult = DISPLAY_MANAGER_OK;
    std::deque<int> keyResults;
    std::deque<int> mouseResults;
    std::vector<Input_KeyEvent> keys;
    std::vector<Input_MouseEvent> mouse;
    std::vector<std::string> calls;
} fake;

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

int Next(std::deque<int>& values)
{
    if (values.empty()) return 0;
    int value = values.front();
    values.pop_front();
    return value;
}

void Reset()
{
    fake = FakeSdk {};
    Require(GetInputProbe().CancelAndRelease() == 0, "fixture cleanup failed");
    Require(fake.liveObjects == 0, "native event leaked in cleanup");
    fake = FakeSdk {};
}

void ExpectKey(size_t index, int code, int action)
{
    Require(index < fake.keys.size(), "missing key event " + std::to_string(index));
    Require(fake.keys[index].code == code && fake.keys[index].action == action,
        "incorrect key code/action at index " + std::to_string(index));
    Require(fake.keys[index].timestamp > 0, "key event timestamp was not set");
}

void JsonContains(const std::string& fragment)
{
    auto json = GetInputProbe().QueryJson();
    // Historical states must not satisfy assertions about the live snapshot.
    auto currentSnapshot = json.substr(0, json.find(",\"history\":"));
    Require(currentSnapshot.find(fragment) != std::string::npos,
        "missing current snapshot fragment: " + fragment + " in " + currentSnapshot);
}

std::vector<std::string> HistoryRecords()
{
    const auto json = GetInputProbe().QueryJson();
    const std::string marker = "\"history\":[";
    auto position = json.find(marker);
    Require(position != std::string::npos, "history array missing");
    position += marker.size();
    std::vector<std::string> records;
    while (position < json.size() && json[position] != ']') {
        if (json[position] == ',') ++position;
        Require(position < json.size() && json[position] == '{', "history item is not an object");
        const auto start = position;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        do {
            const char character = json[position++];
            if (inString) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') inString = false;
            } else if (character == '"') inString = true;
            else if (character == '{') ++depth;
            else if (character == '}') --depth;
        } while (position < json.size() && depth > 0);
        Require(depth == 0 && !inString, "unbalanced history object");
        records.push_back(json.substr(start, position - start));
    }
    Require(position < json.size() && json[position] == ']', "history array is unterminated");
    return records;
}

int64_t NumberField(const std::string& object, const std::string& field)
{
    const auto marker = '"' + field + "\":";
    const auto start = object.find(marker);
    Require(start != std::string::npos, "missing numeric field " + field);
    size_t consumed = 0;
    const auto number = std::stoll(object.substr(start + marker.size()), &consumed);
    const auto end = start + marker.size() + consumed;
    Require(end < object.size() && (object[end] == ',' || object[end] == '}'), "invalid numeric field " + field);
    return number;
}

int64_t WallClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

extern "C" {
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback)
{
    fake.calls.push_back("request");
    if (fake.synchronousCallback) callback(fake.status);
    return fake.requestResult;
}
Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* status)
{
    *status = fake.status;
    return fake.queryResult;
}
void OH_Input_CancelInjection()
{
    ++fake.cancelCount;
    fake.calls.push_back("cancel");
    fake.status = UNAUTHORIZED;
}
Input_KeyEvent* OH_Input_CreateKeyEvent() { ++fake.liveObjects; return new Input_KeyEvent {}; }
void OH_Input_DestroyKeyEvent(Input_KeyEvent** event) { delete *event; *event = nullptr; --fake.liveObjects; }
void OH_Input_SetKeyEventKeyCode(Input_KeyEvent* event, int32_t code) { event->code = code; }
void OH_Input_SetKeyEventAction(Input_KeyEvent* event, int32_t action) { event->action = action; }
void OH_Input_SetKeyEventActionTime(Input_KeyEvent* event, int64_t timestamp) { event->timestamp = timestamp; }
int32_t OH_Input_InjectKeyEvent(const Input_KeyEvent* event)
{
    fake.keys.push_back(*event);
    fake.calls.push_back(event->action == KEY_ACTION_DOWN ? "key.down" : "key.up");
    return Next(fake.keyResults);
}
Input_MouseEvent* OH_Input_CreateMouseEvent() { ++fake.liveObjects; return new Input_MouseEvent {}; }
void OH_Input_DestroyMouseEvent(Input_MouseEvent** event) { delete *event; *event = nullptr; --fake.liveObjects; }
void OH_Input_SetMouseEventDisplayId(Input_MouseEvent* event, int32_t id) { event->displayId = id; }
void OH_Input_SetMouseEventDisplayX(Input_MouseEvent* event, int32_t x) { event->x = x; }
void OH_Input_SetMouseEventDisplayY(Input_MouseEvent* event, int32_t y) { event->y = y; }
void OH_Input_SetMouseEventAction(Input_MouseEvent* event, int32_t action) { event->action = action; }
void OH_Input_SetMouseEventButton(Input_MouseEvent* event, int32_t button) { event->button = button; }
void OH_Input_SetMouseEventActionTime(Input_MouseEvent* event, int64_t timestamp) { event->timestamp = timestamp; }
int32_t OH_Input_InjectMouseEvent(const Input_MouseEvent* event)
{
    fake.mouse.push_back(*event);
    fake.calls.push_back(event->action == MOUSE_ACTION_BUTTON_UP ? "mouse.up" : "mouse.other");
    return Next(fake.mouseResults);
}
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayId(uint64_t* id)
{
    *id = fake.displayId;
    return fake.displayResult;
}
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayWidth(int32_t* width)
{
    *width = fake.width;
    return fake.displayResult;
}
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayHeight(int32_t* height)
{
    *height = fake.height;
    return fake.displayResult;
}
}

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests {
        {"unauthorized requests never inject", [] {
            fake.status = UNAUTHORIZED;
            Require(GetInputProbe().InjectA() == 201, "A bypassed authorization");
            Require(GetInputProbe().InjectCtrlL() == 201, "Ctrl+L bypassed authorization");
            Require(GetInputProbe().MoveMouseToCenter() == 201, "move bypassed authorization");
            Require(GetInputProbe().ClickLeft() == 201, "click bypassed authorization");
            Require(fake.keys.empty() && fake.mouse.empty(), "unauthorized event injected");
        }},
        {"authorization query failure blocks injection", [] {
            fake.queryResult = INPUT_SERVICE_EXCEPTION;
            Require(GetInputProbe().InjectA() == 3800001, "query failure not propagated");
            Require(fake.keys.empty(), "query failure allowed key injection");
            JsonContains("\"authorized\":false");
        }},
        {"A has matching down and up", [] {
            Require(GetInputProbe().InjectA() == 0, "A failed");
            Require(fake.keys.size() == 2, "A event count");
            ExpectKey(0, KEYCODE_A, KEY_ACTION_DOWN);
            ExpectKey(1, KEYCODE_A, KEY_ACTION_UP);
            JsonContains("\"pendingKeys\":[]");
        }},
        {"Ctrl+L releases L before Ctrl", [] {
            Require(GetInputProbe().InjectCtrlL() == 0, "Ctrl+L failed");
            Require(fake.keys.size() == 4, "Ctrl+L event count");
            ExpectKey(0, KEYCODE_CTRL_LEFT, KEY_ACTION_DOWN);
            ExpectKey(1, KEYCODE_L, KEY_ACTION_DOWN);
            ExpectKey(2, KEYCODE_L, KEY_ACTION_UP);
            ExpectKey(3, KEYCODE_CTRL_LEFT, KEY_ACTION_UP);
        }},
        {"failed A down still attempts up", [] {
            fake.keyResults = {201, 0};
            Require(GetInputProbe().InjectA() == 201, "down error was hidden");
            Require(fake.keys.size() == 2, "up was not attempted after failed down");
            ExpectKey(1, KEYCODE_A, KEY_ACTION_UP);
            JsonContains("\"name\":\"a.down\",\"code\":201");
        }},
        {"failed Ctrl down releases Ctrl and never presses L", [] {
            fake.keyResults = {201, 0};
            Require(GetInputProbe().InjectCtrlL() == 201, "Ctrl error hidden");
            Require(fake.keys.size() == 2, "invalid L sequence after failed Ctrl");
            ExpectKey(0, KEYCODE_CTRL_LEFT, KEY_ACTION_DOWN);
            ExpectKey(1, KEYCODE_CTRL_LEFT, KEY_ACTION_UP);
        }},
        {"failed L down releases both keys", [] {
            fake.keyResults = {0, 3800001, 0, 0};
            Require(GetInputProbe().InjectCtrlL() == 3800001, "L error hidden");
            Require(fake.keys.size() == 4, "missing chord cleanup");
            ExpectKey(2, KEYCODE_L, KEY_ACTION_UP);
            ExpectKey(3, KEYCODE_CTRL_LEFT, KEY_ACTION_UP);
        }},
        {"failed up is retained and released before next down", [] {
            fake.keyResults = {0, 3800001};
            Require(GetInputProbe().InjectA() == 3800001, "up error hidden");
            JsonContains("\"pendingKeys\":[2017]");
            Require(GetInputProbe().InjectA() == 0, "cleanup retry failed");
            Require(fake.keys.size() == 5, "cleanup must precede new pair");
            ExpectKey(2, KEYCODE_A, KEY_ACTION_UP);
            ExpectKey(3, KEYCODE_A, KEY_ACTION_DOWN);
            ExpectKey(4, KEYCODE_A, KEY_ACTION_UP);
            JsonContains("\"pendingKeys\":[]");
        }},
        {"failed cleanup blocks a new chord", [] {
            fake.keyResults = {0, 3800001, 201};
            Require(GetInputProbe().InjectA() == 3800001, "initial up error hidden");
            Require(GetInputProbe().InjectCtrlL() == 201, "failed cleanup not returned");
            Require(fake.keys.size() == 3, "new chord began with unreleased key");
            ExpectKey(2, KEYCODE_A, KEY_ACTION_UP);
            JsonContains("\"pendingKeys\":[2017]");
        }},
        {"display service error blocks mouse injection", [] {
            fake.displayResult = DISPLAY_MANAGER_ERROR_SYSTEM_ABNORMAL;
            Require(GetInputProbe().MoveMouseToCenter() == 1400003, "display error hidden");
            Require(fake.mouse.empty(), "mouse injected with invalid display");
        }},
        {"display ID overflow blocks narrowing and injection", [] {
            fake.displayId = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1;
            Require(GetInputProbe().ClickLeft() == InputProbe::INVALID_DISPLAY, "display ID narrowed unsafely");
            Require(fake.mouse.empty(), "mouse injected with overflowing display ID");
        }},
        {"center uses actual display ID and dimensions", [] {
            fake.displayId = 42;
            fake.width = 2560;
            fake.height = 1600;
            Require(GetInputProbe().MoveMouseToCenter() == 0, "center move failed");
            Require(fake.mouse.size() == 1, "mouse move count");
            const auto& event = fake.mouse.front();
            Require(event.displayId == 42 && event.x == 1280 && event.y == 800, "hardcoded display coordinates");
            Require(event.action == MOUSE_ACTION_MOVE && event.timestamp > 0, "move action/time invalid");
        }},
        {"failed mouse down still releases left button", [] {
            fake.mouseResults = {0, 201, 0};
            Require(GetInputProbe().ClickLeft() == 201, "mouse down error hidden");
            Require(fake.mouse.size() == 3, "click cleanup event missing");
            Require(fake.mouse[2].action == MOUSE_ACTION_BUTTON_UP, "left up missing");
            JsonContains("\"pendingMouseLeft\":false");
        }},
        {"cancel releases pending keys before revoking", [] {
            fake.keyResults = {0, 0, 3800001, 201};
            Require(GetInputProbe().InjectCtrlL() == 3800001, "first error not retained");
            JsonContains("\"name\":\"ctrl.up\",\"code\":201");
            JsonContains("\"pendingKeys\":[2028,2072]");
            Require(GetInputProbe().CancelAndRelease() == 0, "cancel cleanup failed");
            Require(fake.keys.size() == 6, "cancel omitted a release");
            ExpectKey(4, KEYCODE_L, KEY_ACTION_UP);
            ExpectKey(5, KEYCODE_CTRL_LEFT, KEY_ACTION_UP);
            Require(fake.calls.back() == "cancel" && fake.cancelCount == 1, "authorization revoked before release");
            JsonContains("\"pendingKeys\":[]");
            JsonContains("\"authorized\":false");
        }},
        {"cancel releases pending mouse before revoking", [] {
            fake.mouseResults = {0, 0, 3800001};
            Require(GetInputProbe().ClickLeft() == 3800001, "mouse up error hidden");
            JsonContains("\"pendingMouseLeft\":true");
            Require(GetInputProbe().CancelAndRelease() == 0, "mouse cleanup failed");
            Require(fake.mouse.size() == 4 && fake.mouse.back().action == MOUSE_ACTION_BUTTON_UP,
                "pending mouse was not released");
            Require(fake.calls.back() == "cancel", "cancel ordering invalid");
            JsonContains("\"pendingMouseLeft\":false");
        }},
        {"request accepted is not authorization granted", [] {
            fake.status = AUTHORIZING;
            Require(GetInputProbe().RequestAuthorization() == 0, "request failed");
            JsonContains("\"authorized\":false");
            JsonContains("\"dialogStatusName\":\"AUTHORIZING\"");
        }},
        {"synchronous authorization callback does not deadlock", [] {
            fake.synchronousCallback = true;
            Require(GetInputProbe().RequestAuthorization() == 0, "request callback failed");
            JsonContains("\"callbackStatus\":2");
            JsonContains("\"authorized\":true");
        }},
        {"history preserves failed operation after successful cleanup", [] {
            fake.keyResults = {0, 3800001};
            Require(GetInputProbe().InjectA() == 3800001, "initial up failure missing");
            const auto failed = HistoryRecords().back();
            Require(failed.find("\"operation\":\"injectA\"") != std::string::npos, "wrong history operation");
            Require(NumberField(failed, "code") == 3800001, "failure code missing from history");
            Require(failed.find("\"pendingKeys\":[2017]") != std::string::npos, "failed release state missing");
            Require(failed.find("\"name\":\"a.up\",\"code\":3800001") != std::string::npos,
                "failed step missing from history");
            Require(GetInputProbe().InjectCtrlL() == 0, "next action failed");
            const auto records = HistoryRecords();
            Require(records.size() >= 2 && records[records.size() - 2] == failed,
                "completed record changed after cleanup or next action");
            Require(records.back().find("\"pendingKeys\":[]") != std::string::npos,
                "successful operation did not snapshot cleared pending state");
            JsonContains("\"lastOperation\":\"injectCtrlL\"");
            JsonContains("\"lastCode\":0");
        }},
        {"history freezes pending mouse state before cancellation", [] {
            fake.mouseResults = {0, 0, 3800001};
            Require(GetInputProbe().ClickLeft() == 3800001, "mouse up error missing");
            const auto failed = HistoryRecords().back();
            Require(failed.find("\"pendingMouseLeft\":true") != std::string::npos, "pending mouse snapshot missing");
            Require(GetInputProbe().CancelAndRelease() == 0, "mouse cleanup failed");
            const auto records = HistoryRecords();
            Require(records[records.size() - 2] == failed, "cancel mutated previous mouse state");
            Require(records.back().find("\"operation\":\"cancelAndRelease\"") != std::string::npos,
                "cancel operation not recorded");
            Require(records.back().find("\"pendingMouseLeft\":false") != std::string::npos,
                "cancel did not capture cleared mouse state");
        }},
        {"history timestamps use Unix milliseconds around the operation", [] {
            const auto before = WallClockMs();
            Require(GetInputProbe().InjectA() == 0, "timed action failed");
            const auto after = WallClockMs();
            const auto record = HistoryRecords().back();
            const auto started = NumberField(record, "startedUnixMs");
            const auto finished = NumberField(record, "finishedUnixMs");
            Require(started >= before && finished >= started && finished <= after,
                "history timestamps are not wall-clock Unix milliseconds for this operation");
        }},
        {"history keeps only the latest 32 completed operations in order", [] {
            const std::string operationNames[] = {"injectA", "injectCtrlL", "moveMouseToCenter"};
            for (int i = 0; i < 40; ++i) {
                const int result = i % 3 == 0 ? GetInputProbe().InjectA() :
                    i % 3 == 1 ? GetInputProbe().InjectCtrlL() : GetInputProbe().MoveMouseToCenter();
                Require(result == 0, "bounded history action failed");
            }
            const auto records = HistoryRecords();
            Require(records.size() == 32, "history limit is not 32");
            for (size_t index = 0; index < records.size(); ++index) {
                const auto expected = "\"operation\":\"" + operationNames[(index + 8) % 3] + '"';
                Require(records[index].find(expected) != std::string::npos, "oldest history eviction or ordering is wrong");
            }
            JsonContains("\"historyLimit\":32");
        }},
        {"polls and authorization callbacks do not append completed operations", [] {
            const auto before = HistoryRecords();
            for (int i = 0; i < 4; ++i) GetInputProbe().QueryJson();
            Require(HistoryRecords() == before, "status poll added or mutated operation history");
            fake.synchronousCallback = true;
            Require(GetInputProbe().RequestAuthorization() == 0, "callback request failed");
            const auto after = HistoryRecords();
            const size_t retained = before.size() < 32 ? before.size() : 31;
            Require(after.size() == retained + 1, "request callback created additional history records");
            for (size_t i = 0; i < retained; ++i) {
                Require(after[i] == before[before.size() - retained + i], "callback overwrote prior operation history");
            }
            Require(after.back().find("\"operation\":\"requestAuthorization\"") != std::string::npos,
                "request operation missing from history");
        }}
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            Reset();
            test.second();
            Require(fake.liveObjects == 0, "native event object leaked");
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    Reset();
    std::cout << tests.size() - failures << '/' << tests.size() << " input probe logic tests passed\n";
    std::cout << "SDK substitutes validate local sequencing only; no device input or authorization was performed.\n";
    return failures == 0 ? 0 : 1;
}
