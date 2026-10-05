#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
capture_sdk="${HRD_NATIVE_SDK:-/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native}"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/harmony-remote-capture-privacy.XXXXXX")"
trap 'rm -rf "$test_build_dir"' EXIT
# Use installed SDK enums, but retain host C/C++ system headers for this pure
# callback policy test. No SDK capture function or display is invoked.
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined \
  -Wno-ignored-attributes -Wno-unknown-attributes \
  -I "$project_dir/host-harmony/entry/src/main/cpp" \
  -idirafter "$capture_sdk/sysroot/usr/include" \
  "$project_dir/tests/capture_state_policy_test.cpp" -o "$test_build_dir/capture_state_policy_test"
"$test_build_dir/capture_state_policy_test"
