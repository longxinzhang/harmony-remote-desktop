#!/usr/bin/env python3
"""Development-only: real H.264 AUs -> HRD1 replay with synthetic 30 Hz timestamps.

ffprobe is only used here to locate access units. It is not a Viewer dependency.
No source PTS, capture FPS, latency, or live network timing is inferred.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def nals(data: bytes):
    starts = list(re.finditer(b"\x00\x00(?:\x00)?\x01", data))
    for i, start in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(data)
        payload = data[start.end():end].rstrip(b"\x00")
        if not payload:
            raise ValueError("empty NAL in source")
        yield payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--expected-frames", type=int, default=287)
    args = parser.parse_args()
    source = args.input.read_bytes()
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets",
                            "-show_entries", "packet=pos,size,flags", "-of", "json", str(args.input)],
                           check=True, capture_output=True, text=True)
    packets = json.loads(probe.stdout)["packets"]
    if len(packets) != args.expected_frames:
        raise ValueError(f"expected {args.expected_frames} AUs, got {len(packets)}")
    parameter_sets = []
    for nal in nals(source):
        if nal[0] & 31 in (7, 8) and nal not in parameter_sets:
            parameter_sets.append(nal)
    if {nal[0] & 31 for nal in parameter_sets} != {7, 8}:
        raise ValueError("source must contain SPS and PPS")
    config = b"".join(b"\x00\x00\x00\x01" + nal for nal in parameter_sets)
    header = struct.Struct("!4sBBHIQI")
    replay = bytearray(header.pack(b"HRD1", 1, 1, 0, 0, 0, len(config)) + config)
    end = 0
    for index, packet in enumerate(packets):
        start, size = int(packet["pos"]), int(packet["size"])
        if start != end or size <= 0 or start + size > len(source):
            raise ValueError("ffprobe AU ranges do not exactly cover source")
        end = start + size
        payload = source[start:end]
        types = [nal[0] & 31 for nal in nals(payload)]
        if not any(t in (1, 5) for t in types):
            raise ValueError("ffprobe packet has no coded picture")
        key = 5 in types
        if index == 0 and not key:
            raise ValueError("first AU is not IDR")
        pts = index * 1_000_000 // 30  # Explicitly synthetic replay timing only.
        replay.extend(header.pack(b"HRD1", 1, 2, int(key), index + 1, pts, size))
        replay.extend(payload)
    if end != len(source):
        raise ValueError("source has unaccounted bytes")
    replay.extend(header.pack(b"HRD1", 1, 2, 2, len(packets) + 1, len(packets) * 1_000_000 // 30, 0))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as output:
        output.write(replay)
    manifest = {
        "source": str(args.input.resolve()), "sourceSha256": hashlib.sha256(source).hexdigest(),
        "fixture": str(args.output.resolve()), "frames": len(packets), "bytes": len(replay),
        "timing": "SYNTHETIC_REPLAY_30_HZ_NOT_LIVE_PTS_OR_MEASURED_FPS",
        "developmentOnly": True, "productFFmpegDependency": False,
    }
    with args.output.with_suffix(args.output.suffix + ".json").open("x") as output:
        json.dump(manifest, output, indent=2)
        output.write("\n")
    print(json.dumps(manifest))


if __name__ == "__main__":
    main()
