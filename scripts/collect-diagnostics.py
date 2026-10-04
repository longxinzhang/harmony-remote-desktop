#!/usr/bin/env python3
"""Export only this probe's five diagnostic files from an explicitly named device."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import tempfile

from probe_tools import HDC, ROOT, run

BUNDLE = "com.longxin.harmonyremote.probe"
# Verified on the installed EntryAbility. This is an ability-specific filesDir.
REMOTE_DIRECTORY = "./data/storage/el2/base/haps/entry/files"
FILENAMES = (
    "capture-probe.json",
    "capture-frame.ppm",
    "input-test.json",
    "capture-snapshot.json",
    "device-info.json",
)
MAX_FILE_BYTES = 8 * 1024 * 1024


def validate_data(filename, payload):
    """Validate structure, not the success of capture or input behavior."""
    if filename.endswith(".ppm"):
        # CaptureProbe::WriteSample emits precisely this binary P6 header.
        header = re.match(rb"P6\n([1-9][0-9]*) ([1-9][0-9]*)\n255\n", payload)
        if not header:
            return "Expected the probe's binary P6 PPM header"
        width, height = map(int, header.groups())
        if width > 1920 or height > 1080:
            return "PPM dimensions exceed this probe's configured bounds"
        if len(payload) - header.end() != width * height * 3:
            return "PPM pixel data size does not match its dimensions"
        return None
    if len(payload) > 1024 * 1024:
        return "Diagnostic JSON exceeds 1 MiB"
    try:
        data = json.loads(payload)
    except (UnicodeError, ValueError):
        return "File is not valid UTF JSON"
    if not isinstance(data, dict):
        return "Expected a JSON object"
    if filename in ("capture-probe.json", "capture-snapshot.json"):
        valid = (data.get("schemaVersion") == 1 and
                 data.get("probe") == "harmony-screen-capture-phase0" and
                 isinstance(data.get("status"), str) and
                 type(data.get("frames")) is int and
                 type(data.get("running")) is bool)
    elif filename == "input-test.json":
        valid = (data.get("mode") == "USER_GRANTED" and
                 type(data.get("queryCode")) is int and
                 type(data.get("authorized")) is bool and
                 isinstance(data.get("steps"), list))
    else:
        valid = (type(data.get("api")) is int and
                 all(isinstance(data.get(key), str) for key in
                     ("deviceType", "osFullName", "sdk")))
    return None if valid else "JSON does not match this probe's diagnostic schema"


def receive_file(target, destination, filename):
    record = {"name": filename, "remote": f"{REMOTE_DIRECTORY}/{filename}",
              "status": "NOT_RECEIVED", "sha256": None, "bytes": 0}
    # Every transfer starts with a nonexistent path, preventing an old local
    # export from being mistaken for a successful HDC transfer.
    with tempfile.TemporaryDirectory(prefix=".transfer-", dir=destination) as temporary:
        incoming = Path(temporary) / filename
        transfer = run([HDC, "-t", target, "file", "recv", "-b", BUNDLE,
                        record["remote"], incoming], timeout=30)
        record["hdc_returncode"] = transfer["returncode"]
        record["hdc_stdout"] = transfer.get("stdout", "")[:2048]
        record["hdc_stderr"] = transfer.get("stderr", "")[:2048]
        if transfer.get("error"):
            record["hdc_error"] = transfer["error"]
        messages = (transfer.get("stdout", "") + "\n" + transfer.get("stderr", "")).lower()
        if re.search(r"no such file|does not exist|doesn't exist|not exist|enoent", messages):
            record["status"] = "MISSING"
            return record
        if (transfer["returncode"] != 0 or
                re.search(r"\[fail\]|\[error\]|error:|failed|fail:", messages)):
            record["status"] = "TRANSFER_FAILED"
            return record
        if incoming.is_symlink() or not incoming.is_file():
            record["status"] = "NO_FILE_RECEIVED"
            record["error"] = "HDC did not create a new regular file; remote file may be missing or unreadable"
            return record
        size = incoming.stat().st_size
        record["bytes"] = size
        if size == 0 or size > MAX_FILE_BYTES:
            record["status"] = "INVALID_DATA"
            record["error"] = "Received file is empty or exceeds the 8 MiB diagnostic limit"
            return record
        payload = incoming.read_bytes()
        record["sha256"] = hashlib.sha256(payload).hexdigest()
        error = validate_data(filename, payload)
        if error:
            record["status"] = "INVALID_DATA"
            record["error"] = error
            local = destination / (filename + ".invalid")
        else:
            record["status"] = "COLLECTED"
            local = destination / filename
        incoming.replace(local)
        record["local"] = str(local)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, help="Exact HDC target ID; never inferred")
    parser.add_argument("--label", required=True,
                        help="New evidence folder: 1-48 lowercase letters/digits with single hyphens")
    args = parser.parse_args()
    if not args.target or args.target.startswith("-") or any(c.isspace() for c in args.target):
        parser.error("--target must be one explicit device ID")
    if len(args.label) > 48 or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", args.label):
        parser.error("--label must be a safe slug, for example browser-inject-a-01")
    base = ROOT / "artifacts/device"
    if not base.resolve().is_relative_to(ROOT.resolve()):
        parser.error("The evidence directory resolves outside this project")
    destination = base / args.label
    try:
        destination.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error("This label already exists; choose a new label to preserve earlier evidence")
    report = {
        "schemaVersion": 1,
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "target": args.target,
        "bundle": BUNDLE,
        "remote_directory": REMOTE_DIRECTORY,
        "label": args.label,
        "files": [],
        "boundary": "Collection validates file transfer and format only; it does not mark any device gate PASS.",
    }
    for filename in FILENAMES:
        try:
            record = receive_file(args.target, destination, filename)
        except (OSError, ValueError) as error:
            record = {"name": filename, "status": "LOCAL_ERROR", "error": str(error),
                      "sha256": None, "bytes": 0}
        report["files"].append(record)
    collected = sum(item["status"] == "COLLECTED" for item in report["files"])
    report["collected_count"] = collected
    report["expected_count"] = len(FILENAMES)
    report["status"] = "COMPLETE" if collected == len(FILENAMES) else ("PARTIAL" if collected else "FAILED")
    report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    output = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    (destination / "collection.json").write_text(output)
    print(output, end="")
    return 0 if report["status"] == "COMPLETE" else (2 if collected else 3)


if __name__ == "__main__":
    raise SystemExit(main())
