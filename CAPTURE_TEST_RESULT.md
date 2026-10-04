# CAPTURE_TEST_RESULT

检查日期：2026-10-03（Asia/Shanghai）

| 项目 | 结果 |
| --- | --- |
| Device | MOR-M1 / HUAWEI MateBook Pro S，2in1，aarch64；HDC 已连接 |
| OS | 系统参数 `const.ohos.fullname` 返回 `OpenHarmony-7.0.0.105` |
| API | 设备实测 API 26 |
| SDK | 本地 26.0.0.105 / API 26 |
| Host build | ArkTS/C++ 与 ARM64 signed HAP 打包、签名验证、安装与启动成功 |
| Capture API | SDK 中已确认 `OH_AVScreenCapture` 创建、初始化、数据回调、开始/停止/释放声明 |
| Resolution | 默认屏 3120×2080；实际输出 1620×1080，保持 3:2 比例 |
| FPS | 请求最大 30；本轮实测平均 **14.09** |
| Frames / Duration | **425 帧 / 30.091 秒** |
| Pixel Format | NativeBuffer format=12（RGBA_8888）；stride=6656 字节；样本转换为 RGB PPM/PNG |
| Result | **PASS_MINIMUM_GATE**：通过 Phase 0A 最低采集门槛；尚未达到持续 30 FPS 性能目标 |

已连接用户提供的 `192.168.31.130:35029`。首次安装返回 `9568423`，用户在设备已连接时重新生成自动签名后解决。最终安装、启动和进程存活已核实。

用户在设备完成系统录屏授权和单轮 30 秒测试。实际 `capture-probe.json` 状态 completed，所有记录的 API 返回码为 0，时间戳单调、无意外音频回调，Stop 与 Release 成功。测试后应用仍存活。记录来自本应用 AVScreenCapture 回调，不是 HDC 截屏。

已查看应用保存的第 149 帧（首帧约 5 秒后）：画面同时包含桌面背景/图标、系统任务栏、Host 窗口和文件管理器窗口，证明捕获范围为系统桌面而非 Host 自身窗口。设备原始 JSON 的 `desktopVerified:false` 保持原样，人工图像核验单独写入 `artifacts/device/capture-review.json`。

证据：`artifacts/device/capture-probe.json`、`capture-frame.ppm`、`capture-frame.png`、`capture-review.json`。原始像素与报告 SHA-256 见 review 文件。

边界：本轮 14.09 FPS 不能写成已达到 30 FPS；未定位帧率变化原因。30 分钟稳定性、拒绝/中断/重启路径、H.264、网络链路、端到端延迟均未验证。

性能补充（仅根据同一份原始报告推算，没有新增设备测试）：首帧、样本第 149 帧和末帧第 425 帧的纳秒 timestamp 分别为 `56076454472185`、`56081489067810`、`56106263836872`。早段 `(149−1) / 5.034595625 ≈ 29.40 FPS`，后段 `(425−149) / 24.774769062 ≈ 11.14 FPS`；整段累计均值约为 14.09 FPS。累计值按 `(frames−1) / elapsedSeconds` 计算，采集结束时已冻结时长，不包含 Stop/Release 耗时。SDK 的 `SetMaxVideoFrameRate(30)` 设置帧率上限，不保证实际持续 30 FPS。现有证据不能确定后段降速原因；下一轮应记录每秒帧数与回调间隔，并在相同配置下对照静态桌面和持续变化画面，不能将当前差异直接归因为静态降帧、设备性能或采样操作。

后续按 `docs/TESTING.md` 补测拒绝授权和停止/恢复，以及独立性能/稳定性验收。
