import { hilog } from '@kit.PerformanceAnalysisKit';
import probe from 'libhrd_probe.so';
import { deviceInfo } from '@kit.BasicServicesKit';

interface DeviceSnapshot {
  appVersion: string;
  sessionMode: string;
  deviceType: string;
  osFullName: string;
  api: number;
  sdk: string;
}

interface CaptureSnapshot {
  status: string;
  running: boolean;
  frames: number;
  averageFps: number;
  elapsedSeconds: number;
}

interface InputSnapshot {
  authorized: boolean;
  dialogStatusName: string;
  queryCode: number;
  callbackCount: number;
}

interface EncoderSnapshot {
  status: string;
  running: boolean;
  frames: number;
  averageFps: number;
  elapsedSeconds: number;
  bytes: number;
}

interface LanSnapshot {
  running: boolean;
  status: string;
  bindAddress: string;
  pin: string;
  videoReady: boolean;
  streaming: boolean;
  sentFrames: number;
  error: string;
}

interface RemoteInputSnapshot {
  allowed: boolean;
  sessionEnabled: boolean;
  errorCode: number;
  pendingKeysCount: number;
  pendingButtonsCount: number;
  pendingAxesCount: number;
}

@Entry
@Component
struct Index {
  @StorageProp('probeFilesDir') filesDir: string = '';
  @State captureText: string = '{}';
  @State inputText: string = '{}';
  @State encoderText: string = '{}';
  @State inputPermission: string = 'UNKNOWN';
  @State frames: number = 0;
  @State fps: number = 0;
  @State elapsed: number = 0;
  @State captureRunning: boolean = false;
  @State encoderRunning: boolean = false;
  @State encoderStatus: string = 'idle';
  @State encoderFrames: number = 0;
  @State encoderFps: number = 0;
  @State encoderElapsed: number = 0;
  @State encoderBytes: number = 0;
  @State motionTick: number = 0;
  @State lanAddress: string = '';
  @State lanBoundAddress: string = '';
  @State lanRunning: boolean = false;
  @State lanStatus: string = 'stopped';
  @State lanPin: string = '';
  @State lanReady: boolean = false;
  @State lanStreaming: boolean = false;
  @State lanFrames: number = 0;
  @State lanError: string = '';
  @State showLocalTests: boolean = false;
  @State remoteAllowed: boolean = false;
  @State remoteEnabled: boolean = false;
  @State remoteError: number = 0;
  @State remotePending: number = 0;
  @StorageLink('hrdSessionMode') sessionMode: string = 'permanent';
  @State status: string = '等待连接';
  @State action: string = '';
  @State countdown: number = 0;
  @State diagnosticsStatus: string = '输入动作完成后自动保存诊断；保留本次启动最近 32 个动作。';
  private refreshTimer: number = -1;
  private actionTimer: number = -1;
  private motionTimer: number = -1;
  private authorizationSnapshot: string = '';
  private lanStateSnapshot: string = '';

  aboutToAppear(): void {
    if (this.sessionMode !== 'debug' && this.sessionMode !== 'permanent') {
      this.sessionMode = 'permanent';
    }
    this.refresh();
    this.persistDiagnostics();
    this.refreshTimer = setInterval(() => this.refresh(), 1000);
  }

  aboutToDisappear(): void {
    if (this.refreshTimer !== -1) { clearInterval(this.refreshTimer); }
    this.cancelAction();
    this.stopMotion();
    probe.stopCapture();
    probe.stopEncoder();
    probe.stopLanServer();
    probe.cancelInput();
  }

