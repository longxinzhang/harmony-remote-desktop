#include "remote_input.h"
#include <multimodalinput/oh_input_manager.h>
#include <window_manager/oh_display_manager.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

struct Input_KeyEvent { int32_t code = -1, action = -1; int64_t time = -1; };
struct Input_MouseEvent {
    int32_t display = -1, x = -1, y = -1, action = -1, button = -99, axis = -1;
    float value = 0;
    int64_t time = -1;
};

namespace {
using namespace std::chrono_literals;
struct Call { bool key; Input_KeyEvent k; Input_MouseEvent m; int result; };
std::mutex sdkMutex, gateMutex;
std::condition_variable gateCV;
std::vector<Call> calls;
std::set<std::thread::id> sdkThreads;
std::deque<int> keyResults, mouseResults;
std::atomic<int> auth {AUTHORIZED}, queryResult {0}, displayResult {0}, width {1920}, height {1080};
std::atomic<uint64_t> displayId {17};
std::atomic<int> rotation {DISPLAY_MANAGER_ROTATION_0};
std::atomic<int> upFailure {0}, objects {0}, allocationFailures {0};
std::atomic<bool> blockNext {false};
std::atomic<bool> throwNextQuery {false}, throwNextKey {false};
bool blocked = false, hold = false;
uint64_t positionEvents = 0;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string("check failed at line ") + std::to_string(__LINE__) + ": " #condition); } while (false)

void SDKThread()
{
    std::lock_guard<std::mutex> lock(sdkMutex);
    sdkThreads.insert(std::this_thread::get_id());
}
void Wait(const std::function<bool()>& condition)
{
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("test wait deadline exceeded");
        std::this_thread::sleep_for(1ms);
    }
}
uint64_t Number(const std::string& json, const std::string& key)
{
    auto start = json.find('"' + key + "\":");
    CHECK(start != std::string::npos);
    return std::stoull(json.substr(start + key.size() + 3));
}
bool True(const std::string& json, const std::string& key) { return json.find('"' + key + "\":true") != std::string::npos; }
std::vector<Call> Calls() { std::lock_guard<std::mutex> lock(sdkMutex); return calls; }
void Reset()
{
    CHECK(objects == 0);
    std::lock_guard<std::mutex> lock(sdkMutex);
    calls.clear(); sdkThreads.clear(); keyResults.clear(); mouseResults.clear();
    auth = AUTHORIZED; queryResult = 0; displayResult = 0; width = 1920; height = 1080; displayId = 17;
    rotation = DISPLAY_MANAGER_ROTATION_0;
    upFailure = 0; allocationFailures = 0; blockNext = false; positionEvents = 0;
    throwNextQuery = false; throwNextKey = false;
}
void Ready(RemoteInput& input) { input.SetAllowed(true); CHECK(input.EnableSession(true)); }
void Done(RemoteInput& input, uint64_t count)
{
    Wait([&] { auto s = input.SnapshotJson(); return Number(s, "processedEvents") >= count + positionEvents && !True(s, "busy"); });
}
void Failed(RemoteInput& input)
{
    Wait([&] { auto s = input.SnapshotJson(); return Number(s, "failures") > 0 && !True(s, "busy"); });
    CHECK(!input.IsSessionEnabled());
}
void BlockQuery()
{
    std::lock_guard<std::mutex> lock(gateMutex);
    blocked = false; hold = true; blockNext = true;
}
void AwaitBlocked()
{
    std::unique_lock<std::mutex> lock(gateMutex);
    CHECK(gateCV.wait_for(lock, 3s, [] { return blocked; }));
}
void Unblock()
{
    { std::lock_guard<std::mutex> lock(gateMutex); hold = false; }
    gateCV.notify_all();
}
RemoteInputEvent Key(const std::string& code, bool down)
{
    RemoteInputEvent event; event.kind = RemoteInputEvent::Kind::Key; event.code = code; event.down = down; return event;
}
RemoteInputEvent Mouse(RemoteInputEvent::Kind kind, double x = 0, double y = 0, const std::string& button = {}, bool down = false)
{
    RemoteInputEvent event; event.kind = kind; event.x = x; event.y = y; event.button = button; event.down = down; return event;
}
void Position(RemoteInput& input, double x, double y)
{
    const auto previous = Number(input.SnapshotJson(), "processedEvents");
    CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, x, y)));
    Wait([&] { auto s = input.SnapshotJson(); return Number(s, "processedEvents") > previous && !True(s, "busy"); });
    ++positionEvents;
    std::lock_guard<std::mutex> lock(sdkMutex); calls.clear();
}
RemoteInputEvent Scroll(double dx, double dy)
{
    auto event = Mouse(RemoteInputEvent::Kind::Scroll, 0.5, 1); event.dx = dx; event.dy = dy; return event;
}
}

