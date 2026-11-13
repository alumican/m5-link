"""UL1 protocol, described in README.md, and the HTTP requests made for the device.

Device frames: 0x1E "UL1 " + JSON + "\\n"
    {"v":1,"id":1,"op":"hello"}
    {"v":1,"id":2,"op":"http","m":"GET","u":"https://...","h":{"content-type":"..."},"b":"<base64>"}
Host replies: 0x1E "UL1 " + JSON + "\\n", followed by an n-byte body sent in c-byte chunks
    {"v":1,"id":1,"op":"hello","s":200,"t":<UNIX time>,"n":0,"c":4096}
    {"v":1,"id":2,"op":"http","s":200,"t":<UNIX time>,"n":1234,"c":4096,"h":{"content-type":"..."}}
The device sends {"v":1,"id":2,"op":"ack","n":<total received>} after every c bytes and at the end, and the
host waits for each ack, because the ESP32-S3 USB serial (arduino-esp32 HWCDC) silently drops data otherwise.
"""

from __future__ import annotations

import base64
import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from urllib.parse import urlparse

PREFIX = b"\x1eUL1 "
VERSION = 1
DEFAULT_ALLOW = ("localhost", "127.0.0.1")
TIMEOUT = 20
CHUNK = 4096


@dataclass
class Request:
    method: str
    url: str
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""


@dataclass
class Reply:
    status: int
    body: bytes = b""
    content_type: str = ""


Fetch = Callable[[Request], Reply]


def fetch(req: Request) -> Reply:
    """Performs the request with urllib. Status is 0 if the server is unreachable."""
    r = urllib.request.Request(req.url, data=req.body or None, method=req.method, headers=req.headers)
    try:
        with urllib.request.urlopen(r, timeout=TIMEOUT) as res:
            return Reply(res.status, res.read(), res.headers.get("content-type", ""))
    except urllib.error.HTTPError as e:
        return Reply(e.code, e.read(), e.headers.get("content-type", ""))
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return Reply(0, str(e).encode(), "text/plain")


def frame(header: dict, body: bytes = b"") -> bytes:
    """A frame line followed by its body. Reply bodies are sent with chunks() instead."""
    header = {"v": VERSION, **header, "n": len(body)}
    return PREFIX + json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode() + b"\n" + body


def reply_header(header: dict, n: int) -> bytes:
    """Header line of a reply whose n-byte body is sent in CHUNK-byte pieces, each awaiting an ack."""
    header = {"v": VERSION, **header, "n": n, "c": CHUNK}
    return PREFIX + json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"


def chunks(body: bytes) -> list[bytes]:
    return [body[i : i + CHUNK] for i in range(0, len(body), CHUNK)]


def parse(line: bytes) -> dict | None:
    """Returns the JSON of a line starting with 0x1E "UL1 ", or None if it is not a valid frame."""
    if not line.startswith(PREFIX):
        return None
    try:
        return json.loads(line[len(PREFIX) :].decode())
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def allowed(url: str, allow: tuple[str, ...]) -> bool:
    host = urlparse(url).hostname or ""
    return host in allow


def answer(
    msg: dict, allow: tuple[str, ...], do_fetch: Fetch = fetch, now: Callable[[], float] = time.time
) -> tuple[dict, bytes]:
    """Reply (header, body) to a device frame."""
    base = {"id": msg.get("id", 0), "op": msg.get("op", ""), "t": int(now())}
    if msg.get("op") == "hello":
        return {**base, "s": 200}, b""
    if msg.get("op") != "http":
        return {**base, "s": 400}, b"unknown op"
    url = msg.get("u", "")
    if not allowed(url, allow):
        return {**base, "s": 403}, f"not allowed: {urlparse(url).hostname} (m5-link --allow)".encode()
    req = Request(
        method=msg.get("m", "GET"),
        url=url,
        headers=dict(msg.get("h") or {}),
        body=base64.b64decode(msg["b"]) if msg.get("b") else b"",
    )
    rep = do_fetch(req)
    return {**base, "s": rep.status, "h": {"content-type": rep.content_type}}, rep.body
