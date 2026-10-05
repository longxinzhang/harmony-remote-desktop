# 密码输入黑屏：采集策略与平台能力审计

核对日期：2026-10-05。修改前源码基线：`872124bcda632f82c6adda319ee1bc0970063e4f`。本文件区分本机 SDK 声明、官方公开源码、当前应用实现和真机结果；API 存在或返回成功不能替代画面恢复验收。

## 1. 环境与修改前现状

| 项目 | 核对结果 | 证据性质 |
| --- | --- | --- |
| 本机 Native SDK | API 26，`26.0.0.105`，platformVersion `26.0.0`，Release | 本机 `native/oh-uni-package.json` |
| 项目目标 / 最低 SDK | `targetSdkVersion = compatibleSdkVersion = 26.0.0` | 本机项目配置；未导出签名配置 |
| 设备类型 | 工程声明 `2in1` | `entry/src/main/module.json5`；不代表当前设备固件版本 |
| 实际设备系统版本 | `OpenHarmony-7.0.0.105`，API 26，deviceType `2in1` | 本轮主任务经设备只读查询取得；保留设备原始返回名称，不将其改写为营销版本号 |
| 当前采集链路 | AVScreenCapture → 编码器 Surface → H.264 硬件编码 → 现有 LAN 视频通道 → Mac VideoToolbox / 显示层 | 应用源码 |
| 原有 CaptureStrategy | 基线 `encoder_probe.cpp` / `capture_probe.cpp` 没有创建或设置 CaptureStrategy | 应用源码；因此本次新增统一策略实例，不覆盖已有显式策略项 |
| 原有隐私回调 | `ENTER_PRIVATE_SCENE = 8`、`EXIT_PRIVATE_SCENE = 9` 只进入通用状态事件日志；未被列入 `cancel` 的终止状态 | `encoder_probe.cpp::OnCaptureState`、`capture_probe.cpp` |
| 用户反馈基线 | 鸿蒙浏览器的 AionUI 网页：点击密码框后整屏黑，点击其他区域恢复 | 用户本轮对该网页的实际反馈；尚不能单凭此现象归因，也不能外推为所有网页、应用或系统弹窗 |
| 原始原因 | 尚不能判定 | 需要同时检查策略、回调、编码、传输、解码与显示 |

本机 SDK 根目录：`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony`。以下声明来自 `native/sysroot/usr/include/multimedia/player_framework/native_avscreen_capture.h`、`native_avscreen_capture_base.h`、`native_avscreen_capture_errors.h`，以及 `ets/api/@ohos.window.d.ts`。

## 2. 优先补丁：窗口区域遮挡

本机声明已核对，均不是根据名称猜测：

```cpp
OH_AVScreenCapture_CaptureStrategy* OH_AVScreenCapture_CreateCaptureStrategy(void);
OH_AVSCREEN_CAPTURE_ErrCode OH_AVScreenCapture_StrategyForPrivacyMaskMode(
    OH_AVScreenCapture_CaptureStrategy* strategy, int32_t value);
OH_AVSCREEN_CAPTURE_ErrCode OH_AVScreenCapture_SetCaptureStrategy(
    OH_AVScreenCapture* capture, OH_AVScreenCapture_CaptureStrategy* strategy);
OH_AVSCREEN_CAPTURE_ErrCode OH_AVScreenCapture_ReleaseCaptureStrategy(
    OH_AVScreenCapture_CaptureStrategy* strategy);
```

四个接口均从 API 20 提供。`Create` 成功返回实例，失败返回空指针；其余返回错误码。mask 值 `0` 表示存在隐私窗口时整幅输出黑色，`1` 表示仅遮挡隐私窗口区域，其他值非法。此选项改变遮挡范围，不能解除密码窗口自身保护。

