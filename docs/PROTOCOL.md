# LAN video, input, clipboard and audio protocol — version 1

本文定义沿用到 **0.6.0-rc.1（Host `1000011` / Mac `8`）** 的基础格式：Harmony 硬件编码通过视频 TCP 连接传输到 Mac Viewer；独立控制 TCP 连接承担配对、心跳及可撤销的远程输入，协商后第三条连接承担纯文字剪贴板，第四条承担系统声音。0.6 新增的身份握手、采集恢复与 RTT 以[会话可靠性扩展](SESSION_RELIABILITY.md)为准；音频格式见[音频与帧率](MEDIA_AUDIO_FPS.md)。下文简化的 PIN 握手继续作为旧客户端兼容路径，新 Mac 使用已协商的签名握手。协议版本仍为 **1**，HRD1 视频格式与 0.3.x 兼容，输入格式沿用 0.4.0、由 hello capability 协商。当前剪贴板构建、安装与验收状态见[剪贴板验证记录](CLIPBOARD_TEST_RESULTS.md)；此前 [0.5.0 会话模式基线部署记录](../artifacts/build-verification-0.5.0.json) 不代表本轮剪贴板已验收。十分钟完整共享、永久模式长期运行尚未真机验收。

0.4.0 已实机确认文字输入、右键菜单和 `⌘L`，0.4.1 已确认窗口拖动、文字拖选与松手停止；这些历史结果不等于完整输入或长期稳定性验收，见 [拖拽证据](../artifacts/device/drag-0-4-1-verified/drag-review.json)。本机协议测试与真机结果分别记录。

## 范围与启动

- 仅用于可信局域网原型。控制、视频、音频与剪贴板均为**明文 TCP**，PIN/session 校验不提供加密或抵御局域网窃听。0.6 已加入持久签名身份；TLS 1.3 与传输加密尚未实现。
- 不使用端口转发、公网暴露、UPnP、NAT-PMP。Mac 客户端只接受 RFC1918 IPv4：`10.0.0.0/8`、`172.16.0.0/12`、`192.168.0.0/16`，不解析主机名。
- Host 默认需点击**开启服务**才监听；用户可另外选择打开应用后自动开启服务。控制端口 `39871/TCP`，视频端口 `39872/TCP`，剪贴板端口 `39873/TCP`，音频端口 `39874/TCP`；只允许一个配对客户端，已有会话时拒绝新客户端。剪贴板或音频监听失败不停止原有视频/输入服务，Host 只在相应服务就绪时宣告该 capability。
- Host 的 API 26 模块已声明 `ohos.permission.INTERNET`。这与用户另行确认系统录屏授权是不同的能力边界。
- 每次 Start 和已配对会话断开后生成新的 6 位随机数字 PIN，5 分钟过期、成功后单次消费；最多 5 次错误尝试。PIN 不得硬编码。
- 成功配对生成密码学随机的 256-bit token，以 64 位十六进制字符串传输，仅当前会话有效；断线/Stop 后失效。0.6 可保存独立设备签名身份；每次恢复仍生成新的会话 token，不保存或复用旧 token。
- 配对及 `video_ready` 只建立会话。用户还需在 Host 选择共享模式、点击 **开始共享屏幕**并完成系统录屏授权，才开始这次视频传输；不得绕过授权。
- Host 的 LAN 模式仅有 **10 分钟调试（600 秒）**和 **永久上线（0，无自动结束时间）**，默认永久。`PersistentStorage` 的 `hrdSessionMode` 只保存模式选择，不保存配对、系统授权或允许远程控制开关。调试模式仅在开发测试页显示动态图案；两种模式均可打开开发测试页，均不保存本地 H.264。重连是单独的可关闭功能；同一认证身份可在 60 秒保留期内继承尚未结束的采集，不能越过系统授权、到期或本地停止。
- `LISTENING` 在 Host 页面显示为“在线，等待连接”；线上字段和诊断状态枚举不变，模式也不改变以下握手格式。

## 控制通道

所有控制消息与视频连接的认证握手消息使用同一 framing：

```text
uint32 payloadLength, big-endian
payloadLength bytes, UTF-8 JSON object
```

长度必须为 `1..4096`。TCP read/write 次数与消息边界无关，必须完整组包；拒绝超长、截断、无效 UTF-8/JSON、重复字段和未知消息。帧首字节到完整 body 的组装有 2 秒期限。生产网络发送同样必须有期限，不能无限阻塞。

握手顺序：

