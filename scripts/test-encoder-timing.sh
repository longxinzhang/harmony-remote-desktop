#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-encoder-timing.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined \
  -I "$project_dir/host-harmony/entry/src/main/cpp" \
  "$project_dir/tests/encoder_timing_test.cpp" \
  -o "$test_build_dir/encoder_timing_test"

"$test_build_dir/encoder_timing_test"
