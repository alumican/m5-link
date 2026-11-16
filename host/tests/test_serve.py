"""The USB side of m5-link: splitting logs from frames, and sending reply bodies chunk by chunk with acks."""

import serial

from m5_link import cli
from m5_link.link import CHUNK, PREFIX, frame, parse


class FakePort:
    """A serial port fed with device output. Each write may queue more device output via on_write.

    Once the input runs out, read() raises SerialException, as when the device resets, so serve() returns.
    """

    def __init__(self, data: bytes = b"", on_write=None):
        self.data = bytearray(data)
        self.written: list[bytes] = []
        self.on_write = on_write
        self.end_when_empty = True

    @property
    def in_waiting(self) -> int:
        return len(self.data)

    def read(self, n: int = 1) -> bytes:
        if not self.data:
            if self.end_when_empty:
                raise serial.SerialException("gone")
            return b""
        out = bytes(self.data[:n])
        del self.data[:n]
        return out

    def write(self, data: bytes) -> int:
        self.written.append(bytes(data))
        if self.on_write:
            self.on_write(self, bytes(data))
        return len(data)

    def flush(self) -> None:
        pass


class FakeTime:
    """Stands in for the time module in cli, advancing 1 s per call so timeouts pass instantly."""

    def __init__(self):
        self.now = 0.0

    def time(self) -> float:
        self.now += 1
        return self.now


def ack(id_: int, n: int) -> bytes:
    return frame({"id": id_, "op": "ack", "n": n})


def test_logs_are_printed_and_frames_returned(capsys):
    port = FakePort(b"boot\nwifi: " + frame({"id": 1, "op": "hello"}) + b"18\xc2\xb0C\n")
    lines = cli.Lines(port)
    assert lines.frame() == {"v": 1, "id": 1, "op": "hello", "n": 0}
    port.data += b"x"  # so the next read does not end the port
    port.end_when_empty = False
    assert lines.frame(timeout=0) is None
    assert capsys.readouterr().out == "boot\nwifi: 18°C\n"


def test_broken_frames_are_skipped(capsys):
    port = FakePort(PREFIX + b"{broken\n" + frame({"id": 2, "op": "hello"}))
    assert cli.Lines(port).frame() == {"v": 1, "id": 2, "op": "hello", "n": 0}
    assert "broken frame" in capsys.readouterr().err


def test_frame_returns_none_after_timeout(monkeypatch):
    monkeypatch.setattr(cli, "time", FakeTime())
    port = FakePort(b"partial log without newline")
    port.end_when_empty = False
    assert cli.Lines(port).frame(timeout=5) is None


def test_body_is_sent_one_chunk_per_ack():
    body = bytes(range(256)) * 40  # 10240 bytes: 4096 + 4096 + 2048
    sent = []

    def device(port, data):
        if data.startswith(PREFIX):
            return
        sent.append(len(data))
        # Before the ack, a hello arrives: it must not be answered mid-body
        port.data += frame({"id": 50, "op": "hello"}) + ack(9, sum(sent))

    port = FakePort(on_write=device)
    assert cli.send_reply(port, cli.Lines(port), {"id": 9, "op": "http", "s": 200}, body)
    header = parse(port.written[0][:-1])
    assert header["n"] == len(body) and header["c"] == CHUNK
    assert sent == [4096, 4096, 2048]
    assert b"".join(port.written[1:]) == body  # nothing but body bytes after the header


def test_acks_for_another_id_do_not_count():
    def device(port, data):
        if not data.startswith(PREFIX):
            port.data += ack(8, len(data)) + ack(9, len(data))

    port = FakePort(on_write=device)
    assert cli.send_reply(port, cli.Lines(port), {"id": 9, "op": "http", "s": 200}, b"x" * 10)
    assert port.data == b""  # the id 9 ack was read too


def test_missing_ack_stops_sending(monkeypatch, capsys):
    monkeypatch.setattr(cli, "time", FakeTime())
    port = FakePort()
    port.end_when_empty = False
    assert not cli.send_reply(port, cli.Lines(port), {"id": 9, "op": "http", "s": 200}, b"x" * (CHUNK * 3))
    assert len(port.written) == 2  # header and the first chunk only
    assert "no ack after 4096 bytes" in capsys.readouterr().err


def test_empty_body_needs_no_ack():
    port = FakePort()
    assert cli.send_reply(port, cli.Lines(port), {"id": 1, "op": "hello", "s": 200}, b"")
    assert len(port.written) == 1


def test_serve_answers_hello_and_refuses_hosts_not_allowed():
    def device(port, data):
        if not data.startswith(PREFIX):
            port.data += ack(2, len(data))

    port = FakePort(
        frame({"id": 1, "op": "hello"}) + frame({"id": 2, "op": "http", "m": "GET", "u": "https://example.com/"}),
        on_write=device,
    )
    try:
        cli.serve(port, ("localhost",))
    except serial.SerialException:
        pass  # the fake port ran out of input
    headers = [parse(w[:-1]) for w in port.written if w.startswith(PREFIX)]
    assert [(h["id"], h["op"], h["s"]) for h in headers] == [(1, "hello", 200), (2, "http", 403)]
    assert b"example.com" in port.written[-1]
