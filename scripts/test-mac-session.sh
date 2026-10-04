#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-mac-session.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
/usr/bin/swiftc -swift-version 5 -O -warnings-as-errors -target arm64-apple-macosx14.0 \
  -module-cache-path "$test_build_dir/module-cache" \
  -framework Foundation -framework CryptoKit -framework Security \
  "$project_dir/client-macos/Sources/WireProtocol.swift" \
  "$project_dir/client-macos/Sources/PairingIdentity.swift" \
  "$project_dir/client-macos/Sources/ConnectionPolicy.swift" \
  "$project_dir/tests/mac_session_test.swift" -o "$test_build_dir/mac_session_test"
"$test_build_dir/mac_session_test"
