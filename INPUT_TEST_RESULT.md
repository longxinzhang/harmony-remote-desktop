# INPUT_TEST_RESULT

检查日期：2026-10-03（Asia/Shanghai）

Authorization method: `OH_Input_RequestInjection` 用户主动授权；通过 `OH_Input_QueryAuthorizedStatus` 查询弹窗授权状态。真机已验证允许、撤销、再次允许和拒绝后的行为；最终状态为 `UNAUTHORIZED`，授权回调次数为 `3`，查询返回 `0`，待释放输入为空。此路径未声明或启用 `CONTROL_DEVICE`。

当前结论：**PASS_THIS_DEVICE_INPUT_PROBE**。在本台 MOR-M1 / API 26 设备上，用户确认四处英文 `a`、鼠标移动、桌面图标左键选中、浏览器 Ctrl+L，以及撤销/拒绝后不能继续输入。最终诊断留存 20 个已完成动作，成功动作的按下/抬起配对和两次预期授权阻断与用户反馈一致。通过范围仅为本设备、本次输入探针动作；不代表任意输入、其他设备、H.264 或整个远控产品通过。

初版 **0.1.0** 本地输入逻辑测试 **17/17 通过**，覆盖授权失败阻断、Down/Up 配对、Up 失败保留与重试、取消释放和显示参数错误。这些测试通过 SDK 替身执行，没有真实系统权限或跨应用输入。17/17 不代表新版测试总数；新版结果以实际验证记录为准。

| 验证项 | 结果 |
| --- | --- |
| OH_Input_RequestInjection | **PASS_THIS_DEVICE**：允许后可以注入，撤销/拒绝后被阻断；最终 `UNAUTHORIZED`、查询 `0`、回调次数 `3` |
| Mouse global injection | **PASS_THIS_DEVICE_USER_CONFIRMED**：用户确认“鼠标移动OK”；最终历史保留 4 次独立移动动作，`mouse.move=0` |
| Keyboard global injection | **PASS_ENGLISH_A_USER_CONFIRMED**：四处英文 `a` 测试已获用户确认，诊断记录四次 `a.down=0`、`a.up=0`；仅限本次单字符测试 |
| Host App | **PASS_ENGLISH_A_USER_CONFIRMED**：按四项测试问题的第①项确认 |
| Desktop | **PASS_TESTED_MOUSE_ACTIONS**：用户确认移动和左键选中图标；不扩展为任意桌面动作 |
| Browser | **PASS_TESTED_A_AND_CTRL_L**：英文 `a` 和 Ctrl+L 地址栏聚焦获用户确认 |
| System Settings | **PASS_ENGLISH_A_USER_CONFIRMED**：按第③项确认；只覆盖该次可输入区域 |
| Another ordinary app | **PASS_ENGLISH_A_USER_CONFIRMED**：用户明确第④项为备忘录 |
| Left click / Ctrl+L | **PASS_THIS_DEVICE_USER_CONFIRMED**：2 次左键 Down/Up 和 1 次 Ctrl+L 全序列返回 `0`；用户确认图标选中及地址栏聚焦 |
| Revocation / denied flow | **PASS_THIS_DEVICE_BLOCKED_AS_EXPECTED**：两次 Inject A 返回授权门禁 `201`，均未执行 A Down/Up；用户确认均不能输入 |

Device / OS / actual API: MOR-M1，`OpenHarmony-7.0.0.105`（系统参数原值），API 26，2in1，aarch64。最初签名 Profile 未授权设备的 `9568423` 已在用户重新生成自动签名后解决；应用已安装、启动，Gate A 最低采集门槛通过。

## 2026-10-03 20:45（Asia/Shanghai）Gate B 最终复核

三组问题和用户回答按原意记录如下；应用与目标归属来自问题/回答上下文，原始 JSON 不记录当前目标窗口。

| 问题组 | 测试要求 | 用户原文 | 本次判定 |
| --- | --- | --- | --- |
| 第 1 组 | 在 Host App、浏览器、系统设置、另一普通应用四处输入英文 `a`，说明第④项应用名 | “OK了，我在备忘录添加的4” | 四处本次英文 `a` 通过；第④项为备忘录 |
| 第 2 组 | ①左键是否选中桌面图标；②Ctrl+L 是否聚焦浏览器地址栏 | “1、OK；2、OK”（原回复分行） | 两个可见效果均通过 |
| 第 3 组 | ①撤销授权后是否不能输入；②拒绝授权后是否不能输入 | “1、是；2、是”（原回复分行） | 两个授权阻断场景均符合预期 |

[最终原始输入诊断](artifacts/device/input-gate-final-01/input-test.json) SHA-256 为 `97a585fc320c49dc99cd95d525b153e13d2b644b5e2d3d4464c6499de8aee196`；[独立人工核对记录](artifacts/device/input-gate-final-01/input-review.json) 关联三组用户回答、诊断摘要和证据边界。原始文件及此前归档保持不变。

