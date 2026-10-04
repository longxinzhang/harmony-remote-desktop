#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/hrd-mac-pairing-network.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
bash "$project_dir/scripts/test-lan-server.sh" --build-only "$test_build_dir/lan-server"
/usr/bin/swiftc -swift-version 5 -O -warnings-as-errors -parse-as-library \
  -target "$(uname -m)-apple-macos14" -D HRD_NETWORK_TESTING \
  -module-cache-path "$test_build_dir/module-cache" \
  -framework Network -framework CryptoKit -framework Security \
  "$project_dir/client-macos/Sources/WireProtocol.swift" \
  "$project_dir/client-macos/Sources/ClipboardProtocol.swift" \
  "$project_dir/client-macos/Sources/AudioProtocol.swift" \
  "$project_dir/client-macos/Sources/PairingIdentity.swift" \
  "$project_dir/client-macos/Sources/ConnectionPolicy.swift" \
  "$project_dir/client-macos/Sources/LANConnection.swift" \
  "$project_dir/tests/mac_pairing_network_test.swift" -o "$test_build_dir/mac-pairing-network"
LAN_SERVER_FIXTURE="$test_build_dir/lan-server" "$test_build_dir/mac-pairing-network"
