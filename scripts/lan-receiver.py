#!/usr/bin/env python3
"""Bounded, authenticated Phase 1 LAN receiver. Python standard library only."""
from __future__ import annotations

import argparse
import errno
import getpass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import sys
import threading
import time
import warnings


CONTROL_PORT = 39871
VIDEO_PORT = 39872
MAX_CONTROL_BYTES = 4096
MAX_PAYLOAD_BYTES = 8 * 1024 * 1024
MAX_CONFIG_BYTES = 256 * 1024
MAX_FILE_BYTES = 64 * 1024 * 1024
HEADER = struct.Struct("!4sBBHIQI")
LENGTH = struct.Struct("!I")
START_CODE = re.compile(b"\x00\x00\x00\x01|\x00\x00\x01")
LAN_NETWORKS = tuple(ipaddress.IPv4Network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


class ReceiverError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def validate_host(value: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        raise argparse.ArgumentTypeError("--host must be an RFC1918 IPv4 address") from None
    if not any(address in network for network in LAN_NETWORKS):
        raise argparse.ArgumentTypeError("--host must be in 10/8, 172.16/12, or 192.168/16")
    return str(address)


def bounded_timeout(value: str) -> float:
    try:
        seconds = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("--timeout must be 1 to 300 seconds") from None
    if not 1 <= seconds <= 300:
        raise argparse.ArgumentTypeError("--timeout must be 1 to 300 seconds")
    return seconds


def read_pin(stdin_mode: bool = False) -> str:
    if stdin_mode:
        value = sys.stdin.readline(64).rstrip("\r\n")
    else:
        # Refuse getpass's echoing fallback when a controlling terminal is absent.
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("error", getpass.GetPassWarning)
                value = getpass.getpass("Host PIN (hidden): ")
        except (OSError, getpass.GetPassWarning):
            raise ReceiverError("PIN_INPUT", "no terminal; use --pin-stdin only with controlled stdin") from None
    if re.fullmatch(r"[0-9]{6}", value) is None:
        raise ReceiverError("PIN_INPUT", "PIN must contain exactly six ASCII digits")
    return value


def wait_socket(sock: socket.socket, deadline: float, check, *, writable: bool = False) -> None:
    while True:
        check()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ReceiverError("TIMEOUT", "socket operation exceeded its deadline")
        try:
            readable, writeable, _ = select.select([] if writable else [sock], [sock] if writable else [], [], min(0.1, remaining))
        except (OSError, ValueError):
            check()
            raise ReceiverError("SOCKET", "socket became unavailable") from None
        if readable or writeable:
            return


def read_exact(sock: socket.socket, size: int, deadline: float, check) -> bytes:
    data = bytearray()
    while len(data) < size:
        wait_socket(sock, deadline, check)
        try:
            block = sock.recv(min(size - len(data), 65536))
        except (BlockingIOError, InterruptedError):
            continue
        except OSError:
            check()
            raise ReceiverError("SOCKET", "socket read failed") from None
        if not block:
            raise ReceiverError("EOF", "connection closed before a complete message")
        data.extend(block)
    check()
    return bytes(data)


def write_exact(sock: socket.socket, data: bytes, deadline: float, check) -> None:
    view = memoryview(data)
    while view:
        wait_socket(sock, deadline, check, writable=True)
        try:
            sent = sock.send(view)
        except (BlockingIOError, InterruptedError):
            continue
        except OSError:
            raise ReceiverError("SOCKET", "socket write failed") from None
        if not sent:
            raise ReceiverError("EOF", "connection closed during send")
        view = view[sent:]


def connect_socket(address, deadline: float, check) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.setblocking(False)
        result = sock.connect_ex(address)
        if result not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY):
            raise ReceiverError("CONNECT", "could not connect to Host")
        if result:
            wait_socket(sock, deadline, check, writable=True)
            if sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR):
                raise ReceiverError("CONNECT", "could not connect to Host")
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock
    except BaseException:
        sock.close()
        raise