- 历史保留 **20 个完成动作**：3 次授权请求、7 次 Inject A（5 次成功、2 次预期阻断）、4 次独立鼠标移动、2 次中心左键点击、1 次 Ctrl+L、3 次取消/释放。全部记录的 pending 状态为空。
- 成功键盘动作包含相应 Down/Up；Ctrl+L 完整记录 `ctrl.down → l.down → l.up → ctrl.up`，均为 `0`。左键动作均有 `left.down=0` 与 `left.up=0`。
- 授权部分按顺序为：取消 `0` → A 被门禁阻断 `201`；重新请求 `0` → A 成功 `0`；再次取消 `0` → 请求 `0` → A 被门禁阻断 `201` → 最后取消 `0`。**中间重新允许后的成功不抵消最后拒绝授权后的阻断**；请求返回 `0` 仅代表请求受理。
- 两次被阻断的 A 动作只有授权查询和 `userAuthorizationRequired=201`，没有 `a.down/a.up`；最终为 `UNAUTHORIZED`、`callbackCount=3`、`pendingKeys=[]`、`pendingMouseLeft=false`。
- 这次真机导出确认自动保存保留了多个连续动作：最终 20 条，容量报告为 32 条。未声称真机跑满容量、长时间无卡键、任意组合键或所有应用均通过。

Gate B 已满足本次设备输入探针验收。下一阶段可开展 Gate C 的 H.264 编码和 Mac 解码验证；当前尚无 H.264 通过证据，不能宣布 `REMOTE CONTROL FEASIBLE`。

## 2026-10-03 20:42（Asia/Shanghai）英文输入复核

用户回应“四处英文 `a`：①Host App、②浏览器、③系统设置、④另一普通应用及名称”的问题，原文：“OK了，我在备忘录添加的4”。据问题和回复上下文，四项英文输入均获用户确认，第④项应用为备忘录。

- [原始输入诊断](artifacts/device/english-input-01/input-test.json) 保留 6 个已完成动作：授权请求 1 次、Inject A 4 次、鼠标移动 1 次。全部动作 `code=0`，四次 A Down/Up 全部返回 `0`，各动作的 pending 状态均为空。
- 历史时间范围为 **20:40:11–20:42:14（Asia/Shanghai）**。这次真机导出确认了 0.1.1 自动保存与多动作历史留存；实际观察到 6 条，报告容量为 32 条，不能声称已真机跑满 32 条。
- 原始输入 SHA-256：`e83c85c2f7678e7408459df40d71409afa2204ce7a3e16f8b0967c9f7e9d95a7`。用户观察与评定独立保存于 [本轮人工核对记录](artifacts/device/english-input-01/input-review.json)。
- **目标应用归属来自四项提问与用户回复，原始 JSON 本身没有目标应用字段。**不能把某条历史的时间戳或顺序当作已自动识别目标窗口，也不能把 API 返回 `0` 当作所有应用通过。
- 该批导出时第 2/3 组尚待反馈；后续已补齐，见上方 20:45 最终复核。该批历史不回写为后续结果。

## 2026-10-03 20:23（Asia/Shanghai）诊断导出

- [原始输入诊断](artifacts/device/input-check-01/input-test.json)：`lastOperation=injectA`、`lastCode=0`，授权查询和 A 键 Down/Up 均返回 `0`；`pendingKeys=[]`、`pendingMouseLeft=false`，探针没有记录待释放输入。
- 诊断记录实际显示器 `id=0`、`3120×2080`。这不单独证明鼠标动作成功。
- [收集清单](artifacts/device/input-check-01/collection.json)：五份文件均通过传输与格式校验；`COMPLETE` 仅表示诊断收集完成。
- 此输入报告没有动作时间或目标应用字段，只保留最近一次动作，不能据此证明浏览器接收字符、鼠标移动、左键点击或 Ctrl+L，也不能证明长时间没有卡键。
- 原始文件保持不变，SHA-256 为 `f392652d1a39d7d44e98d87a4fd5a8c4a6d3f2005148dc737bda48775551183e`。用户后续观察单独记录在 [输入人工核对记录](artifacts/device/input-check-01/input-review.json)，没有回写原始输入诊断。

## 用户观察补充（0.1.0 初轮，保留历史）

用户原文：“1、授权OK 2、鼠标移动OK 3、有输入a，进入了输入法（目前中文输入中）4、操作完成。”

此前操作指引为：鼠标测试切到桌面；Inject A 测试切到浏览器新标签页的空地址栏并使用英文输入状态。用户实际在中文输入状态下看到按键进入输入法，所以这次确认了按键有可见反应，没有确认英文字符直接提交到目标文本框。“操作完成”不扩展为未要求或未记录的点击、Ctrl+L、系统设置或其他应用验收通过。

该轮之后安排了英文 `a` 和四个目标、中心点击/Ctrl+L、拒绝/撤销授权三组补测；后续结果见上方 20:42 与 20:45 复核。初轮中文 IME 观察不追溯修改为英文提交结果。

本次 **0.1.1** 源码新增的诊断机制会在完成输入动作及授权变化后自动保存，并保留本进程最近最多 32 个已完成动作的历史（Unix 毫秒时间、各 steps/code、pending 状态）；手动保存仍可用。新版还新增本应用空白输入框，用于补测 Host App 目标。0.1.1 已通过 ArkTS/C++ 构建、签名校验、安装和进程启动检查，22/22 本地输入逻辑测试通过；`input-gate-final-01` 真机导出已保留 20 条动作历史。

该批 `input-check-01` 来自 **0.1.0**，仍只有最近一次动作，不会补生历史。进程重启不会恢复进程内历史，旧导出归档保持不变。

后续按 `docs/TESTING.md` 记录授权回调、查询结果、鼠标移动结果、A down/up 返回值与跨应用可见行为。若设备返回权限错误，保留原始错误码，不替换为其他技术路线。
