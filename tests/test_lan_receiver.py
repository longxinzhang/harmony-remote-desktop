"""Protocol/lifecycle tests use localhost fake peers, never device screen data."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("lan_receiver", Path(__file__).resolve().parents[1] / "scripts" / "lan-receiver.py")
receiver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(receiver)
TOKEN = "c4" * 32
PIN = "123456"
CONFIG = b"\x00\x00\x00\x01\x67\x64\x00\x1f\x00\x00\x01\x68\xee\x3c"
IDR = b"\x00\x00\x01\x65\x88\x84"
PFRAME = b"\x00\x00\x01\x41\x9a\x20"


def packet(kind, sequence, payload=b"", flags=0, pts=0, **overrides):
    fields = {"magic": b"HRD1", "version": 1, "kind": kind, "flags": flags,
              "sequence": sequence, "pts": pts, "size": len(payload)}
    fields.update(overrides)
    return receiver.HEADER.pack(*fields.values()) + payload


def complete_stream(eos_payload=False):
    return (packet(1, 0, CONFIG) + packet(2, 1, IDR, flags=1) +
            packet(3, 2) + packet(2, 3, PFRAME, flags=2 if eos_payload else 0, pts=33333) +
            (b"" if eos_payload else packet(2, 4, flags=2, pts=33333)))


class FakeHost:
    def __init__(self, stream=None, *, fragment=False, reject=False, heartbeat=True,
                 video_delay=0, video_eof=True, wrong_session=False, bad_json=None,
                 control_eof=False, eos_wait=0, control_eof_after_video=False):
        self.stream = complete_stream() if stream is None else stream
        self.fragment = fragment
        self.reject = reject
        self.heartbeat = heartbeat
        self.video_delay = video_delay
        self.video_eof = video_eof
        self.wrong_session = wrong_session
        self.bad_json = bad_json
        self.control_eof = control_eof
        self.eos_wait = eos_wait
        self.control_eof_after_video = control_eof_after_video
        self.done = threading.Event()
        self.paired = threading.Event()
        self.attached = threading.Event()
        self.video_sent = threading.Event()
        self.stop_received = threading.Event()
        self.connections = []
        self.errors = []
        self.listeners = []
        for _ in range(2):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                sock.bind(("127.0.0.1", 0))
            except BaseException:
                sock.close()
                raise
            sock.listen(1)
            sock.settimeout(0.1)
            self.listeners.append(sock)
        self.threads = [threading.Thread(target=self.guard, args=(self.control_loop,), daemon=True),
                        threading.Thread(target=self.guard, args=(self.video_loop,), daemon=True)]

    def __enter__(self):
        for thread in self.threads:
            thread.start()
        return self

    def __exit__(self, *args):
        self.done.set()
        for sock in self.connections + self.listeners:
            with contextlib.suppress(OSError):
                sock.shutdown(socket.SHUT_RDWR)
            sock.close()
        for thread in self.threads:
            thread.join(1)
        if self.errors and args[0] is None:
            raise self.errors[0]

    def guard(self, method):
        try:
            method()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass  # The receiver deliberately rejects malformed peers.
        except Exception as error:
            self.errors.append(error)

    def accept(self, index):
        while not self.done.is_set():
            try:
                sock, _ = self.listeners[index].accept()
                sock.settimeout(0.05)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.connections.append(sock)
                return sock
            except socket.timeout:
                continue
        return None

    def read(self, sock, size):
        result = bytearray()
        while len(result) < size and not self.done.is_set():
            try:
                block = sock.recv(size - len(result))
            except socket.timeout:
                continue
            if not block:
                return None
            result.extend(block)
        return bytes(result) if len(result) == size else None

    def read_message(self, sock):
        header = self.read(sock, 4)
        if header is None:
            return None
        size, = struct.unpack("!I", header)
        if not 0 < size <= 4096:
            raise AssertionError("receiver sent an invalid control length")
        body = self.read(sock, size)
        return json.loads(body) if body is not None else None

    def send(self, sock, data):
        if self.fragment:
            for value in data:
                sock.sendall(bytes((value,)))
        else:
            sock.sendall(data)

    def send_message(self, sock, message):
        self.send(sock, receiver.json_frame(message))

    def control_loop(self):
        sock = self.accept(0)
        if sock is None:
            return
        hello = self.read_message(sock)
        assert hello == {"type": "hello", "protocol": 1, "client": "macOS", "clientVersion": "0.3.0"}
        if self.bad_json is not None:
            self.send(sock, self.bad_json)
            return
        self.send_message(sock, {"type": "hello_ack", "protocol": 1, "pairingRequired": True,
                                 "timestampSource": "encoder_callback_monotonic", "nativePtsUnitVerified": False})
        pair = self.read_message(sock)
        assert pair == {"type": "pair", "pin": PIN}
        if self.reject:
            # Peer-controlled errors must never be echoed into reports or terminal output.
            self.send_message(sock, {"type": "error", "error": "SECRET " + PIN + TOKEN})
            return
        self.send_message(sock, {"type": "pair_ok", "sessionToken": TOKEN})
        self.paired.set()
        if self.control_eof_after_video:
            self.video_sent.wait(1)
            self.done.wait(0.05)
            sock.shutdown(socket.SHUT_RDWR)
            sock.close()
            return
        if self.control_eof:
            self.attached.wait(1)
            sock.shutdown(socket.SHUT_RDWR)
            sock.close()
            return
        while not self.done.is_set():
            message = self.read_message(sock)
            if message is None:
                return
            assert message.get("sessionToken") == TOKEN
            if message["type"] == "stop":
                self.stop_received.set()
                return
            if self.heartbeat and message["type"] == "ping":
                self.send_message(sock, {"type": "pong", "sessionToken": "0" * 64 if self.wrong_session else TOKEN})
                self.send_message(sock, {"type": "ping", "sessionToken": TOKEN})

    def video_loop(self):
        sock = self.accept(1)
        if sock is None:
            return
        attach = self.read_message(sock)
        assert attach == {"type": "video_attach", "sessionToken": TOKEN}
        assert self.paired.wait(1)
        self.send_message(sock, {"type": "video_ready"})
        self.attached.set()
        if self.done.wait(self.video_delay):
            return
        self.send(sock, self.stream)
        self.video_sent.set()
        if self.eos_wait:
            self.done.wait(self.eos_wait)
        if self.video_eof:
            sock.shutdown(socket.SHUT_WR)
        while not self.done.wait(0.02):
            if self.stop_received.is_set():
                return

    def connector(self, address, deadline, check):
        assert address[0] == "192.168.50.2"
        index = 0 if address[1] == receiver.CONTROL_PORT else 1
        return receiver.connect_socket(("127.0.0.1", self.listeners[index].getsockname()[1]), deadline, check)


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / "take"
        self.messages = []

    def receive(self, host, *, timeout=1.5, heartbeat_timeout=0.35):
        instance = receiver.Receiver("192.168.50.2", self.output, timeout,
                                     pin_reader=lambda: PIN, connector=host.connector,
                                     emit=self.messages.append, heartbeat_interval=0.05,
                                     heartbeat_timeout=heartbeat_timeout)
        code = instance.run()
        report = json.loads((self.output / "receiver-report.json").read_text())
        self.assertFalse(instance.control_thread and instance.control_thread.is_alive())
        self.assertIsNone(instance.token)
        serialized = json.dumps(report) + "\n".join(self.messages)
        self.assertNotIn(TOKEN, serialized)
        self.assertNotIn('"pin"', serialized)
        self.assertNotIn("sessionToken", serialized)
        return code, report

    def assert_failure(self, stream, error, **options):
        with FakeHost(stream, **options) as host:
            code, report = self.receive(host)
        self.assertEqual(code, 1)
        self.assertEqual(report["errors"][0]["code"], error)
        self.assertTrue((self.output / "capture.h264.partial").exists())
        self.assertFalse((self.output / "capture.h264").exists())
        return report

    def test_fragmented_control_and_video_complete_eos(self):
        with FakeHost(fragment=True, video_delay=0.18) as host:
            code, report = self.receive(host)
            self.assertTrue(host.stop_received.wait(0.5))
        self.assertEqual(code, 0)
        self.assertEqual((self.output / "capture.h264").read_bytes(), CONFIG + IDR + PFRAME)
        self.assertFalse((self.output / "capture.h264.partial").exists())
        self.assertEqual(report["accessUnits"], 2)
        self.assertEqual(report["keyframes"], 1)
        self.assertEqual(report["keepalivePackets"], 1)
        self.assertEqual(report["sequence"], {"first": 0, "last": 4, "gaps": 0})
        self.assertEqual(report["ptsUs"], {"first": 0, "last": 33333, "regressions": 0})
        self.assertGreater(report["heartbeat"]["pongsReceived"], 0)
        self.assertGreater(report["heartbeat"]["pingsReceived"], 0)
        self.assertEqual(report["gateResult"], "NOT_AUTOMATICALLY_DETERMINED")

    def test_eos_can_carry_final_access_unit(self):
        with FakeHost(complete_stream(eos_payload=True)) as host:
            code, report = self.receive(host)
        self.assertEqual(code, 0)
        self.assertTrue(report["eosReceived"])
        self.assertEqual(report["accessUnits"], 2)

    def test_normal_control_eof_after_complete_eos_is_not_failure(self):
        original_fsync = receiver.os.fsync
        def delayed_fsync(fd):
            time.sleep(0.15)  # Control EOF arrives while the completed file is being finalized.
            original_fsync(fd)
        with FakeHost(control_eof_after_video=True) as host, mock.patch.object(receiver.os, "fsync", delayed_fsync):
            code, report = self.receive(host)
        self.assertEqual(code, 0)
        self.assertTrue(report["eosReceived"])

    def test_auth_rejection_does_not_expose_peer_error(self):
        with FakeHost(reject=True) as host:
            code, report = self.receive(host)
        self.assertEqual(code, 1)
        self.assertEqual(report["errors"][0]["code"], "PAIR_REJECTED")
        self.assertFalse(report["videoReady"])
        self.assertNotIn(PIN, json.dumps(report) + "\n".join(self.messages))

    def test_invalid_headers_and_lengths_rejected_before_payload_read(self):
        cases = [("magic", b"BAD!", "VIDEO_HEADER"), ("version", 2, "VIDEO_HEADER"),
                 ("kind", 9, "VIDEO_HEADER"), ("flags", 4, "VIDEO_HEADER"),
                 ("size", receiver.MAX_PAYLOAD_BYTES + 1, "VIDEO_LENGTH")]
        for key, value, error in cases:
            with self.subTest(key=key):
                self.output = Path(self.temporary.name) / key
                kwargs = {"kind": 2, "sequence": 0, key: value}
                self.assert_failure(packet(**kwargs), error)

    def test_config_limit(self):
        self.assert_failure(packet(1, 0, size=receiver.MAX_CONFIG_BYTES + 1), "VIDEO_LENGTH")

    def test_sequence_gap(self):
        report = self.assert_failure(packet(1, 0, CONFIG) + packet(2, 2, IDR, flags=1), "SEQUENCE")
        self.assertEqual(report["sequence"]["gaps"], 1)

    def test_unexpected_eof_retains_partial(self):
        report = self.assert_failure(packet(1, 0, CONFIG) + packet(2, 1, IDR, flags=1), "EOF")
        self.assertEqual(report["accessUnits"], 1)
        self.assertEqual((self.output / "capture.h264.partial").read_bytes(), CONFIG + IDR)

    def test_short_packet_body_fails(self):
        self.assert_failure(packet(1, 0, CONFIG, size=len(CONFIG) + 1), "EOF")

    def test_short_header_fails(self):
        self.assert_failure(packet(1, 0, CONFIG)[:13], "EOF")

    def test_eos_without_video_fails(self):
        self.assert_failure(packet(1, 0, CONFIG) + packet(2, 1, flags=2), "INCOMPLETE_EOS")

    def test_idr_required_after_config(self):
        self.assert_failure(packet(1, 0, CONFIG) + packet(2, 1, PFRAME), "IDR_REQUIRED")

    def test_access_unit_without_config_fails(self):
        self.assert_failure(packet(2, 0, IDR, flags=1), "VIDEO_CONFIG")

    def test_config_must_include_both_parameter_sets(self):
        self.assert_failure(packet(1, 0, b"\x00\x00\x01\x67\x64"), "VIDEO_CONFIG")

    def test_keyframe_flag_must_match_idr(self):
        self.assert_failure(packet(1, 0, CONFIG) + packet(2, 1, PFRAME, flags=1), "ACCESS_UNIT")

    def test_pts_regression_fails(self):
        self.assert_failure(packet(1, 0, CONFIG) + packet(2, 1, IDR, flags=1, pts=100) + packet(2, 2, PFRAME, pts=99), "PTS_REGRESSION")

    def test_control_worker_eof_interrupts_video_wait(self):
        started = time.monotonic()
        report = self.assert_failure(b"", "EOF", video_eof=False, control_eof=True)
        self.assertLess(time.monotonic() - started, 1.2)
        self.assertFalse(report["eosReceived"])

    def test_wrong_session_interrupts_video_wait(self):
        self.assert_failure(b"", "INVALID_SESSION", video_eof=False, wrong_session=True)

    def test_heartbeat_timeout_interrupts_video_wait(self):
        started = time.monotonic()
        self.assert_failure(b"", "HEARTBEAT_TIMEOUT", video_eof=False, heartbeat=False)
        self.assertLess(time.monotonic() - started, 1.2)

    def test_overall_timeout_despite_live_heartbeat(self):
        with FakeHost(b"", video_eof=False) as host:
            code, report = self.receive(host, timeout=0.3, heartbeat_timeout=1)
        self.assertEqual(code, 1)
        self.assertEqual(report["errors"][0]["code"], "TIMEOUT")

    def test_partial_video_header_has_short_assembly_deadline(self):
        started = time.monotonic()
        with FakeHost(b"H", video_eof=False) as host:
            code, report = self.receive(host, timeout=3, heartbeat_timeout=0.5)
        self.assertEqual(code, 1)
        self.assertEqual(report["errors"][0]["code"], "TIMEOUT")
        self.assertLess(time.monotonic() - started, 2.8)

    def test_file_limit_keeps_only_written_prefix(self):
        with mock.patch.object(receiver, "MAX_FILE_BYTES", len(CONFIG) + len(IDR)):
            report = self.assert_failure(complete_stream(), "FILE_LIMIT")
        self.assertEqual(report["h264Bytes"], len(CONFIG) + len(IDR))

    def test_existing_directory_is_never_modified(self):
        self.output.mkdir()
        sentinel = self.output / "capture.h264"
        sentinel.write_bytes(b"keep")
        instance = receiver.Receiver("192.168.50.2", self.output, pin_reader=lambda: PIN, emit=self.messages.append)
        self.assertEqual(instance.run(), 1)
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_existing_final_file_is_not_overwritten(self):
        with FakeHost(video_delay=0.15) as host:
            def emit(message):
                if "video ready" in message:
                    (self.output / "capture.h264").write_bytes(b"keep")
            instance = receiver.Receiver("192.168.50.2", self.output, 1.5, pin_reader=lambda: PIN,
                                         connector=host.connector, emit=emit)
            self.assertEqual(instance.run(), 1)
        self.assertEqual((self.output / "capture.h264").read_bytes(), b"keep")
        self.assertEqual((self.output / "capture.h264.partial").read_bytes(), CONFIG + IDR + PFRAME)

    def test_control_length_and_malformed_json(self):
        cases = [struct.pack("!I", 4097), struct.pack("!I", 2) + b"\xff\xff",
                 struct.pack("!I", 26) + b'{"type":"a","type":"hello"}']
        for index, data in enumerate(cases):
            with self.subTest(index=index):
                self.output = Path(self.temporary.name) / str(index)
                with FakeHost(bad_json=data) as host:
                    code, report = self.receive(host)
                self.assertEqual(code, 1)
                self.assertIn(report["errors"][0]["code"], ("CONTROL_LENGTH", "CONTROL_JSON"))

    def test_cli_rejects_public_loopback_and_non_ipv4_hosts(self):
        for address in ("127.0.0.1", "8.8.8.8", "169.254.1.2", "::1", "example.com", "192.0.0.8"):
            with self.subTest(address=address), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                receiver.main(["--host", address, "--output", str(self.output)])
        self.assertFalse(self.output.exists())
        for address in ("10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.1.2"):
            self.assertEqual(receiver.validate_host(address), address)

    def test_controlled_stdin_pin_validation(self):
        with mock.patch.object(receiver.sys, "stdin", io.StringIO(PIN + "\n")):
            self.assertEqual(receiver.read_pin(True), PIN)
        with mock.patch.object(receiver.sys, "stdin", io.StringIO("１２３４５６\n")), self.assertRaises(receiver.ReceiverError):
            receiver.read_pin(True)


@unittest.skipUnless(os.environ.get("LAN_SERVER_FIXTURE"), "set LAN_SERVER_FIXTURE to the real C++ server fixture binary")
class RealServerIntegration(unittest.TestCase):
    def test_actual_cpp_server_to_python_receiver(self):
        process = subprocess.Popen([os.environ["LAN_SERVER_FIXTURE"], "--serve-fixture"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def cleanup():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(2)
            process.stdout.close()
            process.stderr.close()
        self.addCleanup(cleanup)
        ready, _, _ = select.select([process.stdout], [], [], 5)
        self.assertTrue(ready, "C++ fixture did not announce listening ports")
        # Random fixture credentials are consumed only in memory, never printed or saved.
        fixture = json.loads(process.stdout.readline())
        ports = {receiver.CONTROL_PORT: fixture["controlPort"], receiver.VIDEO_PORT: fixture["videoPort"]}
        def connector(address, deadline, check):
            return receiver.connect_socket(("127.0.0.1", ports[address[1]]), deadline, check)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "actual-server"
            client = receiver.Receiver("192.168.50.2", output, 10,
                                       pin_reader=lambda: fixture["pin"], connector=connector, emit=lambda _: None)
            self.assertEqual(client.run(), 0, client.report["errors"])
            self.assertEqual(client.report["accessUnits"], 2)
            self.assertEqual(client.report["configPackets"], 1)
            self.assertTrue(client.report["eosReceived"])
            self.assertEqual((output / "capture.h264").read_bytes(), bytes.fromhex(fixture["expectedH264Hex"]))
            report_text = (output / "receiver-report.json").read_text()
            self.assertNotIn('"pin"', report_text)
            self.assertNotIn("sessionToken", report_text)
            self.assertIsNone(client.token)
        self.assertEqual(process.wait(5), 0, "C++ fixture did not finish cleanly after client stop")


if __name__ == "__main__":
    unittest.main()
