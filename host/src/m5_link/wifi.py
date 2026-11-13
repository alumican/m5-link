"""WifiSerial client: logs and typed input over TCP, described in README.md.

The device listens; the host connects and authenticates with HMAC-SHA256 over a nonce, in UL1-style frames:
    device: 0x1E "UL1 " {"v":1,"op":"auth","nonce":"<32 hex>"}\\n
    host:   0x1E "UL1 " {"v":1,"op":"auth","mac":"<hex HMAC-SHA256(password, nonce)>"}\\n
    device: 0x1E "UL1 " {"v":1,"op":"auth","s":200}\\n   (401 and close if wrong)
After that, bytes flow both ways unchanged.
"""

from __future__ import annotations

import codecs
import hashlib
import hmac
import json
import socket
from typing import TextIO

from .link import PREFIX, VERSION, parse

DEFAULT_PORT = 2323
TIMEOUT = 5


class AuthError(Exception):
    """The device rejected the password."""


def auth_mac(password: str, nonce: str) -> str:
    return hmac.new(password.encode(), nonce.encode(), hashlib.sha256).hexdigest()


def auth_frame(body: dict) -> bytes:
    return PREFIX + json.dumps({"v": VERSION, "op": "auth", **body}, separators=(",", ":")).encode() + b"\n"


def parse_target(target: str) -> tuple[str, int]:
    """'host', 'host:port' or '[v6]:port' -> (host, port)."""
    if target.startswith("["):
        host, _, rest = target[1:].partition("]")
        return host, int(rest[1:]) if rest.startswith(":") else DEFAULT_PORT
    if target.count(":") == 1:
        host, port = target.split(":")
        return host, int(port)
    return target, DEFAULT_PORT


def keep_alive(sock: socket.socket) -> None:
    """Notices a device that reset or left the network within a few seconds, even when it is silent."""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    idle = getattr(socket, "TCP_KEEPIDLE", getattr(socket, "TCP_KEEPALIVE", None))  # macOS calls it TCP_KEEPALIVE
    for opt, value in ((idle, 2), (getattr(socket, "TCP_KEEPINTVL", None), 1), (getattr(socket, "TCP_KEEPCNT", None), 3)):
        if opt is not None:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, opt, value)
            except OSError:
                pass


def _line(sock: socket.socket, buf: bytearray) -> bytes:
    while b"\n" not in buf:
        data = sock.recv(256)
        if not data:
            raise ConnectionError("closed during authentication")
        buf += data
    i = buf.index(b"\n")
    line = bytes(buf[:i])
    del buf[: i + 1]
    return line


def connect(host: str, port: int, password: str, timeout: float = TIMEOUT) -> tuple[socket.socket, bytes]:
    """Connects and authenticates. Returns the socket and any bytes received after the handshake.

    Raises AuthError if the password is wrong, OSError if the device cannot be reached.
    """
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        keep_alive(sock)
        buf = bytearray()
        challenge = parse(_line(sock, buf))
        if not challenge or challenge.get("op") != "auth" or not challenge.get("nonce"):
            raise ConnectionError("not a WifiSerial device")
        sock.sendall(auth_frame({"mac": auth_mac(password, challenge["nonce"])}))
        result = parse(_line(sock, buf))
        if not result or result.get("s") != 200:
            raise AuthError("wrong password")
        sock.settimeout(None)
        return sock, bytes(buf)
    except BaseException:
        sock.close()
        raise


def pump(sock: socket.socket, first: bytes, out: TextIO) -> None:
    """Writes everything the device sends to out as UTF-8 text until the connection closes."""
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    data = first
    while True:
        if data:
            out.write(decoder.decode(data))
            out.flush()
        data = sock.recv(4096)
        if not data:
            return
