import base64
import http.server
import socket
import threading

from m5_link.link import (
    CHUNK,
    PREFIX,
    Reply,
    Request,
    allowed,
    answer,
    chunks,
    fetch,
    frame,
    parse,
    reply_header,
)


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
           "h": {"content-type": "text/plain"}, "b": base64.b64encode("It's 18\u00b0C.".encode()).decode()}
    msg, body = split(answer(req, ("localhost",), do_fetch=fake))
    assert msg["s"] == 200 and msg["h"]["content-type"] == "audio/wav" and body == b"RIFF...."
    assert seen[0].method == "POST" and seen[0].body == "It's 18\u00b0C.".encode()


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


def test_unknown_op():
    msg, body = split(answer({"v": 1, "id": 5, "op": "dance"}, ("localhost",)))
    assert msg["s"] == 400 and msg["op"] == "dance"


def test_allowlist_matches_the_host_only():
    allow = ("localhost", "api.open-meteo.com")
    assert allowed("http://localhost:8000/api", allow)
    assert allowed("https://api.open-meteo.com/v1/forecast?x=1", allow)
    assert allowed("http://LOCALHOST/", allow)
    assert not allowed("http://localhost.example.com/", allow)
    assert not allowed("http://example.com/localhost", allow)
    assert not allowed("http://localhost@example.com/", allow)  # userinfo, not the host
    assert not allowed("http://open-meteo.com/", allow)
    assert not allowed("not a url", allow)


class Server:
    """A local HTTP server answering /ok, /echo (POST), and anything else with 404."""

    def __init__(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _send(self, status, body, content_type):
                self.send_response(status)
                self.send_header("content-type", content_type)
                self.send_header("content-length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/ok":
                    self._send(200, b"fine", "text/plain")
                else:
                    self._send(404, b"missing", "text/plain")

            def do_POST(self):
                body = self.rfile.read(int(self.headers["content-length"]))
                self._send(201, body, self.headers["content-type"])

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_port}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def test_fetch_returns_status_body_and_content_type():
    server = Server()
    try:
        assert fetch(Request("GET", server.url + "/ok")) == Reply(200, b"fine", "text/plain")
        assert fetch(Request("GET", server.url + "/nope")) == Reply(404, b"missing", "text/plain")
        rep = fetch(Request("POST", server.url + "/echo", {"content-type": "application/json"}, b'{"a":1}'))
        assert rep == Reply(201, b'{"a":1}', "application/json")
    finally:
        server.close()


def test_fetch_unreachable_is_status_0():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()  # nothing listens on this port now
    rep = fetch(Request("GET", f"http://127.0.0.1:{port}/"))
    assert rep.status == 0 and rep.content_type == "text/plain" and rep.body
