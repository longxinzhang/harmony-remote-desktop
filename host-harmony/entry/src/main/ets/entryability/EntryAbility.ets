import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import probe from 'libhrd_probe.so';

export default class EntryAbility extends UIAbility {
  onWindowStageCreate(windowStage: window.WindowStage): void {
    PersistentStorage.persistProp<string>('hrdSessionMode', 'permanent');
    AppStorage.setOrCreate('probeFilesDir', this.context.filesDir);
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(0x0000, 'HRD', 'UI load failed %{public}d', error.code);
      }
    });
  }

  onDestroy(): void {
    probe.stopCapture();
    probe.stopEncoder();
    probe.stopLanServer();
    probe.cancelInput();
  }
}
