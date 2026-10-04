#ifndef HARMONY_REMOTE_TEST_OH_INPUT_MANAGER_H
#define HARMONY_REMOTE_TEST_OH_INPUT_MANAGER_H

// Test-only declarations matching the subset used by input_probe.cpp. These
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
    MOUSE_ACTION_BUTTON_DOWN = 2, MOUSE_ACTION_BUTTON_UP = 3
};
enum Input_MouseEventButton { MOUSE_BUTTON_NONE = -1, MOUSE_BUTTON_LEFT = 0 };
enum Input_KeyCode { KEYCODE_A = 2017, KEYCODE_L = 2028, KEYCODE_CTRL_LEFT = 2072 };
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
int32_t OH_Input_InjectMouseEvent(const Input_MouseEvent* event);
}

#endif
