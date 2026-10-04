# 启动设置与平台边界

本轮启动功能包括 Mac 登录启动适配器、鸿蒙开机自启动状态读取，以及独立的「打开鸿蒙应用后启动服务」偏好。安装新版不会自行修改系统登录项，也不会替用户授权屏幕共享或输入控制。

## Mac

`StartupController` 使用系统 `SMAppService.mainApp`。这是**当前用户登录后**启动主应用；不是登录前运行的系统服务。Apple 对主应用的 [register() 说明](https://developer.apple.com/documentation/servicemanagement/smappservice/register())明确了后续登录时启动的行为。

界面只将系统 `.enabled` 显示为已启用。`.requiresApproval` 表示等待用户到系统设置批准；成功调用 `register()` 本身不等于获准。注册、注销发生错误后仍重读系统状态，显示错误，不存储另一个会漂移的启用布尔值。设置页出现和应用回到前台时应刷新状态。

「打开系统登录项」调用官方 [openSystemSettingsLoginItems()](https://developer.apple.com/documentation/servicemanagement/smappservice/opensystemsettingsloginitems())，仅在用户点击后打开系统设置。启用、关闭、打开设置都不应在应用初始化时自动调用。用户移动应用或替换安装位置后，需核对系统登录项；开发构建的注册与真实登录启动仍需目标 Mac 验证。

## HarmonyOS PC / API 26

当前本地 SDK 的 `@ohos.app.ability.autoStartupManager.d.ts` 向普通 Stage 应用提供两个公开方法：

- `isAutoStartupSupported()`：API 26，判断设备是否支持开机自启动。
- `getAutoStartupStatusForSelf()`：API 21 起，异步读取本应用当前的系统自启动状态。

本地声明与 [OpenHarmony 官方接口文档](https://github.com/openharmony/docs/blob/master/zh-cn/application-dev/reference/apis-ability-kit/js-apis-app-ability-autoStartupManager.md)一致，没有面向本应用的公开注册或修改方法，也没有要求为这两个读取调用增加权限。`StartupService.readStatus()` 先检查设备能力，再读状态；设备返回不支持与读取失败分别显示。查询结果为 `false` 仅表示系统未启用，并不等于设备不支持。

本轮不能通过普通应用接口替用户开启鸿蒙的开机自启动。官方 [拉起系统应用能力清单](https://github.com/openharmony/docs/blob/master/zh-cn/application-dev/application-models/system-app-startup.md)要求仅使用列出的设置跳转，当前清单未给出自启动设置入口。因此本轮没有猜测 PC 设置路径、私有 ability 名称或未公开深链，也没有以后台广播、定时任务或企业管理权限替代。目标设备若提供系统自启动管理，可由用户自行设置，然后点击刷新读取实际结果；具体入口尚未在该 PC 验证。

「打开应用后启动服务」是独立功能：默认为关闭，用持久偏好保存用户选择，应用创建后尝试一次启动 LAN 监听；失败显示实际服务错误。它不会让未运行的应用在开机时自行运行，也不会自动恢复录屏、输入或剪贴板授权。永久模式可持续监听；调试模式仍遵守十分钟限制。

## 验证

`bash scripts/test-mac-startup.sh` 使用注入的内存后端验证注册、注销、待批准、系统外部变更、失败回滚及缺失应用状态；不读写真实登录项，不打开系统设置。此验证不替代真实登录/重启验收。

需在用户可交互时验收：Mac 启用后在系统设置确认、退出登录再登录、关闭后再次验证；鸿蒙先刷新实际状态，单独验证「打开应用后启动服务」的持久化与启动失败提示。录屏和输入授权必须继续由本机用户完成。
