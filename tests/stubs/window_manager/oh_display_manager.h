#ifndef HARMONY_REMOTE_TEST_OH_DISPLAY_MANAGER_H
#define HARMONY_REMOTE_TEST_OH_DISPLAY_MANAGER_H

#include <cstdint>
enum NativeDisplayManager_ErrorCode {
    DISPLAY_MANAGER_OK = 0,
    DISPLAY_MANAGER_ERROR_SYSTEM_ABNORMAL = 1400003
};

extern "C" {
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayId(uint64_t* id);
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayWidth(int32_t* width);
NativeDisplayManager_ErrorCode OH_NativeDisplayManager_GetDefaultDisplayHeight(int32_t* height);
}

#endif