extern "C" {
Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* status)
{
    SDKThread();
    if (throwNextQuery.exchange(false)) throw std::bad_alloc();
    if (blockNext.exchange(false)) {
        std::unique_lock<std::mutex> lock(gateMutex);
        blocked = true; gateCV.notify_all(); gateCV.wait(lock, [] { return !hold; });
    }
    *status = static_cast<Input_InjectionStatus>(auth.load());
    return static_cast<Input_Result>(queryResult.load());
}
Input_KeyEvent* OH_Input_CreateKeyEvent()
{
    SDKThread(); if (allocationFailures.exchange(0) != 0) return nullptr;
    ++objects; return new Input_KeyEvent;
}
void OH_Input_DestroyKeyEvent(Input_KeyEvent** event) { SDKThread(); delete *event; *event = nullptr; --objects; }
void OH_Input_SetKeyEventKeyCode(Input_KeyEvent* event, int32_t code) { SDKThread(); event->code = code; }
void OH_Input_SetKeyEventAction(Input_KeyEvent* event, int32_t action) { SDKThread(); event->action = action; }
void OH_Input_SetKeyEventActionTime(Input_KeyEvent* event, int64_t time) { SDKThread(); event->time = time; }
int32_t OH_Input_InjectKeyEvent(const Input_KeyEvent* event)
{
    SDKThread(); std::lock_guard<std::mutex> lock(sdkMutex);
    if (throwNextKey.exchange(false)) throw std::bad_alloc();
    int result = event->action == KEY_ACTION_UP ? upFailure.load() : 0;
    if (!keyResults.empty()) { result = keyResults.front(); keyResults.pop_front(); }
    calls.push_back({true, *event, {}, result}); return result;
}
Input_MouseEvent* OH_Input_CreateMouseEvent()
{
    SDKThread(); if (allocationFailures.exchange(0) != 0) return nullptr;
    ++objects; return new Input_MouseEvent;
}
void OH_Input_DestroyMouseEvent(Input_MouseEvent** event) { SDKThread(); delete *event; *event = nullptr; --objects; }
void OH_Input_SetMouseEventDisplayId(Input_MouseEvent* event, int32_t display) { SDKThread(); event->display = display; }
void OH_Input_SetMouseEventDisplayX(Input_MouseEvent* event, int32_t x) { SDKThread(); event->x = x; }
void OH_Input_SetMouseEventDisplayY(Input_MouseEvent* event, int32_t y) { SDKThread(); event->y = y; }
void OH_Input_SetMouseEventAction(Input_MouseEvent* event, int32_t action) { SDKThread(); event->action = action; }
void OH_Input_SetMouseEventButton(Input_MouseEvent* event, int32_t button) { SDKThread(); event->button = button; }
void OH_Input_SetMouseEventActionTime(Input_MouseEvent* event, int64_t time) { SDKThread(); event->time = time; }
void OH_Input_SetMouseEventAxisType(Input_MouseEvent* event, int32_t axis) { SDKThread(); event->axis = axis; }
void OH_Input_SetMouseEventAxisValue(Input_MouseEvent* event, float value) { SDKThread(); event->value = value; }
int32_t OH_Input_InjectMouseEvent(const Input_MouseEvent* event)
{
    SDKThread(); std::lock_guard<std::mutex> lock(sdkMutex);
    int result = (event->action == MOUSE_ACTION_BUTTON_UP || event->action == MOUSE_ACTION_AXIS_END) ? upFailure.load() : 0;
    if (!mouseResults.empty()) { result = mouseResults.front(); mouseResults.pop_front(); }
    calls.push_back({false, {}, *event, result}); return result;
}
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayId(uint64_t* id)
{ SDKThread(); *id = displayId; return static_cast<NativeDisplayManager_ErrorCode>(displayResult.load()); }
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayWidth(int32_t* value)
{ SDKThread(); *value = width; return static_cast<NativeDisplayManager_ErrorCode>(displayResult.load()); }
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayHeight(int32_t* value)
{ SDKThread(); *value = height; return static_cast<NativeDisplayManager_ErrorCode>(displayResult.load()); }
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayRotation(NativeDisplayManager_Rotation* value)
{ SDKThread(); *value = static_cast<NativeDisplayManager_Rotation>(rotation.load()); return static_cast<NativeDisplayManager_ErrorCode>(displayResult.load()); }
}

