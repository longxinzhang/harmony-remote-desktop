# Mac 原生查看器：0.4.1 五分钟查看与拖拽复测通过

2026-10-04，0.4.1 实机窗口拖动、文字拖选及松开鼠标停止已由用户确认。此前 0.4.0 的英文输入、右键菜单、Mac ⌘L 映射到鸿蒙浏览器 Ctrl+L 也由用户确认；不能因此把所有键鼠项目都判为通过。

本轮 0.4.1 Host 编码/发送、Mac 接收/解码均 **9001 帧**，1620×1080、硬件解码确认，Host 采集 **300.021 秒 / 29.995 FPS**；EOS 完整，网络中止数为 0，不保存本地录像。Mac 显示层提交 8207 次、提交前替换 794 帧，不能作为物理刷新率。关闭控制及会话结束后，待处理队列与待释放键/按钮/滚轮状态均为 0，释放错误码为 0。

证据：[本轮完整会话](artifacts/device/drag-0-4-1-completed/session-review.json)、[用户拖拽复测与释放诊断](artifacts/device/drag-0-4-1-verified/drag-review.json)、[Mac 原始导出](artifacts/device/drag-0-4-1-verified/mac-viewer-report.json)、[0.4.1 构建与部署](artifacts/build-verification-0.4.1.json)。完整 30 分钟稳定性、滚轮方向、异常断线及持键失焦等专项、端到端延迟仍未通过验收。

# 0.4.0 历史五分钟实时查看

2026-10-04，该历史批次结论为 **`PASS_THIS_DEVICE_5_MINUTE_VIEWING`**，Harmony Host / Mac Viewer 均为 **0.4.0**。用户完成系统共享授权，本设备五分钟实际查看通过。

Host 编码、Mac 接收和解码均 **8985 帧**，尺寸 **1620×1080**，Mac **硬解已确认**、无显示错误。Host 独立采集计时窗口 **300.023 秒**，平均 **29.941 FPS**；编码正常结束、EOS 完整、网络输出未记录拒绝或失败。`localRecordingEnabled=false`、录像路径为空、`saved=false`，本次共享未保存桌面录像。

Mac 显示层提交 **7698** 次、已解码帧在提交前替换 **1287** 次，合计 8985；这些计数不是实际屏幕刷新帧数，也不证明恒定逐帧 30 FPS 或端到端延迟。Mac 报告的 `startedAt` 与稍后导出的 `recordedAt` 不是采集起止时间，300.023 秒来自 Host 原生计时。Host 服务在最终导出前重启，LAN 计数已清零，本次核对依据编码会话身份和 Mac 报告，未将重启后的网络计数当成本次发送数。

- [五分钟独立核对](artifacts/device/control-live-take-01/five-minute-review.json)
- [Mac 五分钟原始诊断](artifacts/device/control-live-take-01/mac-viewer-five-minute.json)
- [Host 五分钟原始编码诊断](artifacts/device/control-live-take-01-host-after/encoder-probe.json)

本批没有成功的远程键鼠输入：请求控制时 Host 应用的允许开关关闭。Mac 的 `inputControlMessagesSent=1`、`inputEnabledAtReport=false` 仅记录控制消息与状态，不能作为注入效果证据。30 分钟稳定性、内存增长、滚轮和拖拽等远程输入效果仍待验收；已有单点 RSS 采样不构成内存稳定性结果。Mac 目前已由用户解锁。

以下保留 **0.3.0 短时 GUI 会话**的历史证据，结论为 `PASS_THIS_DEVICE_MAC_GUI_LIVE_VIEWING`。

用户在鸿蒙 PC 手动启动服务，在 Mac 原生 App 输入新 PIN，随后在鸿蒙端开始 LAN Capture 并看到实时桌面。助手通过 Mac 原生窗口的可访问性状态和截图再次确认：接收 **303 帧**、解码 **303 帧**、**1620×1080**、**已确认硬解**；画面包含桌面、其他窗口和任务栏，比例保持正确，结束后保留最后一帧。

Host 导出与这次会话对应：编码/保存/发送均为 **303 帧**，约 **10.015 秒**，独立单调时钟口径平均 **30.055 FPS**，`streamCompleted=true`、EOS 完整、无记录错误。导出的 H.264 在 Mac 再次完整解码得到 303 帧。该历史结论为 `PASS_THIS_DEVICE_MAC_GUI_LIVE_VIEWING`。

- [本次独立核对](artifacts/device/lan-gui-take-01/live-review.json)
- [Host 原始诊断](artifacts/device/lan-gui-take-01-host/encoder-probe.json)
- [Host 网络计数](artifacts/device/lan-gui-take-01-host/lan-snapshot.json)
- [完整解码复核](artifacts/device/lan-gui-take-01-host/h264-validation/validation.json)
- [先前本地回放诊断](artifacts/device/lan-take-01/mac-viewer-replay-01.json)：287 解码 / 284 显示提交，仅验证离线链路。

