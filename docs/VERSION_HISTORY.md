# Source history and Git publication

The intended repository name is `harmony-remote-desktop`. This document separates preserved source from build and device evidence. A binary archive or a list of hashes cannot reconstruct missing source.

## 0.6.0-rc.1 development candidate

The current candidate label is `v0.6.0-rc.1`, product version 0.6.0, Host build `1000011` and Mac build `8`. It adds six capabilities on top of the preserved 0.5.0 UI and clipboard repair:

- Automatic reconnect for authenticated peers with bounded backoff and a cancellable wait; the Host keeps an existing authorized capture for the same identity for at most 60 seconds. Input and pending paste operations are released, not replayed.
- Persistent P-256 identities, independent fresh challenges from both peers, explicit enrollment/removal, the Mac login Keychain and Host HUKS with a signed public-key allowlist.
- Explicit Mac launch-at-login registration and actual OS status. HarmonyOS API 26 exposes startup status queries only; starting the LAN listener when the app opens is a separate opt-in preference, not a system boot registration.
- Host 30/60 FPS options with capability checks, applied to the next sharing session after the current one stops; actual 60 FPS performance remains device-dependent.
- Optional one-way Harmony-to-Mac system playback audio, a separate bounded transport, mute support, no microphone capture and no saved audio files.
- Network type, matched-probe RTT/jitter, video throughput and received FPS. RTT is not a measurement of end-to-end input-to-display latency.

The transport remains unencrypted LAN TCP. First PIN enrollment uses trust on first use on a trusted LAN; remembered identity validation is not TLS or payload encryption. System screen-sharing, input and clipboard authorization remains local to the Host and is not bypassed by persistence or reconnect.

Both full app builds initially passed; the final Host build also passed. Final Mac source-to-binary verification, archive/publication and installation evidence are recorded separately. At this documentation update, the Mac is locked and the HDC connection failed: no new 0.6.0 install, visual acceptance, real reboot/login test, HUKS execution or live audio/FPS/reconnect result is claimed. Do not treat the candidate label here as proof that its Git tag has already been pushed.

Available automated evidence includes 52 Mac clipboard, 21 presentation, 21 input, 107 network, 89 session/identity, 16 startup, 24 native audio, 24 Mac audio, five C++/CryptoKit signature interoperability checks, and three actual Mac/C++ modern pairing/reconnect scenarios. Host native LAN final results are kept in [SESSION_RELIABILITY.md](SESSION_RELIABILITY.md). These suites use ephemeral test identities and synthetic media where stated; they do not replace the Notes clipboard retest, full remote-input matrix, sustained resource checks, 60 FPS hardware acceptance, real sound output, or actual OS startup validation.

See [session reliability](SESSION_RELIABILITY.md), [audio and FPS](MEDIA_AUDIO_FPS.md), [startup](STARTUP.md), and [UI/version boundaries](UI_WORKSPACE.md). Earlier commits and tags remain unchanged; formal `v0.5.0` and `v0.6.0` acceptance is not inferred from this candidate.

## Verified source inventory

| Version | Available matching files / recorded hashes | Source status |
| --- | ---: | --- |
| 0.1.0 | 14 / 25 | Incomplete; original root and entry build profiles are deliberately excluded from the safe snapshot |
| 0.1.1 | 4 / 10 | Incomplete |
| 0.2.0 | 4 / 12 | Incomplete |
| 0.2.1 | 4 / 13 | Incomplete |
| 0.3.0 | 6 / 16 | Incomplete |
| 0.4.0 | 23 / 32 | Partial verified source, preserved in `versions/0.4.0-partial/manifest.json`; 9 recorded files are missing |
| 0.4.1 | 15 / 22 | Partial verified source, preserved in `versions/0.4.1-partial/manifest.json`; 7 recorded files are missing |
| 0.5.0 session modes | 23 / 23 | Pre-clipboard source baseline captured before further implementation; all recorded application-source hashes match |

The counts compare bytes against the corresponding original local `artifacts/build-verification*.json`. These manifests enumerate selected files, not complete project inventories. Matching every recorded hash alone does not prove that an older checkout is complete.

The 0.5.0 source-only snapshot is at local `artifacts/source-baseline-0.5.0/`: 91 source, documentation, script, test and safe configuration files, plus its capture manifest. Dependencies, compiler caches, generated output, device exports, binaries, signing material and actual `build-profile.json5` files are excluded. The Git import additionally supplies signing-free templates, a local configuration initializer, a safe ignore file and this provenance documentation. These are packaging additions, not a claim that the signed historical build was rebuilt from those templates.