def json_frame(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("utf-8")
    if not 0 < len(body) <= MAX_CONTROL_BYTES:
        raise ReceiverError("CONTROL_LENGTH", "invalid outgoing control length")
    return LENGTH.pack(len(body)) + body


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate field")
        result[key] = value
    return result


def read_json(sock, deadline, check) -> dict:
    first = read_exact(sock, 1, deadline, check)
    message_deadline = min(deadline, time.monotonic() + 2)
    size, = LENGTH.unpack(first + read_exact(sock, LENGTH.size - 1, message_deadline, check))
    if not 0 < size <= MAX_CONTROL_BYTES:
        raise ReceiverError("CONTROL_LENGTH", "invalid control payload length")
    raw = read_exact(sock, size, message_deadline, check)
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_json_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError("non-finite number")))
    except (UnicodeError, ValueError, RecursionError):
        raise ReceiverError("CONTROL_JSON", "invalid UTF-8 JSON control message") from None
    if not isinstance(value, dict) or not isinstance(value.get("type"), str):
        raise ReceiverError("CONTROL_JSON", "control message must be an object with a type")
    return value


def nal_types(payload: bytes) -> set[int]:
    """Only validate Annex-B framing/NAL kinds; this is not a video decoder."""
    kinds = set()
    previous_end = None
    for marker in START_CODE.finditer(payload):
        if previous_end is None and any(payload[:marker.start()]):
            raise ReceiverError("ANNEX_B", "payload does not start with Annex-B framing")
        if previous_end is not None and marker.start() <= previous_end:
            raise ReceiverError("ANNEX_B", "empty Annex-B NAL unit")
        if marker.end() >= len(payload):
            raise ReceiverError("ANNEX_B", "trailing empty Annex-B NAL unit")
        header = payload[marker.end()]
        kind = header & 31
        if header & 128 or not 1 <= kind <= 23:
            raise ReceiverError("ANNEX_B", "invalid Annex-B NAL header")
        kinds.add(kind)
        previous_end = marker.end()
    if not kinds:
        raise ReceiverError("ANNEX_B", "payload contains no Annex-B NAL units")
    return kinds


