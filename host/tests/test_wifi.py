import io
import json
import socket
import threading

import pytest

from m5_link import wifi
from m5_link.link import PREFIX, parse

NONCE = "0123456789abcdef0123456789abcdef"


def test_auth_mac_matches_openssl():
    # printf <nonce> | openssl dgst -sha256 -hmac 'correct horse'
    assert wifi.auth_mac("correct horse", NONCE) == "3b92b707a556200102ffbc086bef881ae4046d69262c014dbc1233b27b363fdf"


def test_parse_target():
    assert wifi.parse_target("myapp.local") == ("myapp.local", wifi.DEFAULT_PORT)
    assert wifi.parse_target("192.0.2.10:4000") == ("192.0.2.10", 4000)
    assert wifi.parse_target("[fe80::1]:4000") == ("fe80::1", 4000)
    assert wifi.parse_target("fe80::1") == ("fe80::1", wifi.DEFAULT_PORT)


class FakeDevice:
    """Behaves like WifiSerial: sends a nonce, checks the HMAC, then sends logs and records input."""

    def __init__(self, password: str, logs: bytes = b""):
        self.password, self.logs = password, logs
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.got = b""
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _send(self, conn, body):
        conn.sendall(PREFIX + json.dumps({"v": 1, "op": "auth", **body}).encode() + b"\n")

    def _run(self):
        conn, _ = self.server.accept()
        with conn:
            self._send(conn, {"nonce": NONCE})
            line = b""
            while not line.endswith(b"\n"):
                line += conn.recv(1)
            msg = parse(line[:-1])
            if msg.get("mac") != wifi.auth_mac(self.password, NONCE):
                self._send(conn, {"s": 401})
                return
            self._send(conn, {"s": 200})
            conn.sendall(self.logs)
            conn.settimeout(2)
            while not self.got.endswith(b"!"):
                data = conn.recv(64)
                if not data:
                    break
                self.got += data
        self.server.close()


def test_connect_streams_logs_and_sends_input():
    dev = FakeDevice("pw", logs="boot\n18°C\npartial".encode())
    sock, first = wifi.connect("127.0.0.1", dev.port, "pw")
    sock.sendall(b"r!")
    out = io.StringIO()
    wifi.pump(sock, first, out)  # returns when the device closes
    sock.close()
    dev.thread.join(2)
    assert out.getvalue() == "boot\n18°C\npartial"
    assert dev.got == b"r!"


def test_wrong_password_is_rejected():
    dev = FakeDevice("pw")
    with pytest.raises(wifi.AuthError):
        wifi.connect("127.0.0.1", dev.port, "nope")
    dev.thread.join(2)


def test_not_a_device():
    server = socket.create_server(("127.0.0.1", 0))

    def run():
        conn, _ = server.accept()
        conn.sendall(b"SSH-2.0-OpenSSH\n")
        conn.close()

    threading.Thread(target=run, daemon=True).start()
    with pytest.raises(ConnectionError):
        wifi.connect("127.0.0.1", server.getsockname()[1], "pw")
    server.close()


class Chunks:
    """A socket whose recv() returns the given pieces, then b"" (closed)."""

    def __init__(self, *pieces: bytes):
        self.pieces = list(pieces)

    def recv(self, n: int) -> bytes:
        return self.pieces.pop(0) if self.pieces else b""


def test_pump_joins_utf8_split_across_reads():
    text = "18°C 晴れ\n".encode()
    deg = text.index("°".encode())
    out = io.StringIO()
    wifi.pump(Chunks(text[: deg + 1], text[deg + 1 : deg + 6], text[deg + 6 :]), text[:0], out)
    assert out.getvalue() == "18°C 晴れ\n"


def test_pump_replaces_invalid_bytes():
    out = io.StringIO()
    wifi.pump(Chunks(b"ok\xff\n"), b"", out)
    assert out.getvalue() == "ok�\n"