```json
{"type":"hello","protocol":1,"client":"macOS","clientVersion":"0.6.0"}
```

```json
{"type":"hello_ack","protocol":1,"pairingRequired":true,"timestampSource":"encoder_callback_monotonic","nativePtsUnitVerified":false,"inputSupported":true}
```

`hello_ack` 可以附加设备名称等非敏感字段；客户端必须检查上述版本与时间戳语义。仅安装完整输入 hooks 的 Host 报 `inputSupported:true`。旧 Host 缺少此字段时 Mac 只看画面； capability 不代表用户已授权输入。新增剪贴板能力用可选 `clipboardSupported:true`、`clipboardPort:39873` 协商；新客户端只在有效 capability 存在时开第三条连接，旧客户端可忽略新增字段。

```json
{"type":"pair","pin":"<6 ASCII digits>"}
```

```json
{"type":"pair_ok","sessionToken":"<64 hex characters>"}
```

PIN 通过 Mac GUI 隐藏输入框、CLI 终端隐藏输入或显式的受控 stdin 获取，不作为 argv/env，不写日志或报告；GUI 提交后清空输入框。PIN 与 token 只保存在当前会话内存中；剪贴板就绪时，`pair_ok` 另附 `clipboardEpoch` 和独立单次 `clipboardBindToken`，同样不得持久化或记录。现代签名握手还携带公钥、独立挑战和签名，不传设备序列号等额外硬件标识。

配对后两端每 2 秒发送 `ping`，接到有效 `ping` 返回 `pong`；超过 6 秒没有有效 `ping/pong` 则关闭当前传输会话；符合条件的采集进入恢复保留期。`rttSupported:true` 时 Mac ping 附加 16 位十六进制 `probeId`，Host pong 原样回显用于匹配单调时钟 RTT。以下每条消息都必须携带完全匹配的 token：

```json
{"type":"ping","sessionToken":"<token>"}
{"type":"pong","sessionToken":"<token>"}
{"type":"stop","sessionToken":"<token>"}
```

不要求 `stop_ack`。Host 收到 stop 后关闭控制、视频、音频及已建立的剪贴板连接并清除会话，终止采集保留期。认证前错误为 `{"type":"error","error":"<reason>"}`；认证后错误还须携带 sessionToken。接收器只保存固定错误分类，不回显服务端提供的任意文本，以免把认证材料带入日志。

## 纯文字剪贴板扩展

完整字段、顺序与限制见 [CLIPBOARD_WIRE](CLIPBOARD_WIRE.md)，权限和真实设备验证边界见 [PLATFORM_CAPABILITIES](PLATFORM_CAPABILITIES.md)。剪贴板只提供 **关闭 / Mac → 鸿蒙 / 鸿蒙 → Mac / 双向**四种模式，首次默认关闭，Mac 可记住方向偏好，Host 本地允许开关独立。方向/允许状态改变及重连只建立当前版本基线，不主动发送原有内容；显式 `clipboard_pull` 是例外的用户拉取动作。

配对时第三条连接使用独立、30 秒过期的单次绑定 token，先 `data_bind` → `data_ready`，可以在关闭模式下完成绑定。Host 允许后才按协商方向同步。鸿蒙读取使用 `READ_PASTEBOARD`，需正确签名 ACL 和运行时用户授权；网络不会发起授权弹窗。Mac 剪贴板访问同样受系统权限约束。

每条剪贴板消息是 `uint32BE` JSON 头长度 + 1..4096 字节头 + 已验证的原始 payload，最多 **1 MiB UTF-8 纯文字**；不保排版，拒绝 NUL、错误编码及文件/图片剪贴板。文件传输和文件粘贴不在本版。此通道的组包/发送期限为 **5 秒**，独立于控制/视频的 2 秒期限；在途更新及最新待发更新有界，旧 epoch、版本、迟到 ACK 及模式变化须受顺序检查。剪贴板通道故障只关闭该能力，画面与输入继续。

Mac → 鸿蒙启用时，`⌘V` / `Ctrl+V` 必须先收到对应 `clipboard_applied`，再在原控制通道发送 `paste_commit`；执行前重查焦点、会话、期限和剪贴板版本，失败不回落到 raw V。`paste_result=committed` 只证明注入完成，不证明目标应用粘贴成功。诊断仅包含计数、字节量及错误，不记录 payload、内容摘要、PIN 或绑定凭据。

## 远程输入（沿用 0.4.x 格式）

