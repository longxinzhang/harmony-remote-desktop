# v0.6 会话恢复、设备身份与网络测量

本文记录当前协议设计与验证边界。身份认证并不为现有局域网传输增加加密；本版仍应在可信局域网使用。

## 设备配对

双方使用 P-256 / SHA-256 签名，公钥采用小写十六进制的 65 字节 X9.63 格式，签名采用 DER 格式。`pairingScheme` 是 `p256-sha256-v1`。初次配对仍须输入鸿蒙端显示的六位 PIN，用户可以选择是否持久保存设备。

Host 的 hello 包含公钥、32 字节随机挑战和签名。其签名文本为：

```text
HRDHELLO1
<hostChallenge>
<hostPublicKey>
```

Mac 先核对已记住的 Host 公钥并验证 hello，再为每次握手用 `SecRandomCopyBytes` 产生独立的 32 字节 `clientChallenge`。双方签名使用以下完整文本，各行以换行符分隔，末尾无额外换行：

```text
HRDPAIR1
<hostChallenge>
<clientChallenge>
<hostPublicKey>
<clientPublicKey>
<pin 或 resume>
<remember 或 session>
```

Mac 请求携带 `clientChallenge` 和该文本的签名；只有 PIN 模式附带 PIN。Host 的完成签名覆盖该文本再加上 `\nhost\n<sessionToken>`。Mac 验证完成签名之后才接受 Host 身份和返回的会话。

本轮审查发现，仅由 Host 提供随机挑战，会让旧 hello 和旧完成证明在相同客户身份下被整体重放。已为两端协议增加独立的 Mac 挑战。即使收到完全相同的旧 hello，新 Mac 握手的文本也不同，旧完成证明不能通过。测试还覆盖 Host 身份改变、会话令牌被替换、模式或保存选择被篡改以及其他密钥伪造证明。

首次收到的 Host 公钥仍未预先受信任。该握手没有替代可信网络、传输加密或用户在两端核对设备的要求。后续输入、视频、音频和剪贴板通道仍是现有明文 LAN 传输；不应将「已配对」描述为「连接已加密」。持久身份也不是系统录屏、输入或剪贴板权限。

## 身份保存和撤销

Mac 将 Host 公钥和客户端私钥作为一条凭据存入系统登录钥匙串，按服务名 `com.longxin.harmonyremote.paired-peer.v1` 与设备地址限定查询。会话凭据保留在进程内；未选择记住设备时不写入持久记录。存储失败必须显示错误，不能回退到普通 JSON、偏好或明文密钥文件。

