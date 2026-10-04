# Phase 0A 屏幕捕获 SDK 核对

核对日期：2026-10-03。依据为本机 DevEco Studio 附带的 API 26 SDK 头文件；这份记录不代表已在 HarmonyOS PC 真机执行成功。

SDK 根目录：`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native`。

实现：`host-harmony/entry/src/main/cpp/capture_probe.h`、`capture_probe.cpp`。没有使用第三方 JSON 库。

## 配置与 PRD 对齐

| 项目 | 实际配置与理由 |
| --- | --- |
| 捕获模式 | `OH_CAPTURE_HOME_SCREEN`，与 PRD Phase 0A 配置一致。SDK 枚举注释为 capture home screen。 |
| 默认屏信息 | `OH_NativeDisplayManager_GetDefaultDisplayId/Width/Height`。默认 ID 用于诊断和 `SetDisplayCallback` 返回 ID 的一致性校验。 |
| Display ID 参数 | `videoCapInfo.displayId` 仅用于 `OH_CAPTURE_SPECIFIED_SCREEN`，因此 HOME_SCREEN 配置不通过该字段选屏。实际回调 ID 与默认 ID 不一致时终止并记录错误。 |
| 数据类型 | `OH_ORIGINAL_STREAM`。 |
| 视频源 | `OH_VIDEO_SOURCE_SURFACE_RGBA`。实际图片格式仍检查 NativeBuffer config，不仅依赖请求格式。 |
| 输出尺寸 | 根据默认屏宽高等比缩小至不超过 1920 × 1080，不放大小屏。宽高向下取偶数，单轴最多产生不足 2 像素的舍入误差。 |
| 帧率 | 捕获真正开始之后调用 `OH_AVScreenCapture_SetMaxVideoFrameRate(capture, 30)`。这是最大帧率请求，实际 fps 由回调计数测量，不假设达到 30。 |
| 光标 | `OH_AVScreenCapture_ShowCursor(capture, true)`，在 Init 后、Start 前调用，检查返回值。SDK 头文件没有限定该接口必须在开始前或开始后调用。其目标设备行为仍须真机确认。 |
| 音频 | 整个 config 零初始化，麦克风和内部音频的 sample rate、channels 均为零。SDK 明确规定此组合忽略对应音频采集；另调用 `SetMicrophoneEnabled(false)`。 |
| 采样图 | 第 150 个有效视频回调或从首帧起满 5 秒后的首个视频回调，二者先到时复制一次。仅保存一帧，给操作者切换桌面的时间。 |
| 运行时长 | 从第一帧开始运行至少 30 秒；授权等待不计入。没有视频帧时，在请求后 60 秒超时释放。用户取消、API 错误或主动停止会提前结束并保留诊断。 |

实际图片仍可能包含应用窗口。程序不会自动认定图片就是桌面，操作者必须检查 `capture-frame.ppm`。

## 头文件声明与时序

相对 SDK `sysroot/usr/include`：

| 头文件 | 本实现使用的声明和约束 |
| --- | --- |
| `multimedia/player_framework/native_avscreen_capture.h` | `OH_AVScreenCapture_Create()` 返回捕获实例，必须以 `OH_AVScreenCapture_Release()` 配对。 |
| 同上 | `OH_AVScreenCapture_SetStateCallback(capture, callback, userData)`、`SetDataCallback`、`SetErrorCallback` 均在 Start 前注册。 |
| 同上 | `OH_AVScreenCapture_SetDisplayCallback(capture, callback, userData)` 在 Start 前注册，记录实际捕获屏 ID。 |
| 同上 | `OH_AVScreenCapture_Init(capture, OH_AVScreenCaptureConfig config)` 的 config 按值传递，头文件建议先零初始化。 |
| 同上 | `OH_AVScreenCapture_StartScreenCapture(capture)` 的返回值不足以证明用户已经同意或已产生帧。实现等待 STARTED 状态或第一帧后再设置最大帧率。 |
| 同上 | `OH_AVScreenCapture_SetMaxVideoFrameRate(capture, int32_t frameRate)` 的头文件明确要求在屏幕捕获开始之后调用。 |
| 同上 | `OH_AVScreenCapture_ShowCursor(capture, bool showCursor)` 无显式开始前/后时序约束；返回 unsupported 或其他错误时本轮记录失败，不忽略。 |
| `multimedia/player_framework/native_avscreen_capture_base.h` | 数据回调签名为 `(OH_AVScreenCapture*, OH_AVBuffer*, OH_AVScreenCaptureBufferType, int64_t timestamp, void* userData)`；timestamp 单位为纳秒。 |
| `multimedia/player_framework/native_avbuffer.h` | `OH_AVBuffer_GetNativeBuffer(OH_AVBuffer*)` 返回的 NativeBuffer 必须手动 `OH_NativeBuffer_Unreference()`。数据回调传入的 AVBuffer 不归应用销毁，回调结束后不可再使用。 |
| `native_buffer/native_buffer.h` | `OH_NativeBuffer_GetConfig(buffer, &config)` 返回 void。config.stride 的单位是字节；`OH_NativeBuffer_Map(buffer, &address)` 与 `Unmap` 配对。 |
| `window_manager/oh_display_manager.h` | 默认 display ID 输出为 `uint64_t*`，宽高输出为 `int32_t*`，返回 `NativeDisplayManager_ErrorCode`。 |

