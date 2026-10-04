# Source history and Git publication

The intended repository name is `harmony-remote-desktop`. This document separates preserved source from build and device evidence. A binary archive or a list of hashes cannot reconstruct missing source.

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

The first local commit imports the isolated pre-clipboard baseline, not the concurrently edited working directory. Its annotated tag is `v0.5.0-session-modes`. The subsequent clipboard implementation remains product version 0.5.0 with Host build `1000008` and Mac build `5`; its source tag is `v0.5.0`. Both builds and automated regressions have passed; runtime clipboard permission and actual bidirectional system paste remain separate device acceptance items documented in `CLIPBOARD_TEST_RESULTS.md`. This preserves an honest before/after history without inventing unavailable old release source.

Before publication, inspect staged paths and content, verify that actual build profiles and all `artifacts/` remain ignored, run the relevant checks, and check the final source/compiled artifact hashes. Do not use `git add -f` to bypass the signing or device-artifact exclusions.

The public repository is [longxinzhang/harmony-remote-desktop](https://github.com/longxinzhang/harmony-remote-desktop). The user explicitly chose to keep it public. The configured `origin` uses its HTTPS URL; credentials are supplied through the authenticated GitHub CLI and are not stored in this repository. The source history preserves both the session-mode baseline and the clipboard implementation, while the partial older snapshots retain their stated limitations. Published commits and tags can be checked against the repository's remote refs.