当前开发包采用 ad-hoc 签名，故明确使用 macOS 传统登录钥匙串，并不宣称 Data Protection Keychain 的 `ThisDeviceOnly` 属性已经生效。Apple 说明：[kSecAttrAccessible](https://developer.apple.com/documentation/security/ksecattraccessible) 在 macOS 上要求显式选择 Data Protection Keychain 或同步钥匙串；本实现移除了不适用的属性，没有为获得该属性开启同步。Apple 的 [TN3137](https://developer.apple.com/documentation/technotes/tn3137-on-mac-keychains) 说明了两类钥匙串、默认行为和签名授权差异。

应用二进制更新、签名或安装位置改变后，macOS 可能重新要求用户允许访问原有钥匙串记录。该系统弹窗必须由用户处理。当前测试未修改真实钥匙串，因此跨应用更新、注销登录和重启后的持久记录读取仍待实机确认。

鸿蒙端使用 HUKS 持有长期私钥，不将私钥导出到应用文件。文件内只有经过 Host 身份签名的可信公钥名单，并检查文件类型、所有者、权限、大小与签名；写入使用临时文件、同步和替换。Host 可撤销已记住设备，Mac 可移除本地配对。两端移除操作含义不同：Mac 忘记本地身份并不替 Host 撤销它此前允许的其他客户端。

HUKS 代码与目标 API 26 编译检查和主机测试属于不同证据。macOS 原生测试夹具使用进程内 Security.framework 密钥；它不证明目标 PC 的 HUKS 密钥生成、签名及跨进程保留已成功。目标设备的首次配对、应用重启后恢复以及撤销后拒绝恢复仍需验证。

## 断线重连

`ReconnectPolicy` 仅在用户启用自动重连且已有认证设备身份时，对连接失败、连接关闭、超时或心跳超时给出重试间隔。间隔为 1、2、4、8、15、30 秒，后续保持 30 秒上限。恢复后重置退避计数。

手动取消、无效地址/PIN、Host 拒绝、会话无效、设备身份改变、签名失败和协议错误不自动重试。缺少认证身份时不会反复提交 PIN。UI 显示重连过程，并须允许用户取消。

恢复建立的是新会话，重新核对身份和 Host 的当前能力状态。断线前的键盘、鼠标、拖拽和待执行粘贴不能排队重放；需释放按下状态并取消未完成动作。恢复不能绕过 Host 用户撤销授权、停止共享或系统录屏确认。模型和设备级验证应特别检查「拖拽时掉线」「等待粘贴时掉线」以及「重连等待中手动断开」。

## 网络状态与统计

RTT 只接受与当前未完成探测 ID 匹配的 pong，以本机单调时间的收发差值计算，不使用两台机器的墙上时钟差。重复、未知、超过六秒及时间倒退的回应不计入。最多保留四个未完成探测，避免断线时积累。抖动为最近最多三十个有效 RTT 样本之间相邻差值绝对值的平均数。

视频 Mbps 和接收 FPS 根据收到的视频字节、帧数增量及实际经过时间计算，至少间隔一秒更新；计数器重置不产生负数，后续重新建立基线。接收 FPS、配置的 30/60 FPS 和最终显示 FPS 是不同指标，不能混用。

RTT 表示网络控制通道往返时间，不等于录屏、编码、传输、解码、屏幕呈现合计的端到端交互延迟。本版没有用该统计推算鼠标到画面的真实延迟，也没有从 TCP 回应估算丢包率。

## 当前专项测试

`bash scripts/test-mac-session.sh` 编译实际 `PairingIdentity.swift`、`ConnectionPolicy.swift` 和 `WireProtocol.swift`。2026-10-05 本轮完成 **89 项检查**，日志位于忽略发布的 `artifacts/mac-session-tests-v0.6.log`。

检查包括真实 CryptoKit P-256 签名与验证、每次握手新鲜度、记住/临时及 PIN/恢复四种组合、失败的身份与令牌验证、严格十六进制和 DER 解析、重连退避及取消、RTT 匹配与窗口上限、30/60 接收帧率和视频速率计算。全部密钥仅在内存中生成；没有调用 `PairingStore`、修改用户钥匙串、网络连接或登录项。

这些测试证明协议和状态计算的指定分支；不证明真实设备断网恢复、HUKS/钥匙串跨重启保持、系统自动启动或新音频通道已验收。集成测试与目标 PC 结果另行记录。

## Host 协议与恢复边界

启用身份后，`hello_ack` 附加 `pairingScheme`、`hostPublicKey`、`challenge`、`hostHelloSignature` 和 `rememberAllowed`。`pair` 仅接受 `type`、`pin`、`clientPublicKey`、`clientChallenge`、`remember`、`clientSignature` 六个字段；`pair_resume` 使用同样的五个非 PIN 字段。Host 对每条控制连接产生新的挑战，挑战在首次校验时即消耗；校验失败必须重新连接，不能复用它。现代握手独立计时六秒，已保存身份的恢复不依赖当前 PIN 是否过期。

`remember=false` 的公钥只保留在当前 Host 服务内存中，Stop Server 后清除。`remember=true` 的首次加入还要求 Host 明确打开「允许记住设备」，最多保存十六个公钥。关闭这一开关只禁止新增持久配对；已信任设备需使用「撤销所有设备」移除。签名名单使用应用 filesDir 下的 `trusted-peers-v1`，权限为 `0600`，私钥不写入该文件。HUKS 初始化会完成公钥导出、签名和独立验签自检；失败时不通告现代配对能力，诊断提供阶段及错误码，不能将旧 PIN 连接当作持久配对成功。

OpenHarmony 的 [HUKS C/C++ 公钥导出文档](https://github.com/openharmony/docs/blob/master/zh-cn/application-dev/security/UniversalKeystoreKit/huks-export-key-ndk.md) 说明 `OH_Huks_ExportPublicKeyItem` 返回 X.509 DER。实现解析 P-256 的 SPKI 编码后输出 X9.63 公钥；内部 HUKS 密钥材料结构不能直接当作该导出接口的格式。是否与目标 PC 厂商实现一致，仍由设备上的自检结果决定。

每次恢复签发新的控制会话令牌、剪贴板 epoch/绑定令牌及音频 epoch/绑定令牌。音频能力使用 `audioSupported` / `audioPort`，配对回复使用 `audioEpoch` / `audioBindToken`；RTT 使用 `rttSupported=true` 及严格限定为十六个小写十六进制字符的 `probeId`，Host 原样回显该 ID。

已通过签名身份连接、且原采集尚未结束时，意外断线可保留原采集任务 **最多六十秒**。等待中立即释放输入、关闭旧剪贴板与音频绑定、清空待发送帧；编码器继续推进但离线视频帧全部丢弃，只保留一个有大小上限的编解码配置。只有同一公钥的重新认证才能继承这次采集。其他设备即使用新 PIN 连接，也必须重新开始共享。

视频重新绑定后从序号零发送 CONFIG，要求编码器产生新的 IDR，在它到达之前丢弃依赖旧参考帧的 P 帧。强制 IDR 导致的新配置可以替换尚未发送的缓存 CONFIG，不增加配置队列上限。若断线发生在原系统确认尚未完成、第一份 CONFIG 尚未产生时，重连继续等待原采集的首次 CONFIG 和 IDR，既不自动接受系统确认，也不重新发起确认。原采集时限、系统撤销、编码失败、显式停止、撤销设备或六十秒到期都会终止这段保留期。

音频恢复额外使用 `CanResumeCapture()` 核对同一公钥、原采集仍在进行、尚未 EOS、未取消以及有效保留期，不能仅由编码线程尚未退出推断允许恢复。输入和待执行粘贴不会随认证恢复而自动启用。Mac 对传输中断关闭连接并触发恢复；显式断开才发送终止共享的 `stop`，防止网络故障被误当作用户停止。

## Host 与真实协议联调证据

2026-10-05 完成以下独立检查，均不读取真实用户剪贴板或写入真实钥匙串：

| 检查 | 结果 | 范围 |
| --- | --- | --- |
| `bash scripts/test-lan-server.sh` | 46/46 | 原生服务的真实回环 TCP、PIN 锁定、挑战重放拒绝、保存/撤销、名单篡改拒绝、PIN 过期后恢复、六十秒保留期、不同设备隔离、CONFIG/IDR 和有界队列 |
| `bash scripts/test-clipboard-service.sh` | 31/31 | 新控制服务集成后，原剪贴板协议、权限及绑定隔离回归 |
| `bash scripts/test-pairing-interop.sh` | 5/5 | C++ Security.framework 与 Swift CryptoKit 的实际 X9.63/DER 签名互验、篡改和错钥拒绝 |
| `bash scripts/test-mac-pairing-network.sh` | 3/3 场景 | 实际 `LANConnection` 连接 C++ Host：持久身份加入、仅视频链路故障后的重新认证、新 CONFIG/IDR、RTT、输入保持关闭、临时配对主动退出，以及已记住 Host 身份不符时拒绝降级 |
| API 26 原生严格编译 | 通过 | `pairing_identity.cpp` 与 `lan_server.cpp` 使用设备 SDK 头文件，`-Wall -Wextra -Werror` |

日志分别位于未发布的 `artifacts/test-lan-v06-expanded.log`、`artifacts/test-native-clipboard-v06.log`、`artifacts/test-pairing-interop-v06.log`、`artifacts/test-mac-pairing-network-v06.log` 和 `artifacts/compile-native-pairing-api26.log`。现代网络联调使用合成 H.264 协议数据，证明传输、顺序和重新绑定，不等于实际画面解码或设备断网验收。目标 PC 的 HUKS 执行、系统录屏保留、输入释放及音频恢复仍须在设备上验证。