使用 SetDataCallback 后，不混用旧式 AcquireVideoBuffer/ReleaseVideoBuffer。SDK 头文件明确指出设置数据回调后这些旧式接口会失败。

启动顺序：默认屏查询 → Create → 注册四个回调 → Init → 禁用麦克风 → ShowCursor → Start → 等待 STARTED/首帧 → SetMaxVideoFrameRate → 采集 30 秒 → Stop → Release → 最终报告。

## 线程、资源与证据

- `Start(filesDir)` 返回 0 只表示已受理异步启动。返回 -1 表示无效目录，-2 表示已有任务运行，-3 表示无法创建 worker。实际 API 返回值见 Snapshot JSON。
- 只有一个可 join 的 worker 负责配置、停止、释放及写文件。`Stop()` 发出取消信号并 join；没有 detached 线程。
- 回调只更新统计或复制指定单帧，不写文件，也不调用 Stop/Release。错误、取消、选错屏通过条件变量通知 worker 清理。
- NativeBuffer 使用 RAII 处理映射与引用。接受 RGBA_8888、RGBX_8888、BGRA_8888，检查尺寸和 byte stride 后逐行复制；其他格式记录错误，不猜测内存布局。
- 最多一个待写图像，API/state 诊断事件各上限 64 条，没有无限帧队列。
- PPM 转 RGB 和磁盘写入由 worker 完成。图像为 `filesDir/capture-frame.ppm`，报告为 `filesDir/capture-probe.json`，通过同目录临时文件 rename 更新。
- JSON 每秒更新一次，在 Stop/Release 后再写最终状态。JSON 中的纳秒时间戳和 display ID 使用字符串，避免 JavaScript 数字精度损失；Unix 毫秒时间为数值。
- 采集结束的单调时钟在停止请求或 30 秒截止时冻结。`elapsedSeconds` 和 `averageFps` 不包含 SDK Stop/Release、最终文件保存耗时；fps 为 `(frames - 1) / elapsedSeconds`。

## 判定边界

`frameDurationGateMet` 要求完整跑到 30 秒截止、至少 300 个有效视频 buffer 回调、冻结的采集时长至少 30 秒。

`numericalGateMet` 还要求启动/采集/释放流程的 `errorCode` 为零、时间戳没有倒退、没有意外音频回调。图片映射错误单独反映在 `image.error` 和 `apiCodes`，不被误当成零帧；缺少图片会阻止进入人工桌面复核状态。时间戳只检查不递减，重复时间戳不会被当成已经验证了每帧内容变化。

即使数值达标且样本已保存，`verdict` 也只会是 `NEEDS_MANUAL_DESKTOP_REVIEW`，`desktopVerified` 始终是 false。样本缺失时为 `INCOMPLETE_IMAGE_EVIDENCE`。此模块不输出 PASS，也不证明桌面内容、硬件 H.264 编码、网络传输、输入注入、延迟或真机稳定性。

`apiCodes` 保留停止接口返回值。取消或系统主动停止后再次调用 Stop 可能返回状态错误；释放失败会使本轮失败。NativeBuffer 的 Map/Unmap/Unreference 返回值单独记录，图片存在也不代表未出现资源操作错误，应审阅完整 JSON。

## 本机验证

以下命令已执行成功，返回 0：

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/llvm/bin/clang++ \
  --target=aarch64-linux-ohos \
  --sysroot=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot \
  -std=c++17 -Wall -Wextra -Werror -fsyntax-only \
  host-harmony/entry/src/main/cpp/capture_probe.cpp
```

需链接 `native_avscreen_capture`、`native_media_core`、`native_buffer`、`native_display_manager`。此检查仅验证 SDK 头文件下的 C++ 语法和签名，不代表链接、HAP 安装或真机运行已经完成。