  private refresh(): void {
    this.captureText = probe.captureSnapshot();
    this.inputText = probe.inputSnapshot();
    this.encoderText = probe.encoderSnapshot();
    try {
      const snapshot = JSON.parse(this.captureText) as CaptureSnapshot;
      this.frames = snapshot.frames;
      this.fps = snapshot.averageFps;
      this.elapsed = snapshot.elapsedSeconds;
      this.captureRunning = snapshot.running;
      const encoder = JSON.parse(this.encoderText) as EncoderSnapshot;
      this.encoderRunning = encoder.running;
      if (!encoder.running) { this.stopMotion(); }
      this.encoderStatus = encoder.status;
      this.encoderFrames = encoder.frames;
      this.encoderFps = encoder.averageFps;
      this.encoderElapsed = encoder.elapsedSeconds;
      this.encoderBytes = encoder.bytes;
      const lan = JSON.parse(probe.lanSnapshot()) as LanSnapshot;
      this.lanRunning = lan.running;
      this.lanStatus = lan.status;
      this.lanBoundAddress = lan.bindAddress;
      this.lanPin = lan.pin;
      this.lanReady = lan.videoReady;
      this.lanStreaming = lan.streaming;
      this.lanFrames = lan.sentFrames;
      this.lanError = lan.error;
      const remote = JSON.parse(probe.remoteInputSnapshot()) as RemoteInputSnapshot;
      this.remoteAllowed = remote.allowed;
      this.remoteEnabled = remote.sessionEnabled;
      this.remoteError = remote.errorCode;
      this.remotePending = remote.pendingKeysCount + remote.pendingButtonsCount + remote.pendingAxesCount;
      const lanState = `${lan.status}:${lan.streaming}:${encoder.running}:${remote.allowed}:${remote.sessionEnabled}`;
      if (this.lanStateSnapshot !== '' && lanState !== this.lanStateSnapshot) {
        this.persistDiagnostics();
      }
      this.lanStateSnapshot = lanState;
      const input = JSON.parse(this.inputText) as InputSnapshot;
      this.inputPermission = input.queryCode === 0 ? input.dialogStatusName : `查询失败：${input.queryCode}`;
      const authorization = `${input.dialogStatusName}:${input.queryCode}:${input.callbackCount}`;
      if (this.authorizationSnapshot !== '' && authorization !== this.authorizationSnapshot) {
        this.persistDiagnostics();
      }
      this.authorizationSnapshot = authorization;
    } catch (error) {
      this.status = '状态 JSON 解析失败';
    }
  }

  private startCapture(): void {
    const code = probe.startCapture(this.filesDir);
    this.status = `Start Capture 返回 ${code}；请在系统弹窗选择整个主屏幕`;
    this.refresh();
  }

  private startEncoder(): void {
    const code = probe.startEncoder(this.filesDir);
    this.status = `Start H.264 返回 ${code}；请在系统弹窗选择整个主屏幕`;
    if (code === 0) { this.startMotion(); }
    this.refresh();
  }

  private startMotion(): void {
    this.stopMotion();
    this.motionTick = 0;
    this.motionTimer = setInterval(() => {
      this.motionTick = (this.motionTick + 1) % 120;
    }, 33);
  }

  private startLanServer(): void {
    const code = probe.startLanServer(this.lanAddress.trim());
    this.status = code === 0 ? '服务已启动；请在 Mac 接收端输入本页 PIN 配对。' : `启动服务失败：${code}`;
    this.refresh();
    this.persistDiagnostics();
  }

  private startLanCapture(): void {
    this.cancelAction();
    const code = probe.startLanCapture(this.filesDir, this.sessionMode === 'debug' ? 600 : 0);
    this.status = code === 0 ? (this.sessionMode === 'debug' ?
      '请允许共享整个主屏幕；开始共享后 10 分钟自动结束，可随时停止。' :
      '请允许共享整个主屏幕；永久上线模式不设自动结束时间，可随时停止。') : `局域网录屏启动失败：${code}`;
    if (code === 0 && this.sessionMode === 'debug') { this.startMotion(); }
    this.refresh();
  }

  private modeLabel(): string {
    return this.sessionMode === 'debug' ? '10 分钟调试' : '永久上线';
  }

  private serviceLabel(): string {
    if (this.lanStatus === 'LISTENING') { return '在线，等待连接'; }
    if (this.lanStatus === 'AWAITING_HELLO' || this.lanStatus === 'AWAITING_PAIR') { return '正在配对'; }
    if (this.lanStatus === 'PAIRED') { return '已配对'; }
    if (this.lanStatus === 'VIDEO_READY') { return '已连接，等待共享'; }
    if (this.lanStatus === 'STREAMING') { return this.encoderFrames > 0 ? '正在共享' : '等待共享授权'; }
    if (this.lanStatus === 'DRAINING') { return '正在结束共享'; }
    if (this.lanStatus === 'STREAM_FAILED') { return '共享已中断'; }
    return this.lanRunning ? '服务准备中' : '未上线';
  }

