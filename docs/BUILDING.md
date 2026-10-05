# 构建与安装

当前主线为产品版本 **0.6.0**，Host build **1000013** / Mac build **9**。[Preview 1](https://github.com/longxinzhang/harmony-remote-desktop/releases/tag/v0.6.0-preview.1) 提供 Mac ZIP 与需要自行签名的 unsigned HAP；用户安装及授权说明见 [INSTALL.md](INSTALL.md)。鸿蒙通用直装版尚未提供，预览版与历史标签均不代表所有功能已完成真机验收。

## 开发环境

| 端 | 当前要求 |
| --- | --- |
| Mac 控制端 | Apple Silicon，macOS 14 或更高；Xcode Command Line Tools / Swift 编译器 |
| HarmonyOS 被控端 | API 26 的鸿蒙 PC / 2in1，当前工程 ABI 为 arm64-v8a |
| 鸿蒙构建工具 | DevEco Studio，已安装的 HarmonyOS SDK 26；本项目核对版本为 26.0.0.105 |
| 辅助脚本 | Python 3、Git |

当前实测设备返回的系统版本为 `OpenHarmony-7.0.0.105` / API 26。其他固件、设备、Intel Mac 与其他控制端尚未验证。Mac 客户端采用系统框架，不需要安装 FFmpeg 才能运行。

## 取得源码

```bash
git clone https://github.com/longxinzhang/harmony-remote-desktop.git
cd harmony-remote-desktop
python3 scripts/init-local-config.py
```

初始化脚本只为缺失的本地构建配置复制模板，不覆盖已有签名配置。真实 `build-profile.json5`、签名文件与 `artifacts/` 均不纳入 Git。

## 构建 Mac 客户端

在仓库根目录执行：

```bash
bash scripts/build-mac.sh
```

输出：`client-macos/build/HarmonyRemote.app`。在 Finder 中打开该应用即可。脚本使用本机 ad-hoc 签名，并验证签名；这不是经过 Apple 公证的发行包。请保留一个常用 App 入口，历史版本使用压缩包归档。

## 构建并安装鸿蒙端

1. 在 DevEco Studio 中打开 `host-harmony`。
2. 按设备系统说明开启调试并连接目标 PC，在本项目的签名配置中为该设备配置调试签名。
3. 文字剪贴板读取需要签名 Profile 中的 `READ_PASTEBOARD` ACL 与实际系统授权；只声明权限不等于已获授权。
4. 在仓库根目录运行：

```bash
bash scripts/build-hap.sh
```

脚本默认使用 `/Applications/DevEco-Studio.app` 内的 Node、JBR、OHPM 与 Hvigor。其他安装位置可通过 `DEVECO_HOME` 指定。

输出目录为 `host-harmony/entry/build/default/outputs/default/`。配置签名后应取得 `entry-default-signed.hap`；只有 unsigned 文件时，还不能按下面步骤安装。

使用 DevEco 的运行按钮部署，或明确指定当前设备 ID：

```bash
python3 scripts/device-check.py --target '实际 HDC 设备 ID'
python3 scripts/install-hap.py --target '实际 HDC 设备 ID' \
  --hap host-harmony/entry/build/default/outputs/default/entry-default-signed.hap
```

无线调试的设备 ID 以当前设备页面显示的地址和端口为准，先通过 SDK 的 HDC 工具连接。不要使用他人的签名 Profile。安装脚本会校验包、签名和指定设备；HDC 仅用于开发部署与诊断，不是 Harmony Remote 的产品远控通道。

常见签名问题：`9568423` 需检查 Profile 是否包含当前设备；`9568289` 需检查受限权限是否得到对应签名授权。修改签名后重新构建再安装。

## 第一次连接

1. 两端处于同一可信局域网。鸿蒙端点击 **开启服务**。
2. 在 Mac 输入鸿蒙端显示的地址与连接码，点击 **连接设备**。
3. 鸿蒙端点击 **开始共享屏幕**，手动允许系统共享。默认永久上线；也可在设置中选择 10 分钟调试。
4. 要使用键鼠，在鸿蒙端完成系统授权并 **允许远程控制**，再在 Mac 启用控制、点击画面。使用 `⌘⇧Esc` 释放画面焦点。
5. 要使用文字剪贴板，在鸿蒙端允许读取和同步，在 Mac 选择单向或双向；之后重新复制测试文字。当前不传文件或图片。

30 / 60 FPS、系统声音以及调试遮挡模式在停止当前共享后修改，下次共享生效。应用登录启动、打开应用后监听和系统录屏授权是不同设置；开机或配对不会自动获得录屏、键鼠与剪贴板权限。

当前连接没有传输加密，身份签名不等于数据加密。跨公网连接和无人值守控制不属于现有可用范围。

## 开发验证与诊断

常用检查入口：

```bash
bash scripts/test-capture-privacy.sh
bash scripts/test-mac-presentation.sh
bash scripts/test-mac-network.sh
```

各脚本的设备、工具和测试素材条件以其内容为准；历史视频回放测试需要本地素材，源码仓库不包含用户桌面录像。更多步骤见 [测试说明](TESTING.md)。

日常局域网共享不保存录像。**开发测试页中的本机录屏 / H.264 探针会生成测试画面或录像**，只在无敏感内容的专用场景使用。诊断导出只保留状态与数字；仍应在提交 Issue 前检查并移除与问题无关的设备信息。

历史开发文档中的 `artifacts/` 路径是本机证据引用，不是 GitHub 下载链接。当前公开首页、协议和源码均可直接阅读，历史原始诊断、签名材料和录屏不随源码发布。
