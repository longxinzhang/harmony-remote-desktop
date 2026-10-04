#!/usr/bin/env python3
"""Read local SDK/tool metadata and API declarations. Does not contact devices."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import plistlib
import re
from probe_tools import DEVECO, SDK, NATIVE, HDC, JAVA, run


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as error:
        return {"error": str(error)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Also save the JSON report here")
    args = parser.parse_args()
    declarations = {
        "multimedia/player_framework/native_avscreen_capture.h": [
            "OH_AVScreenCapture_Create", "OH_AVScreenCapture_Init",
            "OH_AVScreenCapture_SetDataCallback", "OH_AVScreenCapture_SetErrorCallback",
            "OH_AVScreenCapture_SetStateCallback", "OH_AVScreenCapture_StartScreenCapture",
            "OH_AVScreenCapture_StopScreenCapture", "OH_AVScreenCapture_Release",
            "OH_AVScreenCapture_StartScreenCaptureWithSurface", "OH_AVScreenCapture_ShowCursor"],
        "multimodalinput/oh_input_manager.h": [
            "OH_Input_RequestInjection", "OH_Input_QueryAuthorizedStatus",
            "OH_Input_InjectMouseEvent", "OH_Input_InjectMouseEventGlobal",
            "OH_Input_InjectKeyEvent", "OH_Input_CancelInjection"],
        "native_buffer/native_buffer.h": ["OH_NativeBuffer_GetConfig"],
    }
    checks = []
    for relative, symbols in declarations.items():
        path = NATIVE / "sysroot/usr/include" / relative
        try:
            source = path.read_text()
        except OSError:
            source = ""
        # Exclude mentions in documentation; record the actual declaration line.
        stripped = re.sub(r"/\*.*?\*/", lambda m: "\n" * m[0].count("\n"), source, flags=re.S)
        for symbol in symbols:
            match = re.search(r"\b" + re.escape(symbol) + r"\s*\(", stripped)
            checks.append({"symbol": symbol, "header": str(path), "present": bool(match),
                           "line": stripped.count("\n", 0, match.start()) + 1 if match else None})
    libraries = [NATIVE / "sysroot/usr/lib/aarch64-linux-ohos" / name for name in
                 ("libnative_avscreen_capture.so", "libohinput.so", "libnative_buffer.so")]
    try:
        info = plistlib.loads((DEVECO / "Contents/Info.plist").read_bytes())
        deveco_version = info.get("CFBundleShortVersionString")
        deveco_build = info.get("CFBundleVersion")
    except (OSError, ValueError):
        deveco_version = None
        deveco_build = None
    tools = {
        "node": run([DEVECO / "Contents/tools/node/bin/node", "--version"]),
        "java": run([JAVA, "-version"]),
        "xcode_select": run(["/usr/bin/xcode-select", "-p"]),
        "xcode_installed": run(["/Applications/Xcode.app/Contents/Developer/usr/bin/xcodebuild", "-version"]),
        "clang": run([NATIVE / "llvm/bin/clang", "--version"]),
    }
    sdk = read_json(SDK / "sdk-pkg.json")
    native = read_json(NATIVE / "oh-uni-package.json")
    ready = (all(check["present"] for check in checks) and
             all(path.is_file() for path in libraries) and native.get("apiVersion") == "26" and
             tools["node"]["returncode"] == 0 and tools["java"]["returncode"] == 0 and
             tools["clang"]["returncode"] == 0 and
             (DEVECO / "Contents/tools/hvigor/bin/hvigorw").is_file() and
             (DEVECO / "Contents/tools/ohpm/bin/ohpm").is_file())
    report = {
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "host": {"system": platform.system(), "version": platform.mac_ver()[0],
                 "architecture": platform.machine()},
        "deveco": {"path": str(DEVECO), "version": deveco_version, "build": deveco_build},
        "sdk": {"path": str(SDK), "package": sdk, "native": native},
        "hvigor_version": read_json(DEVECO / "Contents/tools/hvigor/hvigor/package.json").get("version"),
        "tools": tools, "api_declarations": checks,
        "libraries": [{"path": str(path), "present": path.is_file()} for path in libraries],
        "hdc": {"path": str(HDC), "present": HDC.is_file(), "executed": False},
        "status": "SDK_READY_FOR_BUILD" if ready else "ENVIRONMENT_INCOMPLETE",
        "boundary": "Header and library presence does not prove device authorization or runtime behavior.",
    }
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    return 0 if ready else 1


if __name__ == "__main__":
    raise SystemExit(main())
