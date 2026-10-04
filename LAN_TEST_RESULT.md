# Phase 1 局域网视频验证

核对日期：2026-10-04（Asia/Shanghai）。

当前结论：**PASS_THIS_DEVICE_LAN_H264_TRANSFER_AND_MAC_DECODE**。用户已完成 0.3.0 真机配对和系统录屏授权，Mac CLI 经局域网接收的 H.264 已完整解码。此结论限定本设备、本次约 10 秒视频链路功能，不代表 Mac GUI、网络键鼠、恒定 30 FPS、端到端延迟或长期稳定性通过。见 [独立核对记录](artifacts/device/lan-take-01/lan-review.json)。

## 本轮真机结果

| 项目 | 实测结果 |
| --- | --- |
| 版本 / 目标 | Host 与 CLI **0.3.0**；Host `192.168.31.130`，API 26、2in1、`OpenHarmony-7.0.0.105` |
| 硬件 / 输出 | `OMX.hisi.video.encoder.avc`，H.264 High / yuv420p，**1620×1080** |
| 编码 / Host 写入 / LAN 发送 / Mac 接收 / 完整解码 | **287 / 287 / 287 / 287 / 287** 帧或非空 AU |
| 原生停采集窗口 | **10.036 秒**；停采集时 286 帧，间隔口径平均 **28.397 FPS**；最终排空后 287 帧 |
| Mac 接收窗口 | 首末 AU 到达跨度 **9.994878 秒**，间隔口径 **28.614657 FPS** |
| 视频协议 | 289 包：1 个 CONFIG、287 个非空 AU、1 个空 EOS；序号 **0–288**，无缺口，10 个关键帧 |
| H.264 字节数 | Host 文件与 Mac LAN 接收文件均 **1,730,857 字节** |
| 视频协议字节计数 | 两端均 **1,737,793 字节** = H.264 载荷 + 289×24 字节包头；不含控制消息/TCP 开销 |
| 结束 / 错误 | Host `eosSent`、`encoderEndedSuccess`、`streamCompleted` 均 true；`cancelled=false`；接收端 `eosReceived=true`、错误列表为空 |
| 队列 / 释放 | 编码输出队列错误/丢弃/释放失败均 0；网络队列最高 1 包，结束后为空，`abortedStreams=0` |
| 画面 | 主 agent 查看 Mac 解码中间帧 **143**，确认整个桌面、任务栏、其他应用和前台 0.3.0 动态图案 |

Host 与 Mac 文件的共同 SHA-256：

```text
989a46975e95a4e41460f4c6af16e05c00b74c836ac7d70c2a8e71d80af16f9d
```

Mac 的 [接收报告](artifacts/device/lan-take-01/receiver-report.json) 和 [码流](artifacts/device/lan-take-01/capture.h264) 来自实际 TCP 视频接收；[Host 导出](artifacts/device/lan-take-01-host/collection.json) 仅用于独立对照。两端文件逐字节一致，相关哈希与采集/接收报告吻合。不能用 HDC 导出本身替代 LAN 证据。

[完整解码记录](artifacts/device/lan-take-01/h264-validation/validation.json) 的 FFprobe、FFmpeg 完整解码和中间 PNG 提取均返回 0，解码得到 287 帧、1620×1080。该工具在接收目录没有找到 `encoder-probe.json`，其 `native_evidence.present=false` 保持原样；本次独立 review 对照旁边 `lan-take-01-host` 的原始报告补齐跨来源核对，没有复制或改写原始文件。

## 计时与性能边界

原生平均值 **28.397 FPS** 使用 `(framesAtCaptureStop - 1) / 首帧回调至停采集的未舍入单调时钟时长`，本轮分子为 285。Mac 的 **28.614657 FPS** 使用 `(received AUs - 1) / 首末 AU 到达时长`，分子为 286。二者统计点和窗口不同，不能混为同一帧率。接收进程总计 **109.183749 秒** 包含配对和用户操作等候，不是录屏时长。

线上 `pts_us` 明确来自编码输出回调单调时钟、以本会话首个输出回调为起点；首尾 AU 差为 **10.064053 秒**，没有倒退。它不是屏幕采集时刻，也不是已经同步的双机时钟，不能由接收/Host 时间差推导端到端延迟。native raw PTS 单位仍为 **UNVERIFIED**；FFprobe `1200000/1`、`25/1` 及解码输出 11.44 秒均不代表真实供帧率或录制窗口。

本轮通过依据是认证后的真实 LAN 传输、完整数据一致性、正常 EOS、正确画面和完整解码，不以 30.000 FPS 作为功能验收硬门槛。实际 28.397 FPS 保留；不宣称恒定逐帧 30 FPS、完整 1920×1080、上游采集/编码零丢帧或首次降帧原因已经确定。

## 已完成

- Host 原生服务：用户启动后绑定本机 RFC1918 IPv4，控制 39871、视频 39872；PIN 配对及视频 token 认证、单控制端、心跳和超时。
- 编码 worker 将完整 H.264 包交给有界网络队列；SDK 回调不阻塞发送。满队列或发送超时中止试验，保留可见错误。
- Mac 标准库 CLI：完整组包、序号/大小/CONFIG/IDR/EOS 校验、受限文件保存和接收统计；不记录 PIN/token。
- NAPI/UI 接入显式 Start Server 与 Start LAN Capture；配对不会自动录屏，仍需系统授权。
- 25/25 C++ host TCP 测试通过，启用 UBSan；28/28 Python receiver 测试通过，其中一项使用实际 C++ server fixture 逐字节互通。构造的 NAL 仅用于协议测试，没有宣称可解码视频。
- 0.3.0 ArkTS/C++ 构建、官方工具签名验证、HDC 安装、启动和设备版本查询通过。

证据：[构建和源码指纹](artifacts/build-verification-0.3.0.json)、[服务端测试](artifacts/lan-server-tests.log)、[接收端测试](artifacts/lan-receiver-tests.log)、[设备启动](artifacts/launch-0.3.0.json)。

## 后续复测

操作与采证命令见 [TESTING.md](docs/TESTING.md#8-phase-1pin-配对与-lan-视频接收)。本轮正常 EOS 后 Host 回到 `LISTENING`，服务仍在运行，已使用 PIN 无效；下一轮须 Stop Server → Start Server 生成新 PIN，并使用新的输出目录和导出 label。旧批次保留不变。

## 验证边界

当前是可信局域网明文原型，PIN/token 配对不等于加密。Mac GUI、网络键鼠、TLS、端到端延迟、长期稳定性和真实网络故障恢复仍未验收，不能扩展为完整远控产品通过。已有 0.2.1 本地编码 29.882 FPS 的历史结果和本轮 0.3.0 LAN 功能结果分别保留。