输入默认关闭，配对成功不自动启用。Host 本地必须先完成系统输入授权并允许本会话，网络不能触发授权弹窗。Mac 仅在已配对、视频通道就绪且 Host 正在传输时请求启用；Host 还逐次检查本地允许状态。每条输入消息都有当前 `sessionToken`，不接受前一会话 token。

```json
{"type":"input_enable","enabled":true,"sessionToken":"<token>"}
{"type":"input_status","enabled":true,"sessionToken":"<token>"}
{"type":"mouse_move","x":0.75,"y":0.25,"sessionToken":"<token>"}
{"type":"mouse_button","button":"left","action":"down","sessionToken":"<token>"}
{"type":"mouse_button","button":"left","action":"up","sessionToken":"<token>"}
{"type":"scroll","dx":0,"dy":-1,"sessionToken":"<token>"}
{"type":"key","code":"KEY_CTRL_LEFT","action":"down","sessionToken":"<token>"}
{"type":"key","code":"KEY_CTRL_LEFT","action":"up","sessionToken":"<token>"}
{"type":"release_all_keys","sessionToken":"<token>"}
```

- `input_enable.enabled` 必须是 JSON 布尔值；`true` 必须同时满足 streaming、videoReady、已接收本轮首个有效视频 AU、本地授权/允许。仅 BeginStream 或 CONFIG 阶段仍拒绝输入。`false` 禁用并释放全部远程按键与按钮。Host 回复带 token 的 `input_status`，即使启用请求被本地状态拒绝也回复 `enabled:false`。
- `mouse_move.x/y` 是画面左上角为原点的归一化坐标，必须为有限 JSON 数字且在 `[0,1]`。Mac 从去除黑边后的有效视频区域映射；Host 映射到实际主屏。只有 Move 更新光标位置。Button/Scroll 使用最近的 Move 位置，启用后首次 Button/Scroll 前必须有成功 Move；不接受这些消息自行带 x/y。
- `button` 只允许 `left/right/middle`；`action` 只允许 `down/up`。按下/释放按发送顺序保留，不将自动重复伪装成多个 down。0.4.1 Host 使用已跟踪的按下状态为 MOVE 设置按钮身份，按钮抬起后恢复无按钮移动；wire Move 无须增加按钮字段。多个按钮同时按下时当前按左、中、右优先选择单个 MOVE 按钮 ID，多按钮组合拖动尚未实机验收。
- `scroll.dx/dy` 是滚轮单位，两个轴均须为有限 JSON 数字且各在 `[-120,120]`；不是像素坐标。Mac 精细滚动按 10 points = 1 单位换算，普通滚轮使用原 delta。
- `key.code` 是物理键语义白名单，不发送字符文本、NSEvent 整数 keyCode 或任意系统键码。允许 `KEY_A…KEY_Z`、`KEY_0…KEY_9`、`KEY_F1…KEY_F12`；其余名称见下表。不存在 `KEY_CTRL` 等未分侧别名。
- `release_all_keys` 释放所有远程按键和按钮并清除待处理输入；成功时保留已启用会话。失焦/离开输入捕获使用它；关闭控制使用 `input_enable:false`。此释放操作即使当前已禁用也可重复调用，不需要回复。
- 未启用时收到合法的 Move/Button/Scroll/Key，Host 回复 `input_status:false`，不注入。非法字段、数字范围、未知键/按钮、认证错误或 native 提交失败都关闭会话并释放输入，不尝试纠正消息。
- Host 每约 100ms 查询 `enabled()` 检查本地撤销/系统权限变化，变化后推送 `input_status:false`。每次输入提交也检查 enabled。断线、Stop、编码结束、EOS、失败都关闭逻辑输入门并释放；已完成视频 EOS 后的正常断线仍保留视频成功状态。

| 键组 | 除统一 `KEY_` 前缀后的名称 |
|---|---|
| 标点 | `MINUS EQUALS LEFT_BRACKET RIGHT_BRACKET BACKSLASH SEMICOLON APOSTROPHE GRAVE COMMA PERIOD SLASH` |
| 编辑/导航 | `ENTER ESCAPE TAB SPACE BACKSPACE DELETE UP DOWN LEFT RIGHT HOME END PAGE_UP PAGE_DOWN` |
| 修饰 | `SHIFT_LEFT SHIFT_RIGHT CTRL_LEFT CTRL_RIGHT ALT_LEFT ALT_RIGHT META_LEFT META_RIGHT CAPS_LOCK` |

