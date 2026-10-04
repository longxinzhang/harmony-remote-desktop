# 构建和真机验收

当前范围为 API 26 的 Phase 0A/0B/0C 探针和 Phase 1 局域网视频传输。构建成功只能证明 ArkTS/C++ 编译与打包通过；屏幕采集、授权弹窗、跨应用输入、硬件编码与实际帧率结论必须来自 HarmonyOS PC 真机。

## 1. 开发机构建

```bash
cd /Users/zhanglongxin/Desktop/longxincode/harmony-remote
python3 scripts/audit-environment.py --output artifacts/environment.json
bash scripts/build-hap.sh
```

脚本使用本机 DevEco 内置 Node、Java、Hvigor wrapper，先执行 `ohpm install --all` 解析本项目 `file:` 原生类型依赖，再以 `entry@default`、`debug` 构建 `host-harmony`。当前没有远程 npm/OHPM 依赖。未配置本项目签名时生成 unsigned HAP，输出位于 `host-harmony/entry/build/default/outputs/default/`。不自动安装、升级 SDK，也不自动创建签名。构建参数可追加，例如 `--no-incremental`。

## 2. 准备指定真机

在目标 HarmonyOS PC 上开启开发者模式与调试，完成正常的设备连接确认。记录型号、系统版本、API 版本和显示器尺寸。只在本项目中通过 DevEco 配置匹配该设备的签名，再重新构建。

```bash
python3 scripts/device-check.py --target '实际设备 ID'
python3 scripts/install-hap.py --target '实际设备 ID' \
  --hap host-harmony/entry/build/default/outputs/default/entry-default-signed.hap
```

安装脚本要求显式 target，只接受本项目固定路径的 signed HAP，并检查内部 bundleName `com.longxin.harmonyremote.probe`、entry 模块、官方签名验证和文件 SHA-256。它只安装，不启动其他应用、不模拟系统输入。目标设备的签名适用性最终由系统安装校验决定。尚未有真机时不要执行安装步骤。

## 3. Gate A：桌面采集

1. 在设备上打开探针，点击 Start Capture，完成系统录屏授权。
2. 观察帧数持续增长，记录实际分辨率、缓冲区像素格式、FPS、开始/停止返回值及时间戳。不能把配置分辨率当作实测分辨率。
3. 切到桌面和其他应用，确认采集继续。观察/保存可见帧证据后，才能判定采集的是系统桌面；只有计数变化不足以证明画面内容。
4. 停止、重新开始，拒绝授权，再次授权；确认资源释放、状态显示和错误码符合实际情况。
5. 记录遮蔽内容与输入法候选窗限制。若缺少画面证据或某项未执行，就在结果中保留 `UNVERIFIED`。

填写根目录 `CAPTURE_TEST_RESULT.md`。没有真机时维持 `BLOCKED_NO_DEVICE`，不得填 PASS/FAIL。

## 4. Gate B：用户授权和全局输入

1. 点击 Request Input Permission，记录请求返回值、授权回调及状态查询结果；分别验证拒绝、允许、撤销。
2. 用户点击 Move Mouse to Center 后，在 5 秒倒计时内切到桌面或目标应用。观察系统指针是否到达主屏中心，并记录实际位置和 API 返回值。
3. 用户点击 Inject A 后，在 5 秒内切换到另一个应用的无敏感内容文本框。先切换英文输入状态，确认一个 `a` 直接提交，并记录 A down/up 两次返回值。如果字符仅进入中文输入法的预编辑状态，记录“IME 有反应、文本提交未验证”，不要记成英文字符提交通过。
4. 依次检查桌面、浏览器、系统设置可输入区域和另一普通应用。0.1.1 新增的本应用空白测试框可覆盖 Host App 目标，但不能用探针内文本改变代替跨应用注入 PASS。
5. 检查撤销授权后不能继续注入；重复运行不会产生按键卡住或多余输入。测试不得用 `hdc shell uitest`、Accessibility 或 HID 代替 Input Kit。

填写根目录 `INPUT_TEST_RESULT.md`，分别记录授权、鼠标、键盘及每个跨应用目标的证据。发生权限阻塞时保留实际错误码并停止推进依赖它的网络远控功能。

## 5. 后续 Gate

在 Gate A/B 有真机证据前，暂不推进网络远控客户端。Phase 0C 已完成本设备动态场景 1620×1080、约 10 秒、29.882 FPS 的硬编和完整解码，见 `ENCODER_TEST_RESULT.md`。不能扩大为长期稳定性或完整 1920×1080 结论。只有桌面采集、H.264 编码、全局鼠标和全局键盘四项均通过，才可写 `REMOTE CONTROL FEASIBLE`。

