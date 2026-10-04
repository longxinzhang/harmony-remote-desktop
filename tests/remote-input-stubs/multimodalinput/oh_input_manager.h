#ifndef HARMONY_REMOTE_REMOTE_TEST_OH_INPUT_MANAGER_H
#define HARMONY_REMOTE_REMOTE_TEST_OH_INPUT_MANAGER_H

// Test-only declarations matching the subset used by remote_input.cpp. These
// headers do not implement device authorization or real OS input injection.
#include <cstdint>

enum Input_Result {
    INPUT_SUCCESS = 0,
    INPUT_PERMISSION_DENIED = 201,
    INPUT_PARAMETER_ERROR = 401,
    INPUT_DEVICE_NOT_SUPPORTED = 801,
    INPUT_SERVICE_EXCEPTION = 3800001
};
enum Input_InjectionStatus { UNAUTHORIZED = 0, AUTHORIZING = 1, AUTHORIZED = 2 };
using Input_InjectAuthorizeCallback = void (*)(Input_InjectionStatus);
enum Input_KeyEventAction { KEY_ACTION_CANCEL = 0, KEY_ACTION_DOWN = 1, KEY_ACTION_UP = 2 };
enum Input_MouseEventAction {
    MOUSE_ACTION_CANCEL = 0, MOUSE_ACTION_MOVE = 1,
    MOUSE_ACTION_BUTTON_DOWN = 2, MOUSE_ACTION_BUTTON_UP = 3,
    MOUSE_ACTION_AXIS_BEGIN = 4, MOUSE_ACTION_AXIS_UPDATE = 5, MOUSE_ACTION_AXIS_END = 6
};
enum Input_MouseEventButton { MOUSE_BUTTON_NONE = -1, MOUSE_BUTTON_LEFT = 0, MOUSE_BUTTON_MIDDLE = 1, MOUSE_BUTTON_RIGHT = 2 };
enum InputEvent_MouseAxis { MOUSE_AXIS_SCROLL_VERTICAL = 0, MOUSE_AXIS_SCROLL_HORIZONTAL = 1 };
// Enum numeric values copied from local API 26 oh_key_code.h.
enum Input_KeyCode {
    KEYCODE_0 = 2000,
    KEYCODE_A = 2017,
    KEYCODE_ALT_LEFT = 2045,
    KEYCODE_ALT_RIGHT = 2046,
    KEYCODE_APOSTROPHE = 2063,
    KEYCODE_BACKSLASH = 2061,
    KEYCODE_CAPS_LOCK = 2074,
    KEYCODE_COMMA = 2043,
    KEYCODE_CTRL_LEFT = 2072,
    KEYCODE_CTRL_RIGHT = 2073,
    KEYCODE_DEL = 2055,
    KEYCODE_DPAD_DOWN = 2013,
    KEYCODE_DPAD_LEFT = 2014,
    KEYCODE_DPAD_RIGHT = 2015,
    KEYCODE_DPAD_UP = 2012,
    KEYCODE_ENTER = 2054,
    KEYCODE_EQUALS = 2058,
    KEYCODE_ESCAPE = 2070,
    KEYCODE_F1 = 2090,
    KEYCODE_FORWARD_DEL = 2071,
    KEYCODE_GRAVE = 2056,
    KEYCODE_LEFT_BRACKET = 2059,
    KEYCODE_META_LEFT = 2076,
    KEYCODE_META_RIGHT = 2077,
    KEYCODE_MINUS = 2057,
    KEYCODE_MOVE_END = 2082,
    KEYCODE_MOVE_HOME = 2081,
    KEYCODE_PAGE_DOWN = 2069,
    KEYCODE_PAGE_UP = 2068,
    KEYCODE_PERIOD = 2044,
    KEYCODE_RIGHT_BRACKET = 2060,
    KEYCODE_SEMICOLON = 2062,
    KEYCODE_SHIFT_LEFT = 2047,
    KEYCODE_SHIFT_RIGHT = 2048,
    KEYCODE_SLASH = 2064,
    KEYCODE_SPACE = 2050,
    KEYCODE_TAB = 2049
};
struct Input_KeyEvent;
struct Input_MouseEvent;

extern "C" {
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback);
Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* status);
void OH_Input_CancelInjection();
Input_KeyEvent* OH_Input_CreateKeyEvent();
void OH_Input_DestroyKeyEvent(Input_KeyEvent** event);
void OH_Input_SetKeyEventKeyCode(Input_KeyEvent* event, int32_t code);
void OH_Input_SetKeyEventAction(Input_KeyEvent* event, int32_t action);
void OH_Input_SetKeyEventActionTime(Input_KeyEvent* event, int64_t timestamp);
int32_t OH_Input_InjectKeyEvent(const Input_KeyEvent* event);
Input_MouseEvent* OH_Input_CreateMouseEvent();
void OH_Input_DestroyMouseEvent(Input_MouseEvent** event);
void OH_Input_SetMouseEventDisplayId(Input_MouseEvent* event, int32_t displayId);
void OH_Input_SetMouseEventDisplayX(Input_MouseEvent* event, int32_t x);
void OH_Input_SetMouseEventDisplayY(Input_MouseEvent* event, int32_t y);
void OH_Input_SetMouseEventAction(Input_MouseEvent* event, int32_t action);
void OH_Input_SetMouseEventButton(Input_MouseEvent* event, int32_t button);
void OH_Input_SetMouseEventActionTime(Input_MouseEvent* event, int64_t timestamp);
void OH_Input_SetMouseEventAxisType(Input_MouseEvent* event, int32_t axisType);
void OH_Input_SetMouseEventAxisValue(Input_MouseEvent* event, float axisValue);
int32_t OH_Input_InjectMouseEvent(const Input_MouseEvent* event);
}

#endif