Mac 输入邮箱与待发送消息有界，最多 128 条未完成输入；只合并相邻尚未发送的 Move，不跨越按钮/按键边沿、滚轮、启用或释放消息。控制发送一次只保留一个在途 NWConnection send，并限制其队列为 128；超限断开并释放，不静默丢按键。每批输入绑定接收时的连接 generation，重连会清空旧批次。显式断开或正常结束时，同一 TCP 顺序发送 `release_all_keys` 后 `stop`；传输故障直接关闭连接，使 Host 清理输入并按恢复规则保留采集，不能错误地发送 stop 提前终止它。发送失败时 Host 断线清理仍负责释放。

输入 hooks 由独立串行门调用，不持 Host server 状态锁，允许 native 查询/释放等待。Server 诊断只包含 `inputSupported/inputEnabled/inputAccepted/inputRejected/inputReleases`；Mac 只增加 `inputSupported/inputEnabled/inputSent`（成功交给 NWConnection 的输入控制消息数，包含启用/释放），不记录键名、文本、PIN 或 token。


## 视频认证与二进制帧

配对完成后，Mac 建立第二条连接到 `39872`，首先发送一个上述长度 framing 的 JSON：

```json
{"type":"video_attach","sessionToken":"<same control session token>"}
```

Host 必须将视频连接绑定到有效且唯一的控制会话。未经认证不发送任何视频数据。成功后返回一个 framed JSON：

```json
{"type":"video_ready"}
```

其后立即切换为二进制视频协议，不再混用 JSON。每包是 24 字节 header + payload，所有整数采用网络字节序：

| Offset | 字段 | 类型 | 约束 |
|---:|---|---|---|
| 0 | magic | 4 bytes | ASCII `HRD1` |
| 4 | version | uint8 | `1` |
| 5 | type | uint8 | `1` config、`2` AU、`3` keepalive |
| 6 | flags | uint16 | bit 0 KEYFRAME，bit 1 EOS；其余为 0 |
| 8 | sequence | uint32 | 每条视频连接从 0 开始，所有类型均逐包 +1，按 uint32 回绕 |
| 12 | pts_us | uint64 | 见下方时间戳定义 |
| 20 | payload_len | uint32 | 上限 8 MiB；config 进一步限制到 256 KiB |

Python layout 为 `struct.Struct("!4sBBHIQI")`。不要直接发送有 ABI padding 的 C++ struct。

- **VIDEO_CONFIG = 1**：flags 为 0，非空 Annex-B，至少含 SPS 与 PPS，不含 VCL slice。每个新视频连接及编码参数变化必须重发 config。GUI 将 config 交给解码器；旧 CLI 取证接收器将其写入 H.264 文件。
- **VIDEO_ACCESS_UNIT = 2**：一个非空 payload 是一个完整 Annex-B access unit，可含多个 NAL；不得把任意 TCP 分片当成一帧。KEYFRAME 标记与 IDR NAL 对应。每个 config 后第一 AU 必须是 IDR。其后编码顺序保持不变。
- **VIDEO_KEEPALIVE = 3**：flags=0、payload_len=0，不算视频帧；不能替代控制心跳。
- **EOS**：只允许 type=2。空 EOS 使用 flags=2、payload_len=0；也允许最后一个完整 AU 同时带 EOS，KEYFRAME 取决于该 AU。EOS 必须位于至少一份有效 config 和至少一个 IDR/AU 之后。收到 EOS 后流完成；EOF 本身不是 EOS。
- 控制会话正常保持到接收器发出 stop。完整 EOS 后正常关闭不撤销已经收到的流；EOS 前的断线、组包错误、序号跳变、无效会话都属于失败。

## 时间戳边界

`pts_us` 当前为**本轮首个编码 output callback 为零点的 steady clock 到达时间，单位微秒**。Host 从已知单位的单调时钟计算它，不把 SDK 返回的原始 `OH_AVCodecBufferAttr.pts` 未经核实地放入该字段。

这不是原始采集 PTS，也不是 native raw PTS；不能用它计算采集延迟、跨机器时钟差或端到端延迟。`hello_ack.nativePtsUnitVerified=false` 必须保持。原始 PTS 与纳秒候选仍留在 Host 编码诊断中。AU 时间戳要求非递减，config/keepalive/空 EOS 不参与 AU PTS 统计。

当前 Mac CLI 按接收顺序落盘，不做播放 pacing。裸 H.264 文件不保留此 wire header 的 PTS；FFprobe 的推定帧率与时长不能替代 Host 和接收端的单调时钟统计。

