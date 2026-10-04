#!/usr/bin/env python3
"""Export the latest Host LAN trial; never starts a server or recording."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import tempfile

from probe_tools import HDC, ROOT, run

BUNDLE = "com.longxin.harmonyremote.probe"
REMOTE = "./data/storage/el2/base/haps/entry/files"
FILES = {"lan-snapshot.json": 1024 * 1024, "encoder-snapshot.json": 1024 * 1024, "encoder-probe.json": 1024 * 1024,
         "device-info.json": 1024 * 1024, "capture.h264": 64 * 1024 * 1024}
LAN_VERSIONS = ("0.3.0", "0.4.0", "0.4.1", "0.5.0", "0.6.0")
CREDENTIAL_FIELDS = frozenset(("pin", "token", "sessiontoken", "bindtoken", "clipboardbindtoken", "audiobindtoken",
                              "epoch", "sessionepoch", "clipboardepoch", "audioepoch", "challenge", "clientchallenge",
                              "hostsignature", "hosthellosignature", "clientsignature", "signature",
                              "privatekey", "clientprivatekey", "hostprivatekey", "clientpublickey", "hostpublickey"))


def reject_private_fields(document, forbidden, description):
    """Inspect decoded keys, including nested objects and JSON-escaped keys."""
    pending = [document]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            for key, child in value.items():
                normalized = re.sub(r"[^a-z0-9]", "", key.casefold())
                if normalized in forbidden:
                    raise ValueError(description)
                if isinstance(child, (dict, list)):
                    pending.append(child)
        elif isinstance(value, list):
            pending.extend(child for child in value if isinstance(child, (dict, list)))


def validate_audio_snapshot(document):
    reject_private_fields(document, CREDENTIAL_FIELDS | {"pcm", "pcmdata", "payload", "data", "samples", "audiodata"},
                          "Audio diagnostic unexpectedly contains private data fields")
    flags = {"running", "bound", "streaming"}
    counters = {"queuedBytes", "highWaterBytes", "maxQueuedBytes", "receivedBytes", "sentBytes", "droppedBytes",
                "rejectedBuffers", "channelFailures"}
    if set(document) != flags | counters | {"sampleRate", "channels", "format"}:
        raise ValueError("Unexpected audio snapshot fields")
    if (any(type(document[key]) is not bool for key in flags) or
            any(type(document[key]) is not int or not 0 <= document[key] <= (1 << 64) - 1 for key in counters) or
            type(document["sampleRate"]) is not int or document["sampleRate"] != 48000 or
            type(document["channels"]) is not int or document["channels"] != 2 or document["format"] != "s16le" or
            document["maxQueuedBytes"] != 23040 or document["queuedBytes"] > document["maxQueuedBytes"] or
            document["highWaterBytes"] > document["maxQueuedBytes"]):
        raise ValueError("Unexpected audio snapshot values")


def receive(target, destination, name, limit):
    result = {"name": name, "status": "FAILED"}
    with tempfile.TemporaryDirectory(prefix=".incoming-", dir=destination) as folder:
        path = Path(folder) / name
        transfer = run([HDC, "-t", target, "file", "recv", "-b", BUNDLE,
                        f"{REMOTE}/{name}", path], timeout=60)
        result["transfer"] = transfer
        messages = transfer.get("stdout", "") + transfer.get("stderr", "")
        if (transfer["returncode"] != 0 or re.search(r"\[fail\]|\[error\]|failed|no such file", messages, re.I)
                or path.is_symlink() or not path.is_file()):
            return result
        if not 0 < path.stat().st_size <= limit:
            result["status"] = "INVALID_SIZE"
            return result
        data = path.read_bytes()
        if name.endswith(".json"):
            document = json.loads(data)
            if not isinstance(document, dict):
                raise ValueError("Expected a JSON object")
            if name in ("encoder-probe.json", "encoder-snapshot.json") and (document.get("schemaVersion") != 2 or
                    document.get("probe") != "harmony-h264-phase0c"):
                raise ValueError("Unexpected encoder report")
            if name == "lan-snapshot.json":
                if not isinstance(document.get("status"), str):
                    raise ValueError("Unexpected LAN snapshot")
                # PIN and session credentials must never be exported as diagnostics.
                reject_private_fields(document, CREDENTIAL_FIELDS,
                                      "LAN diagnostic unexpectedly contains credential fields")
            if name == "device-info.json":
                if document.get("appVersion") not in LAN_VERSIONS:
                    raise ValueError("Device snapshot is not from a supported LAN version")
                if "appBuild" in document and (type(document["appBuild"]) is not int or document["appBuild"] < 0):
                    raise ValueError("Device snapshot has an invalid build number")
            if name == "clipboard-snapshot.json":
                if not isinstance(document.get("allowed"), bool) or not isinstance(document.get("mode"), int):
                    raise ValueError("Unexpected clipboard snapshot")
                reject_private_fields(document, CREDENTIAL_FIELDS | {"text", "payload", "sha256"},
                                      "Clipboard diagnostic unexpectedly contains private data fields")
            if name == "audio-snapshot.json":
                validate_audio_snapshot(document)
        path.replace(destination / name)
        result.update(status="COLLECTED", bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--label", required=True)
    args = parser.parse_args()
    if not args.target or args.target.startswith("-") or any(c.isspace() for c in args.target):
        parser.error("One explicit HDC target is required")
    if len(args.label) > 48 or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", args.label):
        parser.error("Use a new lowercase slug of at most 48 characters")
    base = ROOT / "artifacts/device"
    if not base.resolve().is_relative_to(ROOT.resolve()):
        parser.error("Evidence directory resolves outside the project")
    destination = base / args.label
    try:
        destination.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error("Label already exists; evidence will not be overwritten")
    report = {"schemaVersion": 1, "kind": "lan-host-evidence-collection", "target": args.target,
              "collectedAt": datetime.now(timezone.utc).isoformat(), "files": [],
              "boundary": "Latest device files only. Correlate encoder trial, version, timestamps, sender counts and stream hash with receiver-report.json; collection is not a Gate PASS."}
    for name, limit in FILES.items():
        try:
            if name == "capture.h264":
                # Long sessions intentionally leave any prior short trial intact.
                # Never mislabel that old file as this session's recording.
                encoder = json.loads((destination / "encoder-probe.json").read_text())
                current = json.loads((destination / "encoder-snapshot.json").read_text())
                if not current.get("requestedAtUnixMs") or current.get("requestedAtUnixMs") != encoder.get("requestedAtUnixMs"):
                    report["files"].append({"name": name, "status": "SKIPPED_STALE_CAPTURE"})
                    report["encoderReportMatchesCurrentSession"] = False
                    continue
                report["encoderReportMatchesCurrentSession"] = True
                output = encoder.get("output", {})
                if output.get("localRecordingEnabled") is False or not output.get("saved"):
                    report["files"].append({"name": name, "status": "SKIPPED_NOT_RECORDED"})
                    continue
            record = receive(args.target, destination, name, limit)
        except (OSError, ValueError) as error:
            record = {"name": name, "status": "FAILED", "error": str(error)}
        report["files"].append(record)
    optional_files = []
    try:
        device = json.loads((destination / "device-info.json").read_text())
        if device.get("appVersion") in ("0.4.0", "0.4.1", "0.5.0", "0.6.0"):
            optional_files.append("remote-input.json")
        if device.get("appBuild", 0) >= 1000008:
            optional_files.append("clipboard-snapshot.json")
        if device.get("appBuild", 0) >= 1000011:
            optional_files.append("audio-snapshot.json")
    except (OSError, ValueError) as error:
        report["files"].append({"name": "remote-input.json", "status": "FAILED", "error": str(error)})
    for name in optional_files:
        try:
            report["files"].append(receive(args.target, destination, name, 1024 * 1024))
        except (OSError, ValueError) as error:
            report["files"].append({"name": name, "status": "FAILED", "error": str(error)})
    complete = all(item["status"] in ("COLLECTED", "SKIPPED_NOT_RECORDED", "SKIPPED_STALE_CAPTURE") for item in report["files"])
    report["status"] = "COMPLETE" if complete else "INCOMPLETE"
    (destination / "collection.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "directory": str(destination)}, indent=2))
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
