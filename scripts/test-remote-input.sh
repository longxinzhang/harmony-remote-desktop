#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-remote-input.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -fsanitize=undefined -fno-sanitize-recover=all \
  -I "$project_dir/tests/remote-input-stubs" \
  -I "$project_dir/host-harmony/entry/src/main/cpp" \
  "$project_dir/host-harmony/entry/src/main/cpp/remote_input.cpp" \
  "$project_dir/tests/remote_input_test.cpp" \
  -o "$test_build_dir/remote_input_test"

"$test_build_dir/remote_input_test"
