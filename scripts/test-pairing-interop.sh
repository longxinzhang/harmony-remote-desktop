#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/hrd-pairing-interop.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
bash "$project_dir/scripts/test-lan-server.sh" --build-only "$test_build_dir/lan-server"
/usr/bin/swiftc -swift-version 5 -O -warnings-as-errors -parse-as-library \
  -module-cache-path "$test_build_dir/module-cache" \
  "$project_dir/tests/pairing_crypto_interop.swift" -o "$test_build_dir/interop"
LAN_SERVER_FIXTURE="$test_build_dir/lan-server" "$test_build_dir/interop"
