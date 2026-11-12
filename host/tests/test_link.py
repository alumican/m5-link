import base64
import json

from usblink.link import CHUNK, PREFIX, Reply, answer, chunks, frame, parse, reply_header


def split(out):
    header, body = out
    msg = parse(reply_header(header, len(body))[:-1])
    assert msg is not None and msg["n"] == len(body) and msg["c"] == CHUNK
    return {k: v for k, v in msg.items() if k != "c"}, body


def test_hello_returns_time():
    msg, body = split(answer({"v": 1, "id": 7, "op": "hello"}, ("localhost",), now=lambda: 1793900000))
    assert msg == {"v": 1, "id": 7, "op": "hello", "t": 1793900000, "s": 200, "n": 0} and body == b""


def test_http_is_fetched_with_body_and_headers():
    seen = []

    def fake(req):
        seen.append(req)
        return Reply(200, b"RIFF....", "audio/wav")

    req = {"v": 1, "id": 3, "op": "http", "m": "POST", "u": "http://localhost:8000/api/hello",
           "h": {"content-type": "text/plain"}, "b": base64.b64encode("It's 18°C.".encode()).decode()}
    msg, body = split(answer(req, ("localhost",), do_fetch=fake))
    assert msg["s"] == 200 and msg["h"]["content-type"] == "audio/wav" and body == b"RIFF...."
    assert seen[0].method == "POST" and seen[0].body == "It's 18°C.".encode()


def test_host_not_allowed():
    msg, body = split(answer({"v": 1, "id": 4, "op": "http", "m": "GET", "u": "https://example.com/"}, ("localhost",)))
    assert msg["s"] == 403 and b"example.com" in body


def test_parse_ignores_logs_and_broken_frames():
    assert parse(b"wifi: ok 192.0.2.10") is None
    assert parse(PREFIX + b"{broken") is None
    assert parse(frame({"id": 1})[:-1]) == {"v": 1, "id": 1, "n": 0}


def test_chunks_cover_the_body():
    body = bytes(range(256)) * 40  # 10240 bytes
    parts = chunks(body)
    assert [len(p) for p in parts] == [4096, 4096, 2048] and b"".join(parts) == body
