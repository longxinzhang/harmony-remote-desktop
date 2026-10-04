#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-clipboard.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
/usr/bin/swiftc -swift-version 5 -O -warnings-as-errors -target arm64-apple-macosx14.0 \
  -module-cache-path "$test_dir/module-cache" -D HRD_CLIPBOARD_TESTING -D HRD_NETWORK_TESTING \
  -framework AppKit -framework Network \
  "$project_dir/client-macos/Sources/WireProtocol.swift" \
  "$project_dir/client-macos/Sources/ClipboardProtocol.swift" \
  "$project_dir/client-macos/Sources/ClipboardCoordinator.swift" \
  "$project_dir/client-macos/Sources/ClipboardChannel.swift" \
  "$project_dir/client-macos/Sources/PairingIdentity.swift" \
  "$project_dir/client-macos/Sources/ConnectionPolicy.swift" \
  "$project_dir/client-macos/Sources/AudioProtocol.swift" \
  "$project_dir/client-macos/Sources/LANConnection.swift" \
  "$project_dir/client-macos/Sources/AppKitClipboardAdapter.swift" \
  "$project_dir/client-macos/Sources/RemoteInput.swift" \
  "$project_dir/tests/mac_clipboard_test.swift" -o "$test_dir/mac-clipboard-tests"
"$test_dir/mac-clipboard-tests" "$@"
