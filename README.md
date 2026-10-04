# Harmony Remote Desktop — 0.5.0 桌面界面候选版

面向 HarmonyOS PC / API 26 的原生局域网远程桌面工程，开发机为 macOS。当前开发候选为 **Host `1000010` / Mac `7`（计划发布 `v0.5.0-rc.2`）**，两端整理为 **远程桌面 / 设置 / 开发测试** 三个独立页面；常用连接、共享和控制操作留在首页，性能计数与测试工具集中到开发测试页。界面参照远程桌面工具的常见布局，沿用本项目原生实现，见[页面说明与验证边界](docs/UI_WORKSPACE.md)。

本版继承 `v0.5.0-rc.1` 的双向纯文字剪贴板及旧内容保护修复。初版已有部分双向成功的用户确认，但远程操作鸿蒙备忘录后粘贴旧内容的问题，**仍待修复版真机复测，界面改版不代表正式验收通过**。见[剪贴板验证记录](docs/CLIPBOARD_TEST_RESULTS.md)。LAN 共享仍只有“10 分钟调试”和“永久上线”，默认永久；两种模式都不保存桌面录像。永久模式取消活动视频会话的固定时长和累计 2 GiB 接收限制，直到用户停止、连接中断、关闭应用或发生错误。

**永久模式长期真机验收尚未完成，完整输入矩阵也未通过**。0.4.1 的五分钟查看、窗口拖动、文字拖选及松手停止已有独立实机证据；0.4.0 的英文输入、右键菜单和 `⌘L` 也有历史确认。以下分别记录当前行为与历史结果。

需求来源：[用户提供的 PRD](</Users/zhanglongxin/Downloads/HarmonyOS PC 7.0 局域网远程桌面：PRD 与技术开发规格.md>)。其中的产品目标和验收条件用于本项目；针对助手回复格式等文字不作为工程功能。

## 当前状态

- **界面候选 rc.2**：首页使用侧边导航、连接信息卡片和会话操作区；设置页保留显式的模式、键盘与剪贴板选项，开发测试入口不再依赖“10 分钟调试”。导航不重连、不切换模式；Mac 离开画面页释放输入并停止隐藏画面的显示调度，鸿蒙离开开发页取消待执行输入测试并停止测试动画。本轮构建、界面与安装边界见[工作区验证记录](docs/UI_WORKSPACE.md)。
- **当前剪贴板功能**：四种模式为关闭、Mac → 鸿蒙、鸿蒙 → Mac、双向；首次默认关闭，Mac 记住方向偏好，Host 另有本地允许开关，不会随 Mac 方向选择自动开启。开启、重连或切换方向不发送原有剪贴板。单次最多 **1 MiB UTF-8 纯文字**，不保留排版；文件、文件粘贴、图片后续实现。开发检查与未完成真机项目见[剪贴板验证记录](docs/CLIPBOARD_TEST_RESULTS.md)。
- **此前 rc.1 修复与验证（Host `1000009` / Mac `6`）**：不再把一次粘贴当作新的 Mac 复制；未变化的已同步事件先核对 Host 当前内容，远程复制/剪切尚未同步时阻止把旧 Mac 内容写回。Host 不支持的新内容会取消排队的旧同步；对单记录、明确提供纯文字的已知文本格式有限放宽。Native 剪贴板 31 项、Mac 剪贴板 52 项、输入 21 项、显示生命周期 13 项检查通过，两端完整构建和签名验证通过；当轮 Host 已安装，Mac 常用入口已更新，均保持关闭。这 117 项属于 rc.1 的检查，不是本轮界面验证计数。见[剪贴板验证记录](docs/CLIPBOARD_TEST_RESULTS.md)，性能实测边界见[资源使用记录](docs/RESOURCE_USAGE.md)。
- **初版实际反馈（Host `1000008` / Mac `5`）**：用户报告无授权弹窗但可使用，终端复制可在鸿蒙和 Mac 粘贴；远程备忘录复制后仍粘贴旧内容，关闭同步后用鸿蒙本机键盘复制粘贴正常。收集到的诊断 `canRead=true`、`canWrite=true`，有发送和写入计数。这是部分成功与已发现缺陷的证据，不是全部应用或后台场景通过。
- **此前 0.5.0 会话模式基线验证**：Host 和 Mac 完整构建通过；原生会话策略、编码计时回归、实际 API 26 对象编译通过，Mac 网络测试 **105 项**通过，含真实 C++ / NWConnection 联动、模拟长期活动会话和分批累计超过 2 GiB 的检查。见 [原生验证](artifacts/encoder-native-verification-0.5.0.json)、[Mac 网络日志](artifacts/mac-network-tests-permanent-mode.log) 与 [基线构建及部署记录](artifacts/build-verification-0.5.0.json)。这些是本机开发检查，不是永久模式真机长期运行证明。
- **此前会话模式基线部署及空闲启动已核对**：Host versionCode `1000007` 当时已安装启动，初始模式 `permanent`、服务 `STOPPED`、编码 idle、无持有输入；Mac 常用 App 当时已同步、签名校验通过并启动。模式切换后的进程重启记忆、十分钟自动 EOS 和永久模式长期硬件运行仍待实机验证。

