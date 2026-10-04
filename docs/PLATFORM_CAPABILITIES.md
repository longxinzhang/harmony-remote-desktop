# Clipboard platform capabilities and verification

This audit covers the 0.5.0 plain-text clipboard work on the installed API 26 SDK (`26.0.0.105`) and the HarmonyOS PC / `2in1` target. It distinguishes source checks, the initial build's actual device feedback and the repair candidate (Host `1000009` / Mac `6`). File transfer, file clipboard, images and rich-text preservation are outside this implementation. The repair candidate has not passed a new live acceptance run.

## What is established

SDK paths below are relative to `/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/`.

| Evidence | Established fact | Does not establish |
| --- | --- | --- |
| `toolchains/lib/PermissionDefinitions.json:5577` | `ohos.permission.READ_PASTEBOARD` is `user_grant`, level `system_basic`, `availableType=NORMAL`, `provisionEnable=true`; its device list explicitly includes `2in1` | That this app's existing signed profile contains the ACL, or that this device has granted it |
| `native/sysroot/usr/include/database/pasteboard/oh_pasteboard.h:357` | `OH_Pasteboard_GetData(OH_Pasteboard*, int*)` declares READ_PASTEBOARD and returns data plus an operation status | Successful background reads on this PC |
| Same header, `:380` | `OH_Pasteboard_SetData(OH_Pasteboard*, OH_UdmfData*)` writes clipboard data and returns an error code; it has no READ_PASTEBOARD annotation | That every actual write succeeds, or that receiving text proves another application pasted it |
| Same header, `:404` and `:408` | MIME inspection and `uint32_t` change count APIs exist. Recopying identical content increments the count; service/system restart resets it; failure may return zero | A globally unique revision, successful content access, or an immutable copy/paste transaction |
| `native/sysroot/usr/include/database/pasteboard/oh_pasteboard_err_code.h:55` | Success `0`, permission failure `201`, unsupported capability `801`, busy `12900003` and other errors are distinguishable | That every error is a permission problem |
| `ets/api/@ohos.abilityAccessCtrl.d.ts:306` | `requestPermissionsFromUser` is a Stage UIAbility user-grant request; denial may require system settings rather than another prompt | That declaring the permission or resolving its Promise grants access |
| `ets/api/security/PermissionRequestResult.d.ts:45` | `authResults`: `0` granted, `-1` not authorized, `2` invalid request; dialog/result details distinguish failure paths | That all false results mean the user explicitly denied the request |

The main task's read-only summary of the **old session-modes 0.5.0 signed HAP** was `readPasteboardNamedInProfile=false`, `profileAPL=normal`, `type=debug`, and `bundleMatch=true`. No private profile fields are reproduced here. This old package predates the clipboard permission declaration. It is not evidence that the new clipboard package is authorized, nor proof that a newly generated profile will fail. Keep the app at its intended normal APL and use the documented ACL route; do not edit the profile or raise APL manually.

The first **new clipboard HAP** subsequently built successfully, but installation returned **9568289**, `grant request permissions failed`, naming `ohos.permission.READ_PASTEBOARD`. The actual installer output is preserved in [the first installation log](../artifacts/install-0.5.0-clipboard-first.log). This was a permission-grant/install blocker for that signed package, not a missing clipboard API or a runtime feature failure. The user then refreshed the signing configuration as described below.

The main task's read-only check of the refreshed profile reported `configuredProfileHasReadPasteboard=true`, `bundleMatch=true`, `profileAPL=normal`, and `type=debug`. The rebuilt HAP's embedded profile was confirmed to include READ_PASTEBOARD, and installation succeeded. See the [redacted permission summary](../artifacts/clipboard-hap-permission-0.5.0.json) and [successful installation log](../artifacts/install-0.5.0-clipboard.log). Host startup diagnostics confirm build `1000008` with clipboard disabled. No private profile content is reproduced here.

The user subsequently reported that no new permission dialog appeared but clipboard use worked, including terminal text pasted on both Harmony and Mac. The [collected device snapshot](../artifacts/device/clipboard-0-5-0-copy-report/clipboard-snapshot.json) has `canRead=true`, `canWrite=true`, 10 sent updates and 30 applied writes. This is real partial capability evidence, not proof that every source application or background condition works. The same run exposed a Notes remote-copy defect: later paste could retain old terminal text. With sync disabled and the Harmony keyboard used locally, Notes copy/paste worked. Those two changed variables require separate follow-up tests; the actual Notes data shape was not captured.