这次 Mac 自动诊断最初指向先前回放文件，程序正确拒绝覆盖；首次手动导出又因 Mac 锁定未完成。用户于 2026-10-04 解锁后，已从仍保留原会话的 **0.3.0** 进程成功补导出 [Mac 原始实时诊断](artifacts/device/lan-gui-take-01/mac-viewer-live-export-01.json)，并记录于 [补导出说明](artifacts/device/lan-gui-take-01/live-export-supplement.json)。原始 JSON 的 SHA-256 为 `c939c80447f57353571b5a58f0f1fa2c3e42e18e342134c498385574a420d72e`，确认接收/解码 **303/303**、显示提交 **242** 次、帧替换 **61** 次、无显示错误、硬解已确认。原会话开始时间仍为 `2026-10-03T17:00:53Z`；补导出不是新录屏或 0.4.0 验证。人工核对、窗口观察和 App 原始诊断分别保留，未覆盖旧文件。0.4.0 已改为目标文件存在时另存新的会话文件。

0.3.0 结论仅覆盖这一次短时实时查看；0.4.0 五分钟结果来自文首的新会话，并非继承旧版本结论。两批均未证明完整 1920×1080、60 FPS、恒定逐帧 30 FPS、端到端延迟、网络键鼠或 30 分钟稳定性。


## 0.4.0 部分键鼠通过、拖拽失败；约 24 分钟时主动停止

`control-live-take-03` 原计划共享 30 分钟，用户授权暂停共享以修复拖拽问题并重新测试。本轮最终状态是 **`USER_STOPPED_30_MINUTE_NOT_COMPLETED_PARTIAL_INPUT_VERIFIED_DRAG_FAILED`**，不记为 30 分钟通过。[停止后独立复核](artifacts/device/control-live-take-03/stopped-session-review.json) 保留各原始文件的 SHA-256。

用户已确认 Mac 输入英文、右键菜单和 Command+L 定位鸿蒙浏览器地址栏生效；**窗口拖拽、文件拖拽和文本选择拖拽均失败**。这是部分实际输入效果的确认，不由 API 返回或消息计数推定。[原输入核对](artifacts/device/control-live-take-03/input-review.json) 保留其共享期间的记录时间及“当时仍在运行”的含义，未改写为停止后状态。

Mac [停止后诊断](artifacts/device/control-live-take-03/mac-viewer-stopped.json) 为 `USER_STOPPED`，接收 **42790** 帧、解码快照 **42786** 帧，1620×1080、硬解已确认。显示提交 **33644** 次与提交前替换 **9146** 次合计 **42790**，比解码快照多 4；取消期间快照与在飞处理边界未对齐，保留这一差异，不修正计数，也不以合计相等宣称完整解码或完整 EOS。显示计数仍不是物理刷新计数。

Host [停止后编码报告](artifacts/device/control-live-take-03-stopped/encoder-probe.json) 为 `network_cancelled`，最终编码 **42792** 帧，采集窗口 **1438.943 秒（约 23 分 59 秒）**，平均 **29.737 FPS**；请求时长仍为 1800 秒，但 `timerFinished=false`、网络 `eosAccepted=false`、`finishedSuccess=false`。本地编码清理收到 EOS 不等于网络完整 EOS。应用输出队列未记录丢包或失败，不代表上游无损；本轮未保存本地 H.264，也没有完整离线解码证据。后来 LAN 快照已回到 LISTENING、未配对且计数为零，不用于补齐本次传输总数。Mac 导出时间不能用来替代 Host 的实际采集时长。

新鲜的 [停止后输入状态](artifacts/device/control-live-take-03-stopped/remote-input.json) 显示 `allowed=false`、`sessionEnabled=false`、队列及 pending key/button/axis 均为 0、`busy=false`、`lastReleaseCode=0`。系统授权本身仍为 true；这说明本地会话门已关闭且诊断中没有待释放输入，不能替代新的“按住键失焦/断线”效果验收。

[内存采样](artifacts/device/control-live-take-03/memory-samples.jsonl) 包含 **10 个有效点**（9 个定时点和移交时追加的 1 点），覆盖 UTC **05:01:15—05:09:15** 约 8 分钟，双端读取无错误。Mac PID 67060 RSS 为 **106512–133744 KiB（104.02–130.61 MiB）**；Host PID 27248 为 **193336–214508 KiB（188.80–209.48 MiB）**。首尾分别由 118080 降至 107792 KiB、214508 降至 203772 KiB；采样从会话中段开始，缺少全程初始基线，不能据此宣布无内存泄漏或 30 分钟稳定。

本段仅记录已停止的 **0.4.0** 会话，不作为后续拖拽和计时修复版本的真机结果；文首五分钟查看通过的历史结论保持独立。