int main()
{
    try {
        int count = 0;
        auto test = [&](const char* name, const std::function<void()>& body) {
            Reset(); body(); CHECK(objects == 0);
            { std::lock_guard<std::mutex> lock(sdkMutex); CHECK(sdkThreads.size() <= 1); CHECK(sdkThreads.count(std::this_thread::get_id()) == 0); }
            ++count; std::cout << "PASS " << name << '\n';
        };
        test("default off and explicit official authorization", [] {
            RemoteInput input;
            CHECK(!input.IsSessionEnabled()); CHECK(!input.EnableSession(true)); CHECK(!input.Submit(Key("KEY_A", true)));
            auth = UNAUTHORIZED; input.SetAllowed(true); CHECK(!input.EnableSession(true)); CHECK(Calls().empty());
            CHECK(!True(input.SnapshotJson(), "allowed"));
            auth = AUTHORIZED; Ready(input); CHECK(True(input.SnapshotJson(), "authorized"));
        });
        test("query error blocks consent", [] {
            RemoteInput input; queryResult = INPUT_SERVICE_EXCEPTION; input.SetAllowed(true);
            CHECK(!input.EnableSession(true)); CHECK(!input.Submit(Key("KEY_A", true))); CHECK(Calls().empty());
            CHECK(Number(input.SnapshotJson(), "errorCode") == INPUT_PERMISSION_DENIED);
        });
        test("normalized display edges center and button positions", [] {
            RemoteInput input; Ready(input);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0, 0))); Done(input, 1);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 1, 1))); Done(input, 2);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.5, 0.5))); Done(input, 3);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "right", true))); Done(input, 4);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "right", false))); Done(input, 5);
            auto c = Calls(); CHECK(c.size() == 5);
            CHECK(c[0].m.x == 0 && c[0].m.y == 0 && c[1].m.x == 1919 && c[1].m.y == 1079);
            CHECK(c[3].m.x == 960 && c[3].m.y == 540 && c[3].m.button == MOUSE_BUTTON_RIGHT);
            for (const auto& call : c) CHECK(call.m.display == 17 && call.m.time > 0);
        });
        for (const auto& button : std::vector<std::pair<std::string, int>> {
            {"left", MOUSE_BUTTON_LEFT}, {"middle", MOUSE_BUTTON_MIDDLE}, {"right", MOUSE_BUTTON_RIGHT}}) {
            test((button.first + " drag preserves button through move then returns to hover").c_str(), [&] {
                RemoteInput input; Ready(input); Position(input, 0.1, 0.2);
                CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, button.first, true))); Done(input, 1);
                CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.2, 0.3))); Done(input, 2);
                CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.8, 0.6))); Done(input, 3);
                CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, button.first, false))); Done(input, 4);
                CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.9, 0.7))); Done(input, 5);
                const auto c = Calls(); CHECK(c.size() == 5);
                CHECK(c[0].m.action == MOUSE_ACTION_BUTTON_DOWN && c[0].m.button == button.second);
                for (int i : {1, 2}) CHECK(c[i].m.action == MOUSE_ACTION_MOVE && c[i].m.button == button.second);
                CHECK(c[1].m.x == 384 && c[1].m.y == 324 && c[2].m.x == 1535 && c[2].m.y == 647);
                CHECK(c[3].m.action == MOUSE_ACTION_BUTTON_UP && c[3].m.button == button.second && c[3].m.x == 1535 && c[3].m.y == 647);
                CHECK(c[4].m.action == MOUSE_ACTION_MOVE && c[4].m.button == MOUSE_BUTTON_NONE);
                CHECK(Number(input.SnapshotJson(), "pendingButtonsCount") == 0 && input.IsSessionEnabled());
            });
        }
        test("failed drag move releases held button at latest position", [] {
            RemoteInput input; Ready(input); Position(input, 0.1, 0.2);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", true))); Done(input, 1);
            { std::lock_guard<std::mutex> lock(sdkMutex); mouseResults = {INPUT_SERVICE_EXCEPTION, 0}; }
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.8, 0.6))); Failed(input);
            const auto c = Calls(); CHECK(c.size() == 3);
            CHECK(c[1].m.action == MOUSE_ACTION_MOVE && c[1].m.button == MOUSE_BUTTON_LEFT);
            CHECK(c[2].m.action == MOUSE_ACTION_BUTTON_UP && c[2].m.button == MOUSE_BUTTON_LEFT && c[2].m.x == 1535 && c[2].m.y == 647);
            CHECK(Number(input.SnapshotJson(), "pendingButtonsCount") == 0);
        });
        test("all key mappings and paired edges", [] {
            RemoteInput input; Ready(input);
            std::vector<std::pair<std::string, int>> mapping;
            for (int i = 0; i < 26; ++i) mapping.push_back({std::string("KEY_") + char('A' + i), 2017 + i});
            for (int i = 0; i < 10; ++i) mapping.push_back({std::string("KEY_") + char('0' + i), 2000 + i});
            for (int i = 0; i < 12; ++i) mapping.push_back({"KEY_F" + std::to_string(i + 1), 2090 + i});
            const std::pair<std::string, int> other[] = {
                {"KEY_COMMA",2043},{"KEY_PERIOD",2044},{"KEY_ALT_LEFT",2045},{"KEY_ALT_RIGHT",2046},
                {"KEY_SHIFT_LEFT",2047},{"KEY_SHIFT_RIGHT",2048},{"KEY_TAB",2049},{"KEY_SPACE",2050},
                {"KEY_ENTER",2054},{"KEY_BACKSPACE",2055},{"KEY_GRAVE",2056},{"KEY_MINUS",2057},
                {"KEY_EQUALS",2058},{"KEY_LEFT_BRACKET",2059},{"KEY_RIGHT_BRACKET",2060},{"KEY_BACKSLASH",2061},
                {"KEY_SEMICOLON",2062},{"KEY_APOSTROPHE",2063},{"KEY_SLASH",2064},{"KEY_PAGE_UP",2068},
                {"KEY_PAGE_DOWN",2069},{"KEY_ESCAPE",2070},{"KEY_DELETE",2071},{"KEY_CTRL_LEFT",2072},
                {"KEY_CTRL_RIGHT",2073},{"KEY_CAPS_LOCK",2074},{"KEY_META_LEFT",2076},{"KEY_META_RIGHT",2077},
                {"KEY_HOME",2081},{"KEY_END",2082},{"KEY_UP",2012},{"KEY_DOWN",2013},{"KEY_LEFT",2014},{"KEY_RIGHT",2015}
            };
            mapping.insert(mapping.end(), std::begin(other), std::end(other));
            uint64_t processed = 0;
            for (const auto& entry : mapping) {
                CHECK(input.Submit(Key(entry.first, true))); CHECK(input.Submit(Key(entry.first, false)));
                processed += 2; Done(input, processed);
            }
            auto c = Calls(); CHECK(c.size() == mapping.size() * 2);
            for (size_t i = 0; i < mapping.size(); ++i) {
                CHECK(c[i*2].k.code == mapping[i].second && c[i*2+1].k.code == mapping[i].second);
                CHECK(c[i*2].k.action == KEY_ACTION_DOWN && c[i*2+1].k.action == KEY_ACTION_UP && c[i*2].k.time > 0);
            }
        });
        test("scroll axes use begin update end with SDK units", [] {
            RemoteInput input; Ready(input); Position(input, 0.5, 1); CHECK(input.Submit(Scroll(-2.5, 3.25))); Done(input, 1);
            auto c = Calls(); CHECK(c.size() == 6);
            for (int i = 0; i < 6; ++i) {
                CHECK(c[i].m.action == MOUSE_ACTION_AXIS_BEGIN + i % 3);
                CHECK(c[i].m.axis == (i < 3 ? MOUSE_AXIS_SCROLL_HORIZONTAL : MOUSE_AXIS_SCROLL_VERTICAL));
                CHECK(c[i].m.button == MOUSE_BUTTON_NONE && c[i].m.display == 17 && c[i].m.x == 960 && c[i].m.y == 1079);
                CHECK(c[i].m.value == (i == 1 ? -2.5f : i == 4 ? 3.25f : 0));
            }
            CHECK(Number(input.SnapshotJson(), "pendingAxesCount") == 0);
        });
        test("wire buttons and scroll retain last nonzero move position", [] {
            RemoteInput input; Ready(input); Position(input, 0.7, 0.3);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", true)));
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", false)));
            auto scroll = Scroll(0, 2); scroll.x = 0; scroll.y = 0;
            CHECK(input.Submit(scroll)); Done(input, 3);
            auto c = Calls(); CHECK(c.size() == 5);
            for (const auto& call : c) CHECK(call.m.x == 1343 && call.m.y == 324 && call.m.display == 17);
        });
        test("buttons and scroll require a move after each new session", [] {
            RemoteInput input; Ready(input);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", true))); Failed(input);
            CHECK(Calls().empty());
            CHECK(input.EnableSession(true)); Position(input, 1, 1);
            CHECK(input.EnableSession(false)); CHECK(input.EnableSession(true));
            const auto previous = Number(input.SnapshotJson(), "failures");
            CHECK(input.Submit(Scroll(0, 1)));
            Wait([&] { auto s = input.SnapshotJson(); return Number(s, "failures") > previous && !True(s, "busy"); });
            CHECK(!input.IsSessionEnabled() && Calls().empty());
        });
        test("failed down still releases and disables", [] {
            RemoteInput input; Ready(input);
            { std::lock_guard<std::mutex> lock(sdkMutex); keyResults = {INPUT_PERMISSION_DENIED, 0}; }
            CHECK(input.Submit(Key("KEY_CTRL_LEFT", true))); Failed(input);
            auto c = Calls(); CHECK(c.size() == 2 && c[0].k.action == KEY_ACTION_DOWN && c[1].k.action == KEY_ACTION_UP);
            CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 0 && Number(input.SnapshotJson(), "errorCode") == INPUT_PERMISSION_DENIED);
        });
        test("failed up remains pending then explicit retry releases", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_A", true))); Done(input, 1);
            upFailure = INPUT_PERMISSION_DENIED; CHECK(input.Submit(Key("KEY_A", false))); Failed(input);
            CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 1);
            CHECK(Number(input.SnapshotJson(), "lastReleaseCode") == INPUT_PERMISSION_DENIED);
            CHECK(!input.EnableSession(true)); CHECK(!input.Submit(Key("KEY_0", true)));
            upFailure = 0; input.ReleaseAll(); CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 0);
            CHECK(!input.IsSessionEnabled()); CHECK(input.EnableSession(true));
        });
        test("failed mouse down still attempts button up", [] {
            RemoteInput input; Ready(input); Position(input, 0.7, 0.3);
            { std::lock_guard<std::mutex> lock(sdkMutex); mouseResults = {INPUT_SERVICE_EXCEPTION, 0}; }
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", true))); Failed(input);
            auto c = Calls(); CHECK(c.size() == 2 && c[0].m.action == MOUSE_ACTION_BUTTON_DOWN && c[1].m.action == MOUSE_ACTION_BUTTON_UP);
            CHECK(c[1].m.x == 1343 && c[1].m.y == 324 && Number(input.SnapshotJson(), "pendingButtonsCount") == 0);
        });
        test("failed focus release retains mouse button and disables", [] {
            RemoteInput input; Ready(input); Position(input, 0.7, 0.3);
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "middle", true))); Done(input, 1);
            upFailure = INPUT_PERMISSION_DENIED; input.ReleaseAll(); CHECK(!input.IsSessionEnabled());
            CHECK(Number(input.SnapshotJson(), "pendingButtonsCount") == 1 && Number(input.SnapshotJson(), "lastReleaseCode") == INPUT_PERMISSION_DENIED);
            CHECK(Calls().size() == 2); // One failed up is not hidden by an immediate second retry.
            upFailure = 0; input.ReleaseAll(); CHECK(Number(input.SnapshotJson(), "pendingButtonsCount") == 0);
            CHECK(Calls().back().m.button == MOUSE_BUTTON_MIDDLE);
        });
        test("duplicate key down rejects without injecting repeat", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_A", true))); Done(input, 1);
            CHECK(!input.Submit(Key("KEY_A", true))); Failed(input);
            auto c = Calls(); CHECK(c.size() == 2 && c[1].k.action == KEY_ACTION_UP);
            CHECK(Number(input.SnapshotJson(), "errorCode") == 9100007);
        });
        test("focus release preserves session and releases reverse key order", [] {
            RemoteInput input; Ready(input); Position(input, 0.2, 0.3);
            CHECK(input.Submit(Key("KEY_CTRL_LEFT", true))); CHECK(input.Submit(Key("KEY_A", true)));
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0.2, 0.3, "middle", true))); Done(input, 3);
            input.ReleaseAll(); CHECK(input.IsSessionEnabled());
            auto c = Calls(); CHECK(c.size() == 6 && c[3].m.action == MOUSE_ACTION_BUTTON_UP);
            CHECK(c[4].k.code == KEYCODE_A && c[5].k.code == KEYCODE_CTRL_LEFT && c[4].k.action == KEY_ACTION_UP);
            CHECK(input.Submit(Key("KEY_B", true))); CHECK(input.Submit(Key("KEY_B", false))); Done(input, 5);
        });
        test("disable and host revoke release held state", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_SHIFT_LEFT", true))); Done(input, 1);
            CHECK(input.EnableSession(false)); CHECK(!input.IsSessionEnabled()); CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 0);
            CHECK(input.EnableSession(true)); Position(input, 1, 0); CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", true))); Done(input, 2);
            input.SetAllowed(false); CHECK(!input.IsSessionEnabled()); CHECK(!True(input.SnapshotJson(), "allowed"));
            CHECK(Number(input.SnapshotJson(), "pendingButtonsCount") == 0);
        });
        test("periodic authorization revocation releases without new input", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_ALT_LEFT", true))); Done(input, 1);
            auth = UNAUTHORIZED; Failed(input); CHECK(Calls().back().k.action == KEY_ACTION_UP);
            CHECK(!True(input.SnapshotJson(), "allowed"));
        });
        test("revoked authorization still attempts denied releases", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_META_LEFT", true))); Done(input, 1);
            upFailure = INPUT_PERMISSION_DENIED; auth = UNAUTHORIZED; Failed(input);
            CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 1 && Calls().back().k.action == KEY_ACTION_UP);
            upFailure = 0; input.ReleaseAll(); CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 0);
        });
        test("geometry change fails closed and releases using original display", [] {
            RemoteInput input; Ready(input); Position(input, 1, 1); CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 0, 0, "left", true))); Done(input, 1);
            width = 1280; displayId = 99; Failed(input);
            auto c = Calls(); CHECK(c.size() == 2 && c[1].m.display == 17 && c[1].m.x == 1919 && c[1].m.y == 1079);
            CHECK(Number(input.SnapshotJson(), "errorCode") == 9100006);
        });
        test("display read errors and invalid geometry block injection", [] {
            RemoteInput input; input.SetAllowed(true); displayResult = DISPLAY_MANAGER_ERROR_SYSTEM_ABNORMAL;
            CHECK(!input.EnableSession(true)); CHECK(Calls().empty());
            displayResult = 0; displayId = uint64_t(std::numeric_limits<int32_t>::max()) + 1; CHECK(!input.EnableSession(true));
            displayId = 17; width = 0; CHECK(!input.EnableSession(true)); CHECK(Calls().empty());
        });
        test("same-size display rotation change revokes session", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_SHIFT_LEFT", true))); Done(input, 1);
            rotation = DISPLAY_MANAGER_ROTATION_180; Failed(input);
            CHECK(Number(input.SnapshotJson(), "errorCode") == 9100006 && Calls().back().k.action == KEY_ACTION_UP);
        });
        test("scroll update failure sends end and suppresses second axis", [] {
            RemoteInput input; Ready(input); Position(input, 0.5, 1);
            { std::lock_guard<std::mutex> lock(sdkMutex); mouseResults = {0, INPUT_SERVICE_EXCEPTION, 0}; }
            CHECK(input.Submit(Scroll(1, 2))); Failed(input); auto c = Calls(); CHECK(c.size() == 3);
            CHECK(c[2].m.action == MOUSE_ACTION_AXIS_END && c[2].m.axis == MOUSE_AXIS_SCROLL_HORIZONTAL);
            CHECK(Number(input.SnapshotJson(), "pendingAxesCount") == 0);
        });
        test("scroll end failure stays pending until release succeeds", [] {
            RemoteInput input; Ready(input); Position(input, 0.5, 1); upFailure = INPUT_SERVICE_EXCEPTION;
            CHECK(input.Submit(Scroll(0, -1))); Failed(input);
            CHECK(Number(input.SnapshotJson(), "pendingAxesCount") == 1);
            upFailure = 0; input.ReleaseAll(); CHECK(Number(input.SnapshotJson(), "pendingAxesCount") == 0);
        });
        test("allocation failure fails closed with no phantom held key", [] {
            RemoteInput input; Ready(input); allocationFailures = 1;
            CHECK(input.Submit(Key("KEY_A", true))); Failed(input);
            CHECK(Calls().empty() && Number(input.SnapshotJson(), "pendingKeysCount") == 0);
        });
        test("worker exception completes command and releases uncertain down", [] {
            RemoteInput input; throwNextQuery = true; input.SetAllowed(true);
            CHECK(Number(input.SnapshotJson(), "errorCode") == 9100001 && !input.IsSessionEnabled());
            Ready(input); throwNextKey = true; CHECK(input.Submit(Key("KEY_A", true)));
            Wait([&] { auto s = input.SnapshotJson(); return Number(s, "failures") == 2 && !True(s, "busy"); });
            auto c = Calls(); CHECK(c.size() == 1 && c[0].k.action == KEY_ACTION_UP);
            CHECK(objects == 0 && Number(input.SnapshotJson(), "pendingKeysCount") == 0 && !input.IsSessionEnabled());
        });
        test("invalid coordinates scroll and unknown keys reject", [] {
            const auto badMove = Mouse(RemoteInputEvent::Kind::Move, std::numeric_limits<double>::quiet_NaN(), 0);
            const std::vector<RemoteInputEvent> invalid = {badMove, Mouse(RemoteInputEvent::Kind::Move, -0.1, 0),
                Mouse(RemoteInputEvent::Kind::Move, 0, 1.1), Scroll(121, 0), Scroll(0, 0), Key("KEY_POWER", true),
                Mouse(RemoteInputEvent::Kind::Button, 0, 0, "forward", true)};
            RemoteInput input;
            for (const auto& event : invalid) {
                Ready(input); const auto failures = Number(input.SnapshotJson(), "failures");
                CHECK(!input.Submit(event));
                Wait([&] { auto s = input.SnapshotJson(); return Number(s, "failures") > failures && !True(s, "busy"); });
                CHECK(!input.IsSessionEnabled());
            }
            CHECK(Calls().empty());
        });
        test("only adjacent queued moves coalesce across button barriers", [] {
            RemoteInput input; Ready(input); BlockQuery(); CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0, 0))); AwaitBlocked();
            for (int i = 0; i <= 1000; ++i) CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, double(i) / 1000, 0)));
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Button, 1, 0, "left", true)));
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.5, 1)));
            CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0.25, 1)));
            CHECK(Number(input.SnapshotJson(), "queuedEvents") == 3 && Number(input.SnapshotJson(), "coalescedMoves") == 1001);
            Unblock(); Done(input, 4); auto c = Calls(); CHECK(c.size() == 4);
            CHECK(c[1].m.x == 1919 && c[1].m.button == MOUSE_BUTTON_NONE && c[2].m.action == MOUSE_ACTION_BUTTON_DOWN &&
                c[3].m.x == 480 && c[3].m.action == MOUSE_ACTION_MOVE && c[3].m.button == MOUSE_BUTTON_LEFT);
            input.ReleaseAll();
        });
        test("queue overflow does not drop edges silently and releases", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_CTRL_LEFT", true))); Done(input, 1);
            BlockQuery(); CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0, 0))); AwaitBlocked();
            for (int i = 0; i < 128; ++i) CHECK(input.Submit(Key("KEY_A", i % 2 == 0)));
            CHECK(!input.Submit(Key("KEY_A", true))); CHECK(!input.IsSessionEnabled());
            CHECK(Number(input.SnapshotJson(), "queuedEvents") == 0);
            Unblock(); Failed(input); auto c = Calls(); CHECK(c.size() == 2 && c[1].k.code == KEYCODE_CTRL_LEFT && c[1].k.action == KEY_ACTION_UP);
            CHECK(Number(input.SnapshotJson(), "errorCode") == 9100005);
        });
        test("release barrier clears old queue before later submissions", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_CTRL_LEFT", true))); Done(input, 1);
            BlockQuery(); CHECK(input.Submit(Mouse(RemoteInputEvent::Kind::Move, 0, 0))); AwaitBlocked();
            CHECK(input.Submit(Key("KEY_A", true)));
            std::thread release([&] { input.ReleaseAll(); });
            Wait([&] { return Number(input.SnapshotJson(), "queuedEvents") == 0; });
            CHECK(input.Submit(Key("KEY_B", true))); CHECK(input.Submit(Key("KEY_B", false)));
            Unblock(); release.join(); Done(input, 4);
            auto c = Calls(); CHECK(c.size() == 5);
            CHECK(c[2].k.code == KEYCODE_CTRL_LEFT && c[2].k.action == KEY_ACTION_UP);
            CHECK(c[3].k.code == KEYCODE_A + 1 && c[3].k.action == KEY_ACTION_DOWN);
            CHECK(input.IsSessionEnabled());
        });
        test("paste releases previous modifiers and injects complete independent CtrlV", [] {
            RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_SHIFT_LEFT", true))); Done(input, 1);
            CHECK(input.Paste([] { return true; }, std::chrono::steady_clock::now() + 1s));
            auto c = Calls(); CHECK(c.size() == 6 && c[1].k.code == KEYCODE_SHIFT_LEFT && c[1].k.action == KEY_ACTION_UP);
            CHECK(c[2].k.code == KEYCODE_CTRL_LEFT && c[2].k.action == KEY_ACTION_DOWN);
            CHECK(c[3].k.code == KEYCODE_A + ('V' - 'A') && c[3].k.action == KEY_ACTION_DOWN);
            CHECK(c[4].k.action == KEY_ACTION_UP && c[5].k.code == KEYCODE_CTRL_LEFT && c[5].k.action == KEY_ACTION_UP);
            CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 0 && input.IsSessionEnabled());
        });
        test("paste rechecks clipboard at execution after SDK query", [] {
            RemoteInput input; Ready(input); std::atomic<bool> valid {true}; BlockQuery();
            auto result = std::async(std::launch::async, [&] { return input.Paste([&] { return valid.load(); }, std::chrono::steady_clock::now() + 1s); });
            AwaitBlocked(); valid = false; Unblock(); CHECK(!result.get()); CHECK(Calls().empty());
        });
        test("paste receipt deadline prevents delayed V", [] {
            RemoteInput input; Ready(input); BlockQuery();
            auto result = std::async(std::launch::async, [&] { return input.Paste([] { return true; }, std::chrono::steady_clock::now() + 30ms); });
            AwaitBlocked(); std::this_thread::sleep_for(50ms); Unblock(); CHECK(!result.get()); CHECK(Calls().empty());
        });
        test("paste injection failure releases uncertain modifiers and key", [] {
            RemoteInput input; Ready(input);
            { std::lock_guard<std::mutex> lock(sdkMutex); keyResults = {0, INPUT_SERVICE_EXCEPTION}; }
            CHECK(!input.Paste([] { return true; }, std::chrono::steady_clock::now() + 1s));
            CHECK(!input.IsSessionEnabled() && Number(input.SnapshotJson(), "pendingKeysCount") == 0);
            auto c = Calls(); CHECK(c.size() == 4 && c[2].k.action == KEY_ACTION_UP && c[3].k.action == KEY_ACTION_UP);
        });
        test("paste cancellation with failed CtrlUp disables and preserves cleanup evidence", [] {
            RemoteInput input; Ready(input); int checks = 0; upFailure = INPUT_SERVICE_EXCEPTION;
            CHECK(!input.Paste([&] { return ++checks < 3; }, std::chrono::steady_clock::now() + 1s));
            auto c = Calls(); CHECK(c.size() == 2 && c[0].k.code == KEYCODE_CTRL_LEFT && c[0].k.action == KEY_ACTION_DOWN);
            CHECK(c[1].k.code == KEYCODE_CTRL_LEFT && c[1].k.action == KEY_ACTION_UP);
            CHECK(!input.IsSessionEnabled() && Number(input.SnapshotJson(), "pendingKeysCount") == 1);
            CHECK(Number(input.SnapshotJson(), "errorCode") == INPUT_SERVICE_EXCEPTION);
            upFailure = 0; input.ReleaseAll(); CHECK(Number(input.SnapshotJson(), "pendingKeysCount") == 0);
        });
        test("diagnostics contain counts only and destructor releases", [] {
            {
                RemoteInput input; Ready(input); CHECK(input.Submit(Key("KEY_A", true))); Done(input, 1);
                const auto json = input.SnapshotJson(); CHECK(json.find("KEY_A") == std::string::npos);
                CHECK(json.find("history") == std::string::npos && json.find("\"x\"") == std::string::npos);
                CHECK(Number(json, "pendingKeysCount") == 1);
            }
            auto c = Calls(); CHECK(c.size() == 2 && c.back().k.action == KEY_ACTION_UP);
        });
        std::cout << "remote input tests passed: " << count << "; SDK calls use one worker, no live injection.\n";
        return 0;
    } catch (const std::exception& error) {
        Unblock(); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
