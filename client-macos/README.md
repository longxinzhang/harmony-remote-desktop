# Harmony Remote macOS Viewer

原生局域网查看与键鼠客户端：SwiftUI/AppKit 窗口、Network.framework 局域网连接、VideoToolbox H.264 解码，以及只保留最新已解码图像的显示路径。当前版本 **0.5.0**，每次连接默认仅查看，在 Host 授权并允许之后可在 Mac 启用键鼠控制。Host 只提供“10 分钟调试”和“永久上线”两种 LAN 共享模式，两者都不录像。

**0.5.0 已构建并安装、启动，部署由 [build-verification-0.5.0](../artifacts/build-verification-0.5.0.json) 记录**。首个有效非空 AU 到达后，Mac 不再设置固定会话总时长或累计 2 GiB 接收上限。永久模式长期真机运行尚未验收，以下 0.4.1 结果属于保留的历史证据。

**0.4.1 用户实机确认窗口拖动、文字拖选及松手停止正常**，见 [拖拽记录](../artifacts/device/drag-0-4-1-verified/drag-review.json)。本批五分钟查看随后完整结束：Host 编码/发送、Mac 接收/解码均 **9001 帧**，**300.021 秒 / 29.995 FPS**，1620×1080、硬解确认、EOS 完整、无记录错误、未保存录像，见 [完整会话记录](../artifacts/device/drag-0-4-1-completed/session-review.json)。**0.4.0 已确认 Mac 英文输入、右键菜单和 `⌘L`**；该版拖拽失败保留于 [历史输入记录](../artifacts/device/control-live-take-03/input-review.json)。此前 8985 帧五分钟查看及 0.3.0 的 303 帧短时 GUI 证据保留在 [历史查看结果](../MAC_VIEWER_TEST_RESULT.md)。文件拖动、新版多按钮组合、滚轮方向、持键失焦/断线等仍待验收，不能将这些局部成功当成完整输入通过。

## 本机构建与运行

0.5.0 已完成 Host 与 Mac 完整构建；Mac 网络 **105 项**测试通过，含真实 C++ / NWConnection 联动、模拟长期活动会话、分批累计超过 2 GiB，以及保留本地回放和单包上限的检查，见 [网络测试日志](../artifacts/mac-network-tests-permanent-mode.log)。原生会话策略、计时回归及实际 API 26 对象编译也已通过，见 [原生验证](../artifacts/encoder-native-verification-0.5.0.json)。常用启动路径已同步为 0.5.0、签名校验通过并启动，具体指纹与部署状态见 [构建及部署记录](../artifacts/build-verification-0.5.0.json)：

```bash
cd /Users/zhanglongxin/Desktop/longxincode/harmony-remote
open -n client-macos/build/HarmonyRemote.app
```

0.5.0 归档为 [Mac ZIP](../artifacts/releases/0.5.0/HarmonyRemote-Mac-0.5.0.zip) 与 [Host HAP](../artifacts/releases/0.5.0/HarmonyRemote-Host-0.5.0.hap)。Host 空闲启动已核对，十分钟完整共享、永久模式长期运行及模式切换后重启记忆尚未真机验收。

0.4.1 历史验收时运行进程 PID `96911` 与归档二进制匹配，Mac 网络 97 项、原生输入 32 项开发测试通过，见 [历史构建验证](../artifacts/build-verification-0.4.1.json)。已验收归档仍在 `artifacts/releases/0.4.1/mac-build/HarmonyRemote.app`，[Mac ZIP](../artifacts/releases/0.4.1/HarmonyRemote-Mac-0.4.1.zip) 位于 `mac-build` 上一层；当时安装的 [0.4.1 Host HAP](../artifacts/releases/0.4.1/HarmonyRemote-Host-0.4.1.hap) 也保留不变。

当前脚本面向 Apple Silicon，最低系统版本为 macOS 14，使用已安装的 Apple Swift 编译器和系统框架。在项目根目录执行：

```bash
cd /Users/zhanglongxin/Desktop/longxincode/harmony-remote
bash scripts/build-mac.sh
open -n client-macos/build/HarmonyRemote.app
```

构建脚本输出 `client-macos/build/HarmonyRemote.app`，并进行本机 ad-hoc 签名校验；不是公证或发行安装包。客户端本身不依赖 FFmpeg。

## 实时局域网查看

1. 确认两端使用 0.5.0 构建及部署记录中的版本。鸿蒙端点击 **Start Server**，读取页面上的 RFC1918 IPv4 和一次性 6 位 PIN；“在线，等待连接”对应原状态 `LISTENING`。测试双方处于同一可信局域网。
2. 在 Mac 窗口输入地址和 PIN，点击 **连接**。PIN 使用隐藏输入框，提交后清空；不要把 PIN 或 session token 放入启动参数、环境变量或日志。
3. 在 Host 选择 **10 分钟调试**或 **永久上线**，等待视频通道就绪后点击 **Start LAN Capture**，并由用户在系统弹窗授权共享整个主屏幕。调试从首帧开始计时，600 秒自动结束；永久模式时长值为 0，不设自动结束时间。配对不会自动发起录屏。
4. Mac 窗口显示收到的桌面，默认仅查看。Host 点击允许远程控制后，可在 Mac 点击启用键鼠控制并点击画面操作。结束后保留最后一帧。**断开**只终止本次查看；下一轮连接前在 Host 执行 **Stop Server → Start Server**，生成新 PIN。

