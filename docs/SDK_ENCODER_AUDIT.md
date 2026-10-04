# API 26 本地硬件 H.264 / Surface 编码接口审计

2026-10-03 只读核验本机官方 SDK。Build SDK 为 **26.0.0.105 / API 26**，依据 `/Applications/DevEco-Studio.app/Contents/sdk/default/sdk-pkg.json:6-12` 与 `openharmony/native/oh-uni-package.json:2-10`。

以下相对头文件路径均位于：

`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/`

`PF/` 在本表中表示 `multimedia/player_framework/`。本次确认的是本机 SDK 声明、参数语义及所有权，**尚不能据此宣布真机支持硬件 AVC、Surface 捕获可用或输出可解码**。

| 事项 | 实际接口与 SDK 依据 |
| --- | --- |
| 明确选择硬件 AVC | `PF/native_avcapability.h:170`：`OH_AVCapability *OH_AVCodec_GetCapabilityByCategory(const char *mime, bool isEncoder, OH_AVCodecCategory category)`；`HARDWARE=0` 见 73–75 行。检查非空并用 `OH_AVCapability_IsHardware(capability)`（198 行），读取 `OH_AVCapability_GetName(capability)`（217 行），再以 `OH_VideoEncoder_CreateByName(name)`（`PF/native_avcodec_videoencoder.h:86`）创建。只调用 CreateByMime 不能作为硬件选择证据。 |
| 编码输入 Surface | `OH_VideoEncoder_GetSurface(OH_AVCodec*, OHNativeWindow**)`（`PF/native_avcodec_videoencoder.h:410-423`）必须在 Configure 后、Prepare 前。应用管理 window，结束时调用 `OH_NativeWindow_DestroyNativeWindow`。 |
| Surface 像素格式 | `PF/native_avformat.h:75-78`：`AV_PIXEL_FORMAT_SURFACE_FORMAT=4` 表示从 Surface 获取格式，仅在 Surface 模式生效；NV12=2 是另一具体像素格式。Configure 的真实返回值仍需检查。 |
| 现代编码回调 | `OH_VideoEncoder_RegisterCallback(OH_AVCodec*, OH_AVCodecCallback, void*)`（`PF/native_avcodec_videoencoder.h:192-208`）在 Prepare 前调用。`OH_AVCodecCallback` 及输入/输出 `OH_AVBuffer*` 回调签名见 `PF/native_avcodec_base.h:131-190`。 |
| 直接捕获到 Surface | `OH_AVScreenCapture_StartScreenCaptureWithSurface(OH_AVScreenCapture*, OHNativeWindow*)`（`PF/native_avscreen_capture.h:358-373`），API 12；实际仍可能返回 UNSUPPORT 或 OPERATE_NOT_PERMIT。产品适配表 `sdk/default/hms/ets/api/device-define/api-version/MediaKit.json:19499` 对该 API 列出 2in1，自 API 13。 |
| 无音频与光标 | `PF/native_avscreen_capture.h:67-74` 要求先清零 config；micCapInfo 与 innerCapInfo **各自**采样率和声道数均为 0 时不采集该路音频。`SetMicrophoneEnabled(capture,false)`（246–264 行）只关闭麦克风，不代表禁用内部音频。`ShowCursor(capture,bool)`（518–530 行）API 15 可用。 |
| 捕获帧率 | `OH_AVScreenCapture_SetMaxVideoFrameRate(capture,int32_t)`（`PF/native_avscreen_capture.h:494-515`）必须在捕获开始后调用，最高 60 FPS。设置 30 仅为配置，不证明实际输出达到 30 FPS。 |
| 输出属性 | `OH_AVBuffer_GetBufferAttr(buffer,&attr)`（`PF/native_avbuffer.h:85-96`）；`OH_AVCodecBufferAttr`（`PF/native_avbuffer_info.h:92-112`）中 pts 为**微秒**、size 为字节、offset 为有效数据偏移。GetAddr/GetCapacity 见 `PF/native_avbuffer.h:143-166`。先验证 offset、size、capacity，使用 `size <= capacity-offset` 防溢出，再复制 `addr+offset`。 |
| 配置数据、关键帧与 EOS | `PF/native_avbuffer_info.h:47-85` 定义 EOS=1、SYNC_FRAME=2、INCOMPLETE_FRAME=4、CODEC_DATA=8，按位判断。不要把配置包或零字节 EOS 计为视频帧。`OH_MD_KEY_CODEC_CONFIG`（`PF/native_avcodec_base.h:873-878`）承载视频 SPS/PPS。 |
| 输出释放 | `OH_VideoEncoder_FreeOutputBuffer(codec,index)`（`PF/native_avcodec_videoencoder.h:522-540`）必须及时调用，否则阻塞编码。复制到有界队列后归还；失败、队列满、零字节 EOS 路径也要归还。不对 SDK 回调提供的 OH_AVBuffer 调用用户对象 Destroy。 |
| 结束与生命周期 | Surface 模式使用 `OH_VideoEncoder_NotifyEndOfStream(codec)`（`PF/native_avcodec_videoencoder.h:445-461`）。Stop 释放输入/输出缓冲（327–340 行），Flush 使已回调的 index 失效（343–346 行）。`native_window/external_window.h:429-438` 明确 DestroyNativeWindow 为非线程安全的减引用操作，必须串行、仅一次。 |

配置值类型不可混用：`OH_MD_KEY_BITRATE` 为 int64_t（`PF/native_avcodec_base.h:675-680`），`OH_MD_KEY_FRAME_RATE` 为 double（732–737 行），`OH_MD_KEY_I_FRAME_INTERVAL` 为 int32_t **毫秒**（771–779 行）。1000 表示约一秒一个关键帧，并非 1000 帧。编码分辨率与帧率组合可由 `OH_AVCapability_AreVideoSizeAndFrameRateSupported`（`PF/native_avcapability.h:490-503`）检查。

回调中的 `OH_AVFormat*` 仅在回调执行期间有效（`PF/native_avcodec_base.h:88-89`）；需要保存 SPS/PPS 时复制内容。API 26 可选 `OH_MD_KEY_VIDEO_ENCODER_REPEAT_HEADER_BEFORE_SYNC_FRAMES=1`（同文件 1939–1950 行），默认关闭，可在同步帧前插入配置数据，但不能代替真实码流检验。

本地头文件**没有保证编码输出必为 Annex B**。必须检查实际起始码、SPS/PPS、IDR 等 NAL，并用独立解码器验证；不能仅因 CODEC_DATA 标志或某次调用返回 0 就认定码流可播放。输出回调内及时 FreeOutputBuffer 符合已声明的缓冲所有权，但本地头没有允许任意回调重入 Stop/Destroy 的保证。

建议正常关闭顺序：停捕获生产者 → NotifyEndOfStream → 有界等待输出 EOS、归还回调缓冲、排空写盘队列 → Stop/Destroy 编码器 → 串行释放不再被捕获引用的 NativeWindow。等待超时必须保留错误，不能当作排空成功；持有回调会获取的锁时不要调用可能等待回调退出的停止/销毁 API。

Phase 0C 真机验证仍必须完成：在用户授权下运行 **至少 10 秒，目标 30 FPS**，记录真实 codec 名称与硬件属性、每步返回码、实际输出帧数/时间戳/帧率、字节数、SPS/PPS/IDR/EOS、队列溢出及停止结果，导出 `.h264` 并独立解码核对画面。未取得这些运行证据前，只能标记“SDK 接口核验完成”，不能标记编码 Gate 通过。