## 6. 诊断导出

本节描述 **0.1.1** 引入并经真机验证的自动保存与历史记录功能；20 个已完成动作见 `input-gate-final-01`。该版本在每次输入动作完成及授权状态变化后自动保存诊断；“保存当前诊断到应用目录”仍可手动保存。完成一次测试后，在项目目录执行：

```bash
python3 scripts/collect-diagnostics.py --target '实际 HDC 设备 ID' \
  --label browser-inject-a-01
```

`--target` 和 `--label` 都必须明确指定。label 仅允许小写字母、数字和单个连字符，最多 48 字符；已存在的 label 不会覆盖，请为每次测试换一个名称。

脚本只从固定包名 `com.longxin.harmonyremote.probe` 的已核准 Ability 目录 `./data/storage/el2/base/haps/entry/files` 逐个读取 `capture-probe.json`、`capture-frame.ppm`、`input-test.json`、`capture-snapshot.json`、`device-info.json`，保存到 `artifacts/device/<label>/`。本机真机核查显示，实际目录是 `haps/entry/files`，不是通用示例中的 `base/files`。应用必须使用调试签名并已启动，沙箱导出权限由 HDC/设备系统检查。

每次导出生成 `collection.json`，逐项记录缺失、传输失败、数据无效或收集成功，以及有效接收数据的 SHA-256。只有新文件确实生成并通过 JSON/PPM 结构校验才记为 `COLLECTED`，不会只凭 HDC 退出码 0 判定成功。退出码 `0` 表示五份文件均收集完成，`2` 表示部分完成，`3` 表示全部未收集成功；这不代表录屏或输入功能验收结果。

输入报告保留最近动作字段，并包含本进程最近最多 32 个已完成动作的历史，每项记录 Unix 毫秒时间、各 steps/code 与 pending 状态。进程重启不会恢复进程内历史；设备端自动/手动保存会更新当前诊断文件。桌面、浏览器、系统设置及每个输入动作仍应使用不同 label 导出，旧本地归档不会覆盖。脚本不连接新目标、不启动应用、不截图、不调用任何输入动作，也不访问其他应用。

旧批次 `artifacts/device/input-check-01/` 来自 **0.1.0**，仅含最近一次动作，原始文件保持不变，不会补生动作历史。新机制的构建和设备部署以实际记录为准；历史中的 API 返回 `0` 只表明调用结果，各目标应用中的实际效果仍需用户观察确认。

## 7. Phase 0C H.264 硬编

0.2.0 首轮为 224 帧 / 10.008 秒 / 22.283 FPS；0.2.1 动态复测已得到 301 帧完整解码，停采集窗口 300 帧 / 10.006 秒 / 29.882 FPS。这是已完成的历史验证。重复测试时：点击 **Start H.264 · 10s**，在系统授权中允许共享整个主屏幕，然后留在本页，编码期间的移动图案提供持续变化画面；也可另外测试静态桌面或其他应用并记录测试场景。程序从首个编码帧开始计时，结束后排空编码器并保存报告，确认状态已结束再导出。失败时先收集原始报告，不把失败归为成功录制。

```bash
python3 scripts/collect-encoder.py --target '实际 HDC 设备 ID' --label encoder-new-take
python3 scripts/validate-h264.py --input artifacts/device/encoder-new-take/capture.h264
```

收集脚本只取固定应用目录的 `encoder-probe.json` 和 `capture.h264`，拒绝覆盖已有 label。验证脚本使用现有 ffprobe/ffmpeg 完整解码、统计 Annex B NAL，并提取中间帧 PNG，输出到该批次的 `h264-validation/`；输出目录已存在时需指定新的 `--output-label`。工具不会安装依赖、启动录屏或自动判定 Gate C。

必须人工核对：原生报告硬件身份/实际配置、10 秒计时/PTS与实测帧数、丢弃与 EOS、解码结果及其他应用画面。schema v2 另记原始 PTS、声明单位与候选单位、独立回调时长、逐秒计数和最大间隔；单位不一致时保留异常，不能静默归一化。裸 H.264 缺少容器时间信息，ffprobe 的帧率和时长可能是元数据或估算，不能单独证明持续 30 FPS。填写根目录 `ENCODER_TEST_RESULT.md`，保留真实尺寸与任何未验证项。

## 8. Phase 1：PIN 配对与 LAN 视频接收

