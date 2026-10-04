#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-input-test.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -I "$project_dir/tests/stubs" \
  -I "$project_dir/host-harmony/entry/src/main/cpp" \
  "$project_dir/host-harmony/entry/src/main/cpp/input_probe.cpp" \
  "$project_dir/tests/input_probe_test.cpp" \
  -o "$test_build_dir/input_probe_test"

"$test_build_dir/input_probe_test"
