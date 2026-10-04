# v0.4.0 原生远程输入 SDK 核验

核验日期：2026-10-04。Build SDK **26.0.0.105 / API 26**，版本来自本机 `/Applications/DevEco-Studio.app/Contents/sdk/default/sdk-pkg.json:6-12`。本次只读取 SDK、编译源码、运行本机替身测试；未安装应用、申请设备授权或向真机注入输入。

## 本地官方接口依据

以下相对路径均以 `/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/` 为根。

| 头文件与行号 | 核验内容 |
| --- | --- |
| `multimodalinput/oh_input_manager.h:113-166` | 鼠标 MOVE/DOWN/UP、AXIS_BEGIN=4、UPDATE=5、END=6；垂直轴=0、水平轴=1 |
| 同上 `:173-203` | NONE=-1、LEFT=0、MIDDLE=1、RIGHT=2 |
| 同上 `:734-760` | `OH_Input_InjectKeyEvent(const Input_KeyEvent*)`；修饰键 down 后必须及时 up；用户授权与 CONTROL_DEVICE 是不同路径 |
| 同上 `:978-999` | `OH_Input_InjectMouseEvent(const Input_MouseEvent*)` 使用指定显示屏左上角为原点的屏幕相对坐标 |
| 同上 `:1155-1205` | `void OH_Input_SetMouseEventAxisType(Input_MouseEvent*, int32_t)`；`void OH_Input_SetMouseEventAxisValue(Input_MouseEvent*, float)`；正值 forward、负值 backward，1.0 表示一个 SDK 滚动单位 |
| 同上 `:1213-1218` | 鼠标事件 actionTime 为系统启动以来的微秒；实现使用 `CLOCK_MONOTONIC` 微秒 |
| 同上 `:1759-1805` | RequestInjection/CancelInjection/QueryAuthorizedStatus；API 26 的 Query 只反映弹窗授权，不能判断 CONTROL_DEVICE 权限 |
| `multimodalinput/oh_key_code.h:148-198`、`:233-358`、`:598-653` | 数字、A–Z、F1–F12 的连续枚举值 |
| 同上 `:208-223`、`:363-558` | 方向、标点、编辑键和左右修饰键枚举；Backspace 对应 DEL，Delete 对应 FORWARD_DEL |
| `window_manager/oh_display_manager.h:59-99` | 默认屏 ID、像素宽高和旋转查询；ID 返回 uint64_t，注入 API 使用 int32_t，必须检查转换范围 |
| `window_manager/oh_display_info.h:58-79` | 旋转 0/90/180/270 对应枚举 0/1/2/3 |

SDK 声明与 ARM64 编译通过证明这些 API 可被当前构建引用，不证明设备运行时行为。链接仍使用 `libohinput.so` 和 `libnative_display_manager.so`。

## 本轮实现约束

实现位于 `host-harmony/entry/src/main/cpp/input_event.h`、`remote_input.h/.cpp`，提供全局 `GetRemoteInput()`。`SetAllowed(bool)` 是 Host 明示许可，默认 false；`EnableSession(bool)` 是单次已认证 LAN 会话的控制开关，两者必须同时满足。启用前查询官方授权；每个事件执行前，以及启用期间约每 250ms 再次查询授权和显示几何。SDK 自身没有在这些声明中提供调用超时参数，因此这里不声称 250ms 是设备撤销生效的硬实时上限。

本模块只采用用户弹窗授权路径，要求 Query 返回成功且 AUTHORIZED。不请求 CONTROL_DEVICE，不从其可能存在推断授权，也不从网络线程触发 RequestInjection 弹窗。关闭应用内控制开关会释放并禁用会话；它不会自行调用 CancelInjection 改变系统授权。系统授权的主动撤销仍由 Host 原有明确操作负责。

所有本模块 SDK 调用在一个 worker 上串行执行。管理方法等待 worker 完成，调用者不得持有 LAN server 的互斥锁。`Submit` 返回 true 仅表示入队；OS 注入仍可能随后失败，不能将网络确认作为视觉效果成功。已存在的本地 InputProbe 是另一个入口，Host 应在运行本地探针前禁用远程会话，避免两套动作交错。

输入队列最多 128 个事件，只合并相邻且尚未执行的 Move。按键、按钮边沿不丢弃或合并；溢出进入显式失败、清队列、释放并禁用会话。重复 down、无对应 down 的 up 在 Submit 时被拒绝，包括尚在队列中的边沿。客户端自动重复须发送完整 up/down。`ReleaseAll()` 清除旧队列并作为释放屏障：之后提交的事件在释放完成后执行；成功时保留已启用会话，失败时禁用。断线和退出应使用 `EnableSession(false)`。

每次启用记录真实默认显示屏 ID、宽高、旋转。Move 将有限的归一化 `[0,1]` 坐标映射为 `round(x*(width-1))`、`round(y*(height-1))`，不使用编码分辨率。仅 Move 改变当前位置。Button/Scroll 的 wire 消息没有 x/y，因此沿用最后一次成功 Move；新会话未收到成功 Move 前拒绝 Button/Scroll。显示 ID、尺寸或旋转改变均失败关闭，包括宽高不变的 180° 旋转；清理仍尝试用原显示屏参数释放已持有按钮。

