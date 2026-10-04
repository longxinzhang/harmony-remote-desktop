# 本机 InputKit 核验与最小验证实现

核验日期：2026-10-03。只读检查本机 SDK 文件与动态库导出符号；未调用输入注入、未控制设备。此记录证明接口可在本机 SDK 中找到，不能替代 HarmonyOS PC 真机授权和跨应用控制验证。

## SDK 版本

- SDK 根目录：`/Applications/DevEco-Studio.app/Contents/sdk/default`。
- `sdk-pkg.json:6-12`：API **26**，名称 `HarmonyOS 26.0.0`，版本 **26.0.0.105**，Release。
- `openharmony/native/oh-uni-package.json:2-10` 同样声明 API 26、Native 26.0.0.105。
- `/Users/zhanglongxin/Library/Huawei/Sdk` 本次检查只有 `productConfig.json`，不是另一套已安装 Native SDK。
- SDK 名称不能单独证明已连接设备的消费端操作系统名称或版本。未以 API 26 自动推断设备正在运行 HarmonyOS PC 7.0。

## 核验文件

下文的 `input header` 指：

`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h`

| 内容 | 本机文件与行号 |
| --- | --- |
| InputKit、Input.Core 系统能力、链接库 `libohinput.so` | input header:28-35 |
| 授权状态枚举 | input header:274-289 |
| SDK 返回码 | input header:373-458 |
| 授权回调签名 | input header:604 |
| 键盘注入及权限语义 | input header:734-760 |
| 单屏相对坐标鼠标注入 | input header:978-999 |
| 主屏原点全局坐标鼠标注入 | input header:1002-1023 |
| 申请授权、取消、查询 | input header:1759-1805 |
| CONTROL_DEVICE 权限元数据 | `/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/lib/PermissionDefinitions.json:9077-9086` |
| 手动权限设置入口 | `/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/ets/api/@ohos.abilityAccessCtrl.d.ts:468-501` |
| 默认屏幕 ID、宽高 | `/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/window_manager/oh_display_manager.h:59-84` |
| 显示管理链接库 `libnative_display_manager.so` | 同上文件:34-36 |

使用 SDK 附带 `llvm-readelf -Ws` 检查了以下文件，确认包含 RequestInjection、QueryAuthorizedStatus、CancelInjection、InjectKeyEvent、InjectMouseEvent、InjectMouseEventGlobal 导出符号：

`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/lib/aarch64-linux-ohos/libohinput.so`

该文件是 SDK 链接桩，符号存在不等于设备端实现已运行。

## 实际 C API 签名

```cpp
typedef enum Input_InjectionStatus {
    UNAUTHORIZED = 0,
    AUTHORIZING = 1,
    AUTHORIZED = 2
} Input_InjectionStatus;
typedef void (*Input_InjectAuthorizeCallback)(Input_InjectionStatus authorizedStatus);

Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback); // API 20
Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* status);     // API 20
void OH_Input_CancelInjection();                                               // API 12
int32_t OH_Input_InjectKeyEvent(const struct Input_KeyEvent* keyEvent);           // API 12
int32_t OH_Input_InjectMouseEvent(const struct Input_MouseEvent* mouseEvent);     // API 12
int32_t OH_Input_InjectMouseEventGlobal(const struct Input_MouseEvent* mouseEvent); // API 20
```

`RequestInjection` 返回 0 表示请求受理，仍须等待用户选择以及授权回调。不能据此设置“已授权”。`QueryAuthorizedStatus` 查询成功后才可解释输出状态；错误时输出状态不可沿用。

## 错误码与平台限制

| 错误码 | SDK 含义 |
| --- | --- |
| 0 | INPUT_SUCCESS |
| 201 | INPUT_PERMISSION_DENIED |
| 401 | INPUT_PARAMETER_ERROR |
| 801 | INPUT_DEVICE_NOT_SUPPORTED |
| 3800001 | INPUT_SERVICE_EXCEPTION |
| 3900005 | INPUT_INJECTION_AUTHORIZING |
| 3900006 | INPUT_INJECTION_OPERATION_FREQUENT；连续授权请求间隔不超过 3 秒 |
| 3900007 | INPUT_INJECTION_AUTHORIZED；当前应用已授权 |
| 3900008 | INPUT_INJECTION_AUTHORIZED_OTHERS；其他应用已授权 |

授权申请返回码说明位于 input header:1767-1777。单个注入方法明确列出 0、201、401；不能把枚举中所有错误码都声称为每个方法的确定返回集合。

产品适配元数据 `/Applications/DevEco-Studio.app/Contents/sdk/default/hms/ets/api/device-define/api-version/InputKit.json` 中，RequestInjection:7056-7071 仅列 `2in1`，自 API 20；InjectKeyEvent:46889-46904 和 InjectMouseEvent:46908-46923 仅列 `2in1`，该设备类型自 API 13；全局鼠标注入:46927-46940 列 `2in1`，自 API 20。不得以手机上的编译成功或可安装代替 PC 功能验证。

## 两条授权路径必须区分

本机 API 26 header 明确说明：用户弹窗授权或持有 `ohos.permission.CONTROL_DEVICE` 均可提供注入能力。已有 CONTROL_DEVICE 时可以直接调用注入；RequestInjection 的行为与 CONTROL_DEVICE 独立。

`QueryAuthorizedStatus` 自 API 26 起**只查询弹窗授权状态**，不能用它判断 CONTROL_DEVICE 是否已授予。因此将“Query 返回 UNAUTHORIZED”解释为“应用完全不具有输入能力”是不正确的；本次 MVP 明确仅启用弹窗授权路径，故以此作为 MVP 注入门槛是有意设计。

