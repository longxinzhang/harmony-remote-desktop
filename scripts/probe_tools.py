"""Shared local tool discovery; never installs SDKs or reads signing keys."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEVECO = Path(os.environ.get("DEVECO_HOME", "/Applications/DevEco-Studio.app"))
SDK_HOME = Path(os.environ.get("DEVECO_SDK_HOME", str(DEVECO / "Contents/sdk")))
SDK = Path(os.environ.get("HARMONY_SDK", str(SDK_HOME / "default")))
NATIVE = SDK / "openharmony/native"
HDC = SDK / "openharmony/toolchains/hdc"
JAVA = DEVECO / "Contents/jbr/Contents/Home/bin/java"


def run(command, timeout=15, **kwargs):
    """Return a bounded subprocess result without shell interpolation."""
    args = [str(item) for item in command]
    try:
        result = subprocess.run(args, capture_output=True, text=True,
                                timeout=timeout, **kwargs)
        return {"command": args, "returncode": result.returncode,
                "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except subprocess.TimeoutExpired:
        return {"command": args, "returncode": None, "error": "TIMEOUT"}
    except OSError as error:
        return {"command": args, "returncode": None, "error": str(error)}


def check_devices(target=None, timeout=15):
    result = run([HDC, "list", "targets"], timeout=timeout)
    output = result.get("stdout", "").strip()
    errors = result.get("stderr", "").strip()
    if result["returncode"] != 0:
        status, targets = "CONNECTION_ERROR", []
    elif output == "[Empty]" and not errors:
        status, targets = "NO_DEVICE", []
    elif not output or errors or any(token in output.lower() for token in
            ("[fail]", "[error]", "failed", "error:", "connect server failed")):
        status, targets = "CONNECTION_ERROR", []
    else:
        targets = [line.strip() for line in output.splitlines() if line.strip()]
        if any(any(character.isspace() for character in item) or item.startswith("[")
               for item in targets):
            status, targets = "CONNECTION_ERROR", []
        else:
            status = "READY" if target is None or target in targets else "TARGET_NOT_FOUND"
    return {"status": status, "targets": targets, "requested_target": target,
            "probe": result}
