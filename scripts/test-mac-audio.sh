#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-mac-audio.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=undefined -DHRD_AUDIO_TESTING \
 -I "$project_dir/host-harmony/entry/src/main/cpp" "$project_dir/host-harmony/entry/src/main/cpp/audio_service.cpp" \
 "$project_dir/tests/audio_bridge_fixture.cpp" -o "$test_build_dir/audio_fixture"
/usr/bin/swiftc -swift-version 5 -O -warnings-as-errors -D HRD_AUDIO_TESTING -target arm64-apple-macosx14.0 \
 -module-cache-path "$test_build_dir/module-cache" -framework AVFoundation -framework Network \
 "$project_dir/client-macos/Sources/AudioProtocol.swift" "$project_dir/client-macos/Sources/AudioConnection.swift" \
 "$project_dir/client-macos/Sources/AudioPlayback.swift" "$project_dir/client-macos/Sources/WireProtocol.swift" \
 "$project_dir/tests/mac_audio_test.swift" -o "$test_build_dir/mac_audio_test"
"$test_build_dir/mac_audio_test" "$test_build_dir/audio_fixture"
