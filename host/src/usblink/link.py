"""usblink の取り決め（README.md）と、頼まれた HTTP を代わりにすること。

マイコンからのフレーム: 0x1E "UL1 " + JSON + "\\n"
    {"v":1,"id":1,"op":"hello"}
    {"v":1,"id":2,"op":"http","m":"GET","u":"https://...","h":{"content-type":"..."},"b":"<base64>"}
パソコンからの返事: 0x1E "UL1 " + JSON + "\\n" のあとに本文 n バイトを c バイトずつ
    {"v":1,"id":1,"op":"hello","s":200,"t":<UNIX 時刻>,"n":0,"c":4096}
    {"v":1,"id":2,"op":"http","s":200,"t":<UNIX 時刻>,"n":1234,"c":4096,"h":{"content-type":"..."}}
マイコンは c バイト受け取るごと（と最後）に {"v":1,"id":2,"op":"ack","n":<受け取った合計>} を返し、
パソコンはそれを待ってから次を送る。ESP32-S3 の USB シリアルは、受け取りが追いつかないと黙って捨てるので
（arduino-esp32 の HWCDC。2026-11-05、514KB の WAV が届かなかった）。
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
    """urllib で HTTP をする。つながらなければ status 0。"""
    r = urllib.request.Request(req.url, data=req.body or None, method=req.method, headers=req.headers)
    try:
        with urllib.request.urlopen(r, timeout=TIMEOUT) as res:
            return Reply(res.status, res.read(), res.headers.get("content-type", ""))
    except urllib.error.HTTPError as e:
        return Reply(e.code, e.read(), e.headers.get("content-type", ""))
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return Reply(0, str(e).encode(), "text/plain")


def frame(header: dict, body: bytes = b"") -> bytes:
    """一行のフレーム（と、続けて送る本文）。返事の本文は chunks() で分けて送る。"""
    header = {"v": VERSION, **header, "n": len(body)}
    return PREFIX + json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode() + b"\n" + body


def reply_header(header: dict, n: int) -> bytes:
    """返事の一行目。本文は n バイトを CHUNK ずつ送り、そのたびに ack を待つ。"""
    header = {"v": VERSION, **header, "n": n, "c": CHUNK}
    return PREFIX + json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"


def chunks(body: bytes) -> list[bytes]:
    return [body[i : i + CHUNK] for i in range(0, len(body), CHUNK)]


def parse(line: bytes) -> dict | None:
    """0x1E "UL1 " で始まる一行なら JSON を返す。壊れていれば None。"""
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
    """マイコンからのフレームに返すもの（返事の見出しと本文）。"""
    base = {"id": msg.get("id", 0), "op": msg.get("op", ""), "t": int(now())}
    if msg.get("op") == "hello":
        return {**base, "s": 200}, b""
    if msg.get("op") != "http":
        return {**base, "s": 400}, b"unknown op"
    url = msg.get("u", "")
    if not allowed(url, allow):
        return {**base, "s": 403}, f"not allowed: {urlparse(url).hostname} (usblink --allow)".encode()
    req = Request(
        method=msg.get("m", "GET"),
        url=url,
        headers=dict(msg.get("h") or {}),
        body=base64.b64decode(msg["b"]) if msg.get("b") else b"",
    )
    rep = do_fetch(req)
    return {**base, "s": rep.status, "h": {"content-type": rep.content_type}}, rep.body
