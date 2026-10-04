#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-mac-network.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT

if [[ -n "${LAN_SERVER_FIXTURE:-}" ]]; then
  fixture_binary="$LAN_SERVER_FIXTURE"
else
  fixture_binary="$test_build_dir/lan-server-fixture"
  bash "$project_dir/scripts/test-lan-server.sh" --build-only "$fixture_binary"
fi

swiftc -swift-version 5 -O -warnings-as-errors \
  -target "$(uname -m)-apple-macos14" \
  -module-cache-path "$test_build_dir/module-cache" -D HRD_NETWORK_TESTING \
  -framework Network \
  "$project_dir/client-macos/Sources/WireProtocol.swift" \
  "$project_dir/client-macos/Sources/ClipboardProtocol.swift" \
  "$project_dir/client-macos/Sources/LANConnection.swift" \
  "$project_dir/tests/mac_network_test.swift" \
  -o "$test_build_dir/mac-network-tests"

LAN_SERVER_FIXTURE="$fixture_binary" "$test_build_dir/mac-network-tests"