Repair Host build `1000009` has passed a full build and embedded-signature/ACL check ([build](../artifacts/build-0.5.0-clipboard-build9.log), [permission summary](../artifacts/clipboard-hap-permission-0.5.0-build9.json)). An initial installation check reported `NO_DEVICE`; after reconnecting the same known device, [installation succeeded](../artifacts/install-0.5.0-clipboard-build9-reconnect.log). The application was not started and has no new runtime acceptance result. Applications remain closed after the user's resource-use report.

The current module declares READ_PASTEBOARD with a reason and `EntryAbility` / `when: always`. The permission helper requests it only through a visible Host action and rechecks `checkAccessTokenSync`. The declaration describes intended use; it does not by itself guarantee background permission or service behavior. Reading while Host is not the foreground app must be tested independently after signing and runtime authorization.

## Signing and permission path

Huawei lists READ_PASTEBOARD among automatic-signing ACL permissions. The documented workflow depends on whether the project is associated with a registered AGC app. Adding the manifest declaration does **not** update an already issued profile by itself. [Huawei automatic signing](https://developer.huawei.com/consumer/cn/doc/doccenter-deveco-studio/ide-signing-auto)

1. In DevEco, open **File → Project Structure → Project → Signing Configs** and inspect whether **Associate with registered application** is selected. Use the existing project/account/device; do not change identity to bypass permission checks.
2. **Associated app, DevEco 26.0.0:** use **Enable ACL Permissions**, select READ_PASTEBOARD for `entry`, fill its reason, timing and ability, then complete the request reason/attachment flow. The IDE synchronizes ACL entries with `module.json5`. The documentation distinguishes a temporary profile while approval is pending from a longer-lived approved profile.
3. **Unassociated debug app:** the documented flow is to declare the supported ACL permission in the module, then run **Automatically generate signature** and confirm with **OK**. This can request a new debug profile; it is not a promise that the current stale profile already suffices. Inspect the resulting managed profile's ACL summary.
4. Rebuild with the generated profile. Before installation, verify package identity, device coverage and the presence of READ_PASTEBOARD in the **new HAP's actual signed profile**, reporting only redacted booleans/status. Do not publish keys, passwords, certificates or the complete profile. [Huawei automatic-signing workflows](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/ide-signing-auto)

If using manual signing instead, obtain the app's ACL permission in AGC, then create/download a new debug Profile with **受限ACL权限（HarmonyOS API9及以上）** selected and the existing debug certificate/device. A profile created before an ACL change must be recreated; configure that new profile locally and rebuild. This is a conditional alternative, not a requirement to switch away from automatic signing. [Huawei debug Profile guide](https://developer.huawei.com/consumer/cn/doc/doccenter-getting-started/agc-help-debug-profile-0000002248181278)

After successful installation, invoke the Host's explicit permission button from its visible UI. Record the API error (if thrown), grant result and whether the dialog appeared; verify the access token result and a real clipboard read separately. If already denied, direct the user to system settings or the documented permission-settings flow; do not repeatedly prompt from network callbacks. A normal return from the request, successful signing or successful installation alone is not clipboard capability acceptance.

## Independent verification matrix

The table is an acceptance checklist, not a blanket pass result. Automated checks and initial partial device results are recorded separately in [test results](CLIPBOARD_TEST_RESULTS.md); the repaired Notes path and remaining device cases are still pending. Use fixed, harmless fixtures and an empty test document. Reports identify the case, direction, byte count, result, fixed format category and error code, without clipboard payloads, payload hashes, raw custom type strings, PINs or binding tokens.

| Case | Required observation | Best verification layer |
| --- | --- | --- |
| P1: signing and runtime authorization | New HAP ACL summary; installation result; explicit grant/deny outcome; token check and actual read result remain separate | New HAP + real PC |
| P2: disabled and initial state | Either endpoint off means no sync. Enabling or changing direction does not send preexisting clipboard content | Native/Mac tests + live |
| P3: each direction | Mac → Harmony and Harmony → Mac plain text appears in the destination clipboard and can be pasted into the intended app | Real two-app observation |
| P4: text fidelity | CJK, emoji, combining characters, LF/CRLF, tabs, leading/trailing spaces and valid empty text are preserved; absence of text is not treated as an empty-text update | Parser/platform tests + selected live cases |
| P5: scope exclusion | File URL, file promise, image-only and file-plus-text flavors do not become file paths or text updates; rich text is not claimed preserved | Platform tests + live file-copy negative case |
| P6: bounded parsing | Fragmented header/body, invalid UTF-8/NUL, wrong digest, duplicate/unknown fields, lengths over 1 MiB, invalid counter and truncated body fail the clipboard channel; video/input continue | C++/Swift wire integration |
| P7: session binding | Wrong/expired/reused bind token, duplicate bind, old epoch and wrong sender origin are rejected without exposing content | Actual C++/Swift fixture |
| P8: activation delay | Pair with clipboard off, wait past the 30-second bind window, then enable; the intended established-channel path works without silently reusing an expired token | Simulated clock + live short check |
| P9: repeated copies and echo | Two user copies of identical text remain two events; a self-written revision does not bounce indefinitely; no stable content hash is used as a permanent suppression key | Platform fixture + live |
| P10: concurrent changes | Local copy between remote read/write, simultaneous updates, delayed ACK and out-of-order/stale data converge according to the logical version rule without overwriting a newer local change | Deterministic platform/wire tests |
| P11: paste barrier success | A new Mac copy waits for actual applied ACK; an unchanged synchronized event is reused without rewriting Host content. Host rechecks the current event/revision before one complete Ctrl+V; no additional raw V; observe the destination | Ordered fixture + real target app |
| P12: paste barrier cancellation | Write failure, wrong ACK/event, focus loss, changed local/remote revision, changed mode, expired TTL, duplicate operation ID or disconnect never triggers a stale/duplicate paste | Deterministic fault tests + selected live cases |
| P13: inactive/background behavior | Harmony → Mac still works when Viewer is inactive; Host reads are tested while another Harmony app is foreground and while Host is minimized. Unsupported background access becomes a visible capability/error state | Real PC/Mac; SDK existence cannot substitute |
| P14: revoke and teardown | Revocation/Host off/stop/disconnect closes the clipboard gate, invalidates in-flight work and pending paste, releases held input, and leaves unrelated video behavior as specified | Fault tests + real revoke/stop |
| P15: bounds and diagnostics | Repeated large text updates keep queues/memory bounded; no payload, payload digest or credentials in logs/JSON. Stop/start clears stale bindings and baselines | Integration/resource audit + bounded live run |

`clipboard_applied=applied` can prove a completed system clipboard write, once tied to the actual platform return. It cannot prove a target application pasted the intended text. `paste_result=committed` can prove the injection sequence completed, not that an application accepted it or that a protected field displayed it. Observe the real destination as well. File transfer and file-paste success are not acceptance criteria for this release.

## Review of the wire contract

The existing [clipboard contract](CLIPBOARD_WIRE.md) explicitly includes both-end opt-in, independent session binding, epoch checking, bounded frame sizes, strict UTF-8, no initial-content push, logical versions, no payload logging, and an applied-before-paste barrier. These are requirements to test; the document itself is not implementation evidence.

The following properties remain part of the implementation/test review and must not be weakened:

- Bind sender identity to direction: Mac must send `originId=mac`, Host must send `originId=harmony`; accepting either enum value from either peer would permit a peer to impersonate the other origin in conflict ordering.
- Resolve the 30-second one-use binding window: pair-time connection may bind immediately while mode stays off. If binding is deferred until later opt-in, the existing token will expire during a permanent video session. Do not silently refresh/reuse it.
- Recompute payload SHA-256 and validate it; the hash is an integrity check inside the existing trusted-LAN protocol, not encryption or an authenticated transport guarantee.
- Match ACKs to the active update/message and epoch, and recheck the platform revision at paste execution. Resetting a mode/session must invalidate pending asynchronous read/write and paste work, not only reset numeric counters.
- A clipboard-only failure must not fall back to an unguarded raw V key path. Otherwise a failed synchronization could paste unrelated older destination content.

The native implementation owner has confirmed sender-origin enforcement and pair-time binding while mode remains off. The independent source read also found origin enforcement and SHA-256 comparison in native `Apply`, and digest comparison in Swift `ClipboardWire.validate`. These observations narrow the review questions; they do not replace the planned protocol and real-device checks.

The transport remains plaintext on a trusted LAN. This audit does not claim TLS, secure Internet exposure, unattended permission acquisition or permanently available background clipboard access.

## Remaining device evidence

Initial build `1000008` has actual read/write activity and user-confirmed terminal copy/paste, alongside the user-reported Notes failure. The repair candidate is now installed but needs its own startup and Notes retest; absence of a new authorization dialog alone is not evidence of denial. Native format handling now permits a bounded set of known text alternatives only when one record explicitly contains plain text. It records capped type/record counts and fixed rejection categories, but the real Notes shape remains unknown. Complete background, denial/revocation, concurrency, cancellation and resource tests remain open; see [current build and test results](CLIPBOARD_TEST_RESULTS.md). Signing and API existence alone cannot close these acceptance items.