## 队列、超时与释放

- Capture/encoder callback 只拷贝到有界队列并及时归还 AVBuffer，不做阻塞 socket send；发送由网络线程执行。
- 待发送队列最多 **3 个 AU**，另有字节/单包上限。当前验证版在队列满或发送超时时**中止此次 trial**；不会隐形丢弃参考帧后继续宣称流完整。原 PRD 的丢旧 non-IDR 策略需要完整的 IDR 恢复机制，未在本阶段引入。
- 任何 invalid session、格式错误、断线、录屏拒绝/中断、编码错误、显示变化必须进入可见失败状态并释放资源。Host **停止共享**先停 producer 并排空编码 EOS；**Stop Server** 关闭服务，可能在排队 EOS 发完之前断开，不保证接收端正常 EOS 结束。两者先禁用并释放远程输入。网络自身先断线时，`IsStreamCancelled` 通知 encoder worker 收尾，不能继续生产到失效的连接。旧 CLI 不发送输入事件，Mac Viewer 使用上述显式启用流程。
- Host LAN 仅接受 **600 / 0 秒**，均禁止本地录像。600 秒从首帧开始计时；0 不触发时长结束，仍会因手动停止、连接中断、关闭应用或错误结束。旧 LAN 时长 10/300/1800 秒不再接受；独立 10 秒本地编码探针仅在开发面板手动启动，不使用网络 hooks。
- 0.5.0 Mac Viewer 从连接开始到首个有效非空视频 AU 有 **1920 秒（32 分钟）**等待期限；只有在期限内收到该 AU 才取消固定会话期限。CONFIG、keepalive、空 EOS 不触发取消；**到期或迟到的首 AU 不能复活会话**，接收回调与定时器均检查截止。首 AU 后无固定总时长、无累计 2 GiB payload 上限，持续按包处理而非保留整场视频。单包、队列、计数溢出检查和解码在飞限制仍保留。本地开发回放仍限 **64 MiB**。
- 活动视频会话等待下一包不再受总时长期限限制，但每包首字节到完整 packet 的组装仍必须在 **2 秒**内完成；握手期限、每 2 秒心跳及 6 秒失联结束独立生效。控制通道失败必须打断正在等待的视频读取。Host 开始采集后等待首个编码帧的 60 秒期限、EOS 排空期限及现有错误处理也仍有效。
- 历史 0.4.1 对首 AU 前后分别设置 1920 秒期限，并保留累计 2 GiB payload 上限；该版结果见 [历史构建记录](../artifacts/build-verification-0.4.1.json)，不是 0.5.0 当前限制。0.4.0 曾由用户在约 24 分钟时停止，不能作为完整长会话通过；**0.5.0 永久模式长期真机运行尚未验收**。
- 旧 CLI `--timeout` 默认 120 秒、范围 1–300 秒，从 PIN 输入完成后计网络期限，包含连接、手动录屏授权等待与接收；等待视频时独立处理心跳。PIN 的人工输入等待不占网络期限，也不持有 Host 未认证连接。CLI 不随 GUI 的永久模式修改期限。
- 旧 CLI 本次流最多落盘 64 MiB。只接受全新的输出目录，不覆盖已有文件；始终先写 `capture.h264.partial`，只有有效 EOS 且本地写入完成后，独占发布为 `capture.h264`。失败保留 partial 与错误报告，并返回非零退出码。

## Mac CLI 与取证

以下保留 Phase 1 短会话 CLI 取证入口，不是 0.5.0 十分钟/永久模式的主客户端。CLI 的 120 秒默认期限和 64 MiB 文件上限仍生效，不能接收完整十分钟共享来代替 GUI 验收。从仓库根目录运行（示例地址需替换为 Host 实际局域网 IP，输出目录必须全新）：

```sh
python3 scripts/lan-receiver.py --host 192.168.31.130 --output artifacts/device/lan-take-01 --timeout 120
```

历史 0.3.x 取证步骤为 Host Start Server → 隐藏输入 PIN → CLI 显示 `Paired; video ready` → Host 手动启动 10 秒 LAN 共享 → 系统授权 → 收到 EOS。0.5.0 已移除该 10 秒 LAN 选项；若仅做短时 CLI 兼容检查，需在上述 CLI 期限与文件上限内由用户点击 Host **停止共享**，记录实际时长，不将它当作十分钟完整或永久模式长期验收。`--pin-stdin` 仅供已控制的 stdin/PTY 自动化使用，不在命令行拼接 PIN；该开关本身不关闭外部 PTY 的回显。

