# 0.5.0 剪贴板构建验证

当前构建标识：**Host versionCode `1000008` / Mac CFBundleVersion `5`**，两端对外版本仍为 `0.5.0`。本页与此前 `1000007` 会话模式基线、0.4.x 真机证据分开记录。

## 已有开发证据

| 范围 | 当前结果 | 原始证据与边界 |
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

早期失败日志及此前 37 项成功联调保留不覆盖；不能把不同运行合并成一次运行。已请求用户确认真实鸿蒙读取授权，尚未收到回复；本轮尚未进行真实鸿蒙/Mac 双向复制或目标应用粘贴验收。

## 仍需真机确认

按[平台能力与完整验证矩阵](PLATFORM_CAPABILITIES.md)分别确认：运行时读取授权及拒绝、双方允许/方向设置、开启不传原有内容、双向 Unicode 纯文字、重复复制与冲突、文件/图片排除、后台读取、写入确认后粘贴、失焦/禁用/断线取消。单次上限 1 MiB，不保留排版，文件传输及文件粘贴后续实现。

`clipboard_applied=applied` 只能证明系统写入路径报告成功；`paste_result=committed` 只能证明按键注入完成。目标应用内出现正确文字仍需观察。开发测试、成功签名和安装不能替代这项验证，也不证明永久模式长期稳定性。完整线上合同见 [CLIPBOARD_WIRE](CLIPBOARD_WIRE.md)。

## 发布边界

公开仓库为 [longxinzhang/harmony-remote-desktop](https://github.com/longxinzhang/harmony-remote-desktop)，用户已明确选择保持公开并同步代码。Git 保留会话模式基线和剪贴板实现，旧版不完整源码单独标注，详见[版本来源](VERSION_HISTORY.md)。代码发布与真机功能验收分别记录。

发布前只读扫描覆盖 Git tracked 与未忽略的新增文件；未发现敏感路径、私钥块、常见 GitHub/AWS 凭据、URL 内嵌凭据或非空密码/secret 字面量命中。实际签名配置、签名材料、构建输出、诊断导出及 source baseline 均由 `.gitignore` 排除，未读取或发布其内容。模式扫描只能检查已知形式，不等于对任意秘密的完整证明；提交前仍应核对最终暂存清单。