CONTROL_DEVICE 的本机权限定义是：

```json
{
  "name": "ohos.permission.CONTROL_DEVICE",
  "grantMode": "manual_settings",
  "availableLevel": "system_basic",
  "availableType": "NORMAL",
  "since": "26.0.0",
  "provisionEnable": true,
  "deviceTypes": ["2in1"]
}
```

它不是普通 `user_grant` 权限。声明与签名配置的可用性仍须独立核实；仅在清单写入权限不会证明已经获得。SDK 提供的设置入口为：

```ts
openPermissionOnSetting(context: Context, permission: Permissions): Promise<SelectedResult>;
```

该方法自 API 22 起用于 manual_settings，用户必须在系统设置中手动选择。普通授权弹窗不能直接申请这种权限。`getSelfPermissionStatus(permissionName)` 位于同一 d.ts:542-561，可独立检查本应用权限状态。本次实现只预留模式枚举，不请求 CONTROL_DEVICE、不更改签名权限等级、不假定该权限已存在。

## 本次实现

文件：`host-harmony/entry/src/main/cpp/input_probe.h`、`input_probe.cpp`。全局 `GetInputProbe()` 返回单例，供根层 N-API 桥接。

- `RequestAuthorization()`：请求弹窗授权并保留 SDK 原始返回码；异步回调仅更新原生状态，不跨线程直接访问 ArkTS/N-API。
- `QueryJson()`：查询真实弹窗授权，返回模式、queryCode、状态、回调计数、上一次操作与步骤码、默认屏幕信息、待释放按键。无 INFO 日志，适合每秒轮询。每次动作重置步骤数组，且固定最多 64 条。
- `MoveMouseToCenter()`：读取实际默认屏幕 ID、像素宽高；校验 ID 能安全转换成 InputKit 的 int32_t，然后以屏幕相对坐标移动到中心。
- `ClickLeft()`：先移到默认屏幕中心，再注入左键 down/up。UI 应明确写“中心左键点击”，避免误以为点击当前位置。
- `InjectA()`：A down/up；`InjectCtrlL()`：Ctrl down、L down/up、Ctrl up。后续 down 只在前一步成功时开始；每次已尝试的 down 都会对应 up 尝试，所有失败进入步骤结果。
- `CancelAndRelease()`：先尝试释放所有待释放按键/鼠标，再调用 void 型 CancelInjection，然后查询授权。不能伪造取消 API 的成功返回值。返回 0 只说明可观察的释放/查询没有报告错误；实际弹窗状态仍在 JSON 中。
- 如果 up 失败，保留待释放状态；新注入前必须先成功重试释放。操作返回第一个错误，JSON 保留其余步骤错误，避免后一次成功掩盖前一次失败。
- 对象分配失败返回应用码 9000001；显示参数无效 9000002；系统单调时钟读取失败 9000003。其余错误保留 SDK 原始码。

使用 `CLOCK_MONOTONIC` 微秒时间戳，符合 input header:840-845、1213-1218 的系统启动以来微秒语义。创建对象后始终调用对应 Destroy，注意 Destroy 接受二级指针。

链接必须包含 `libohinput.so` 和 `libnative_display_manager.so`。ArkTS 层负责 5 秒倒计时让用户切换至自己准备的测试窗口；原生方法没有阻塞等待。尚未证明键盘焦点切换、组合键实际效果、跨应用输入、撤销后的行为或多屏动态切换，必须在授权后的 HarmonyOS PC 真机上验收。

## 本地主机故障路径测试

执行 `./scripts/test-input.sh`，直接以主机 clang++ 编译真实的 `input_probe.cpp`，仅将两个 HarmonyOS SDK 头文件和对应运行时函数替换为 `tests/stubs` 与 `tests/input_probe_test.cpp` 中的最小替身。编译参数为 C++17、`-Wall -Wextra -Werror -pthread`，可执行文件生成在临时目录并于结束时清理。

2026-10-03 初版 0.1.0 实跑结果：**17/17 通过**。覆盖未授权及授权查询失败时不注入、A 与 Ctrl+L 配对顺序、down 失败后的 up、up 失败保留 pending、下次动作先重试释放、清理失败阻止新动作、显示查询错误与 ID 溢出、使用实际查询尺寸计算中心、鼠标失败释放、取消前释放键盘与鼠标、申请受理不等于授权、同步回调不会死锁，并逐个测试断言事件对象无泄漏。

这些测试证明本地 C++ 操作排序、错误传播和释放逻辑。替身不申请真实系统权限、不向设备发送任何输入，也不能证明真机全局注入成功、系统焦点行为、设备兼容性或权限弹窗的实际表现。

### 0.1.1 诊断历史回归

新版保留本进程最近 32 个完成操作的不可变快照，含 Unix 毫秒开始/完成时间、操作步骤与返回码、待释放键和鼠标状态。系统墙钟读取失败时输出 null；此时间不是输入事件使用的单调微秒时间。轮询与授权回调不新增操作记录，进程重启不恢复历史。

实跑 **22/22 通过**，见 `artifacts/input-logic-0.1.1.log`。新增覆盖历史中的失败状态不被后续释放修改、鼠标失败状态冻结、墙钟时间单位、32 条 FIFO 边界、轮询与同步回调不增加伪操作。仍仅验证主机侧逻辑，不代替真机可见效果。