输出 `receiver-report.json` 保存协议计数、字节量、config/IDR/AU 数、序号、PTS 范围、接收时钟、SHA-256 与固定错误分类，不保存 PIN/token。`gateResult=NOT_AUTOMATICALLY_DETERMINED`：这是传输证据，不是解码/画面/性能的自动 PASS。

建议把 Host 导出另存为 `artifacts/device/lan-take-01-host`，对比两侧 H.264 字节量、SHA-256、AU/帧数，并对**Mac 接收文件**运行独立完整解码。不得用 HDC 导出代替 LAN 传输验证，也不得用 `-r` 改写帧率来通过验收。接收端 ACK/落盘不能证明真实屏幕内容正确，仍需 Host 编码诊断和图像观察共同确认。

独立假服务器测试：

```sh
python3 -B -m unittest discover -s tests -p test_lan_receiver.py -v
```

测试仅用 localhost 临时端口及构造的 Annex-B NAL，不读取屏幕；在禁止 loopback bind 的沙箱中需允许本机端口绑定。它验证分包、认证拒绝、无效头/长度/序号、超时、断线、EOS 与文件发布，不证明真实编码、设备授权或真机 LAN 已成功。真实 C++ server 联动和真机结果应另存证据。

实际 C++ server 联动的复现命令：

```sh
bash scripts/test-lan-server.sh --build-only /tmp/harmony-remote-lan-server-test
LAN_SERVER_FIXTURE=/tmp/harmony-remote-lan-server-test python3 -B -m unittest discover -s tests -p test_lan_receiver.py -v
```

Phase 1 已执行上述真实 server fixture 与接收器联动：**28/28 通过，0 skipped**。fixture 使用真实 `lan_server.cpp`、仅测试构建开放的 loopback 临时端口、随机测试 PIN，以及合成的 SPS/PPS/IDR/P-frame；验证接收文件逐字节相等，并特意在客户端 stop 后才通知编码完成，以检查 EOS 收尾竞态。历史执行记录见 `artifacts/lan-receiver-tests.log`。真机 LAN 传输、实际屏幕码流与持续性能仍未由这些测试证明。


## 本机输入协议测试

`bash scripts/test-lan-server.sh` 验证 Host 认证、输入范围/白名单、禁用/撤权、不持状态锁回调、提交失败/断线/结束释放及原视频状态机。`bash scripts/test-mac-network.sh` 验证解析、控制、地址、输入字段、顺序发送、仅相邻 Move 合并、Host 撤权通知、129 个边沿触发断开、EOS 之后清理，以及当前会话期限和大小边界。

此前 0.5.0 会话模式基线的 Mac 网络 **105 项**通过：35 项 parser、22 项 control/address、2 项基础 NWConnection 联动、22 项输入校验/队列边界、2 项输入 fixture、12 项模拟时钟、2 项期限接收 fixture、8 项长流检查。长流分批处理超过 2 GiB、模拟首 AU 后一年时钟推进，仍保留过期首 AU 拒绝、整数溢出保护、本地回放 64 MiB 与单包限制；这些不是一年真机运行。真实 C++ fixture 与 NWConnection 收到的 payload 逐字节一致，见 [基线网络日志](../artifacts/mac-network-tests-permanent-mode.log)。原生会话策略、计时回归及实际 API 26 对象编译见 [原生验证](../artifacts/encoder-native-verification-0.5.0.json)，当时完整 Host/Mac 构建与部署见 [基线构建记录](../artifacts/build-verification-0.5.0.json)。当前剪贴板扩展另见[本轮验证记录](CLIPBOARD_TEST_RESULTS.md)。

0.4.0 结果原文保留于 `artifacts/lan-server-tests-0.4.0.log`、`artifacts/mac-network-tests-0.4.0.log`。0.4.1 Mac 网络 **97 项**通过，含 **12 个模拟时钟边界**及 **2 个关闭计时器的真实 C++/NWConnection 过期接收 fixture**，确认接收回调不会让过期等待复活；见 [历史网络日志](../artifacts/mac-network-tests-0.4.1.log) 与 [构建验证](../artifacts/build-verification-0.4.1.json)。原生输入 **32 项**开发测试通过。所有开发测试的输入 hooks 均为内存 mock，未调用系统输入、未连接真机，也未打开授权对话框。受限沙箱禁止本机 loopback bind 时，测试在允许本机网络服务的执行环境中运行；这些结果不替代实机效果或长期稳定性。