滚轮使用 **Input_MouseEvent** 的轴接口，不混用监听用的 Input_AxisEvent。非零水平轴、垂直轴依次各发送 Begin(0) → Update(delta) → End(0)。两轴 delta 必须有限且在 ±120 内；这限制的是 SDK 单位，不把它解释为像素。Begin/Update 失败仍尝试 End；第一轴失败后不继续第二轴。方向、自然滚动和实际步长需要真机人工核验。

已尝试的 down 在 SDK 注入前登记为待释放；SDK 返回错误也尝试 up。up/End 失败保留 pending，后续启用前必须成功释放。清理绕过授权门槛，以便撤销时仍尽力释放；SDK 拒绝时保留 pending 和错误码，不能保证应用有权撤销系统已经拒绝的输入。普通按键按 down 的逆序释放。析构先停止生产，再由 worker 尽力释放并 join；事件对象采用 RAII，worker 异常不会遗弃等待中的管理命令。

Snapshot 只含 allowed/sessionEnabled/authorized、SDK 查询状态、排队/处理/合并/失败计数、pending 数量及固定错误码。不包含键名、鼠标坐标、文本、事件历史或认证 token。原生错误保留 SDK 数值；应用错误为 9100001（分配或 worker 异常）、9100002（显示参数）、9100003（时钟）、9100004（事件或缺少定位）、9100005（队列溢出）、9100006（显示变化）、9100007（重复/不匹配边沿）。`errorCode` 保留操作错误，`lastReleaseCode` 单独反映清理结果。

## 键码协议

固定白名单共 82 键：KEY_A–KEY_Z、KEY_0–KEY_9、KEY_F1–KEY_F12；KEY_MINUS、KEY_EQUALS、KEY_LEFT_BRACKET、KEY_RIGHT_BRACKET、KEY_BACKSLASH、KEY_SEMICOLON、KEY_APOSTROPHE、KEY_GRAVE、KEY_COMMA、KEY_PERIOD、KEY_SLASH；KEY_ENTER、KEY_ESCAPE、KEY_TAB、KEY_SPACE、KEY_BACKSPACE、KEY_DELETE、KEY_UP、KEY_DOWN、KEY_LEFT、KEY_RIGHT、KEY_HOME、KEY_END、KEY_PAGE_UP、KEY_PAGE_DOWN；KEY_SHIFT_LEFT/RIGHT、KEY_CTRL_LEFT/RIGHT、KEY_ALT_LEFT/RIGHT、KEY_META_LEFT/RIGHT、KEY_CAPS_LOCK。

这些是物理按键事件，不是文本注入；不保证输入法、布局、快捷键语义与 Mac 相同，不实现剪贴板或任意 Unicode 文本粘贴。

## 已执行验证

本机直接编译真实 `remote_input.cpp`，仅用独立的 `tests/remote-input-stubs` 和 `tests/remote_input_test.cpp` 替换 InputKit/Display SDK；未修改旧 InputProbe 的测试桩。执行：

```sh
bash -o pipefail -c 'bash scripts/test-remote-input.sh 2>&1 | tee artifacts/remote-input-tests-0.4.0.log'
```

**28/28 通过，退出码 0**。完整 stdout/stderr 在 `artifacts/remote-input-tests-0.4.0.log`。构建启用 C++17、Wall/Wextra/Werror、UBSan、pthread。覆盖 82 个键码和完整边沿、实际显示坐标、按钮/滚轮沿用非零位置、初始未定位拒绝、两轴字段与顺序、down/up/End 错误和 pending、授权查询/撤销、显示改变含旋转、队列溢出与合并边界、释放屏障、析构清理、worker 异常处理；每例检查 SDK 只在一个非主线程调用、事件对象无泄漏。

真实 SDK 语法检查同样退出 0：

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/llvm/bin/clang++ \
  --target=aarch64-linux-ohos \
  --sysroot=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot \
  -std=c++17 -Wall -Wextra -Werror -fsyntax-only -DOHOS_PLATFORM -D__OHOS_API__=26 \
  host-harmony/entry/src/main/cpp/remote_input.cpp
```

本机测试不证明真实授权弹窗、LAN 输入链路、屏幕定位、拖拽、滚轮方向、修饰键或焦点行为已通过。此前视频 GUI 真机成功及本地输入探针通过，均不能替代 v0.4.0 远程输入的真机验收；本轮未进行这些设备操作。


## 0.4.1 拖拽事件修复与实机结果

0.4.0 可点击、键盘输入，但用户复测窗口拖动、文件拖动和文字拖选均失败。Host 处理 MOVE 时总是使用 MOUSE_BUTTON_NONE；0.4.1 从 worker 已维护的按下集合中选出当前按钮，设置到 MOVE 的 OH_Input_SetMouseEventButton，UP 后恢复 NONE。SDK 枚举为 NONE=-1、LEFT=0、MIDDLE=1、RIGHT=2；没有另造拖动 action 或改动 Mac 前台/焦点门禁。多个按钮同时持有时按左、中、右优先选取，组合拖动尚未实机验收。

原生回归 32 项通过，覆盖三种按钮 down→MOVE→up→hover、合并移动不跨按键边沿、失败移动后释放；实际 API 26 ARM64 编译检查通过。用户在安装后的 0.4.1 确认窗口拖动、文字拖选及松手停止正常，见 [实机证据](../artifacts/device/drag-0-4-1-verified/drag-review.json)。不将测试桩中的 API 参数正确直接视为系统拖拽成功。