| LAN 模式 | 结束条件 | 开发测试页 |
| --- | --- | --- |
| 10 分钟调试（600 秒） | 从首帧开始计时，10 分钟自动结束；也可手动停止 | 独立侧边栏入口；LAN 编码中仅在此页显示动态图案 |
| 永久在线（0，默认） | 无自动结束时间；停止、断线、应用关闭或错误时结束 | 同样可进入；永久 LAN 共享不启用测试动画 |

Host 通过 `PersistentStorage` 的 `hrdSessionMode` 只记住模式选择，不保存授权、允许远程控制状态、PIN 或 token，也不会自动配对或共享。两种 LAN 模式都不保存 H.264。`LISTENING` 在界面显示为“在线，等待连接”，协议和诊断状态枚举不变。Mac 等待首个有效非空 AU 最多 32 分钟；首 AU 到达后无固定总时长或累计字节上限，心跳、组包、单包、队列及解码在飞限制仍生效。本地开发回放仍限制 64 MiB，见 [协议](docs/PROTOCOL.md)。

## 使用纯文字剪贴板

1. 两端使用配套构建，在可信局域网中进入 Host 的**远程桌面**页，点击**开启服务**；Mac 在**远程桌面**页输入设备地址和配对码，点击**连接设备**。Host 需再点击**开始共享屏幕**并确认系统录屏授权。
2. Host 点击 **允许读取剪贴板**并确认页面显示已允许（鸿蒙 → Mac 所需；系统已授权时可能没有新弹窗），再点击 **允许剪贴板同步**；Mac 在**设置 → 文字剪贴板**选择所需方向，也可使用会话工具栏的**剪贴板**菜单。默认不读取或交换原有内容，之后重新复制文字才触发同步；**复制远端文字到本机**是显式拉取当前内容。
3. 普通同步只更新另一端剪贴板。远端粘贴还需 Host 点击**授权键鼠权限**、双方启用远程控制及 Mac 画面焦点。Mac → 鸿蒙开启时，`⌘V` / `Ctrl+V` 对新 Mac 内容等待写入确认；已同步且未变化的内容复用当前事件，由 Host 再核对当前剪贴板版本后粘贴。开启后须重新复制或收到远端更新，显式粘贴也不会发送开启前的旧内容。远程复制/剪切尚未得到新内容、失败或失焦时会取消，不用旧内容覆盖远端。

`READ_PASTEBOARD` 需要签名 Profile 中的 ACL 及运行时用户授权；声明权限或成功构建均不等于授权成功。Mac 也可能要求系统剪贴板访问许可。见[权限与能力边界](docs/PLATFORM_CAPABILITIES.md)及[剪贴板协议](docs/CLIPBOARD_WIRE.md)。控制、视频与剪贴板仍为可信局域网明文传输；诊断不保存剪贴板文本、文本摘要或配对凭据。

