import { hilog } from '@kit.PerformanceAnalysisKit';
import probe from 'libhrd_probe.so';
import { deviceInfo } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { ClipboardPermission } from '../services/ClipboardPermission';

interface DeviceSnapshot {
  appVersion: string;
  appBuild: number;
  sessionMode: string;
  deviceType: string;
  osFullName: string;
  api: number;
  sdk: string;
}

interface ClipboardSnapshot {
  allowed: boolean;
  bound: boolean;
  mode: number;
  canRead: boolean;
  canWrite: boolean;
  sent: number;
  received: number;
  applied: number;
  readCategory: string;
  readTypeCount: number;
  readRecordCount: number;
  error: string;
  errorCode: number;
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
  @State currentPage: string = 'remote';
  @State operationDetail: string = '尚未执行操作';
  @State clipboardText: string = '{}';
  @State clipboardError: boolean = false;
  @State remoteAllowed: boolean = false;
  @State remoteEnabled: boolean = false;
  @State remoteError: number = 0;
  @State remotePending: number = 0;
  @State clipboardAllowed: boolean = false;
  @State clipboardBound: boolean = false;
  @State clipboardReadGranted: boolean = false;
  @State clipboardPermissionBusy: boolean = false;
  @State clipboardSummary: string = '关闭';
  @State clipboardNotice: string = '';
  private clipboardStateSnapshot: string = '';
  private pageScroller: Scroller = new Scroller();
  @StorageLink('hrdSessionMode') sessionMode: string = 'permanent';
  @State status: string = '';
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
    probe.allowClipboard(false);
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
      this.clipboardReadGranted = ClipboardPermission.granted();
      probe.setClipboardPermission(this.clipboardReadGranted);
      const clipboard = JSON.parse(probe.clipboardSnapshot()) as ClipboardSnapshot;
      this.clipboardText = JSON.stringify(clipboard);
      this.clipboardAllowed = clipboard.allowed;
      this.clipboardBound = clipboard.bound;
      const directions: string[] = ['关闭', 'Mac → 鸿蒙', '鸿蒙 → Mac', '双向'];
      this.clipboardSummary = !clipboard.allowed ? '关闭' : !clipboard.bound ? '等待 Mac 连接' :
        directions[clipboard.mode] ?? '关闭';
      this.clipboardError = clipboard.error.length > 0;
      const clipboardState = `${clipboard.allowed}:${clipboard.bound}:${clipboard.mode}:${clipboard.canRead}:${clipboard.canWrite}:${clipboard.sent}:${clipboard.applied}:${clipboard.errorCode}:${clipboard.error}:${clipboard.readCategory}:${clipboard.readTypeCount}:${clipboard.readRecordCount}`;
      if (this.clipboardStateSnapshot !== '' && clipboardState !== this.clipboardStateSnapshot) {
        this.persistDiagnostics();
      }
      this.clipboardStateSnapshot = clipboardState;
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
      this.updateMotion();
    } catch (error) {
      this.status = '暂时无法更新状态，请稍后重试。';
      this.operationDetail = '状态 JSON 解析失败';
    }
  }

  private startCapture(): void {
    const code = probe.startCapture(this.filesDir);
    this.setOperationResult(code, '请在系统弹窗选择整个主屏幕，开始 30 秒录屏测试。', '无法开始录屏测试，请检查开发测试中的诊断。');
    this.refresh();
  }

  private async requestClipboardPermission(): Promise<void> {
    if (this.clipboardPermissionBusy) { return; }
    this.clipboardPermissionBusy = true;
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      this.clipboardReadGranted = await ClipboardPermission.request(context);
      probe.setClipboardPermission(this.clipboardReadGranted);
      this.clipboardNotice = this.clipboardReadGranted ? '读取权限已允许；开启同步后可将鸿蒙文字同步到 Mac。' :
        '读取未获授权，鸿蒙 → Mac 暂不可用。可在系统设置中允许；Mac → 鸿蒙接收仍可单独使用。';
    } catch (error) {
      const code = (error as BusinessError).code;
      this.clipboardNotice = '读取授权失败，请检查系统权限。详细信息可在开发测试中查看。';
      this.operationDetail = `读取授权失败 (${code})；请核对系统授权及签名 READ_PASTEBOARD。`;
    } finally {
      this.clipboardPermissionBusy = false;
      this.refresh();
      this.persistDiagnostics();
    }
  }

  private toggleClipboard(): void {
    const code = probe.allowClipboard(!this.clipboardAllowed);
    this.clipboardNotice = code === 0 ? '同步方向在 Mac 端选择；开启或重连不会交换原有剪贴板。' : '无法更改同步状态，请检查开发测试中的诊断。';
    this.operationDetail = `剪贴板开关返回 ${code}`;
    this.refresh();
    this.persistDiagnostics();
  }

  private startEncoder(): void {
    const code = probe.startEncoder(this.filesDir);
    this.setOperationResult(code, '请在系统弹窗选择整个主屏幕，开始 10 秒编码测试。', '无法开始编码测试，请检查开发测试中的诊断。');
    this.refresh();
  }

  private startMotion(): void {
    if (this.currentPage !== 'developer' || !this.encoderRunning ||
      (this.lanStreaming && this.sessionMode !== 'debug')) { return; }
    this.stopMotion();
    this.motionTick = 0;
    this.motionTimer = setInterval(() => {
      this.motionTick = (this.motionTick + 1) % 120;
    }, 33);
  }

  private startLanServer(): void {
    const code = probe.startLanServer(this.lanAddress.trim());
    this.setOperationResult(code, '服务已上线。在 Mac 输入本机地址与连接码进行配对。', '服务启动失败，请检查本机地址或开发测试中的诊断。');
    this.refresh();
    this.persistDiagnostics();
  }

  private startLanCapture(): void {
    this.cancelAction();
    const code = probe.startLanCapture(this.filesDir, this.sessionMode === 'debug' ? 600 : 0);
    this.setOperationResult(code, this.sessionMode === 'debug' ?
      '请允许共享整个主屏幕；开始共享后 10 分钟自动结束，可随时停止。' :
      '请允许共享整个主屏幕；永久上线模式不设自动结束时间，可随时停止。',
      '无法开始共享，请检查连接状态或开发测试中的诊断。');
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
      appBuild: 1000010,
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
    this.status = '诊断保存结果与目录可在开发测试中查看。';
  }

  private cancelAction(): void {
    if (this.actionTimer !== -1) { clearInterval(this.actionTimer); }
    this.actionTimer = -1;
    this.countdown = 0;
    this.action = '';
  }

  private scheduleAction(name: string): void {
    if (this.lanRunning || this.currentPage !== 'developer') { return; }
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
      this.setOperationResult(code, '测试动作已执行，请人工确认目标应用中的实际效果。', '测试动作未完成，请检查开发测试中的诊断。');
      this.operationDetail = `${pending} 返回 ${code}`;
      hilog.info(0x0000, 'HRD', 'INPUT %{public}s result=%{public}d', pending, code);
      this.refresh();
      this.persistDiagnostics();
    }, 1000);
  }

  private setOperationResult(code: number, success: string, failure: string): void {
    this.status = code === 0 ? success : failure;
    this.operationDetail = `${code === 0 ? success : failure} 返回 ${code}`;
  }

  private selectPage(page: string): void {
    if (this.currentPage === page) { return; }
    if (this.currentPage === 'developer') {
      if (this.countdown > 0) { this.status = '已取消等待中的本机输入测试。'; }
      this.cancelAction();
    }
    this.currentPage = page;
    this.pageScroller.scrollEdge(Edge.Top);
    this.updateMotion();
  }

  private updateMotion(): void {
    const shouldRun = this.currentPage === 'developer' && this.encoderRunning &&
      (!this.lanStreaming || this.sessionMode === 'debug');
    if (!shouldRun) { this.stopMotion(); }
    else if (this.motionTimer === -1) { this.startMotion(); }
  }

  private stopLanServer(): void {
    this.cancelAction();
    this.stopMotion();
    probe.stopLanServer();
    this.status = '服务已下线，连接与共享已结束。再次上线将生成新的连接码。';
    this.refresh();
    this.persistDiagnostics();
  }

  private requestInputPermission(): void {
    this.cancelAction();
    const code = probe.requestInputAuthorization();
    this.setOperationResult(code, '请在系统弹窗中允许键鼠控制。', '无法请求键鼠权限，请检查开发测试中的诊断。');
    this.refresh();
    this.persistDiagnostics();
  }

  private toggleRemoteControl(): void {
    this.cancelAction();
    const willAllow = !this.remoteAllowed;
    const code = probe.allowRemoteInput(willAllow);
    this.setOperationResult(code, willAllow ? '已允许远程控制，等待 Mac 启用控制。' : '远程控制已关闭，已释放按键。',
      '无法更改远程控制状态，请重新检查授权。');
    this.refresh();
    this.persistDiagnostics();
  }

  private revokeInput(): void {
    this.cancelAction();
    const code = probe.cancelInput();
    this.setOperationResult(code, '已释放按键并撤销输入授权。', '撤销输入授权遇到问题，请检查开发测试中的诊断。');
    this.refresh();
    this.persistDiagnostics();
  }

  private inputPermissionLabel(): string {
    if (this.inputPermission === 'AUTHORIZED') { return '已允许'; }
    if (this.inputPermission === 'UNKNOWN') { return '尚未授权'; }
    return '需要授权';
  }

  private controlLabel(): string {
    return this.remoteEnabled ? 'Mac 正在控制' : this.remoteAllowed ? '等待 Mac 启用控制' : '仅查看';
  }

  private sharingLabel(): string {
    if (this.captureRunning) { return '本机录屏测试中，可在开发测试停止'; }
    if (this.encoderRunning && !this.lanRunning) { return '本机编码测试中，可在开发测试停止'; }
    if (this.lanStreaming && this.encoderFrames > 0) { return '正在共享屏幕'; }
    if (this.lanStreaming || this.encoderRunning) { return '等待共享授权'; }
    if (this.lanReady) { return '已连接，可以开始共享'; }
    return '等待 Mac 配对';
  }

  @Builder
  navItem(page: string, mark: string, label: string) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 12 }) {
        Text(mark).fontSize(18).fontWeight(FontWeight.Medium).width(24)
          .fontColor(this.currentPage === page ? '#2459BD' : '#58677E')
        Text(label).fontSize(15).fontWeight(this.currentPage === page ? FontWeight.Medium : FontWeight.Regular)
          .fontColor(this.currentPage === page ? '#2459BD' : '#58677E')
      }.width('100%').padding({ left: 16, right: 12, top: 14, bottom: 14 })
    }
    .width('100%').height(48).borderRadius(10)
    .backgroundColor(this.currentPage === page ? '#E7F0FF' : Color.Transparent)
    .fontColor(this.currentPage === page ? '#2459BD' : '#58677E')
    .onClick(() => this.selectPage(page))
  }

  @Builder
  sidebar() {
    Column({ space: 8 }) {
      Row({ space: 10 }) {
        Text('H').fontSize(24).fontWeight(FontWeight.Bold).fontColor('#FFFFFF')
          .width(36).height(36).textAlign(TextAlign.Center).backgroundColor('#2964D8').borderRadius(10)
        Column({ space: 2 }) {
          Text('Harmony').fontSize(17).fontWeight(FontWeight.Bold).fontColor('#172A46')
          Text('REMOTE DESKTOP').fontSize(9).fontColor('#7B8BA2').letterSpacing(1)
        }.alignItems(HorizontalAlign.Start)
      }.width('100%').padding({ left: 12, top: 10, bottom: 28 })
      this.navItem('remote', '↗', '远程桌面')
      this.navItem('settings', '⚙', '设置')
      this.navItem('developer', '⌘', '开发测试')
      Blank()
      Column({ space: 7 }) {
        Row({ space: 7 }) {
          Circle().width(7).height(7).fill(this.lanRunning ? '#27A27B' : '#A6B1BF')
          Text(this.lanRunning ? '本机服务在线' : '本机服务未上线').fontSize(12).fontColor('#64748B')
        }
        Text('v0.5.0 · 局域网连接').fontSize(11).fontColor('#93A0B2')
      }.alignItems(HorizontalAlign.Start).width('100%').padding({ left: 16, bottom: 16, top: 16 })
    }.width(184).height('100%').padding({ left: 12, right: 12, top: 16, bottom: 12 })
      .backgroundColor('#FFFFFF').border({ width: { right: 1 }, color: '#E5EAF1' })
  }

  @Builder
  pageHeading(title: string, subtitle: string) {
    Column({ space: 7 }) {
      Text(title).fontSize(26).fontWeight(FontWeight.Bold).fontColor('#182B48')
      Text(subtitle).fontSize(13).fontColor('#708198').lineHeight(19)
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  cardHeading(title: string, description: string) {
    Column({ space: 6 }) {
      Text(title).fontSize(17).fontWeight(FontWeight.Medium).fontColor('#243650')
      if (description.length > 0) {
        Text(description).fontSize(12).fontColor('#75859B').lineHeight(18)
      }
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  statusNotice() {
    if (this.status.length > 0) {
      Row({ space: 10 }) {
        Text('i').fontSize(13).fontColor('#4772B5').fontWeight(FontWeight.Bold)
          .width(18).height(18).borderRadius(9).textAlign(TextAlign.Center).backgroundColor('#E3ECFA')
        Text(this.status).fontSize(12).fontColor('#527097').lineHeight(18).layoutWeight(1)
      }.padding(14).width('100%').backgroundColor('#EDF3FC').borderRadius(10)
    }
  }

  @Builder
  deviceCard() {
    Column({ space: 14 }) {
      Row() {
        Text('此设备').fontSize(17).fontWeight(FontWeight.Medium).fontColor('#243650')
        Blank()
        Text(this.serviceLabel()).fontSize(11).fontColor(this.lanRunning ? '#237D63' : '#738096')
          .padding({ left: 9, right: 9, top: 5, bottom: 5 })
          .backgroundColor(this.lanRunning ? '#E8F7F0' : '#F0F3F7').borderRadius(6)
      }.width('100%')
      Column({ space: 7 }) {
        Text('本机地址').fontSize(12).fontColor('#78889E')
        Text(this.lanBoundAddress.length > 0 ? this.lanBoundAddress : '上线后显示')
          .fontSize(this.lanBoundAddress.length > 0 ? 23 : 18).fontWeight(FontWeight.Medium).fontColor('#203552')
        Text('连接码').fontSize(12).fontColor('#78889E').margin({ top: 4 })
        Text(this.lanPin.length > 0 ? this.lanPin : '— — — — — —')
          .fontSize(30).fontWeight(FontWeight.Bold).fontColor(this.lanPin.length > 0 ? '#2964D8' : '#BCC7D6')
          .letterSpacing(4)
      }.width('100%').alignItems(HorizontalAlign.Start)
      Text(this.lanPin.length > 0 ? '连接码 5 分钟内有效，仅可使用一次。' : '开启服务后，将地址与连接码输入 Mac 客户端。')
        .fontSize(12).fontColor('#7B8AA0').lineHeight(18)
      Button(this.lanRunning ? '下线并断开连接' : '开启服务')
        .type(ButtonType.Normal).borderRadius(8).width('100%').height(40)
        .backgroundColor(this.lanRunning ? '#F0F3F8' : '#2964D8').fontColor(this.lanRunning ? '#4B607C' : '#FFFFFF')
        .enabled(this.lanRunning || !this.encoderRunning && !this.captureRunning)
        .fontSize(14).onClick(() => { if (this.lanRunning) { this.stopLanServer(); } else { this.startLanServer(); } })
      if (!this.lanRunning && (this.captureRunning || this.encoderRunning)) {
        Text('请先在开发测试中停止当前测试，再开启服务。').fontSize(12).fontColor('#9A783D').lineHeight(18)
      }
      if (this.lanError.length > 0) {
        Text('连接遇到问题，请检查网络或到开发测试查看详情。').fontSize(12).fontColor('#B24336').lineHeight(18)
      }
    }.alignItems(HorizontalAlign.Start).width('100%').padding(20).backgroundColor('#FFFFFF').borderRadius(12)
  }

  @Builder
  sharingCard() {
    Column({ space: 12 }) {
      this.cardHeading('屏幕共享', this.sharingLabel())
      Row({ space: 6 }) {
        Text(this.modeLabel()).fontSize(11).fontColor('#687E9D').padding({ left: 7, right: 7, top: 4, bottom: 4 })
          .backgroundColor('#F1F5FA').borderRadius(5)
        Text('可在设置中更改').fontSize(11).fontColor('#94A0B0')
      }
      Button(this.encoderRunning && this.lanStreaming ? '停止共享' : '开始共享屏幕')
        .type(ButtonType.Normal).borderRadius(8).width('100%').height(40)
        .backgroundColor(this.encoderRunning && this.lanStreaming ? '#FFF0EB' : '#EAF1FE')
        .fontColor(this.encoderRunning && this.lanStreaming ? '#B85535' : '#2A60C3').fontSize(14)
        .enabled(this.encoderRunning && this.lanStreaming ||
          this.lanReady && !this.encoderRunning && !this.captureRunning && !this.lanStreaming)
        .onClick(() => {
          if (this.encoderRunning && this.lanStreaming) {
            this.stopEncoder();
            this.status = '屏幕共享已停止。连接保留，可手动再次共享。';
          } else { this.startLanCapture(); }
        })
      Text('共享需要本机确认；传输画面不会保存为录像。').fontSize(12).fontColor('#7B8AA0').lineHeight(18)
    }.alignItems(HorizontalAlign.Start).width('100%').padding(20).backgroundColor('#FFFFFF').borderRadius(12)
  }

  @Builder
  inputCard() {
    Column({ space: 13 }) {
      this.cardHeading('键鼠控制', '允许已配对的 Mac 操作此电脑。')
      Row() {
        Text('系统权限').fontSize(12).fontColor('#78889E')
        Blank()
        Text(this.inputPermissionLabel()).fontSize(12).fontColor(this.inputPermission === 'AUTHORIZED' ? '#237D63' : '#A17130')
      }.width('100%')
      Row() {
        Text('控制状态').fontSize(12).fontColor('#78889E')
        Blank()
        Text(this.controlLabel()).fontSize(12).fontColor('#405978')
      }.width('100%')
      if (this.inputPermission !== 'AUTHORIZED') {
        Button('授权键鼠权限').type(ButtonType.Normal).borderRadius(8).height(36).width('100%')
          .backgroundColor('#EAF1FE').fontColor('#2A60C3').fontSize(13).enabled(!this.remoteEnabled)
          .onClick(() => this.requestInputPermission())
      }
      Button(this.remoteAllowed ? '关闭远程控制' : '允许远程控制')
        .type(ButtonType.Normal).borderRadius(8).height(36).width('100%')
        .backgroundColor(this.remoteAllowed ? '#F0F3F8' : '#EAF1FE')
        .fontColor(this.remoteAllowed ? '#4B607C' : '#2A60C3').fontSize(13)
        .enabled(this.lanRunning && (this.remoteAllowed || this.inputPermission === 'AUTHORIZED'))
        .onClick(() => this.toggleRemoteControl())
      if (this.remoteError !== 0 || (!this.remoteEnabled && this.remotePending > 0)) {
        Text('控制已暂停，请重新检查授权。详细信息见开发测试。').fontSize(12).fontColor('#B24336').lineHeight(18)
      }
      Text('断线、停止共享或关闭控制会释放按键。').fontSize(12).fontColor('#7B8AA0').lineHeight(18)
    }.alignItems(HorizontalAlign.Start).width('100%').padding(20).backgroundColor('#FFFFFF').borderRadius(12)
  }

  @Builder
  clipboardCard() {
    Column({ space: 13 }) {
      this.cardHeading('文字剪贴板', '同步新复制的文字，方向在 Mac 端选择。')
      Row() {
        Text('读取权限').fontSize(12).fontColor('#78889E')
        Blank()
        Text(this.clipboardReadGranted ? '已允许' : '未允许').fontSize(12)
          .fontColor(this.clipboardReadGranted ? '#237D63' : '#A17130')
      }.width('100%')
      Row() {
        Text('同步状态').fontSize(12).fontColor('#78889E')
        Blank()
        Text(this.clipboardSummary).fontSize(12).fontColor('#405978')
      }.width('100%')
      if (!this.clipboardReadGranted) {
        Button(this.clipboardPermissionBusy ? '正在请求…' : '允许读取剪贴板')
          .type(ButtonType.Normal).borderRadius(8).height(36).width('100%')
          .backgroundColor('#EAF1FE').fontColor('#2A60C3').fontSize(13)
          .enabled(!this.clipboardPermissionBusy).onClick(() => { this.requestClipboardPermission(); })
      }
      Button(this.clipboardAllowed ? '关闭剪贴板同步' : '允许剪贴板同步')
        .type(ButtonType.Normal).borderRadius(8).height(36).width('100%')
        .backgroundColor(this.clipboardAllowed ? '#F0F3F8' : '#EAF1FE')
        .fontColor(this.clipboardAllowed ? '#4B607C' : '#2A60C3').fontSize(13)
        .enabled(this.lanRunning || this.clipboardAllowed).onClick(() => this.toggleClipboard())
      if (this.clipboardError) {
        Text('部分文字暂未同步，请到开发测试查看详情。').fontSize(12).fontColor('#B24336').lineHeight(18)
      }
      Text('仅纯文字，不保留排版；文件与图片暂不支持。').fontSize(12).fontColor('#7B8AA0').lineHeight(18)
      if (this.clipboardNotice.length > 0) {
        Text(this.clipboardNotice).fontSize(11).fontColor('#7086A5').lineHeight(17)
      }
    }.alignItems(HorizontalAlign.Start).width('100%').padding(20).backgroundColor('#FFFFFF').borderRadius(12)
  }

  @Builder
  remotePage() {
    Column({ space: 16 }) {
      this.pageHeading('远程桌面', '在此电脑允许连接后，从 Mac 查看与操作桌面。')
      Row({ space: 16 }) {
        Column({ space: 16 }) {
          this.deviceCard()
          this.sharingCard()
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        Column({ space: 16 }) {
          this.inputCard()
          this.clipboardCard()
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      }.width('100%').alignItems(VerticalAlign.Top)
      this.statusNotice()
      Text('仅限可信局域网 · 当前连接未加密 · 关闭应用将结束服务与共享')
        .fontSize(11).fontColor('#8996A8').lineHeight(18)
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  settingsPage() {
    Column({ space: 18 }) {
      this.pageHeading('设置', '设置本机连接与共享行为。')
      Column({ space: 16 }) {
        this.cardHeading('会话模式', '模式只决定共享时长，开发测试页面在两种模式下均可使用。')
        Select([{ value: '10 分钟调试' }, { value: '永久上线' }])
          .selected(this.sessionMode === 'debug' ? 0 : 1).value(this.modeLabel())
          .enabled(!this.encoderRunning && !this.captureRunning)
          .onSelect((index: number) => {
            this.sessionMode = index === 0 ? 'debug' : 'permanent';
            this.updateMotion();
            this.persistDiagnostics();
          })
        Text(this.sessionMode === 'debug' ? '从第一帧开始计时，10 分钟后自动停止共享。' :
          '不设共享时长上限；主动停止、连接中断或关闭应用时结束。')
          .fontSize(13).fontColor('#61738C').lineHeight(20)
        if (this.encoderRunning || this.captureRunning) {
          Text('停止当前共享或测试后，可更改会话模式。').fontSize(12).fontColor('#9A783D')
        }
        Text('下次打开会记住模式选择，但不会自动上线或开始共享。').fontSize(12).fontColor('#8796AA')
      }.alignItems(HorizontalAlign.Start).width('100%').padding(22).backgroundColor('#FFFFFF').borderRadius(12)
      Column({ space: 16 }) {
        this.cardHeading('本机连接地址', '留空时自动选择局域网 IPv4 地址。地址只用于此电脑的服务。')
        TextInput({ placeholder: '自动选择局域网地址', text: this.lanAddress })
          .enabled(!this.lanRunning).onChange((value: string) => { this.lanAddress = value; })
          .width('100%').height(42).borderRadius(8).backgroundColor('#F2F5FA')
        if (this.lanRunning) {
          Text('服务运行中。请先在远程桌面页面下线，再修改地址。').fontSize(12).fontColor('#8796AA')
        }
        Text('使用一次性连接码配对，仅限可信局域网。当前连接未加密，请勿直接暴露到公网。')
          .fontSize(12).fontColor('#61738C').lineHeight(20)
      }.alignItems(HorizontalAlign.Start).width('100%').padding(22).backgroundColor('#FFFFFF').borderRadius(12)
      Column({ space: 16 }) {
        this.cardHeading('输入授权', '可随时释放所有按键，并撤销本次输入授权。')
        Button('释放按键并撤销授权').type(ButtonType.Normal).height(38).borderRadius(8)
          .fontSize(13).backgroundColor('#FFF0EB').fontColor('#B85535').onClick(() => this.revokeInput())
        Text('重新控制前，需要再次请求本机系统授权。').fontSize(12).fontColor('#8796AA')
      }.alignItems(HorizontalAlign.Start).width('100%').padding(22).backgroundColor('#FFFFFF').borderRadius(12)
      this.statusNotice()
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  motionPattern() {
    if (this.encoderRunning && (!this.lanStreaming || this.sessionMode === 'debug')) {
      Column({ space: 8 }) {
        Text('动态画面测试').fontSize(12).fontColor('#6A7F9C')
        Stack({ alignContent: Alignment.TopStart }) {
          Rect().width(24).height(44).fill('#2964D8').position({ x: this.motionTick * 3, y: 0 })
        }.width(384).height(44).backgroundColor('#E4EDFA').clip(true).borderRadius(6)
        Text(`画面变化计数：${this.motionTick}；离开本页自动停止动画。`).fontSize(11).fontColor('#8796AA')
      }.alignItems(HorizontalAlign.Start).width('100%')
    }
  }

  @Builder
  developerPage() {
    Column({ space: 18 }) {
      this.pageHeading('开发测试', '能力验证与诊断工具。切换页面不会中断远程会话。')
      if (this.lanRunning) {
        Text('局域网服务正在运行，本机录屏与注入测试已停用。可继续查看诊断；下线后即可测试。')
          .fontSize(13).fontColor('#8C6A32').lineHeight(20).padding(16).width('100%')
          .backgroundColor('#FFF6E7').borderRadius(10)
      }
      Column({ space: 14 }) {
        this.cardHeading('录屏与编码', '这些测试会生成本地诊断或测试录像，需要手动允许系统共享。')
        Text(`录屏 · ${this.frames} 帧 · ${this.fps.toFixed(1)} FPS · ${this.elapsed.toFixed(1)} 秒`)
          .fontSize(13).fontColor('#4E6484')
        Row({ space: 10 }) {
          Button('录屏测试 · 30 秒').type(ButtonType.Normal).borderRadius(8).fontSize(13)
            .enabled(!this.lanRunning && !this.captureRunning && !this.encoderRunning)
            .onClick(() => this.startCapture())
          Button('停止录屏').type(ButtonType.Normal).borderRadius(8).fontSize(13)
            .enabled(!this.lanRunning && this.captureRunning).onClick(() => {
              probe.stopCapture();
              this.refresh();
            })
        }
        Divider().color('#EDF1F6')
        Text(`编码 · ${this.encoderStatus} · ${this.encoderFrames} 帧 · ${this.encoderFps.toFixed(1)} FPS · ${this.encoderElapsed.toFixed(1)} 秒 · ${(this.encoderBytes / 1024).toFixed(0)} KB`)
          .fontSize(13).fontColor('#4E6484')
        Row({ space: 10 }) {
          Button('H.264 测试 · 10 秒').type(ButtonType.Normal).borderRadius(8).fontSize(13)
            .enabled(!this.lanRunning && !this.encoderRunning && !this.captureRunning).onClick(() => this.startEncoder())
          Button('停止编码').type(ButtonType.Normal).borderRadius(8).fontSize(13)
            .enabled(!this.lanRunning && this.encoderRunning).onClick(() => this.stopEncoder())
        }
        this.motionPattern()
        Text('允许共享整个主屏幕后，可留在本页观察动态图案，也可切换应用检查其他画面。')
          .fontSize(12).fontColor('#7B8AA0').lineHeight(18)
      }.alignItems(HorizontalAlign.Start).width('100%').padding(22).backgroundColor('#FFFFFF').borderRadius(12)
      Column({ space: 14 }) {
        this.cardHeading('输入测试', '每个动作延迟 5 秒，请只在空白测试输入框中验证。离开本页会取消等待中的动作。')
        Text(`输入权限：${this.inputPermission}`).fontSize(13).fontColor('#4E6484')
        Row({ space: 10 }) {
          Button('请求输入权限').type(ButtonType.Normal).borderRadius(8).fontSize(13)
            .enabled(!this.lanRunning).onClick(() => this.requestInputPermission())
          Button('刷新授权状态').type(ButtonType.Normal).borderRadius(8).fontSize(13)
            .onClick(() => this.refresh())
        }
        Row({ space: 8 }) {
          Button('鼠标移至中心').type(ButtonType.Normal).borderRadius(8).fontSize(12)
            .onClick(() => this.scheduleAction('Move Mouse'))
          Button('输入 A').type(ButtonType.Normal).borderRadius(8).fontSize(12)
            .onClick(() => this.scheduleAction('Inject A'))
          Button('中心左键点击').type(ButtonType.Normal).borderRadius(8).fontSize(12)
            .onClick(() => this.scheduleAction('Left Click'))
          Button('Ctrl+L').type(ButtonType.Normal).borderRadius(8).fontSize(12)
            .onClick(() => this.scheduleAction('Ctrl+L'))
        }.enabled(!this.lanRunning)
        TextInput({ placeholder: '本机输入测试：点击“输入 A”后，将光标放在这里。' })
          .width('100%').maxLength(100).height(42).borderRadius(8).backgroundColor('#F2F5FA')
        if (this.countdown > 0) {
          Text(`${this.action} · ${this.countdown} 秒后执行`).fontColor('#B3772E').fontSize(20)
        }
        Button('取消动作 / 释放按键 / 撤销授权').type(ButtonType.Normal).borderRadius(8).fontSize(13)
          .enabled(!this.lanRunning).onClick(() => this.revokeInput())
      }.alignItems(HorizontalAlign.Start).width('100%').padding(22).backgroundColor('#FFFFFF').borderRadius(12)
      Column({ space: 14 }) {
        this.cardHeading('运行诊断', '仅用于故障排查；不包含剪贴板正文。')
        Text(`版本 0.5.0 · API ${deviceInfo.sdkApiVersion} · ${deviceInfo.deviceType} · SDK 26.0.0.105`)
          .fontSize(12).fontColor('#647B99')
        Text(`LAN ${this.lanStatus} · 已发送 ${this.lanFrames} 帧 · 输入返回码 ${this.remoteError} · 待释放 ${this.remotePending}`)
          .fontSize(12).fontColor('#647B99')
        if (this.lanError.length > 0) { Text(this.lanError).fontSize(12).fontColor('#B24336') }
        Text(this.operationDetail).fontSize(12).fontColor('#647B99').lineHeight(18)
        Text(this.diagnosticsStatus).fontSize(12).fontColor('#647B99').lineHeight(18)
        Button('保存当前诊断').type(ButtonType.Normal).borderRadius(8).fontSize(13).onClick(() => this.saveDiagnostics())
        Text(`诊断目录：${this.filesDir}`).fontSize(11).fontColor('#8796AA').copyOption(CopyOptions.InApp)
        Text('录屏诊断').fontSize(14).fontWeight(FontWeight.Medium).fontColor('#425B7C')
        Text(this.captureText).fontSize(11).fontFamily('monospace').fontColor('#6F8199')
          .copyOption(CopyOptions.InApp).width('100%')
        Text('输入诊断').fontSize(14).fontWeight(FontWeight.Medium).fontColor('#425B7C')
        Text(this.inputText).fontSize(11).fontFamily('monospace').fontColor('#6F8199')
          .copyOption(CopyOptions.InApp).width('100%')
        Text('编码诊断').fontSize(14).fontWeight(FontWeight.Medium).fontColor('#425B7C')
        Text(this.encoderText).fontSize(11).fontFamily('monospace').fontColor('#6F8199')
          .copyOption(CopyOptions.InApp).width('100%')
        Text('剪贴板诊断').fontSize(14).fontWeight(FontWeight.Medium).fontColor('#425B7C')
        Text(this.clipboardText).fontSize(11).fontFamily('monospace').fontColor('#6F8199')
          .copyOption(CopyOptions.InApp).width('100%')
      }.alignItems(HorizontalAlign.Start).width('100%').padding(22).backgroundColor('#FFFFFF').borderRadius(12)
      this.statusNotice()
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  build() {
    Row() {
      this.sidebar()
      Scroll(this.pageScroller) {
        Column() {
          if (this.currentPage === 'remote') { this.remotePage() }
          else if (this.currentPage === 'settings') { this.settingsPage() }
          else { this.developerPage() }
        }.width('100%').padding(24).constraintSize({ maxWidth: 1180 }).alignItems(HorizontalAlign.Start)
      }.layoutWeight(1).height('100%').align(Alignment.TopStart).scrollBar(BarState.Auto)
    }.width('100%').height('100%').backgroundColor('#F4F7FC')
  }
}
