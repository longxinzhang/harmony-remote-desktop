#!/usr/bin/env python3
"""Collect the probe's encoder JSON and H.264 stream without starting any device action."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import tempfile

from probe_tools import HDC, ROOT, run

BUNDLE = "com.longxin.harmonyremote.probe"
REMOTE_DIRECTORY = "./data/storage/el2/base/haps/entry/files"
PROBE_NAME = "harmony-h264-phase0c"
FILES = {"encoder-probe.json": 1024 * 1024, "capture.h264": 64 * 1024 * 1024}


def is_encoder_report(report):
    return (isinstance(report, dict) and type(report.get("schemaVersion")) is int and
            report["schemaVersion"] in (1, 2) and report.get("probe") == PROBE_NAME and
            isinstance(report.get("status"), str))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def receive(target, destination, filename, limit):
    record = {"name": filename, "remote": f"{REMOTE_DIRECTORY}/{filename}",
              "status": "NOT_RECEIVED", "bytes": 0, "sha256": None}
    with tempfile.TemporaryDirectory(prefix=".transfer-", dir=destination) as temporary:
        incoming = Path(temporary) / filename
        transfer = run([HDC, "-t", target, "file", "recv", "-b", BUNDLE,
                        record["remote"], incoming], timeout=60)
        record["hdc_returncode"] = transfer["returncode"]
        record["hdc_stdout"] = transfer.get("stdout", "")[:2048]
        record["hdc_stderr"] = transfer.get("stderr", "")[:2048]
        if transfer.get("error"):
            record["hdc_error"] = transfer["error"]
        messages = (transfer.get("stdout", "") + "\n" + transfer.get("stderr", "")).lower()
        if re.search(r"no such file|does not exist|doesn't exist|not exist|enoent", messages):
            record["status"] = "MISSING"
            return record
        if transfer["returncode"] != 0 or re.search(r"\[fail\]|\[error\]|error:|failed|fail:", messages):
            record["status"] = "TRANSFER_FAILED"
            return record
        if incoming.is_symlink() or not incoming.is_file():
            record["status"] = "NO_FILE_RECEIVED"
            record["error"] = "HDC created no new regular file; remote file may be missing or unreadable"
            return record
        size = incoming.stat().st_size
        record["bytes"] = size
        if size == 0 or size > limit:
            record["status"] = "INVALID_SIZE"
            record["error"] = f"Expected a nonempty file no larger than {limit} bytes"
            return record
        record["sha256"] = sha256(incoming)
        if filename.endswith(".json"):
            try:
                report = json.loads(incoming.read_bytes())
                if isinstance(report, dict):
                    record["source_schema_version"] = report.get("schemaVersion")
                valid = is_encoder_report(report)
            except (UnicodeError, ValueError):
                valid = False
            if not valid:
                record["status"] = "INVALID_REPORT"
                record["error"] = "Encoder report does not match the expected probe/schema"
                local = destination / (filename + ".invalid")
                incoming.replace(local)
                record["local"] = str(local)
                return record
        local = destination / filename
        incoming.replace(local)
        record.update({"status": "COLLECTED", "local": str(local)})
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, help="Exact HDC target ID")
    parser.add_argument("--label", required=True,
                        help="New evidence folder: lowercase letters/digits with single hyphens, up to 48 characters")
    args = parser.parse_args()
    if not args.target or args.target.startswith("-") or any(c.isspace() for c in args.target):
        parser.error("--target must be one explicit device ID")
    if len(args.label) > 48 or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", args.label):
        parser.error("--label must be a safe slug, for example encoder-take-01")
    base = ROOT / "artifacts/device"
    if not base.resolve().is_relative_to(ROOT.resolve()):
        parser.error("Evidence directory resolves outside this project")
    destination = base / args.label
    try:
        destination.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error("This label already exists; choose a new label to preserve earlier evidence")
    report = {"schemaVersion": 1, "kind": "encoder-evidence-collection",
              "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "target": args.target, "bundle": BUNDLE, "remote_directory": REMOTE_DIRECTORY,
              "label": args.label, "files": [],
              "boundary": "Collected files are not an encoder Gate PASS. Decode the H.264 and review native PTS/wall timing, run identity, and picture content."}
    for filename, limit in FILES.items():
        try:
            record = receive(args.target, destination, filename, limit)
        except (OSError, ValueError) as error:
            record = {"name": filename, "status": "LOCAL_ERROR", "error": str(error),
                      "bytes": 0, "sha256": None}
        report["files"].append(record)
    count = sum(item["status"] == "COLLECTED" for item in report["files"])
    report["collected_count"] = count
    report["expected_count"] = len(FILES)
    report["status"] = "COMPLETE" if count == len(FILES) else ("PARTIAL" if count else "FAILED")
    report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    (destination / "collection.json").write_text(text)
    print(text, end="")
    return 0 if count == len(FILES) else (2 if count else 3)


if __name__ == "__main__":
    raise SystemExit(main())
