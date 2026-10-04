# ENCODER_TEST_RESULT

检查日期：2026-10-03（Asia/Shanghai）

当前 Gate C 结论：**PASS_DYNAMIC_SCENE_1080_HEIGHT_APPROX_30FPS**。

用户已完成 **0.2.1 动态画面复测**，本台 MOR-M1 / API 26 的 1620×1080 硬件 H.264 编码与 Mac 完整解码，在约 10 秒窗口内提供了 **29.882 FPS** 的功能证据，可以进入 Phase 1 LAN 视频通道评估。该结论限于本设备、本次动态场景和 1080 像素高度；不代表每帧恒定 30 FPS、完整 1920×1080、长时间稳定性或整个远控产品通过。本批证据见 [take-02 独立核对记录](artifacts/device/encoder-take-02/encoder-review.json)。

## 0.2.1 动态场景实测（take-02）

| 项目 | 实测结果 |
| --- | --- |
| 硬件编码器 | `OMX.hisi.video.encoder.avc`，`hardware.verified=true` |
| 数据路径 | AVScreenCapture → 硬件 AVC 编码器输入 Surface；无 CPU RGBA 转换 |
| 停采集窗口 | 首个编码帧回调至停采集，独立单调时钟 **10.006 秒**；停采集时 **300 帧** |
| 最终编码 / 写入 / Mac 解码 | **301 / 301 / 301 帧**，包括停采集后排空的输出 |
| 实际平均帧率 | **29.882 FPS**；请求值 30 FPS |
| 帧输出节奏 | 首末帧回调跨度 **10.042 秒**；前 10 个完整一秒桶为 **31、30、30、30、29、30、30、30、30、29**，末尾不足一秒桶 2 帧 |
| 最大帧回调间隔 | **67.768 ms**；未观察到单调时钟倒退 |
| 输出尺寸 / 格式 | **1620×1080**，保持 3120×2080 屏幕的 3:2 比例；H.264 High / yuv420p |
| 码流 / Annex B | **2,492,960 字节**；SPS 1、PPS 1、IDR 11、VCL 301 |
| 应用输出队列 | 失败、丢包、丢字节、无效/迟到缓冲、释放失败均 0；最高 2 包 / 157,449 字节；上游采集/编码丢帧未知 |
| 停止 / EOS | `eosRequested=true`、`eosReceived=true`、`eosTimedOut=false`；记录的 API 首次错误码和最后返回码均为 0 |
| 画面检查 | 主 agent 查看中间帧（零基索引 150），确认前台 0.2.1 动态图案及桌面、其他窗口；本次元数据复核没有重复查看图像 |

[原始设备报告](artifacts/device/encoder-take-02/encoder-probe.json)、[原始码流](artifacts/device/encoder-take-02/capture.h264)、[Mac 解码验证](artifacts/device/encoder-take-02/h264-validation/validation.json) 与采集 SHA-256 一致。完整解码和中间 PNG 提取均成功，301 帧及 1620×1080 尺寸与原生最终输出一致。原生报告仍保留 `macDecodeVerified=false` 和待人工复核的 verdict，离线结论另存。

平均帧率口径为 `(framesAtCaptureStop - 1) / 首帧回调至停止的时间`，本轮分子为 **299**；计算采用未舍入的单调时钟时长，报告显示 **29.882**。最终 301 帧、停采集时 300 帧、首末帧的 10.042 秒分别覆盖不同边界，不能混用分子和分母。逐秒桶以首个帧回调为起点，前 10 个完整桶均在 29–31 帧；结合完整解码、硬件选择、队列和 EOS 证据，足以认定本次动态场景达到“约 30 FPS”的功能目标，无需将 `avg >= 30.000` 作为机械门槛。67.768 ms 最大间隔也明确不支持恒定逐帧 30 FPS 的说法。

## 首轮 0.2.0 记录（take-01，保留历史）

首轮结论仍为 **HARDWARE_ENCODE_AND_MAC_DECODE_PASS_PERFORMANCE_GATE_INCOMPLETE**，见 [take-01 核对记录](artifacts/device/encoder-take-01/encoder-review.json)。新版结果不会回写首轮原始证据。