`artifacts/deadline-fix-verification/source-snapshot/` contains 15 files from an intermediate, uninstalled deadline fix. It is not a complete 0.4.0 or 0.4.1 release. The 0.4.0 and 0.4.1 partial manifests retain only exact hash matches: files unchanged in the 0.5.0 baseline reference `v0.5.0-session-modes` and their original path; differing matching bytes are stored under each partial snapshot's `files/`. Every missing recorded path and expected hash is listed. A referenced file can be inspected with `git show v0.5.0-session-modes:<baselinePath>` and checked against its manifest SHA-256.

The release Mac ZIPs inspected contain App bundles, not Swift or Host source. Existing generated ArkTS caches and source maps do not supply the missing historical native source; inspected maps contain no `sourcesContent`. No complete earlier source checkout was found in this project's archives or build caches. These partial directories must not be presented as complete release checkouts or tagged `v0.4.0` / `v0.4.1`.

## Local build configuration

After a fresh clone, run:

```sh
python3 scripts/init-local-config.py
bash scripts/build-hap.sh
bash scripts/build-mac.sh
```

The initializer creates only missing root and entry `build-profile.json5` files from the committed `build-profile.template.json5` files. It preserves existing local profiles without reading their content. The templates target HarmonyOS API 26 / arm64 and contain no signing configuration. A device-installable Host build requires signing configured locally in DevEco; keep all private keys, passwords, certificates, provisioning files and actual build profiles outside Git.

`artifacts/` is deliberately ignored in full: it contains private device diagnostics, captures, local paths and release binaries. Its links in development documentation refer to the local evidence archive, not automatically published GitHub files. The partial source manifests under `versions/` are safe tracked metadata; screen captures and diagnostic payloads are not copied into them.

## Git history and publication boundary

The first local commit, `5498daa`, imports the isolated pre-clipboard baseline, not the concurrently edited working directory. Its annotated tag is `v0.5.0-session-modes`. Commit `8570001` preserves the initial clipboard implementation, product version 0.5.0 with Host build `1000008` and Mac build `5`. It built and ran, with user-confirmed terminal copying but a reported remote Notes copy/paste defect; it is not a fully accepted release. Its original local artifacts are recorded in `artifacts/releases/0.5.0-clipboard-initial/status.json`.

The earlier repaired candidate is product version 0.5.0, Host build `1000009` and Mac build `6`, with source tag `v0.5.0-rc.1`. Its 117 local checks comprise 31 Native clipboard, 52 Mac clipboard, 21 Mac input and 13 Mac presentation checks. They are development evidence, not a successful Notes retest. That candidate's build, test and deployment boundaries remain in `CLIPBOARD_TEST_RESULTS.md`.

The earlier UI candidate remains product version 0.5.0, Host build `1000010` and Mac build `7`, with source tag `v0.5.0-rc.2`. It introduced three sidebar pages: `远程桌面` (Remote desktop), `设置` (Settings), and `开发测试` (Development tests). Host service and sharing controls are labelled `开启服务`, `下线并断开连接`, and `开始共享屏幕`; Mac connects through `连接设备`. Development tools are available through their own page in either session mode. Navigation does not change the selected debug/permanent mode, reconnect the session, or start tests. Mac pauses hidden-view presentation and releases remote input; Host cancels pending delayed test actions and hides its test animation when leaving the development page. See `UI_WORKSPACE.md` for exact behavior and separately recorded UI verification.

The UI candidate inherits the clipboard repair but does not close the Notes, full input, or long-running resource acceptance gates. Formal `v0.5.0` remains reserved. This preserves the baseline, known-defective preview, repair candidate and interface revision without inventing unavailable old release source. Source commit `e3dca10a7a5f1f8f5edb385c36e5a1ea3dcc48e6` and annotated tag `v0.5.0-rc.2` have been pushed and checked against the remote. Later publication-record commits on `main` do not move this source tag.

Before publication, inspect staged paths and content, verify that actual build profiles and all `artifacts/` remain ignored, run the relevant checks, and check the final source/compiled artifact hashes. Do not use `git add -f` to bypass the signing or device-artifact exclusions.

The public repository is [longxinzhang/harmony-remote-desktop](https://github.com/longxinzhang/harmony-remote-desktop). The user explicitly chose to keep it public and updated Contents write permission. The earlier source history and annotated tags were pushed and checked against the remote: `v0.5.0-session-modes` resolves to `5498daa39ff06a6b5f09f760dd9ce3e587863ccc`; `v0.5.0-rc.1` resolves to `e8d60312fed499dc21e29f8c007fc7b62fbff8d2`. Those source tags remain unchanged. The configured `origin` uses its HTTPS URL; credentials are supplied through authenticated GitHub CLI and are not stored in the repository. Remote refs can be compared with the corresponding local commit and annotated-tag objects. No complete older release tags or formal `v0.5.0` are claimed.
