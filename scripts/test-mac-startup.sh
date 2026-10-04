#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-mac-startup.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
/usr/bin/swiftc -swift-version 5 -O -warnings-as-errors -target arm64-apple-macosx14.0 \
  -module-cache-path "$test_build_dir/module-cache" \
  -framework Foundation -framework Combine -framework ServiceManagement \
  "$project_dir/client-macos/Sources/StartupController.swift" \
  "$project_dir/tests/mac_startup_test.swift" -o "$test_build_dir/mac_startup_test"
"$test_build_dir/mac_startup_test"