class Receiver:
    def __init__(self, host: str, output: Path, timeout: float = 120, *, pin_reader=None,
                 connector=connect_socket, emit=None, heartbeat_interval: float = 2,
                 heartbeat_timeout: float = 6):
        self.host = validate_host(host)
        self.output = Path(output)
        if not 0 < timeout <= 300:
            raise ValueError("invalid overall timeout")
        self.timeout = timeout
        self.pin_reader = pin_reader or read_pin
        self.connector = connector
        self.emit = emit or (lambda message: print(message, flush=True))
        self.heartbeat_interval = heartbeat_interval
        self.heartbeat_timeout = heartbeat_timeout
        self.control = self.video = None
        self.control_thread = None
        self.stop_event = threading.Event()
        self.send_lock = threading.Lock()
        self.error_lock = threading.Lock()
        self.control_failure = None
        self.token = None
        self.last_control = self.next_ping = 0.0
        self.deadline = 0.0
        self.directory_created = False
        self.hasher = hashlib.sha256()
        self.first_au_time = self.last_au_time = None
        self.report = {
            "schemaVersion": 1, "clientVersion": "0.3.0", "status": "initializing",
            "host": host, "controlPort": CONTROL_PORT, "videoPort": VIDEO_PORT,
            "paired": False, "videoReady": False, "eosReceived": False,
            "timestampSource": "encoder_callback_monotonic", "nativePtsUnitVerified": False,
            "packets": 0, "configPackets": 0, "accessUnits": 0, "keyframes": 0,
            "keepalivePackets": 0, "payloadBytes": 0, "h264Bytes": 0, "wireBytes": 0,
            "sequence": {"first": None, "last": None, "gaps": 0},
            "ptsUs": {"first": None, "last": None, "regressions": 0},
            "heartbeat": {"pingsSent": 0, "pingsReceived": 0, "pongsReceived": 0},
            "output": {"saved": False, "filename": "capture.h264.partial", "maxBytes": MAX_FILE_BYTES},
            "errors": [], "gateResult": "NOT_AUTOMATICALLY_DETERMINED",
            "frameCountMethod": "non-empty type 2 access-unit packets; Annex-B framing checked, not decoded frames",
            "timingBoundary": "pts_us is session-relative encoder callback arrival time, not capture/native PTS or end-to-end latency",
        }

    def check(self):
        with self.error_lock:
            failure = self.control_failure
        if failure:
            raise failure
        if self.stop_event.is_set():
            raise ReceiverError("STOPPED", "receiver stopped")
        if time.monotonic() >= self.deadline:
            raise ReceiverError("TIMEOUT", "overall receive deadline exceeded")

    def send_control(self, message, *, final=False):
        with self.send_lock:
            deadline = time.monotonic() + 1
            if not final:
                deadline = min(deadline, self.deadline)
            write_exact(self.control, json_frame(message), deadline, (lambda: None) if final else self.check)

    def control_tick(self):
        self.check()
        now = time.monotonic()
        if now - self.last_control >= self.heartbeat_timeout:
            raise ReceiverError("HEARTBEAT_TIMEOUT", "Host heartbeat response timed out")
        if now >= self.next_ping:
            self.send_control({"type": "ping", "sessionToken": self.token})
            self.report["heartbeat"]["pingsSent"] += 1
            self.next_ping = time.monotonic() + self.heartbeat_interval

    def control_loop(self):
        try:
            while not self.stop_event.is_set():
                message = read_json(self.control, min(self.deadline, self.last_control + self.heartbeat_timeout), self.control_tick)
                if message.get("sessionToken") != self.token:
                    raise ReceiverError("INVALID_SESSION", "Host control message has an invalid session")
                kind = message["type"]
                if kind == "ping":
                    self.send_control({"type": "pong", "sessionToken": self.token})
                    self.report["heartbeat"]["pingsReceived"] += 1
                elif kind == "pong":
                    self.report["heartbeat"]["pongsReceived"] += 1
                elif kind in ("error", "stop", "display_changed"):
                    raise ReceiverError("HOST_STOP", "Host stopped the session or reported an error")
                else:
                    raise ReceiverError("CONTROL_TYPE", "unexpected authenticated control message")
                self.last_control = time.monotonic()
        except ReceiverError as error:
            if not self.stop_event.is_set() and not (self.report["eosReceived"] and error.code == "EOF"):
                with self.error_lock:
                    self.control_failure = error
                self.stop_event.set()
        except Exception:
            if not self.stop_event.is_set():
                with self.error_lock:
                    self.control_failure = ReceiverError("CONTROL_THREAD", "control worker failed")
                self.stop_event.set()

    def pair(self):
        # Do not hold an unauthenticated Host socket open while a human reads the PIN.
        self.emit("Enter the six-digit PIN shown on Host. The PIN is not logged.")
        pin = self.pin_reader()
        if not isinstance(pin, str) or re.fullmatch(r"[0-9]{6}", pin) is None:
            raise ReceiverError("PIN_INPUT", "PIN must contain exactly six ASCII digits")
        self.deadline = time.monotonic() + self.timeout
        self.control = self.connector((self.host, CONTROL_PORT), min(self.deadline, time.monotonic() + 5), self.check)
        self.send_control({"type": "hello", "protocol": 1, "client": "macOS", "clientVersion": "0.3.0"})
        hello = read_json(self.control, min(self.deadline, time.monotonic() + 6), self.check)
        if (hello.get("type") != "hello_ack" or type(hello.get("protocol")) is not int or hello["protocol"] != 1
                or hello.get("pairingRequired") is not True
                or hello.get("timestampSource") != "encoder_callback_monotonic"
                or hello.get("nativePtsUnitVerified") is not False):
            raise ReceiverError("HELLO", "Host hello response is incompatible with protocol 1")
        try:
            self.send_control({"type": "pair", "pin": pin})
        finally:
            pin = None
        response = read_json(self.control, min(self.deadline, time.monotonic() + 6), self.check)
        token = response.get("sessionToken")
        if response.get("type") != "pair_ok" or not isinstance(token, str) or re.fullmatch(r"[0-9a-fA-F]{64}", token) is None:
            raise ReceiverError("PAIR_REJECTED", "Host rejected pairing or returned an invalid session")
        self.token = token
        self.report["paired"] = True
        self.last_control = time.monotonic()
        self.next_ping = self.last_control + self.heartbeat_interval
        self.control_thread = threading.Thread(target=self.control_loop, name="lan-control", daemon=True)
        self.control_thread.start()
        self.video = self.connector((self.host, VIDEO_PORT), min(self.deadline, time.monotonic() + 5), self.check)
        write_exact(self.video, json_frame({"type": "video_attach", "sessionToken": self.token}),
                    min(self.deadline, time.monotonic() + 5), self.check)
        ready = read_json(self.video, min(self.deadline, time.monotonic() + 6), self.check)
        if ready.get("type") != "video_ready":
            raise ReceiverError("VIDEO_ATTACH", "Host rejected the video connection")
        self.report["videoReady"] = True
        self.emit("Paired; video ready. On Host, click Start LAN Capture · 10s. Waiting for video…")

    def receive_video(self, output):
        expected_sequence = 0
        need_idr = True
        while True:
            first = read_exact(self.video, 1, self.deadline, self.check)
            packet_deadline = min(self.deadline, time.monotonic() + 2)
            raw = first + read_exact(self.video, HEADER.size - 1, packet_deadline, self.check)
            magic, version, kind, flags, sequence, pts, size = HEADER.unpack(raw)
            if magic != b"HRD1" or version != 1:
                raise ReceiverError("VIDEO_HEADER", "unsupported video magic or version")
            if kind not in (1, 2, 3) or flags & ~3:
                raise ReceiverError("VIDEO_HEADER", "invalid video type or flags")
            if size > MAX_PAYLOAD_BYTES or (kind == 1 and size > MAX_CONFIG_BYTES):
                raise ReceiverError("VIDEO_LENGTH", "video payload exceeds its limit")
            if sequence != expected_sequence:
                self.report["sequence"]["gaps"] += 1
                raise ReceiverError("SEQUENCE", "video packet sequence is not contiguous")
            if (kind == 1 and (flags or not size)) or (kind == 3 and (flags or size)) or (kind == 2 and not size and flags != 2):
                raise ReceiverError("VIDEO_HEADER", "invalid payload or flags for video packet type")
            if size > MAX_FILE_BYTES - self.report["h264Bytes"]:
                raise ReceiverError("FILE_LIMIT", "H.264 output would exceed 64 MiB")
            payload = read_exact(self.video, size, packet_deadline, self.check)
            self.report["packets"] += 1
            self.report["wireBytes"] += HEADER.size + size
            self.report["payloadBytes"] += size
            if self.report["sequence"]["first"] is None:
                self.report["sequence"]["first"] = sequence
            self.report["sequence"]["last"] = sequence
            expected_sequence = (sequence + 1) & 0xFFFFFFFF
            if kind == 1:
                kinds = nal_types(payload)
                if not {7, 8}.issubset(kinds) or kinds.intersection({1, 2, 3, 4, 5}):
                    raise ReceiverError("VIDEO_CONFIG", "config must contain SPS/PPS and no video slices")
                self.report["configPackets"] += 1
                need_idr = True
            elif kind == 2 and size:
                if not self.report["configPackets"]:
                    raise ReceiverError("VIDEO_CONFIG", "access unit arrived before config")
                kinds = nal_types(payload)
                if not kinds.intersection({1, 2, 3, 4, 5}) or bool(flags & 1) != (5 in kinds):
                    raise ReceiverError("ACCESS_UNIT", "invalid access unit or keyframe flag")
                if need_idr and not flags & 1:
                    raise ReceiverError("IDR_REQUIRED", "first access unit after config must be an IDR")
                previous_pts = self.report["ptsUs"]["last"]
                if previous_pts is not None and pts < previous_pts:
                    self.report["ptsUs"]["regressions"] += 1
                    raise ReceiverError("PTS_REGRESSION", "access-unit timestamps moved backwards")
                now = time.monotonic()
                if self.first_au_time is None:
                    self.first_au_time = now
                    self.report["ptsUs"]["first"] = pts
                self.last_au_time = now
                self.report["ptsUs"]["last"] = pts
                self.report["accessUnits"] += 1
                self.report["keyframes"] += bool(flags & 1)
                need_idr = False
            elif kind == 3:
                self.report["keepalivePackets"] += 1
            if payload:
                output.write(payload)
                self.hasher.update(payload)
                self.report["h264Bytes"] += size
            if kind == 2 and flags & 2:
                if not self.report["accessUnits"] or need_idr:
                    raise ReceiverError("INCOMPLETE_EOS", "EOS arrived without a complete configured video sequence")
                self.check()
                self.report["eosReceived"] = True
                return

    def close(self):
        self.stop_event.set()
        if self.control is not None and self.token is not None:
            try:
                self.send_control({"type": "stop", "sessionToken": self.token}, final=True)
            except (ReceiverError, OSError):
                pass  # Primary outcome is retained; socket closure also ends the session.
        for connection in (self.video, self.control):
            if connection is not None:
                try:
                    connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                connection.close()
        if self.control_thread is not None:
            self.control_thread.join(timeout=2)
        self.token = None

    def run(self) -> int:
        started = time.monotonic()
        self.deadline = started + self.timeout
        self.report["startedAtUnixMs"] = time.time_ns() // 1000000
        code = 1
        try:
            try:
                self.output.mkdir(exist_ok=False)
            except FileExistsError:
                raise ReceiverError("OUTPUT_EXISTS", "output directory already exists; choose a new directory") from None
            self.directory_created = True
            partial = self.output / "capture.h264.partial"
            with partial.open("xb") as output:
                self.report["status"] = "pairing"
                self.pair()
                self.report["status"] = "receiving"
                self.receive_video(output)
                output.flush()
                os.fsync(output.fileno())
            self.check()
            # Exclusive publication: unlike rename(), link() cannot overwrite an existing target.
            os.link(partial, self.output / "capture.h264")
            partial.unlink()
            self.report["output"].update(saved=True, filename="capture.h264")
            self.report["status"] = "received_eos_needs_decode_validation"
            code = 0
        except ReceiverError as error:
            self.report["errors"].append({"code": error.code, "message": str(error)})
        except KeyboardInterrupt:
            self.report["errors"].append({"code": "INTERRUPTED", "message": "receiver interrupted by user"})
        except OSError:
            self.report["errors"].append({"code": "LOCAL_IO", "message": "local file or socket operation failed"})
        except Exception:
            self.report["errors"].append({"code": "INTERNAL", "message": "receiver failed internally"})
        finally:
            self.close()
            if code:
                self.report["status"] = "failed"
            self.report["finishedAtUnixMs"] = time.time_ns() // 1000000
            self.report["elapsedSeconds"] = round(time.monotonic() - started, 6)
            span = 0 if self.first_au_time is None else self.last_au_time - self.first_au_time
            self.report["accessUnitReceiveSpanSeconds"] = round(span, 6)
            self.report["accessUnitReceiveFps"] = round((self.report["accessUnits"] - 1) / span, 6) if span > 0 else 0
            self.report["output"]["sha256"] = self.hasher.hexdigest()
            if self.directory_created:
                try:
                    with (self.output / "receiver-report.json").open("x", encoding="utf-8") as report_file:
                        json.dump(self.report, report_file, ensure_ascii=False, indent=2, allow_nan=False)
                        report_file.write("\n")
                except OSError:
                    code = 1
                    self.emit("Failed to write receiver-report.json.")
        if code:
            error = self.report["errors"][0] if self.report["errors"] else {"code": "REPORT", "message": "report could not be saved"}
            self.emit("Receive failed: " + error["code"] + ": " + error["message"])
        else:
            self.emit(f"Received EOS: {self.report['accessUnits']} access units, {self.report['h264Bytes']} H.264 bytes. Decode validation is still required.")
        return code


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=validate_host, help="Harmony Host RFC1918 IPv4 address")
    parser.add_argument("--output", required=True, type=Path, help="new output directory; existing directories are rejected")
    parser.add_argument("--timeout", type=bounded_timeout, default=120, help="network deadline after PIN entry, 1–300 seconds (default 120)")
    parser.add_argument("--pin-stdin", action="store_true", help="read PIN from controlled stdin instead of a hidden terminal prompt")
    args = parser.parse_args(argv)
    return Receiver(args.host, args.output, args.timeout, pin_reader=lambda: read_pin(args.pin_stdin)).run()


if __name__ == "__main__":
    raise SystemExit(main())