## 历史实机证据与未完成验收

- **0.4.1 五分钟会话已完整结束：`PASS_THIS_DEVICE_5_MINUTE_VIEWING_AND_USER_DRAG_RETEST`**。Host 编码/发送、Mac 接收/解码均 **9001 帧**，采集窗口 **300.021 秒 / 29.995 FPS**，`streamCompleted=true`、`eosSent=true`，无记录错误，未保存本地录像。Mac 显示提交 **8207** 次、提交前替换 **794** 帧，不能将其作为物理刷新或延迟测量。见 [本批完整会话复核](artifacts/device/drag-0-4-1-completed/session-review.json)。
- **0.4.1 拖拽实机通过：`PASS_THIS_DEVICE_WINDOW_DRAG_AND_TEXT_SELECTION`**。用户在 Mac 操作，确认窗口拖动、文字拖选和松手立即停止；修复在 Host MOVE 事件中保留已按下按钮的身份。关闭控制后 `queuedEvents=0`、`busy=false`、全部 pending 为 0、`lastReleaseCode=0`，本批无输入失败。文件拖动未在新版独立复测，多按钮组合拖动也未验收。见 [0.4.1 拖拽证据](artifacts/device/drag-0-4-1-verified/drag-review.json)。
- **0.4.1 构建、部署与计时修复历史**：Host versionCode `1000006` 当时已安装启动，Mac 验收时确认运行的是本次归档二进制（PID `96911`）；原生输入 **32 项**、Mac 网络 **97 项**开发测试通过。该版本 Mac 等待首 AU 和收到首个有效非空 AU 后分别有 32 分钟期限，后续帧不延期，过期等待不能被迟到帧复活；不会因手动等待共享而缩短完整 30 分钟接收窗口。见 [构建、签名、源码及部署验证](artifacts/build-verification-0.4.1.json)。[早期 91 项源码验证](artifacts/deadline-fix-verification/verification.json) 仅作修复过程的历史记录；0.5.0 的活动会话已取消此固定期限。
- **0.4.0 部分远程输入已实机确认**：用户英文输入、右键菜单、Mac Friendly `⌘L` 均成功；当时窗口、文件和文字拖拽失败，后续窗口与文字拖拽由 0.4.1 修复并复测。目标应用效果来自用户操作确认，不能用输入 API 计数代替。见 [0.4.0 输入记录](artifacts/device/control-live-take-03/input-review.json)。
- **0.4.0 五分钟实时查看通过：`PASS_THIS_DEVICE_5_MINUTE_VIEWING`**。Host 编码、Mac 接收/解码均 **8985 帧**，采集窗口 **300.023 秒**、原生平均 **29.941 FPS**，1620×1080、Mac 硬解确认、无显示错误；显示层提交 **7698** 次、提交前替换 **1287** 帧，二者不是物理刷新计数。EOS 与编码结束成功，`localRecordingEnabled=false`。本批 Host LAN 计数在最终导出前因服务重启被清零，因此依据编码会话身份与 Mac 原始报告关联，不将重启后的 LAN 计数冒充本次发送数。见 [五分钟独立核对](artifacts/device/control-live-take-01/five-minute-review.json)、[Mac 原始诊断](artifacts/device/control-live-take-01/mac-viewer-five-minute.json) 和 [Mac 实时查看结果](MAC_VIEWER_TEST_RESULT.md)。
- **0.3.0 短时 GUI 实时查看历史保留**：用户和助手已确认完整桌面，Host 编码/保存/发送与 Mac 接收/解码均 **303 帧**。Mac 解锁后，从保留原会话的进程补导出 GUI 原始 JSON，记录 **242** 次显示提交、**61** 次帧替换、硬解正常、无显示错误；这不是新会话或 0.4.0 验证。见 [补导出记录](artifacts/device/lan-gui-take-01/live-export-supplement.json)。
- **0.4.0 构建历史**（versionCode `1000005`）：原生输入 28 项、Host LAN 31 项、Mac 网络 83 项、Mac 输入 21 项，以及 CLI 兼容 28 项均通过。初次启动诊断为服务 STOPPED、录屏 idle、远程控制关闭，无已接受输入或待释放按键；这些是该版本当时的状态。见 [历史构建/测试/部署验证](artifacts/build-verification-0.4.0.json)。
- **剩余验收**：文件拖动、新版多按钮组合拖动、滚轮方向、持键/持按钮时失焦或网络断开释放、完整快捷键矩阵，以及 0.5.0 十分钟完整会话、永久模式长期运行及内存增长仍待真机检查。上一轮 0.4.0 长会话由用户在 **1438.943 秒（约 24 分钟）**时停止，不能算作 30 分钟完成；见 [停止会话复核](artifacts/device/control-live-take-03/stopped-session-review.json)。[下一步验收步骤](docs/REMOTE_CONTROL_ACCEPTANCE.md)；历史归档：[0.4.1 Mac App](artifacts/releases/0.4.1/mac-build/HarmonyRemote.app)、[Mac ZIP](artifacts/releases/0.4.1/HarmonyRemote-Mac-0.4.1.zip)、[当时安装的 Host HAP](artifacts/releases/0.4.1/HarmonyRemote-Host-0.4.1.hap)。
- **0.3.0 真机 LAN 功能通过：`PASS_THIS_DEVICE_LAN_H264_TRANSFER_AND_MAC_DECODE`**。用户完成 PIN 配对及系统录屏授权，Mac CLI 收到正常 EOS；Host 写入/发送、Mac 接收/完整解码均 **287 帧**。两端码流均 **1,730,857 字节**，共同 SHA-256 `989a46975e95a4e41460f4c6af16e05c00b74c836ac7d70c2a8e71d80af16f9d`。Host 原生 **10.036 秒 / 28.397 FPS**，Mac 首末 AU 接收 **9.994878 秒 / 28.614657 FPS**；计时口径不同。序号无缺口、发送和接收无记录错误，Host `streamCompleted=true`、无取消/中止；中间帧 143 已确认整个桌面及前台动态图案。见 [LAN 结果](LAN_TEST_RESULT.md) 与 [独立核对](artifacts/device/lan-take-01/lan-review.json)。
- 0.3.0 构建、签名校验、安装和启动通过，versionCode `1000004`；服务端 **25/25**、接收端 **28/28** 本机测试通过（含真实 C++→Python fixture）。这些开发检查与上述真机 LAN 证据分别记录。见 [构建验证](artifacts/build-verification-0.3.0.json)、[操作步骤](docs/TESTING.md#8-phase-1pin-配对与-lan-视频接收) 和 [协议](docs/PROTOCOL.md)。

- 已确认本机 DevEco Studio `26.0.0.821`、SDK `26.0.0.105 / API 26`，SDK 中存在所需 C API 与 arm64 库。
- 工程目标与最低版本均为 `26.0.0`，设备类型为 `2in1`，包名 `com.longxin.harmonyremote.probe`。
- 2026-10-03 初版 **0.1.0** 实际 ArkTS/C++ 编译与 HAP 打包成功，输入释放/故障逻辑测试 17/17 通过。[构建验证](artifacts/build-verification.json) 含产物 SHA-256、源码指纹与验证边界；[构建日志](artifacts/build.log) 保留 Hvigor 实际输出。17/17 为初版结果，不代表新版测试总数。
- 新版 **0.1.1** 已构建、签名、安装并启动，输入逻辑测试 **22/22 通过**；见 [新版构建验证](artifacts/build-verification-0.1.1.json)。新增自动保存和最多 32 条操作历史；最终真机导出已保留 20 个完成动作，确认多动作历史可用。
- **0.2.0 首轮** Surface 硬件 H.264 探针完成真机录制和 Mac 完整解码：`OMX.hisi.video.encoder.avc`，10.008 秒、224 帧、1620×1080、平均 22.283 FPS；Mac 解码同为 224 帧。当时结论 **HARDWARE_ENCODE_AND_MAC_DECODE_PASS_PERFORMANCE_GATE_INCOMPLETE** 作为历史保留，见 [首轮独立复核](artifacts/device/encoder-take-01/encoder-review.json)。
- **0.2.1 动态画面复测已完成**，Gate C 为 **PASS_DYNAMIC_SCENE_1080_HEIGHT_APPROX_30FPS**：同一硬件编码器，停采集时 300 帧 / 10.006 秒，原生间隔口径平均 **29.882 FPS**；最终编码、写入和 Mac 完整解码均 **301 帧**、**1620×1080**。前 10 个完整一秒桶各 29–31 帧，最大回调间隔 67.768 ms，应用队列丢弃/错误为 0，EOS 完整。主 agent 查看中间帧 150，确认前台动态图案、桌面及其他窗口。该功能结论不要求平均值机械达到 30.000，也不代表恒定逐帧 30 FPS 或完整 1920×1080。见 [编码验证结果](ENCODER_TEST_RESULT.md)、[本批独立复核](artifacts/device/encoder-take-02/encoder-review.json) 和 [新版构建验证](artifacts/build-verification-0.2.1.json)。
- **PTS 诊断修复已有真机证据，原始单位仍未确认**：take-02 schema v2 的 `rawUnitVerified=false`，原始差值 `10040993906` 按 SDK 微秒声明对应 10040.994 秒，纳秒候选对应 10.041 秒，而独立帧输出单调时钟跨度为 10.042 秒。报告明确分开声明、候选和实测，不自动归一化单位；帧率使用独立单调时钟。两轮版本和画面场景不同，不能据此确定首轮降帧原因；旧原始证据保持不变。
- 最新真机：`192.168.31.130:35029`，MOR-M1 / HUAWEI MateBook Pro S，API 26、2in1、aarch64；系统参数返回 `OpenHarmony-7.0.0.105`。设备签名问题已解决，signed HAP 已通过校验、安装并启动。
- **Gate A 最低采集门槛通过**：30.091 秒 / 425 帧，样本已确认包含桌面、任务栏和其他应用；实际 1620×1080，平均 14.09 FPS，该原始帧采集批次未达到持续 30 FPS 目标。**Gate B 为 `PASS_THIS_DEVICE_INPUT_PROBE`**：用户确认本设备上的授权、鼠标移动、Host App/浏览器/系统设置/备忘录英文 `a`、左键图标选中、Ctrl+L，以及撤销/拒绝后不能输入。20 条历史保留按下/抬起配对和两次门禁 `201` 阻断，最终 `UNAUTHORIZED`、无待释放输入。应用归属来自用户回答，JSON 不记录目标窗口。最新输入证据见 [Gate B 最终人工核对记录](artifacts/device/input-gate-final-01/input-review.json)，此前两轮归档保留不变；整个远控产品仍待开发和验收。
- Gate A/B 状态见 [CAPTURE_TEST_RESULT.md](CAPTURE_TEST_RESULT.md) 和 [INPUT_TEST_RESULT.md](INPUT_TEST_RESULT.md)。SDK 声明、编译成功、模拟测试均不能代替真机 PASS。

## 构建与安装

本机常用启动路径为 `client-macos/build/HarmonyRemote.app`，从项目根目录运行 `open -n client-macos/build/HarmonyRemote.app`。界面候选版需核对 Host `1000010` / Mac `7`；本轮构建、空闲界面检查及安装结果见[工作区验证记录](docs/UI_WORKSPACE.md)，源码发布状态见[版本来源](docs/VERSION_HISTORY.md)。此前 rc.1 在应用关闭时开发和部署，其构建证据仍独立保留。此前会话模式基线的签名、源码指纹和部署见 [0.5.0 基线构建记录](artifacts/build-verification-0.5.0.json)，原归档 [Mac ZIP](artifacts/releases/0.5.0/HarmonyRemote-Mac-0.5.0.zip)、[Host HAP](artifacts/releases/0.5.0/HarmonyRemote-Host-0.5.0.hap) 保留，不将它们当作新增剪贴板的产物。已验收的 0.4.1 历史归档仍保留在 `artifacts/releases/0.4.1/mac-build/HarmonyRemote.app`，ZIP 位于其上一层。

在项目目录运行：

```bash
python3 scripts/audit-environment.py --output artifacts/environment.json
bash scripts/build-hap.sh
```

脚本使用已安装 DevEco 的 Node/JBR/OHPM/Hvigor。输出在 `host-harmony/entry/build/default/outputs/default/`。未配置本项目签名时只生成 `entry-default-unsigned.hap`，不能把它视为可安装包。

在 DevEco Studio 打开 `host-harmony`，连接鸿蒙 PC，在本项目的 Signing Configs 中配置该设备的调试签名，然后重新构建。此工程不复用其他应用的证书与 provisioning profile。

如果安装返回 `9568423`，说明 HAP 签名 Profile 不包含目标设备 UDID。保持目标设备连接，在本工程重新生成自动签名，再重新构建。仅生成证书或关联 `signingConfig` 不能补齐设备授权；当前产品已关联用户创建的 `default` 签名。

```bash
python3 scripts/device-check.py --target '实际 HDC 设备 ID'
python3 scripts/install-hap.py --target '实际 HDC 设备 ID' \
  --hap host-harmony/entry/build/default/outputs/default/entry-default-signed.hap
```

无线调试需先按设备页面显示的地址与端口连接；安装脚本不会扫描局域网或自动选择未知设备。HDC 仅用于安装、日志与证据导出，不是产品的输入控制通道。

## 测试录屏

以下为侧边栏**开发测试**页中的独立采集探针，在两种会话模式下均可进入。先保持局域网服务下线；它们不会随配对、共享或页面切换自动运行。

1. 在 PC 上打开 **Harmony Remote**，进入**开发测试**，点击**录屏测试 · 30 秒**，在系统授权界面选择整个主屏幕。
2. 允许后切换到桌面/其他应用。计时从首帧开始，30 秒后自动停止；授权及首帧等待最多 60 秒。
3. 探针统计帧数、实测平均 FPS、回调时间戳、错误码和状态。输出保持屏幕比例，不超过 1920×1080，不放大小屏幕；请求 30fps、显示光标、无音频。
4. 首帧后约 5 秒或第 150 帧（先到者）保存一帧 `capture-frame.ppm`，不持续保存整个桌面录像。只复制这一帧，磁盘写入在 worker 进行。
5. `capture-probe.json` 自动保存运行过程及最终结果。至少 300 帧、连续 30 秒、图像保存成功后，仍需人工查看图像确认是整个桌面。程序不会自行标记 PASS。

若保存时尚未切换到桌面，请重新运行。图像尺寸、格式和字节 stride 来自真实 NativeBuffer。SDK 不提供本探针所需的完整丢帧计数，未把未知丢帧写成 0。

## 测试全局输入

以下为独立**开发测试**页中的本地输入探针，服务运行时禁用；实际 Mac 远程输入按 [远程控制验收](docs/REMOTE_CONTROL_ACCEPTANCE.md) 检查。

点击**请求输入权限**并在系统弹窗中确认；以开发测试页显示的 `输入权限：AUTHORIZED` 为准。请求返回 0 仅表示请求受理。当前 Mode A 使用 `OH_Input_RequestInjection`，未声明或假定持有 `CONTROL_DEVICE`。

点击动作后有 5 秒倒计时，用于切到测试目标：

- **鼠标移至中心**：移动到实际主屏中心。
- **输入 A**：A Down → A Up；需要聚焦另一应用的空白文本框。
- **中心左键点击**：移动到主屏中心，再左键 Down → Up；测试前确认中心位置是安全的目标。
- **Ctrl+L**：Ctrl Down → L Down → L Up → Ctrl Up；可在浏览器验证地址栏获得焦点。
- **取消动作 / 释放按键 / 撤销授权**：取消倒计时，尝试释放残留按键，再撤销系统输入授权。

每次注入前重新检查授权，Down 失败也尝试 Up；Up 失败会保留待释放状态并在下一次动作前重试。按键/鼠标释放失败的原始错误码会显示。普通应用文本输入取决于当前键盘布局、焦点与输入法，不能只用 API 返回 0 判定成功。

从 **0.1.1** 开始加入的诊断机制在每个输入动作完成及授权状态变化后自动保存，开发测试页的**保存当前诊断**按钮可手动保存 `input-test.json`、`capture-snapshot.json`、`device-info.json`。输入报告保留最近动作，并包含本进程最近最多 32 个已完成动作的历史，每项带 Unix 毫秒时间、各步骤及返回码、待释放输入状态。新版还提供本应用空白测试输入框，用于补测 Host App。进程重启不会恢复进程内历史；自动保存也不代表动作在目标应用中生效。0.1.1 已通过 ArkTS/C++ 构建、签名校验、安装和进程启动检查，22/22 本地输入逻辑测试通过；`input-gate-final-01` 真机导出已确认自动保存后保留了 20 个完成动作，尚未真机跑满 32 条容量。

每个目标测试后使用 `python3 scripts/collect-diagnostics.py --target '实际 HDC 设备 ID' --label browser-inject-a-01` 分别导出，后续测试更换 label。脚本拒绝覆盖旧归档；设备端诊断文件则会随自动/手动保存更新。文件目录显示在开发测试页，已核准实际目录为 `./data/storage/el2/base/haps/entry/files`；也可用 DevEco 应用文件工具导出。`capture-frame.ppm` 可以用 Mac 图像工具查看或转换成 PNG。旧批次 `input-check-01` 来自 **0.1.0**，仅保留最近一次动作；不会回写或补生历史。

## 验证边界与下一步

详见 [环境检查](docs/ENVIRONMENT.md)、[真机验收步骤](docs/TESTING.md)、[录屏 SDK 核对](docs/SDK_CAPTURE_AUDIT.md)、[输入 SDK 核对](docs/SDK_INPUT_AUDIT.md)。

0.3.0 仅在用户点击 Start Server 后监听所选局域网地址的 39871/39872 端口，该历史版本没有网络输入注入。原始录屏单次 30 秒，H.264 编码探针单次 10 秒，二者互斥；输入动作只由用户明确点击触发。应用退出释放录屏/编码资源并尝试撤销输入授权。

桌面采集、本设备跨应用输入、硬件 H.264 编码、真实 LAN 传输和 Mac 完整解码均已有证据，PTS 诊断表达修复也已实测生效。0.2.1 本地动态编码 29.882 FPS 与 0.3.0 首轮 LAN 录制 28.397 FPS 分别保留，功能通过不等于恒定 30 FPS。Phase 1 使用独立编码回调单调时钟的相对微秒进行封包；原始 PTS 单位仍未确认，这个时间戳不能表示采集时刻或端到端延迟。裸流 FFprobe 的帧率/时长也不代表实际供帧率。Mac 0.3.0 短时 GUI live、0.4.0 五分钟查看及部分网络输入、0.4.1 窗口拖动与文字拖选分别有证据；0.5.0 已构建，但十分钟完整会话、永久模式长期运行和完整输入矩阵仍待真机验收。现有内存采样没有完整会话基线，不能证明长期内存不增长；显示提交不能代替实际屏幕刷新。本轮未验证完整 1920×1080、恒定帧间隔或端到端延迟，不将完整远控产品视为通过，也不作整体 `REMOTE CONTROL FEASIBLE` 声明。

密码等隐私区域可能被系统遮蔽；输入法候选浮窗是否被采集需真机确认。不会绕过隐私提示或切换到 Accessibility、USB/蓝牙 HID、HDC 输入自动化等其他路线。
