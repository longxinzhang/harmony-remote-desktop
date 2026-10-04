# 0.5.0 剪贴板修复候选版验证

当前候选版标识：**Host versionCode `1000009` / Mac CFBundleVersion `6`**，两端对外版本仍为 `0.5.0`，源码标签 **`v0.5.0-rc.1`**。备忘录实际复制粘贴仍待新版复测，尚不建立正式 `v0.5.0` 标签。此前 `1000007` 会话模式基线、`1000008` / Mac `5` 初版及 0.4.x 真机证据独立保留。

## 用户反馈与已取得的真机证据

初版 `1000008` / Mac `5` 已完整构建、签名、安装及运行。用户随后报告：

- 点击读取授权时没有新弹窗，但功能可用；双向基本可用，鸿蒙终端复制的文字可以在鸿蒙和 Mac 粘贴。
- 通过 Mac 远程操作鸿蒙备忘录，复制后在鸿蒙或 Mac 粘贴失败，仍会出现先前终端的旧内容。
- 关闭剪贴板同步后，直接用鸿蒙键盘在备忘录复制粘贴正常。这次对照同时改变了同步开关和输入来源，不能据此认定某种格式或系统 API 是唯一原因。
- 用户因资源占用较高关闭应用。本轮保持应用关闭开发，没有重新开始共享或实测资源改善幅度。

[初版现场诊断](../artifacts/device/clipboard-0-5-0-copy-report/clipboard-snapshot.json) 记录 `canRead=true`、`canWrite=true`，发送 10 次、接收 34 次、写入 30 次；收集时方向已关闭。它说明出现过读写活动，不能证明每次复制或目标应用粘贴正确，也未记录备忘录当时的格式结构。初版提交 `8570001` 和[原产物状态](../artifacts/releases/0.5.0-clipboard-initial/status.json)保留为存在已报告缺陷的预览版。

## 本轮修复与检查

粘贴不再被当作新的 Mac 复制。已同步且未变化的内容复用事件，由 Host 在执行前核对当前事件和系统 revision；新 Mac 复制仍需等待写入确认。视频画面发送复制/剪切后，尚未收到这次远程复制的新内容时，不把未变化的旧 Mac 内容写回；开启同步后也必须重新复制或收到远端更新，不能通过显式粘贴发送开启前的旧内容。

Host 本地出现不支持的新内容时会取消排队的旧更新；当前有效的鸿蒙事件可供受保护粘贴直接使用。格式策略只对单记录、明确提供 `general.plain-text` 的已知文本替代表示有限放宽，不转换富文本、不合并多记录、不把文件/图片或未知表示变成文字。新增类型数、记录数和固定拒绝类别诊断，不记录类型原文、内容或摘要；备忘录真实结构仍待确认。

Mac 有新帧才安排一次显示计时器，会话结束后停止统计轮询，剪贴板关闭或无发送许可时停止轮询。保留异步显示错误观察、30 FPS 上限和单槽帧替换统计。见[资源使用检查](RESOURCE_USAGE.md)。

| 范围 | 当前结果 | 证据与边界 |
| --- | --- | --- |
| Native ClipboardService / 文本策略 | 修复后 31 项通过 | [日志](../artifacts/clipboard-service-tests-0.5.0-copy-fix.log)；本机 TCP 和内存剪贴板替身 |
| Native API 26 arm64 | 修复后编译检查通过 | [日志](../artifacts/clipboard-native-api26-0.5.0-copy-fix.log)；不是备忘录行为验证 |
| Mac 显示生命周期 | 最终源码 13 项通过 | [日志](../artifacts/mac-presentation-copy-intent-final.log)；无窗口检查，不启动应用、录屏或操作系统剪贴板 |
| Mac 剪贴板回归 | 最终 52 项通过 | [日志](../artifacts/mac-clipboard-copy-intent-final-v4.log)；覆盖初始旧内容、远程复制意图、旧事件复用/拒绝与协议联调，不读取用户 General 剪贴板 |
| Mac 输入回归 | 最终 21 项通过 | [日志](../artifacts/mac-input-copy-intent-final.log)；检查快捷键和输入状态机，不代替目标应用行为 |
| Host build `1000009` | 完整构建、签名及 ACL 验证通过 | [构建日志](../artifacts/build-0.5.0-clipboard-build9.log)、[权限汇总](../artifacts/clipboard-hap-permission-0.5.0-build9.json) |
| 新 Host 安装 | 重连既有设备后安装成功；未启动应用 | [安装日志](../artifacts/install-0.5.0-clipboard-build9-reconnect.log)；此前 `NO_DEVICE` 检查保留，安装成功不等于新版实机复测 |
| Mac build `6` | 最终完整构建、签名深度严格校验及 11 个 Swift 编译源指纹核对通过；常用路径已同步，未启动 | [构建日志](../artifacts/build-mac-0.5.0-build6-final.log)、`client-macos/build/HarmonyRemote.app`；本轮保持应用关闭 |
| 新版实机复制粘贴及资源使用 | 待复测 | 没有用替身测试代替真机验收 |

本地候选产物已归档：[Mac build 6 ZIP](../artifacts/releases/0.5.0-rc.1/HarmonyRemote-Mac-0.5.0-build6.zip)、[Host build 1000009 HAP](../artifacts/releases/0.5.0-rc.1/HarmonyRemote-Host-0.5.0-build1000009.hap)。完整产物指纹、源码和部署核对集中于 `artifacts/build-verification-0.5.0-rc.1.json`。这些本地产物不自动上传公开 Git 仓库。