最小时序：在现有 capture 初始化过程创建一个策略对象 → 设置 mask（默认 `1`，调试可选 `0`）并保留其他策略默认值 / 现有设置 → 调用一次 `SetCaptureStrategy` → 释放策略 → 继续现有 `StartScreenCaptureWithSurface`。`SetCaptureStrategy` 必须在开始采集前完成；已经启动时返回 `INVALID_STATE`。更改模式需停止当前共享后启动下一次共享，不在回调内反复重设策略。官方指南同样描述两种 mask 语义。[OpenHarmony 自定义录屏场景](https://github.com/openharmony/docs/blob/master/en/application-dev/media/media/avscreencapture-c-custom-scenarios.md)

逐项保留结果，不能只记录最后一个返回值：

| 操作 | 需要记录 | 本机 SDK 约定 |
| --- | --- | --- |
| 创建策略 | `created=true/false`，不记录指针地址 | 非空 / 空指针 |
| 设置 mask | 请求值 `0/1`、数值返回码 | `OK=0`；空策略或非法值 `INVALID_VAL=3` |
| 应用策略 | 数值返回码、调用发生在 Start 之前 | `OK=0`、`INVALID_VAL=3`、`INVALID_STATE=8` |
| 释放策略 | 数值返回码，成功或失败路径均释放 | `OK=0`、空策略 `INVALID_VAL=3` |
| 开始采集 | 原有开始接口返回码及后续 `STARTED` 事件 | 返回成功不等于已产生帧 |

补丁已在新构建接入上述时序，默认 mode `1`，保留开发测试页 mode `0` 对照选项。设置失败会保留具体失败调用；未调用项不得写成 `OK=0`。下表是与 SDK 约定分开记录的**实际设备返回值**。

| 操作 | Host 0.6.0 / build 1000013 实际结果 | 证据 |
| --- | --- | --- |
| 创建策略 | 非空；应用的 `CreateCaptureStrategy(non-null)` 检查记录 `0`，这不是创建接口自身返回整数 | `privacy-mode1/sample-01/encoder-probe.json::apiCodes` |
| `StrategyForPrivacyMaskMode(strategy, 1)` | 调用 1 次，`lastCode=0`、`firstError=0` | 同上 |
| `SetCaptureStrategy` | 调用 1 次，`lastCode=0`、`firstError=0` | 同上；源码时序在 Start 前 |
| `ReleaseCaptureStrategy` | 调用 1 次，`lastCode=0`、`firstError=0` | 同上 |
| `StartScreenCaptureWithSurface` | 调用 1 次，`lastCode=0`、`firstError=0`，并收到 `STARTED` | 同上 |
| sample-01 后续运行 | `frames=2517`、`errorCode=0`、`strategyApplied=true`；隐私进入 / 退出计数均为 `0` | 仅确认策略调用与正常编码运行，不证明密码场景通过 |
| mode `0` 调试回退 | 代码已接入；新构建真机 A/B 尚未执行 | 不用旧版默认行为替代新版 mode `0` 验收 |

本表本地证据路径均相对于 `artifacts/`，只引用数字诊断。返回 `0` 仍不保证密码框所在区域可见。

## 3. 状态与黑屏分类

本机头文件把 `ENTER_PRIVATE_SCENE` 描述为当前采集屏幕出现隐私窗口，`EXIT_PRIVATE_SCENE` 为该隐私窗口消失。它们不等于 `CANCELED`、`STOPPED_BY_USER`、`INTERRUPTED_BY_OTHER` 或用户 / 应用暂停。补丁保留既有生命周期：隐私状态只更新状态与计数；需要关闭的事件仍沿既有异步资源释放路径处理。

修改前数字诊断 `artifacts/privacy-baseline/live-prepatch/encoder-probe.json` 在同一会话记录进入时间 `2026-10-05T11:31:20.840Z`（北京时间 19:31:20.840）及退出时间 `11:31:25.367Z`（北京时间 19:31:25.367）。该会话后续达到 `41209` 编码帧，`stream.cancelled=false`。这只证明收到隐私事件后没有永久终止；旧诊断没有事件时各阶段计数，不能证明黑屏期间每一阶段持续工作。

新构建 mode `1` 的 `artifacts/privacy-mode1/sample-02/encoder-probe.json` 已实际收到各一次 `ENTER_PRIVATE_SCENE` / `EXIT_PRIVATE_SCENE`，因此 sample-01 的零计数不能被当作“mode 1 不会通知隐私状态”的证据，也不能仅据零计数断言没有受保护窗口。

| sample-02 同一会话 | 进入隐私 | 退出隐私 | 后续快照 |
| --- | --- | --- | --- |
| 时间 | `2026-10-05T12:00:52.724Z`（北京时间 20:00:52.724） | `12:01:22.487Z`（北京时间 20:01:22.487） | 退出约 51.763 秒后 |
| 编码帧累计 | 3361 | 4255 | 5809 |
| 流接收队列累计包数 | 3361 | 4256 | 5810 |
| 状态 | started，未暂停，未请求取消 | started，未暂停，未请求取消 | `errorCode=0`，`stream.cancelled=false`、`stream.failed=false` |

两事件相隔 29.763 秒，期间新增 894 编码帧。退出后的首个编码帧回调间隔为 13 ms；这是回调时间差，**不是屏幕恢复延迟**。事件内的 `streamAcceptedPackets` 表示既有流接口接受入队，不能当作 socket 完整发送量或 Mac 解码量。该样本支持“隐私期间编码仍产出、退出后同一会话继续编码”，不能替代对窗口外更新、Mac实际显示与输入有效性的观察。

不保存像素、截图、视频文件或实际键值来判断。使用测试窗口内的变化计数器，并由本机 / Mac 操作者观察遮挡范围，结合各阶段计数：

| 观察 | 必要佐证 | 可以下的结论 |
| --- | --- | --- |
| 整屏黑 | 非隐私区域本机计数器仍变化；Mac整幅黑；编码输出、发送、接收、解码、显示计数 / 新帧时间继续推进 | 输出表现是整屏遮挡；只有同时出现隐私事件或可重复 mode 对照，才支持隐私策略解释 |
| 仅窗口黑 | 非隐私区域在 Mac 继续更新，隐私窗口所在区域黑；其余阶段继续推进 | 窗口级遮挡生效；不代表密码窗口内容可被采集 |
| 没有新帧 | 本机测试动画在更新，但编码输出计数停住 / 输出帧龄增长 | 采集或编码上游停止产出；当前 Surface 路径无法仅凭编码输出计数精确拆分两者 |
| 网络阶段停滞 | 编码输出增长但完整发送 / 接收计数不增长 | 优先排查发送队列、连接与丢弃计数 |
| 客户端解码失败 | 接收计数增长；解码失败码 / 次数增长或解码输出停止 | 解码层问题；不能归因于鸿蒙隐私屏蔽 |
| 客户端显示失败 | 解码输出推进，但显示层状态失败或展示计数不推进 | 显示层问题；检查隐藏窗口、显示调度和渲染状态 |

Surface 直接交付编码器时不要为了统计拆成 CPU 像素中转；保持现有低拷贝架构。单纯画面为黑、暂时没有新帧、或成功调用策略都不足以证明某个结论。静止桌面也可能没有明显画面变化，因此对照中必须有不含秘密的动态区域。

进入 / 退出各记录单调时钟时间、最近回调类型、是否 started / paused / stopping、编码包与字节累计、最后编码输出时间、发送 / 接收累计、Mac 解码成功 / 失败与显示状态。隐私退出后观察同一会话恢复，避免把自动重连之后的新会话误写成“原流自动恢复”。除非发生实际终止事件或恢复失败，先不重建采集链路。

## 4. 分场景 A/B 验收

先使用 mode `0` 建立基线，完整停止共享，再使用 mode `1` 重复相同场景。均选“整个主屏幕”，保持相同分辨率、帧率、音频选项和客户端版本。关闭剪贴板同步，避免测试输入进入双方剪贴板。只输入无实际用途的测试字符；系统弹窗只使用专用测试账号，没有该账号时停留在弹窗开关与恢复观察，不尝试真实密码、不连续提交错误密码。

在弹窗 / 目标窗口外放置自建动画或秒计数器，测试窗口不要最大化。每次观察进入前、进入中、退出后至少数秒，记录以下表格每一格；`未测`、`不适用`、`观察不清`均保留，不自动推断。

| 场景 | 模式 | 整屏黑 | 仅窗口黑 | 窗口外继续更新 | 测试键盘输入有效 | 关闭后同会话恢复 | 返回值 / 状态与计数摘要 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 普通网页密码框（自建本地网页） | 0 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 |
| 普通网页密码框（同一网页） | 1 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 |
| 应用登录窗口（自建测试窗口） | 0 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 |
| 应用登录窗口（同一测试窗口） | 1 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 |
| 系统要求密码的认证弹窗 | 0 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 |
| 系统要求密码的认证弹窗 | 1 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 |

已有 AionUI 单网页对照，需与上述三个标准场景的待测表区分：

| 项目 | 修改前 | 新构建 mode `1` |
| --- | --- | --- |
| 用户观察 | 密码框聚焦时整屏黑，移开焦点后恢复 | 用户反馈“会让整个网页黑屏”；不能把网页范围直接记为整屏范围 |
| 非隐私窗口外是否可见 / 更新 | 未记录 | 等待用户确认 |
| 测试键盘输入是否有效 | 未记录 | 未记录 |
| 退出后的可见画面恢复 | 用户确认该网页移开焦点后恢复 | 等待用户确认；已有同会话编码持续的数字证据 |
| 数字证据 | 收到过 8 / 9 事件，其后未永久终止；无事件时计数 | sample-02 隐私期间新增 894 编码帧，退出后继续产出；不代表所有阶段通过 |

窗口级策略保护的是系统标记的窗口区域，并非 HTML 密码输入框的控件矩形；因此“网页黑”不自动说明 mode `1` 无效，也不证明其已经达到预期。当前未得到窗口外区域与退出后的显示反馈，不写“黑屏已解决”。不能用“移开焦点恢复”替代系统认证弹窗关闭后恢复的验收。

应用测试窗口通过不代表任意第三方登录窗口通过；测试密码框通过也不代表系统认证弹窗支持。测试控件只显示输入长度或成功标记，不回显 / 持久化字符，不发起实际登录。系统认证弹窗可能使用独立安全 Surface 或覆盖全屏的窗口，应把剩余遮挡如实保留为平台限制。

## 5. 第二优先级：SkipPrivacyMode 的边界

本机 SDK API 12 起声明：

```cpp
OH_AVSCREEN_CAPTURE_ErrCode OH_AVScreenCapture_SkipPrivacyMode(
    OH_AVScreenCapture* capture, int32_t* windowIDs, int32_t windowCount);
```

该接口接收主窗口及所有相关子窗口的 ID；官方接口文档列出 `OK`、`INVALID_VAL`、`UNSUPPORT`（API 20 起列出）、`OPERATE_NOT_PERMIT`。系统能力为 `SystemCapability.Multimedia.Media.AVScreenCapture`，不能仅因 PC SDK 链接成功就保证当前固件生效。官方指南要求用空列表撤销豁免；C++ 应使用空指针加长度 `0`，不要对空 vector 取 `[0]`。[官方 API 文档](https://github.com/openharmony/docs/blob/master/en/application-dev/reference/apis-media-kit/capi-native-avscreen-capture-h.md#oh_avscreencapture_skipprivacymode)

### 官方公开源码的额外限制（不当作商业固件实测）

截至本轮检查的 OpenHarmony `master`：

1. `ScreenCaptureServer::SkipPrivacyMode` 在启动前保存 ID 列表；采集 active 时调用内部应用方法。内部调用把录屏应用 `appInfo_.appPid` 一起传入窗口系统。这说明“调用成功”可能只表示保存了请求，而非画面已改变。[录屏服务实现](https://github.com/openharmony/multimedia_player_framework/blob/master/services/services/screen_capture/server/screen_capture_server.cpp)
2. `ScreenSessionManager::SetVirtualScreenSecurityExemption` 通过系统服务把窗口 ID 转为 Surface ID。这里的系统服务权限检查不是建议普通应用直接调用私有窗口服务，也不是给普通应用新增 `CAPTURE_SCREEN` 权限的理由。[显示管理实现](https://github.com/openharmony/window_window_manager/blob/master/window_scene/screen_session_manager/src/screen_session_manager.cpp)
3. `SceneSessionManager::GetProcessSurfaceNodeIdByPersistentId` 仅纳入窗口 `callingPid` 与请求 PID 相等的 Surface；不匹配的窗口被略过。这支持“仅自进程窗口可豁免”的实现层推断；传别的应用 / 系统窗口 ID 即使返回成功，也不能证明真正豁免。[窗口归属检查](https://github.com/openharmony/window_window_manager/blob/master/window_scene/session_manager/src/scene_session_manager.cpp)

这些是官方开源分支的行为依据，不是设备厂商承诺。当前不实现窗口 ID 猜测、遍历系统窗口、私有系统服务或替换录屏通道；不把上述 API 当成系统密码框支持。

### 可控的自建窗口试验

先在 Harmony Remote 自身进程创建一个固定大小的测试子窗口，用 `WindowStage.createSubWindow`，内容仅有彩色块、计数器和测试密码框。在显示之后通过该 `Window` 的 `getWindowProperties().id` 获取真实 ID；如果主窗口也设了 privacy，应一起传主窗口 ID。窗口重建后重新取 ID，不保存旧 ID。公开属性接口返回当前窗口属性，不赋予获取任意应用窗口实例的权限。[Window 官方文档](https://github.com/openharmony/docs/blob/master/en/application-dev/reference/apis-arkui/arkts-apis-window-Window.md#getwindowproperties9)

本机声明可用于最小 fixture 的方法为 `WindowStage.createSubWindow(name): Promise<Window>`、`Window.setUIContent(path): Promise<void>`、`resize(width,height): Promise<void>`、`moveWindowTo(x,y): Promise<void>`、`showWindow(): Promise<void>`、`getWindowProperties(): WindowProperties`、`destroyWindow(): Promise<void>`；可设置 480×320 窗口，使外部动态区域始终可见。主窗口需要 `WindowStage.getMainWindowSync()`。这些是已核对 API 的实现建议，并非本轮已运行的窗口试验。

自建窗口的 `setWindowPrivacyMode(true)` 是异步操作，需等成功再测试，捕获 `201` 权限错误和 `1300002` 窗口状态错误。它要求 `ohos.permission.PRIVACY_WINDOW`；官方权限表说明 API 11 起为 normal / system_grant。工程基线没有声明此权限；如果选择添加测试入口，应只为这项自有窗口试验声明并验证授权结果，不新增系统特权。[公开权限定义](https://github.com/openharmony/docs/blob/master/en/application-dev/security/AccessToken/permissions-for-all.md#ohospermissionprivacy_window)

按以下顺序运行且分别记录：

1. 自有窗口普通模式：确认内容可见、外部计数器继续更新。
2. 自有窗口 privacy 开启，无 skip：确认采集端收到的隐私状态及实际遮挡范围。
3. 仅对该本进程已知 ID 应用 skip：记录完整返回值和实际画面；必要时测试 Start 前配置与 STARTED 后配置两种时机，不在回调持锁期间阻塞调用。
4. 使用空列表撤销 skip：确认窗口再次被遮挡。
5. 关闭窗口并清理豁免：确认原画面恢复，正常共享不继承测试 ID。

任何一步失败，记录该步的返回值和实际表现，不推进为系统认证窗口支持。本轮最小修复默认只改变 mask 范围，SkipPrivacyMode 不默认启用。只在本进程试验成功之后才评估目标场景；基于当前 PID 限制证据，跨应用和系统窗口豁免仍属不受保证的能力。

## 6. 交付和验证边界

已完成的最小补丁保持 AVScreenCapture → 硬编 Surface → 现有网络 → Mac 解码 / 显示架构：

| 交付文件 | 修改内容 |
| --- | --- |
| `host-harmony/entry/src/main/cpp/encoder_probe.cpp` / `.h` | Start 前应用 mask；逐项 API 结果；隐私进入 / 退出与事件时编码 / 流入队数字；退出后首帧时间 |
| `host-harmony/entry/src/main/cpp/capture_state_policy.h` | 独立的状态判定，隐私退出不撤销用户暂停、不复活取消会话 |
| `host-harmony/entry/src/main/cpp/napi_init.cpp`、`types/libhrd_probe/index.d.ts` | mode `0/1` 参数贯通与非法值检查 |
| `host-harmony/entry/src/main/ets/pages/Index.ets`、`entryability/EntryAbility.ets` | 默认 mode `1`；停止共享后可切换调试值；连续保存数字诊断开关 |
| `client-macos/Sources/H264Decoder.swift`、`LANConnection.swift`、`VideoSurface.swift`、`ViewerModel.swift`、`HarmonyRemoteApp.swift` | 最近接收 / 解码 / 显示提交时间、显示层错误、有限长度逐秒数字统计与诊断入口 |
| `tests/capture_state_policy_test.cpp`、`scripts/test-capture-privacy.sh`，相关 Mac 测试 | 状态、传输和显示生命周期回归 |

调试入口：鸿蒙 **开发测试 → 隐私场景排查**，选择“窗口级遮挡 · mode 1（默认）”或“整屏遮挡 · mode 0（对照）”；录屏期间不能切换，下次共享开始生效。该区域提供“连续保存数字诊断”。密码场景使用局域网共享，不使用会保存视频的本机录屏测试。Mac **开发测试 → 会话诊断 → 导出诊断** 可导出数字统计；进入开发测试页会暂停显示提交，因此正常共享页观察画面后再导出，不能把主动隐藏显示当成黑屏故障。显示提交计数也不等于屏幕实际呈现帧数。

Host **0.6.0 / build 1000013** 已签名构建、安装并运行；`artifacts/privacy-installed/device-info.json` 核对了安装后应用版本。Mac **0.6.0 / build 9** 已构建并替换为当前保留入口。候选包、源码校验与文件散列见 `artifacts/releases/0.6.0-privacy-rc.1/manifest.json`；构建 / 安装日志为 `privacy-hap-build.log`、`privacy-hap-install.log`、`privacy-mac-build.log`。

本轮已核对的自动测试证据（均在 `artifacts/`）：

| 日志 | 结果与范围 |
| --- | --- |
| `capture-privacy-state-tests.log` | 使用本机 SDK 枚举的 36 项状态策略回归通过；不调用真实采集 |
| `mac-presentation-privacy.log` | 28 项显示生命周期检查通过；没有打开应用窗口或使用系统剪贴板 |
| `mac-network-privacy.log` | 协议解析、真实本机 NWConnection / C++ fixture、输入校验、会话期限、长流边界与断线清理通过；不代表鸿蒙密码场景通过 |
| `mac-decoder-privacy.log` | 历史 `lan-take-01` 流重放硬件解码 287 帧，丢帧 0，5 项非法输入被拒绝；1620×1080，合成 30 Hz 时间轴，不是当前密码场景实时性能 |

审计文档编写只读取 SDK、代码、官方资料与上述数字证据，未保存真实密码、敏感截图或包含密码的输入日志。三个标准场景的完整 A/B 验收仍未完成；自建窗口的 SkipPrivacyMode 试验也未执行。用户正在进行 AionUI 复测，当前只确认其反馈的网页黑色表现和编码数字，不能据此写全部恢复或系统密码框支持。

剩余平台限制：mode `1` 保留隐私窗口本身的屏蔽；API 不提供网页控件级遮挡选项；SkipPrivacyMode 的官方实现存在 PID 归属限制，其他应用和系统窗口豁免未被验证。若系统安全弹窗自身覆盖整屏，窗口级遮挡仍可能表现为大面积或全屏黑色，必须按该场景单独验收。
