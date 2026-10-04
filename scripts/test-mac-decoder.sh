#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-mac-decoder.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
fixture_path="${1:-$test_build_dir/real-device-replay.hrd}"
if [[ $# -eq 0 ]]; then
  python3 "$project_dir/scripts/make-mac-replay.py" \
    "$project_dir/artifacts/device/lan-take-01/capture.h264" "$fixture_path"
fi
xcrun swiftc -swift-version 5 -target arm64-apple-macos14.0 \
  -module-cache-path "$test_build_dir/module-cache" -O \
  "$project_dir/client-macos/Sources/H264Decoder.swift" \
  "$project_dir/tests/mac_decoder_test.swift" -o "$test_build_dir/mac_decoder_test"
"$test_build_dir/mac_decoder_test" "$fixture_path"