可在开发启动时预填地址，并选择一个尚不存在的诊断文件：

```bash
open -n client-macos/build/HarmonyRemote.app --args \
  --host 192.168.31.130 \
  --diagnostics "$PWD/artifacts/mac-live-new.json"
```

Host 默认“永久上线”，`PersistentStorage` 的 `hrdSessionMode` 只保存模式选择；不恢复授权、允许开关或配对。调试显示动态图案和开发面板，永久隐藏；两种 LAN 模式均不保存 H.264。永久模式在用户停止、断线、应用关闭或错误时结束，不包含自动重连或无人确认的共享。

`--host` 仅预填地址，不自动连接。地址可保存在本机 `UserDefaults` 中；PIN/token 只用于内存中的当前配对会话，不写入诊断。控制/视频端口为 39871/39872；协议校验包括配对、心跳、完整组包、序号、CONFIG/IDR 和 EOS。目前仍为可信局域网明文原型，PIN 配对不等于传输加密。

## 离线开发回放

回放输入为带 HRD1 包头的 `.hrd` 文件，不能直接把裸 `.h264` 传给 `--replay`。可将已经采集的真实视频封装为开发回放：

```bash
python3 scripts/make-mac-replay.py \
  artifacts/device/lan-take-01/capture.h264 \
  artifacts/mac-replay/lan-take-01-new.hrd \
  --expected-frames 287

open -n client-macos/build/HarmonyRemote.app --args \
  --replay "$PWD/artifacts/mac-replay/lan-take-01-new.hrd" \
  --diagnostics "$PWD/artifacts/mac-replay/lan-take-01-new-viewer.json"
```

封装工具使用开发机现有的 `ffprobe` 定位访问单元，校验与源文件的对应关系，并拒绝覆盖已有回放文件。它使用明确标注的 **合成 30 Hz 时间戳**；不推断真实采集帧率、原始 PTS 单位或网络延迟。窗口在回放模式持续显示“本地验证回放 · 非实时”。本地回放仍有 **64 MiB** 上限；它只验证本地协议解析、解码和显示，不连接 Host，不构成实时 LAN 证据。

独立解码开发检查入口为：

```bash
bash scripts/test-mac-decoder.sh
```

该脚本使用归档视频构造临时回放并运行解码测试；它不代替整个 GUI 的构建、画面检查或 live 验收。使用其他源视频时，封装命令中的 `--expected-frames` 应与该批实际帧数一致。

## 诊断与验收边界

窗口右下角按钮可导出诊断；`--diagnostics` 在会话完成后自动写入指定路径；已有文件不会覆盖，后续会话另建带会话 ID 的文件。诊断区分 `local_replay` 与 `live_lan`，记录接收/解码帧数、显示提交次数、已解码帧替换次数、尺寸、硬解查询结果和显示错误，不包含 PIN/token。

接收的压缩帧按序交给串行解码队列，VideoToolbox 回调将已解码图像放入单槽 mailbox；界面可以替换尚未显示的旧图像，不随意丢弃 H.264 参考帧。显示提交次数不等于物理屏幕实际刷新次数。`LOCAL_REPLAY_DECODED` / `LIVE_STREAM_DECODED` 表示相应解码会话结果，仍需检查显示错误和实际画面，不能单凭该字段认定 GUI 显示全部通过。

后续每次 GUI live 验收应分别记录 Host 授权、真实连接、接收/解码计数、硬解属性、桌面画面及断开行为。原始 PTS 单位未确认，线上时间戳来自编码输出回调单调时钟；此版本不测端到端延迟。0.4.1 修复后的窗口/文字拖拽已有实机确认，完整输入矩阵仍按 [验收步骤](../docs/REMOTE_CONTROL_ACCEPTANCE.md) 继续检查。

0.5.0 从连接开始等待首个有效非空 AU 仍有 **32 分钟**期限，迟到首 AU 不能恢复已过期的会话；有效首 AU 到达后取消固定总时长和累计字节上限。CONFIG、keepalive、空 EOS 不算首 AU。心跳仍每 2 秒发送、6 秒失联结束；首字节到完整消息/包的组装仍限 2 秒，单个 AU 限 8 MiB、CONFIG 限 256 KiB，输入/发送队列及解码在飞限制保留。取消累计上限不意味着将整个会话缓存到内存，见 [协议](../docs/PROTOCOL.md)。

**永久模式长期运行仍待真机验收**。0.4.1 曾对首 AU 前后分别设置 32 分钟期限并保留累计 2 GiB 上限，那是历史版本的行为；0.4.0 的约 24 分钟用户主动停止记录也不构成长会话完成证据。现有内存采样没有完整会话基线。当前仅用于可信局域网，TLS 属于后续版本。