## 初版开发证据（Host `1000008` / Mac `5`，保留历史）

| 范围 | 当时结果 | 原始证据与边界 |
| --- | --- | --- |
| Native ClipboardService | 24 项通过 | [日志](../artifacts/clipboard-service-tests-0.5.0.log)；真实本机 TCP，内存剪贴板替身 |
| Host LAN server | 32 项通过 | [日志](../artifacts/lan-server-tests-0.5.0-clipboard.log)；包括认证、扩展与原协议兼容边界 |
| Native remote input | 37 项通过 | [日志](../artifacts/remote-input-tests-0.5.0-clipboard.log)；SDK 替身验证顺序与失败释放，不调用设备输入 |
| Native API 26 arm64 | 编译检查通过 | [源码指纹与汇总](../artifacts/clipboard-native-verification-0.5.0.json)、[日志](../artifacts/clipboard-native-api26-0.5.0.log) |
| Mac clipboard 与实际 C++ 服务联调 | 最终 39 项通过，含发送队列 paste-permit 回归 | [build 5 日志](../artifacts/mac-clipboard-tests-0.5.0-build5.log)；NWConnection、本机 C++ 服务、内存平台及测试专用命名 pasteboard，不读取 General 剪贴板 |
| Mac 网络与输入回归 | 网络 105 项、输入 21 项通过 | [网络日志](../artifacts/mac-network-tests-0.5.0-build5.log)、[输入日志](../artifacts/mac-input-tests-0.5.0.log)；本机协议和事件测试 |
| Host 完整签名 HAP | 构建与安装成功 | [构建日志](../artifacts/build-0.5.0-clipboard.log)、[安装日志](../artifacts/install-0.5.0-clipboard.log)；成功安装不代表运行时授权或剪贴板功能验收 |
| Host 签名与空闲启动 | 最终 HAP 的 Profile 含 READ_PASTEBOARD；已安装启动，appBuild `1000008`、剪贴板默认关闭 | [权限汇总](../artifacts/clipboard-hap-permission-0.5.0.json)、[启动诊断](../artifacts/device/clipboard-0-5-0-startup)；未自动请求运行时授权 |
| Mac 完整 App 与界面启动 | build `5` 构建/签名校验通过，常用路径已同步，已打开确认四方向菜单及默认关闭 | `client-macos/build/HarmonyRemote.app`；界面检查不代表真实剪贴板同步成功 |

早期失败日志及中间成功联调保留不覆盖，不能把不同运行合并成一次运行。以上测试属于初版，其后收到的用户部分成功与备忘录失败反馈见本页开头。

## 仍需真机确认

Host `1000009` 已安装但保持关闭。用户准备好后使用配套 Mac `6` 开启两端，再配对、授权共享和启用双方剪贴板。使用新建的安全测试文档按以下顺序复测：

1. 从 Mac 重新复制文字并粘贴到鸿蒙，再从鸿蒙终端复制另一段文字，在鸿蒙及 Mac 粘贴核对。
2. 通过 Mac 远程在鸿蒙备忘录选中新文字，`⌘C` / `Ctrl+C` 后立即及稍等片刻分别粘贴，确认不会出现上一步终端内容。新会话第一次复制也重复检查。剪切只用可丢弃的测试文字。
3. 若仍失败，保存两端诊断，检查 Host 固定类别、类型/记录数量及事件计数，不导出正文。分别对照“同步关闭 + 远程键盘”和“同步开启 + 鸿蒙本地键盘”，区分输入路径和同步行为。
4. 检查关闭同步、断开和失焦后不执行待处理粘贴，观察空闲连接、静止画面、持续操作时两端 CPU/内存；长时间稳定性另行记录。

按[完整验证矩阵](PLATFORM_CAPABILITIES.md)继续补齐拒绝/撤销、双向 Unicode、重复复制、冲突、文件/图片排除及完整后台场景。单次上限 1 MiB，不保留排版，文件传输及文件粘贴后续实现。

`clipboard_applied=applied` 只能证明系统写入路径报告成功；`paste_result=committed` 只能证明按键注入完成。目标应用内出现正确文字仍需观察。开发测试、成功签名和安装不能替代这项验证，也不证明永久模式长期稳定性。完整线上合同见 [CLIPBOARD_WIRE](CLIPBOARD_WIRE.md)。

## 发布边界

公开仓库为 [longxinzhang/harmony-remote-desktop](https://github.com/longxinzhang/harmony-remote-desktop)，用户已明确选择保持公开并同步代码。源码提交与标签对应关系见[版本来源](VERSION_HISTORY.md)。Git 保留会话模式基线、存在已报告缺陷的初版和修复候选版；旧版不完整源码单独标注。代码发布与真机功能验收分别记录，候选标签不表示备忘录问题已实机通过。

发布前只读扫描覆盖 Git tracked 与未忽略的新增文件；未发现敏感路径、私钥块、常见 GitHub/AWS 凭据、URL 内嵌凭据或非空密码/secret 字面量命中。实际签名配置、签名材料、构建输出、诊断导出及 source baseline 均由 `.gitignore` 排除，未读取或发布其内容。模式扫描只能检查已知形式，不等于对任意秘密的完整证明；提交前仍应核对最终暂存清单。
