# v0.6.0：帧率与系统音频

## 实现边界

- 鸿蒙共享选项支持 30 / 60 FPS，下一次开始共享生效。每次按真实输出尺寸查询硬件编码器的帧率能力，再设置编码帧率与屏幕采集最大帧率。不支持的组合直接报告 SDK 错误，不将 30 FPS 标成 60 FPS。静止桌面实际帧率可以低于上限。
- 音频为共享期间的设备内播放声音，默认关闭；开启后与本次屏幕共享一起请求系统授权。麦克风参数始终为零，且显式调用 `OH_AVScreenCapture_SetMicrophoneEnabled(false)`。
- 音频不写本地文件。当前为单向 Harmony → Mac 播放；Mac 可静音。声音受应用、系统隐私策略和音频路由限制，尚不能承诺所有应用都可内录。
- 音频与 H.264 分别传输，当前没有基于两端时钟校准的唇音同步。音频时间戳属于采集端时钟，不能用作单向网络延迟。

## 原生数据路径

`OH_AVScreenCapture_SetDataCallback` 只处理 `OH_SCREEN_CAPTURE_BUFFERTYPE_AUDIO_INNER`。配置为 48,000 Hz、双声道、`OH_ALL_PLAYBACK`；回调检查地址、容量、偏移、长度、4 字节帧对齐及时间戳，只在借用 buffer 有效期间复制。视频依旧走编码器 Surface，回调不处理原始视频。

本地 API 26 头文件 `native_avscreen_capture.h` 声明回调自 API 12 可用，回调后的 buffer 不能保留，也不能混用旧 Acquire/Release 音频 API。音频格式依据 [OpenHarmony 官方 AudioCapturerWrapper 源码](https://github.com/openharmony/multimedia_player_framework/blob/master/services/services/screen_capture/server/audio_capturer_wrapper.cpp)：`BuildCapturerOptions` 使用配置的采样率/声道，原始 PCM 固定 S16LE；播放源映射为 playback capture。此源代码证据不等于当前华为设备已经实测通过。

## 独立音频通道

- TCP 39874，服务绑定同一局域网接口，不绑定通配地址。
- 控制会话完成认证后发放独立的随机 `audioEpoch` / `audioBindToken`，只存内存、不输出到诊断。音频连接先发长度前缀 JSON `audio_bind`，服务端检查当前 epoch/token；每组绑定只能使用一次。断线或撤销控制会话立即关闭音频通道并清空数据，新会话重新生成两项凭据。
- 绑定成功后使用固定 40 字节 `HRDA` v1 头：kind、声道、S16LE 格式、payload 长度、采样率、采集时间戳、递增序号和 stream ID。PCM 每包最多 3,840 字节（20 ms），reset 无 payload。共享开始/结束发送 reset；Mac 校验顺序及 stream ID，拒绝旧数据或格式变化。
- 采集线程只向队列复制，不执行 socket I/O。服务队列最多 23,040 字节（120 ms），溢出丢最旧 PCM；网络发送超时只关闭音频，不影响控制或视频线程。一个发送中的包及操作系统 socket buffer 另计，120 ms 不是总延迟承诺。
- Mac 单次读取最大一包，传输队列直接交给有界播放邮箱；邮箱与 AVAudioPlayerNode 调度队列各最多 5,760 帧（各 120 ms）。过载清理旧音频，避免持续增加播放延迟。静音在排队前丢弃 PCM，停止会话拒绝迟到数据。
- 播放使用 AVAudioEngine / AVAudioPlayerNode，S16LE 转浮点时显式按小端字节读取，不依赖内存对齐。输出设备不可用时记录错误并限制重试频率，不报告虚假的播放成功。

现有 LAN 协议仍为明文局域网传输；音频绑定验证会话归属，不提供传输加密。

## 自动验证（2026-10-05）

- `bash scripts/test-audio-service.sh`：24 项通过。真实 localhost 服务测试未配对拒绝、错误/重复认证字段、正确绑定、PCM 字节/时间戳、120 ms 队列上限、凭据轮换、旧会话拒绝、停止/重启。
- `bash scripts/test-mac-audio.sh`：24 项通过。Swift 协议/PCM 转换边界、静音/停止、连接准备期间取消并释放所有者、真实 C++ 服务至 Network.framework 的双端数据一致性；AVAudioEngine 离线渲染验证实际输出幅度。不播放测试声音、不访问麦克风、不写音频文件。
- `encoder_probe.cpp` 与 `audio_service.cpp` 使用本地 API 26 编译命令进行 `-fsyntax-only -Wall -Wextra -Werror` 检查，通过；剔除 CMake 中对 syntax-only 无意义的 gcc-toolchain 参数。

仍需实机验证：60 FPS 硬件接受与移动画面实际帧率、系统音频回调与 Mac 扬声器播放、静音/重连/停止后的听感，以及持续共享的声画偏差和资源占用。自动测试不能替代这些验收。
