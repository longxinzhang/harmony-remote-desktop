#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEVECO_HOME="${DEVECO_HOME:-/Applications/DevEco-Studio.app}"
export DEVECO_SDK_HOME="${DEVECO_SDK_HOME:-$DEVECO_HOME/Contents/sdk}"
export NODE_HOME="$DEVECO_HOME/Contents/tools/node"
export JAVA_HOME="$DEVECO_HOME/Contents/jbr/Contents/Home"
export PATH="$NODE_HOME/bin:$JAVA_HOME/bin:$DEVECO_HOME/Contents/tools/ohpm/bin:$PATH"
HVIGOR="$DEVECO_HOME/Contents/tools/hvigor/bin/hvigorw"
OHPM="$DEVECO_HOME/Contents/tools/ohpm/bin/ohpm"

if [[ "${1:-}" == "--help" ]]; then
  printf '%s\n' 'Usage: scripts/build-hap.sh [additional Hvigor options]' \
    'Builds host-harmony entry/default debug HAP using the installed DevEco tools.' \
    'Unsigned unless you explicitly configured this new project signing in DevEco.'
  exit 0
fi
for executable in "$NODE_HOME/bin/node" "$JAVA_HOME/bin/java"; do
  if [[ ! -x "$executable" ]]; then
    printf 'Required executable is missing: %s\n' "$executable" >&2
    exit 1
  fi
done
if [[ ! -x "$HVIGOR" || ! -x "$OHPM" || ! -f "$PROJECT_ROOT/host-harmony/build-profile.json5" ]]; then
  printf '%s\n' 'Bundled Hvigor or host-harmony/build-profile.json5 is missing.' >&2
  exit 1
fi
cd "$PROJECT_ROOT/host-harmony"
"$OHPM" install --all
exec "$HVIGOR" --mode module -p product=default \
  -p module=entry@default -p buildMode=debug assembleHap --no-daemon "$@"