| 项目 | 实测结果 |
| --- | --- |
| 版本 / 设备 | 0.2.0，MOR-M1 / HUAWEI MateBook Pro S，API 26 |
| 硬件编码器 | `OMX.hisi.video.encoder.avc`，运行时 `hardware.verified=true` |
| 数据路径 | AVScreenCapture → 硬件 AVC 编码器输入 Surface；无 CPU RGBA 转换 |
| 运行窗口 | 从首个编码输出起，独立单调时钟计时 **10.008 秒** |
| 编码 / 写入 / Mac 解码 | **224 / 224 / 224 帧** |
| 实际平均帧率 | **22.283 FPS**，原生实际运行窗口统计；请求值为 30 FPS |
| 输出尺寸 | **1620×1080**，保持 3120×2080 屏幕的 3:2 比例；不是 1920×1080 |
| 码流 | H.264 High / yuv420p，2,047,022 字节 |
| Annex B 结构 | SPS 1、PPS 1、IDR 8、VCL 224 |
| 应用输出队列 | 记录的队列失败、丢包、丢字节、无效缓冲、释放失败均为 0；上游采集/编码丢帧未知 |
| 停止 / EOS | `eosRequested=true`、`eosReceived=true`、`eosTimedOut=false` |
| 画面检查 | 主 agent 已查看中间帧（零基索引 112），确认包含桌面、任务栏、浏览器和其他应用 |

[原始设备报告](artifacts/device/encoder-take-01/encoder-probe.json)、[原始码流](artifacts/device/encoder-take-01/capture.h264) 和 [Mac 解码验证](artifacts/device/encoder-take-01/h264-validation/validation.json) 相互对应。完整解码与中间 PNG 提取均返回 `0`；实际解码帧数和分辨率与原生输出相同。原始报告中的 `macDecodeVerified=false` 保持原样，离线解码结论写在独立记录中。

首轮帧率口径相同，分子为 223；程序用未四舍五入的单调时钟时长计算，不是把 224 除以 10.008 后得到 22.283。

## PTS 单位异常及判定边界

**0.2.1 的 schema v2 诊断修复已在 take-02 实测生效**：使用 `firstPtsRaw/lastPtsRaw` 保留原始值，`ptsDiagnostics.rawUnitVerified=false`，状态为 `UNIT_MISMATCH_REQUIRES_REVIEW`。原始差值 **10,040,993,906** 按 SDK 微秒声明对应 **10040.994 秒**，与帧输出单调时钟 **10.042 秒**不一致；纳秒候选对应 **10.041 秒**。报告将声明值、候选值与独立计时分列，没有自动更改单位。原始 PTS 单位仍为 **UNVERIFIED**，不能用于已确认的录制时长、帧率或网络播放时间戳契约。

API 26 的 `native_avbuffer_info.h` 将 `OH_AVCodecBufferAttr.pts` 声明为微秒。首轮原始首尾差为 **10,008,696,406**，0.2.0 写出的 `encodedTimestampSpanSeconds=10008.696` 与实际 10.008 秒窗口矛盾，字段名 `firstPtsUs/lastPtsUs` 也不构成单位证明。按纳秒解释的约 `10.008696406` 秒只是假设，原始文件保持不变。两轮基于独立单调时钟的运行窗口和平均帧率均不依赖 PTS 单位。

两轮 FFprobe 均返回 `r_frame_rate=1200000/1`、`avg_frame_rate=25/1`；解码输出时间首轮约 8.92 秒、本轮 12 秒，来自裸流解析/输出时间基，不能替代设备实际供帧率或录制窗口。解码工具未强加 `-r`。完整 1920×1080、恒定帧间隔和长期稳定性仍未验证。

## 后续

**0.2.1 已编译、签名、安装、启动并完成动态画面真机复测**，构建证据见 [新版验证](artifacts/build-verification-0.2.1.json)。schema v2 的 PTS 诊断及固定容量逐秒帧数/最大回调间隔已有真实输出；PTS/计数边界回归通过 UBSan，导出工具兼容 schema v1/v2。

take-01 与 take-02 的版本和画面场景不同，本次不能确定首轮 22.283 FPS 的原因，也没有完成控制变量的静态/动态对照。接下来评估 **Phase 1 LAN 视频通道**，包括传输与接收端时间戳策略；网络传输、配对和 macOS 播放客户端尚未实现或验收。

两次录制和 Mac 解码均已完成，所有原始证据保持不变。FFmpeg 仅作为开发验证工具，不是产品依赖；当前局部 Gate C 结论不等同于完整远控产品或 `REMOTE CONTROL FEASIBLE` 的整体声明。
