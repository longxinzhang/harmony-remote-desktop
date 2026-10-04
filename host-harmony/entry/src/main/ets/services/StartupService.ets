import { autoStartupManager } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

export enum StartupStatus {
  Unknown = 'unknown',
  Enabled = 'enabled',
  Disabled = 'disabled',
  Unsupported = 'unsupported',
  Error = 'error'
}

export class StartupState {
  status: StartupStatus = StartupStatus.Unknown;
  summary: string = '尚未查询';
  detail: string = '开机启动由系统管理；本应用可读取当前状态。';
  // API 26 exposes query methods only for ordinary applications.
  canChangeFromApp: boolean = false;
  errorCode: number = 0;
}

export class StartupService {
  async readStatus(): Promise<StartupState> {
    const result = new StartupState();
    try {
      if (!autoStartupManager.isAutoStartupSupported()) {
        result.status = StartupStatus.Unsupported;
        result.summary = '此设备不支持开机自启动';
        result.detail = '当前系统报告不支持。仍可设置打开应用后启动局域网服务。';
        return result;
      }
      const enabled: boolean = await autoStartupManager.getAutoStartupStatusForSelf();
      result.status = enabled ? StartupStatus.Enabled : StartupStatus.Disabled;
      result.summary = enabled ? '系统已允许开机自启动' : '系统未启用开机自启动';
      result.detail = '当前应用只能读取系统自启动状态，无法在应用内修改。打开应用后启动服务可单独设置；屏幕共享仍需本机授权。';
    } catch (error) {
      const failure = error as BusinessError;
      result.errorCode = failure.code;
      result.status = failure.code === 801 ? StartupStatus.Unsupported : StartupStatus.Error;
      result.summary = failure.code === 801 ? '此设备不支持开机自启动' : '读取自启动状态失败';
      result.detail = failure.code === 801 ? '当前系统不提供此能力。' : `系统返回 ${failure.code}，请稍后刷新。`;
    }
    return result;
  }
}