**当前已有真机功能 PASS（2026-10-04）**：0.3.0 的 `lan-take-01` 经用户配对和系统录屏授权，Host 写入/发送、Mac LAN 接收/完整解码均 287 帧，1620×1080；两端 H.264 均 1,730,857 字节、SHA-256 相同，正常 EOS、无记录传输错误，中间帧已确认整个桌面。限定结论为 `PASS_THIS_DEVICE_LAN_H264_TRANSFER_AND_MAC_DECODE`，见 [LAN_TEST_RESULT.md](../LAN_TEST_RESULT.md) 和 [独立核对记录](../artifacts/device/lan-take-01/lan-review.json)。Host 实测 10.036 秒 / 28.397 FPS，Mac 接收首末 AU 9.994878 秒 / 28.614657 FPS；功能验收不将 30.000 FPS 作为硬门槛，也不混淆两种窗口。Mac 桌面查看器和网络输入不在此阶段。

以下为复测步骤，必须使用新的目录/label；既有 `lan-take-01` 和 `lan-take-01-host` 已归档，不能覆盖。

1. 鸿蒙 PC 打开应用，点击 **Start Server**。默认选择本机 RFC1918 IPv4；有多个网络接口时，可在启动前填入实际局域网地址。记下页面显示的地址和 6 位 PIN。
2. Mac 在项目目录运行以下命令，交互提示时输入 PIN（输入不回显）。地址使用页面显示值，输出目录必须不存在。PIN 不放在命令行参数、环境变量或日志中。

```bash
python3 scripts/lan-receiver.py --host 192.168.31.130 \
  --output artifacts/device/lan-new-take --timeout 180
```

3. 等接收端显示 `Paired; video ready`，在鸿蒙 PC 点击 **Start LAN Capture · 10s**。在系统弹窗允许共享整个主屏幕，保持应用前台观察自动移动图案，约 10 秒后结束。配对本身不会发起录屏。
4. 接收端必须收到完整序号、SPS/PPS、首个 IDR 和 EOS，才将 `.partial` 发布为 `capture.h264`。失败保留 partial 和错误报告；不要将文件存在或程序退出视为视频通过。
5. 录屏结束后，页面应保留最近发送帧数；点击 **保存当前诊断到应用目录** 可显式保存最终快照。控制断线后回到 LISTENING，旧 PIN 已使用，下一轮需 **Stop Server → Start Server** 生成新 PIN。
6. 导出 Host 证据并完整解码 Mac 收到的视频：

```bash
python3 scripts/collect-lan.py --target '192.168.31.130:35029' --label lan-new-take-host
python3 scripts/validate-h264.py --input artifacts/device/lan-new-take/capture.h264
```

检查三个来源：Host 原生编码与发送统计、Mac `receiver-report.json`、完整解码结果。核对同轮版本/时间、最终帧数、EOS、错误/队列计数、H.264 大小和 SHA-256，并查看解码图像。本实现没有有损跳帧恢复；队列满或发送超时会中止本次试验，因此完整传输应逐字节一致。不要仅用 FFprobe 推导帧率。

原生与接收目录独立保存。解码工具只自动寻找码流同目录的原生报告；如果 `native_evidence.present=false`，应在独立 review 中对照 Host 导出，不能为消除该字段而改写原始验证文件。接收会话总时长包含人工等待，本轮为 109.183749 秒，不能当作录屏时长。已有网络传输 PASS 也不代替长期稳定性、端到端延迟或真实网络故障恢复验收。

当前传输为可信局域网明文原型，用户主动启动后才监听 39871/39872；不可把 PIN 配对描述为加密。`pts_us` 使用本轮编码输出回调单调时钟的相对微秒，不能用于端到端延迟结论。详见 [协议](PROTOCOL.md)。


## 0.4.0 网络键鼠与长会话准备

两端构建、Host 签名校验/安装/启动通过，启动导出确认默认不监听、不录屏、不允许远程输入。新增测试结果：原生输入 28/28、Host LAN 31/31、Mac 网络 83 项、Mac 输入 21/21、原 CLI 28/28；均为本机 mock/回环或构建检查。详见 [0.4.0 验证记录](../artifacts/build-verification-0.4.0.json)。

新版实机键鼠、Mac GUI 与长会话操作步骤集中在 [REMOTE_CONTROL_ACCEPTANCE.md](REMOTE_CONTROL_ACCEPTANCE.md)，尚未执行，不能继承 0.3.0 的短时实时查看结果。程序增加 30 分钟选项不代表 30 分钟稳定性已通过。
