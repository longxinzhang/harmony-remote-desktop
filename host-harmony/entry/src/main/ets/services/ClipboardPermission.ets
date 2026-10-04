import { abilityAccessCtrl, bundleManager, common, Permissions } from '@kit.AbilityKit';

const READ_CLIPBOARD: Permissions = 'ohos.permission.READ_PASTEBOARD';

export class ClipboardPermission {
  static granted(): boolean {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      return abilityAccessCtrl.createAtManager().checkAccessTokenSync(info.appInfo.accessTokenId,
        READ_CLIPBOARD) === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
    } catch (_) {
      return false;
    }
  }

  // Called only from the visible permission button, never from the network or polling.
  static async request(context: common.UIAbilityContext): Promise<boolean> {
    const result = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, [READ_CLIPBOARD]);
    return result.authResults.length === 1 && result.authResults[0] === 0 && ClipboardPermission.granted();
  }
}
