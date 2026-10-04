#!/usr/bin/env python3
"""Locally probe/decode a collected H.264 and save evidence; never declares Gate C PASS."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from probe_tools import ROOT

MAX_STREAM_BYTES = 64 * 1024 * 1024
PROBE_NAME = "harmony-h264-phase0c"


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


def execute(command, stdout_path, stderr_path, timeout=60):
    args = [str(value) for value in command]
    result = {"command": args, "stdout_log": str(stdout_path), "stderr_log": str(stderr_path)}
    with stdout_path.open("xb") as output, stderr_path.open("xb") as errors:
        try:
            process = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=output, stderr=errors,
                                     timeout=timeout, check=False)
            result["returncode"] = process.returncode
        except subprocess.TimeoutExpired:
            result.update({"returncode": None, "error": "TIMEOUT"})
        except OSError as error:
            result.update({"returncode": None, "error": str(error)})
    return result


def native_report(input_path):
    path = input_path.parent / "encoder-probe.json"
    result = {"path": str(path), "present": path.is_file(),
              "boundary": "Review native PTS and wall timing together with this stream; demuxer frame-rate estimates are not measured supply FPS."}
    if not path.is_file():
        return result
    if not path.resolve().is_relative_to((ROOT / "artifacts/device").resolve()):
        result["error"] = "Native report resolves outside the device evidence directory"
        return result
    if path.stat().st_size > 1024 * 1024:
        result["error"] = "Native report exceeds 1 MiB"
        return result
    result["sha256"] = sha256(path)
    try:
        data = json.loads(path.read_bytes())
        if isinstance(data, dict):
            result["source_schema_version"] = data.get("schemaVersion")
        if not is_encoder_report(data):
            raise ValueError("Unexpected native report schema/probe")
        result["status"] = data.get("status")
        # Preserve the native evidence separately instead of deriving its wall
        # duration from timestamps that FFmpeg invents for an elementary stream.
        result["report"] = data
    except (UnicodeError, ValueError) as error:
        result["error"] = str(error)
    return result


def read_decode_progress(path):
    result = {}
    if path.is_file():
        for line in path.read_text(errors="replace").splitlines():
            key, separator, value = line.partition("=")
            if separator:
                result[key] = value
    return {"last_values": result,
            "completed": result.get("progress") == "end",
            "decoded_frames": int(result["frame"]) if result.get("frame", "").isdigit() else None,
            "timing_boundary": "out_time and fps here are decode/muxer statistics, not native capture timing or supply rate."}


def annexb_statistics(path):
    payload = path.read_bytes()  # The caller enforces the 64 MiB input bound.
    counts = {}
    empty = 0
    for marker in re.finditer(rb"\x00\x00(?:\x00)?\x01", payload):
        if marker.end() == len(payload):
            empty += 1
            continue
        nal_type = str(payload[marker.end()] & 31)
        counts[nal_type] = counts.get(nal_type, 0) + 1
    return {"nal_type_counts": counts, "empty_trailing_start_codes": empty,
            "sps_present": counts.get("7", 0) > 0,
            "pps_present": counts.get("8", 0) > 0,
            "idr_present": counts.get("5", 0) > 0,
            "boundary": "Annex B structure counts only; NAL counts are not decoded-frame counts or a Gate PASS."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path,
                        help="Nonempty .h264 file inside this project's artifacts/device directory")
    parser.add_argument("--output-label", default="h264-validation",
                        help="New validation subfolder beside input; default h264-validation")
    args = parser.parse_args()
    if len(args.output_label) > 48 or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", args.output_label):
        parser.error("--output-label must be a safe slug of at most 48 characters")
    evidence = (ROOT / "artifacts/device").resolve()
    if not evidence.is_relative_to(ROOT.resolve()):
        parser.error("Evidence directory resolves outside this project")
    source = args.input.expanduser().resolve()
    if not source.is_relative_to(evidence) or source.suffix.lower() != ".h264" or not source.is_file():
        parser.error("--input must resolve to an existing .h264 file within this project's artifacts/device")
    size = source.stat().st_size
    if size == 0 or size > MAX_STREAM_BYTES:
        parser.error("--input must contain 1 byte to 64 MiB")
    ffprobe, ffmpeg = shutil.which("ffprobe"), shutil.which("ffmpeg")
    if not ffprobe or not ffmpeg:
        parser.error("Installed ffprobe and ffmpeg are required; this script never installs tools")
    destination = source.parent / args.output_label
    try:
        destination.mkdir(exist_ok=False)
    except FileExistsError:
        parser.error("Validation folder already exists; choose a new --output-label")
    fingerprint = sha256(source)
    report = {"schemaVersion": 1, "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "input": {"path": str(source), "bytes": size, "sha256": fingerprint},
              "tools": {"ffprobe": ffprobe, "ffmpeg": ffmpeg},
              "native_evidence": native_report(source),
              "annexb_structure": annexb_statistics(source),
              "gate_result": "NOT_AUTOMATICALLY_DETERMINED",
              "timing_boundary": "No -r override is used. Bare H.264 demuxer rates/duration are metadata or estimates, not proof of 10 seconds of native capture or sustained 30 FPS. Review native PTS/wall evidence and video together."}
    probe_path = destination / "ffprobe.json"
    probe_run = execute([ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
                         "-show_entries", "stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_frames,nb_frames,time_base,duration,start_time:format=format_name,duration,size",
                         "-of", "json", source], probe_path, destination / "ffprobe.log")
    report["probe_run"] = probe_run
    stream = None
    try:
        probe_data = json.loads(probe_path.read_text())
        streams = probe_data.get("streams", [])
        if probe_run["returncode"] == 0 and len(streams) == 1 and isinstance(streams[0], dict):
            stream = streams[0]
            report["stream"] = stream
            report["container_metadata"] = probe_data.get("format", {})
    except (OSError, ValueError, AttributeError):
        report["probe_error"] = "Could not parse the expected ffprobe JSON"
    probe_valid = bool(stream and stream.get("codec_name") == "h264" and
                       isinstance(stream.get("width"), int) and stream["width"] > 0 and
                       isinstance(stream.get("height"), int) and stream["height"] > 0 and
                       str(stream.get("nb_read_frames", "")).isdigit() and
                       int(stream["nb_read_frames"]) > 0)
    progress_path = destination / "ffmpeg-decode-progress.txt"
    report["decode_run"] = execute([ffmpeg, "-hide_banner", "-nostdin", "-nostats", "-v", "info",
                                    "-xerror", "-err_detect", "explode", "-i", source,
                                    "-map", "0:v:0", "-an", "-sn", "-dn",
                                    "-fps_mode", "passthrough",
                                    "-progress", progress_path, "-f", "null", "-"],
                                   destination / "ffmpeg-decode.stdout.log", destination / "ffmpeg-decode.log")
    progress = read_decode_progress(progress_path)
    report["decode_progress"] = progress
    decoded = (report["decode_run"]["returncode"] == 0 and progress["completed"] and
               bool(progress["decoded_frames"]) and probe_valid and
               progress["decoded_frames"] == int(stream["nb_read_frames"]))
    sample_ok = False
    if decoded:
        sample = destination / "sample-frame.png"
        sample_index = progress["decoded_frames"] // 2
        report["sample_frame_index"] = sample_index
        report["sample_run"] = execute([ffmpeg, "-hide_banner", "-nostdin", "-v", "info",
                                        "-xerror", "-err_detect", "explode", "-i", source,
                                        "-map", "0:v:0", "-vf", f"select=eq(n\\,{sample_index})",
                                        "-frames:v", "1", "-an", "-sn", "-dn",
                                        "-update", "1", sample],
                                       destination / "ffmpeg-sample.stdout.log", destination / "ffmpeg-sample.log")
        if report["sample_run"]["returncode"] == 0 and sample.is_file():
            with sample.open("rb") as file:
                header = file.read(24)
            if len(header) == 24 and header[:8] == b"\x89PNG\r\n\x1a\n" and header[12:16] == b"IHDR":
                width, height = struct.unpack(">II", header[16:24])
                sample_ok = width == stream["width"] and height == stream["height"]
                report["sample"] = {"path": str(sample), "frame_index_zero_based": sample_index,
                                    "width": width, "height": height,
                                    "bytes": sample.stat().st_size, "sha256": sha256(sample)}
    try:
        unchanged = source.stat().st_size == size and sha256(source) == fingerprint
    except OSError as error:
        unchanged = False
        report["input_recheck_error"] = str(error)
    report["input_unchanged"] = unchanged
    report["full_decode_validated"] = decoded and unchanged
    report["sample_png_validated"] = sample_ok
    report["status"] = "DECODE_VALIDATED_NEEDS_NATIVE_TIMING_AND_VISUAL_REVIEW" if decoded and sample_ok and unchanged else "VALIDATION_FAILED"
    report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    (destination / "validation.json").write_text(text)
    print(text, end="")
    return 0 if decoded and sample_ok and unchanged else 1


if __name__ == "__main__":
    raise SystemExit(main())
