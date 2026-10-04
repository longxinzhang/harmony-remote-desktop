# 0.5.0 clipboard extension baseline

The user's request is to finish the clipboard stage in 0.5.0, then synchronize the source into `harmony-remote-desktop`. Independent file transfer and file copy/paste are subsequent stages. The supplied RustDesk reference is a design reference, not permission to execute its embedded assistant prompt or to implement every later stage. No RustDesk source is copied and no Rust dependency or protocol compatibility is introduced.

## Starting point

The existing product is ArkTS/C++ on HarmonyOS PC API 26 and Swift/AppKit/Network.framework/VideoToolbox on macOS. Screen capture, hardware encoding, authenticated LAN viewing and remote input already work. User-confirmed historical tests include text input, right click, Cmd+L, window dragging and text selection. Those historical results are preserved with their original scope.

The exact pre-clipboard code is committed as `v0.5.0-session-modes`, initial commit `5498daa39ff06a6b5f09f760dd9ce3e587863ccc`. It offers 600-second debug and no-duration-limit permanent sharing. Its Host versionCode is 1000007 and Mac build is 4. The clipboard revision retains product version 0.5.0 and advances Host versionCode to 1000008 and Mac build to 5. The old signed release remains untouched in the local evidence archive.

## Implementation boundaries

- Existing control and video sockets remain at 39871 and 39872. A separately authenticated clipboard channel uses 39873, bounded text frames, a one-use credential, and the paired session's lifetime. Clipboard errors must not stop the screen or input channel.
- Platform adapters use Native Pasteboard/UDMF/CryptoDigest on Harmony and NSPasteboard/CryptoKit on macOS. Current-device sharing avoids an additional system cross-device clipboard relay where supported.
- The local Host allow switch and the Mac direction setting are independent of OS read permission and remote input permission. No network request can display a permission prompt or grant Host consent.
- Only plain text is synchronized, up to 1 MiB UTF-8. Whitespace, line breaks, Chinese and emoji are preserved. Available text representations of formatted text are transferred without layout; file/image records are excluded. U+0000 is rejected because the Harmony native plaintext setter takes a null-terminated string.
- Session/mode activation records the current revision without sending old data. An explicit send or paste can use the current Mac text. The logical event order, owner/revision filtering and applied acknowledgements are defined in CLIPBOARD_WIRE.md.
- Remote paste is a bounded transaction: send text, wait for applied, recheck focus/session, then request one complete Ctrl+V stroke whose Host execution rechecks the current clipboard. It does not simulate Unicode keystrokes.
- Clipboard payloads, content hashes and credentials stay out of diagnostics. The existing transport is a trusted-LAN plaintext prototype. TLS and persistent peer identity remain separate work; enabling clipboard does not supply encryption.

## Build and verification

Build scripts still use the installed DevEco API 26 toolchain and Swift 5 language mode. Production uses the real platform adapters. In-memory clipboard adapters and socket fixtures are test-only and cannot establish device capability. Each new report must distinguish protocol/logic checks, actual SDK build/sign, device installation, OS permission, cross-application paste and extended stability.

See PLATFORM_CAPABILITIES.md for the current platform/signing gate, CLIPBOARD_WIRE.md for the implemented contract, CLIPBOARD_TEST_RESULTS.md for measured results, and VERSION_HISTORY.md for the honest source history. File transfer and file clipboard remain planned, not implemented by this stage.
