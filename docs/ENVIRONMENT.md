# 本地环境检查

检查日期：2026-10-03（Asia/Shanghai）。开发环境已用于实际编译、签名与真机验证；SDK 声明和设备实测结果分别记录如下。

| 项目 | 检查结果 |
| --- | --- |
| 开发机 | macOS 26.5.2，Apple Silicon / arm64 |
| DevEco Studio | 26.0.0.821，`/Applications/DevEco-Studio.app` |
| HarmonyOS SDK | `Contents/sdk/default`，26.0.0.105，API 26 |
| Native SDK | `openharmony/native/oh-uni-package.json` 同样声明 API 26 |
| Hvigor | DevEco 内置 6.26.4 |
| Node / Java | 使用 DevEco 内置版本；完整输出见 `artifacts/environment.json` |
| Xcode | 已安装 26.3；系统 `xcode-select` 当前指向 Command Line Tools |
| 设备连接 | 已连接 `192.168.31.130:35029`，MOR-M1 / HUAWEI MateBook Pro S，2in1 / aarch64 |
| 设备系统 | `const.ohos.fullname` 为 `OpenHarmony-7.0.0.105`，API 26 |
| 真机 Gate A/B | A 最低采集门槛通过：425 帧 / 30.091 秒，全桌面样本核验；B 在本设备已通过鼠标、点击、四应用英文输入、Ctrl+L、撤销/拒绝阻断测试 |
| 真机 H.264 | 硬件 `OMX.hisi.video.encoder.avc`；10.008 秒 / 224 帧 / 1620×1080，Mac 完整解码通过；22.283 FPS 未达 30 FPS，PTS 单位异常已记录 |
| 签名 | 本项目 default 签名已绑定；用户重新生成含本设备的 Profile 后安装成功 |
| 实际构建 | 0.2.1 ArkTS/C++ 编译、ARM64 链接、signed HAP 打包/校验/安装/启动成功；新诊断的录屏复测待用户回来 |
| 本地逻辑测试 | 输入逻辑与历史记录 22/22 通过；编码时间戳/有界计数回归通过 UBSan；不替代真机效果 |
| Mac 解码验证工具 | 已有 `/opt/homebrew/bin/ffmpeg`、`ffprobe`；仅用于开发验证，不是产品依赖 |

首次 unsigned 构建证据见 `artifacts/build.log` 与 `artifacts/build-verification.json`；首次无设备结果在 `artifacts/device-check.json`，均保留为历史证据。当前签名构建见 `artifacts/build-0.2.1.log` 与 `artifacts/build-verification-0.2.1.json`。真机输入证据位于 `artifacts/device/input-gate-final-01/`，0.2.0 编码/解码证据位于 `artifacts/device/encoder-take-01/`，当前部署版本见 `artifacts/device/deployment.json`。HAP 已检查包名、SDK 元数据、签名与安装；这些构建检查不能证明持续 30 FPS。

SDK 头文件实查包括：

- `multimedia/player_framework/native_avscreen_capture.h`：创建、初始化、数据/状态/错误回调、开始/停止/释放、Surface 采集与光标控制接口。
- `multimodalinput/oh_input_manager.h`：`OH_Input_RequestInjection`、`OH_Input_QueryAuthorizedStatus`、鼠标/全局鼠标/键盘注入、撤销授权。
- `native_buffer/native_buffer.h`：真实缓冲区配置读取；审计同时检查 arm64 链接库。

API 26 的头文件说明 `QueryAuthorizedStatus` 返回的是用户授权弹窗状态，不代表 `CONTROL_DEVICE` 权限持有情况。本探针优先使用用户主动授权流程；声明存在不等于当前设备允许调用。

复现审计（不执行 HDC、不安装软件）：

```bash
cd /Users/zhanglongxin/Desktop/longxincode/harmony-remote
python3 scripts/audit-environment.py --output artifacts/environment.json
```

设备检查单独执行，避免把沙箱或 HDC 服务失败误报为“没有设备”：

```bash
python3 scripts/device-check.py --output artifacts/device-check.json
```

`device-check.py` 退出码：`0 READY`、`2 NO_DEVICE`、`3 CONNECTION_ERROR`、`4 TARGET_NOT_FOUND`。READY 只证明指定设备可见。脚本既不重启 HDC 服务，也不更改设备设置或注入输入。

构建可能需要在允许 DevEco/Hvigor 写入用户缓存的终端环境执行。不要通过修改 `HOME`、自动升级 SDK 或复制已有项目证书绕过环境限制。
