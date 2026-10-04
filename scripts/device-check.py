#!/usr/bin/env python3
"""Read-only HDC discovery; READY means reachable, not that device tests passed."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
from probe_tools import check_devices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", help="Require this exact device ID; never selects a default device")
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.timeout < 1 or args.timeout > 60:
        parser.error("--timeout must be between 1 and 60 seconds")
    report = check_devices(args.target, args.timeout)
    report["checked_at_utc"] = datetime.now(timezone.utc).isoformat()
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    return {"READY": 0, "NO_DEVICE": 2, "CONNECTION_ERROR": 3, "TARGET_NOT_FOUND": 4}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
