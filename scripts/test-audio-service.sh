#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-audio.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=undefined -fno-sanitize-recover=all -DHRD_AUDIO_TESTING \
 -I "$project_dir/host-harmony/entry/src/main/cpp" \
 "$project_dir/host-harmony/entry/src/main/cpp/audio_service.cpp" "$project_dir/tests/audio_service_test.cpp" -o "$test_build_dir/audio_service_test"
"$test_build_dir/audio_service_test"
