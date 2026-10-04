#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${HRD_MAC_BUILD_DIR:-$project_dir/client-macos/build}"
app_dir="$build_dir/HarmonyRemote.app"
module_cache="$build_dir/module-cache"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources" "$module_cache"
source_snapshot="$(mktemp -d "$build_dir/.sources.XXXXXX")"
trap 'rm -rf "$source_snapshot"' EXIT
cp "$project_dir"/client-macos/Sources/*.swift "$source_snapshot/"
cp "$project_dir/client-macos/Resources/Info.plist" "$app_dir/Contents/Info.plist"
/usr/bin/swiftc -swift-version 5 -parse-as-library -O -target arm64-apple-macosx14.0 \
  -module-cache-path "$module_cache" \
  -framework SwiftUI -framework AppKit -framework Network -framework VideoToolbox \
  -framework CoreMedia -framework CoreVideo -framework AVFoundation \
  "$source_snapshot"/*.swift -o "$app_dir/Contents/MacOS/HarmonyRemote"
/usr/bin/codesign --force --sign - --timestamp=none "$app_dir"
/usr/bin/codesign --verify --strict "$app_dir"
python3 - "$source_snapshot" "$build_dir/compiled-sources.json" <<'PY'
import hashlib, json, sys
from pathlib import Path
files = Path(sys.argv[1]).glob('*.swift')
Path(sys.argv[2]).write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)},indent=2)+'\n')
PY
printf '%s\n' "$app_dir"