  private stopMotion(): void {
    if (this.motionTimer !== -1) { clearInterval(this.motionTimer); }
    this.motionTimer = -1;
  }

  private stopEncoder(): void {
    this.stopMotion();
    probe.stopEncoder();
    this.refresh();
  }

  private persistDiagnostics(): void {
    const device: DeviceSnapshot = {
      appVersion: '0.5.0',
      sessionMode: this.sessionMode,
      deviceType: deviceInfo.deviceType,
      osFullName: deviceInfo.osFullName,
      api: deviceInfo.sdkApiVersion,
      sdk: '26.0.0.105'
    };
    const code = probe.saveDiagnostics(this.filesDir, JSON.stringify(device));
    this.diagnosticsStatus = code === 0 ? '诊断已保存。可连续执行多个测试，完成后统一导出。' : `诊断保存失败：${code}`;
  }

  private saveDiagnostics(): void {
    this.persistDiagnostics();
    this.status = `诊断目录：${this.filesDir}`;
  }

  private cancelAction(): void {
    if (this.actionTimer !== -1) { clearInterval(this.actionTimer); }
    this.actionTimer = -1;
    this.countdown = 0;
    this.action = '';
  }

  private scheduleAction(name: string): void {
    this.cancelAction();
    this.action = name;
    this.countdown = 5;
    this.status = '5 秒后执行，请切换到目标应用；键盘测试需先聚焦空白文本框。';
    this.actionTimer = setInterval(() => {
      this.countdown -= 1;
      if (this.countdown > 0) { return; }
      const pending = this.action;
      this.cancelAction();
      let code: number = -1;
      if (pending === 'Move Mouse') { code = probe.moveMouseToCenter(); }
      if (pending === 'Inject A') { code = probe.injectA(); }
      if (pending === 'Left Click') { code = probe.clickLeft(); }
      if (pending === 'Ctrl+L') { code = probe.injectCtrlL(); }
      this.status = `${pending} 返回 ${code}；请人工确认其他应用的实际效果。`;
      hilog.info(0x0000, 'HRD', 'INPUT %{public}s result=%{public}d', pending, code);
      this.refresh();
      this.persistDiagnostics();
    }, 1000);
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        Text('Harmony Remote · v0.5.0').fontSize(28).fontWeight(FontWeight.Bold)
        Text('局域网桌面共享与键鼠控制 · API 26 · PC/2in1').fontSize(16)
        Text('连接 Mac 接收端').fontSize(22).fontWeight(FontWeight.Medium).margin({ top: 14 })
        Text('仅用于可信局域网；当前视频传输未加密。启动服务后，使用一次性 PIN 配对。').fontSize(15)
        TextInput({ placeholder: '本机局域网 IPv4，留空自动选择', text: this.lanAddress })
          .enabled(!this.lanRunning).onChange((value: string) => { this.lanAddress = value; }).width(430)
        Row({ space: 12 }) {
          Button('Start Server').enabled(!this.lanRunning).onClick(() => this.startLanServer())
          Button('Stop Server').enabled(this.lanRunning).onClick(() => {
            this.stopMotion();
            probe.stopLanServer();
            this.refresh();
            this.persistDiagnostics();
          })
        }
        Text(`${this.modeLabel()} · ${this.serviceLabel()} · ${this.lanBoundAddress} · 已发送 ${this.lanFrames} 帧`).fontSize(18)
        if (this.lanPin.length > 0) {
          Text(`配对 PIN：${this.lanPin}`).fontSize(28).fontWeight(FontWeight.Bold)
          Text('5 分钟内有效，使用一次即失效。断线后请停止并重新启动服务生成新 PIN。').fontSize(14)
        }
        if (this.lanError.length > 0) { Text(this.lanError).fontColor('#A83220').fontSize(14) }
        Row({ space: 12 }) {
          Select([{ value: '10 分钟调试' }, { value: '永久上线' }])
            .selected(this.sessionMode === 'debug' ? 0 : 1)
            .value(this.modeLabel())
            .enabled(!this.encoderRunning && !this.captureRunning)
            .onSelect((index: number) => {
              this.sessionMode = index === 0 ? 'debug' : 'permanent';
              this.showLocalTests = false;
              this.persistDiagnostics();
            })
          Button('Start LAN Capture')
            .enabled(this.lanReady && !this.encoderRunning && !this.captureRunning && !this.lanStreaming)
            .onClick(() => this.startLanCapture())
          Button('停止共享').enabled(this.encoderRunning).onClick(() => this.stopEncoder())
        }
        Text(this.sessionMode === 'debug' ? '调试共享从首帧开始计时，10 分钟后自动结束。' : '永久共享不设时长上限；主动停止、连接中断或关闭应用时结束。').fontSize(14)
        Text('两种模式均只传输画面，不保存桌面录像。').fontSize(14)
        Text(this.lanStreaming ? '共享期间可随时点击「停止共享」。' : this.lanReady ? 'Mac 已连接。点击 Start LAN Capture，在系统弹窗允许共享。' : '等待 Mac 配对；配对后手动开始共享。').fontSize(14)
        Text(`键鼠授权：${this.inputPermission} · ${this.remoteEnabled ? 'Mac 正在控制' : this.remoteAllowed ? '等待 Mac 启用控制' : '仅查看'}`).fontSize(18)
        Row({ space: 12 }) {
          Button('Request Input Permission').enabled(!this.remoteEnabled).onClick(() => {
            this.cancelAction();
            this.status = `输入授权请求返回 ${probe.requestInputAuthorization()}；请在系统弹窗确认。`;
            this.refresh();
            this.persistDiagnostics();
          })
          Button(this.remoteAllowed ? '关闭远程控制' : '允许远程控制')
            .enabled(this.lanRunning && (this.remoteAllowed || this.inputPermission === 'AUTHORIZED'))
            .onClick(() => {
              this.cancelAction();
              probe.allowRemoteInput(!this.remoteAllowed);
              this.refresh();
              this.persistDiagnostics();
            })
          Button('释放并撤销输入授权').onClick(() => {
            this.cancelAction();
            this.status = `撤销授权返回 ${probe.cancelInput()}`;
            this.refresh();
            this.persistDiagnostics();
          })
        }
        Text('启用后，已配对的 Mac 可操作整个桌面。停止共享、断线或关闭控制时释放按键。').fontSize(14)
        if (this.remoteError !== 0 || (!this.remoteEnabled && this.remotePending > 0)) {
          Text(`输入诊断：返回码 ${this.remoteError}；待释放 ${this.remotePending}。失败后会停止控制，请重新检查授权。`)
            .fontSize(14).fontColor('#A83220')
        }
        if (this.encoderRunning && this.lanStreaming) {
          Text(`${this.encoderFrames} 帧 · ${this.encoderFps.toFixed(1)} FPS · ${this.encoderElapsed.toFixed(1)} 秒`).fontSize(19)
          if (this.sessionMode === 'debug') {
          Stack({ alignContent: Alignment.TopStart }) {
            Rect().width(24).height(44).fill('#1677FF').position({ x: this.motionTick * 3, y: 0 })
          }.width(384).height(44).backgroundColor('#DCEBFA').clip(true)
          Text(`画面变化计数：${this.motionTick}`).fontSize(13)
          }
        }
        if (this.sessionMode === 'debug') {
        Button(this.showLocalTests ? '收起本机能力测试' : '展开本机能力测试')
          .onClick(() => { this.showLocalTests = !this.showLocalTests; })
        }
        if (this.showLocalTests && !this.lanRunning) {
        Text('录屏测试').fontSize(22).fontWeight(FontWeight.Medium).margin({ top: 14 })
        Text(`Capture Frames: ${this.frames}    FPS: ${this.fps.toFixed(1)}    ${this.elapsed.toFixed(1)} 秒`).fontSize(19)
        Row({ space: 12 }) {
          Button('Start Capture · 30s').enabled(!this.captureRunning && !this.encoderRunning).onClick(() => this.startCapture())
          Button('Stop Capture').onClick(() => { probe.stopCapture(); this.refresh(); })
        }
        Text('允许录屏后切换到桌面。数值达标仍需检查保存的画面来自整个桌面。')
        Text('H.264 编码测试').fontSize(22).fontWeight(FontWeight.Medium).margin({ top: 14 })
        Text(`${this.encoderStatus} · ${this.encoderFrames} 帧 · ${this.encoderFps.toFixed(1)} FPS · ${this.encoderElapsed.toFixed(1)} 秒 · ${(this.encoderBytes / 1024).toFixed(0)} KB`).fontSize(19)
        Row({ space: 12 }) {
          Button('Start H.264 · 10s').enabled(!this.encoderRunning && !this.captureRunning).onClick(() => this.startEncoder())
          Button('Stop H.264').onClick(() => this.stopEncoder())
        }
        Text('允许共享整个主屏幕后，可留在本页观察自动移动图案，约 10 秒后停止。也可切换应用测试其他画面。')
        if (this.encoderRunning) {
          Text('自动动态测试区域').fontSize(14)
          Stack({ alignContent: Alignment.TopStart }) {
            Rect().width(24).height(44).fill('#1677FF').position({ x: this.motionTick * 3, y: 0 })
          }.width(384).height(44).backgroundColor('#DCEBFA').clip(true)
          Text(`画面变化计数：${this.motionTick}`).fontSize(13)
        }
        Text('输入测试').fontSize(22).fontWeight(FontWeight.Medium).margin({ top: 14 })
        Text(`Input Permission: ${this.inputPermission}`).fontSize(19)
        Row({ space: 12 }) {
          Button('Request Input Permission').onClick(() => {
            this.status = `RequestInjection 返回 ${probe.requestInputAuthorization()}；授权状态见下方。`;
            this.refresh();
            this.persistDiagnostics();
          })
          Button('刷新授权状态').onClick(() => this.refresh())
        }
        Text('每个动作延迟 5 秒，留出切换应用时间。仅在安全的空白输入框测试。')
        Row({ space: 12 }) {
          Button('Move Mouse to Center').onClick(() => this.scheduleAction('Move Mouse'))
          Button('Inject A').onClick(() => this.scheduleAction('Inject A'))
          Button('中心左键点击').onClick(() => this.scheduleAction('Left Click'))
          Button('Ctrl+L').onClick(() => this.scheduleAction('Ctrl+L'))
        }
        TextInput({ placeholder: '本应用测试输入框：点击 Inject A 后，将光标放在这里。' })
          .width('100%').maxLength(100)
        if (this.countdown > 0) {
          Text(`${this.action} · ${this.countdown} 秒后执行`).fontColor('#B34400').fontSize(24)
        }
        Button('取消待执行动作 / 释放按键 / 撤销输入授权').onClick(() => {
          this.cancelAction();
          this.status = `Cancel/Release 返回 ${probe.cancelInput()}`;
          this.refresh();
          this.persistDiagnostics();
        })
        }
        Text(this.status).fontSize(16).fontColor('#17468A')
        Text(this.diagnosticsStatus).fontSize(14).fontColor('#405466')
        Button('保存当前诊断到应用目录').onClick(() => this.saveDiagnostics())
        Text(`诊断文件目录：${this.filesDir}`).fontSize(13).copyOption(CopyOptions.InApp)
        if (this.showLocalTests) {
        Text('Capture diagnostics').fontSize(18).fontWeight(FontWeight.Medium)
        Text(this.captureText).fontSize(13).fontFamily('monospace').copyOption(CopyOptions.InApp).width('100%')
        Text('Input diagnostics').fontSize(18).fontWeight(FontWeight.Medium)
        Text(this.inputText).fontSize(13).fontFamily('monospace').copyOption(CopyOptions.InApp).width('100%')
        Text('Encoder diagnostics').fontSize(18).fontWeight(FontWeight.Medium)
        Text(this.encoderText).fontSize(13).fontFamily('monospace').copyOption(CopyOptions.InApp).width('100%')
        }
        Text('关闭应用会结束服务与共享；下次打开时保留模式选择。').fontSize(14)
      }.alignItems(HorizontalAlign.Start).padding(28).width('100%').constraintSize({ maxWidth: 1100 })
    }.width('100%').height('100%').backgroundColor('#F4F6FA')
  }
}
