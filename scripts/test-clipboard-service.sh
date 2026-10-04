#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -eq 2 && "$1" == "--build-only" ]]; then
  test_binary="$2"
  run_tests=false
elif [[ $# -eq 0 ]]; then
  test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-clipboard.XXXXXX")"
  trap 'rm -rf "$test_build_dir"' EXIT
  test_binary="$test_build_dir/clipboard_service_test"
  run_tests=true
else
  echo "Usage: $0 [--build-only /absolute/path/to/test-binary]" >&2; exit 2
fi
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations -pthread \
  -fsanitize=undefined -fno-sanitize-recover=all -DHRD_LAN_TESTING \
  -I "$project_dir/host-harmony/entry/src/main/cpp" \
  "$project_dir/host-harmony/entry/src/main/cpp/clipboard_service.cpp" \
  "$project_dir/host-harmony/entry/src/main/cpp/lan_server.cpp" \
  "$project_dir/tests/clipboard_service_test.cpp" -o "$test_binary"
if [[ "$run_tests" == true ]]; then "$test_binary"; fi
