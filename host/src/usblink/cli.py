"""usblink: performs HTTP requests for a UsbLink device over USB serial, and prints its logs.

    usblink                                   # first /dev/cu.usbmodem*; allows localhost only
    usblink --allow api.open-meteo.com        # allow another host (repeatable)
    usblink --port /dev/cu.usbmodem101

Typed lines are sent to the device on Enter (e.g. dev commands). Ctrl-C quits.
Opening the port may reset the device once.
"""

from __future__ import annotations

import argparse
import glob
import sys
import threading
import time

import serial

from .link import DEFAULT_ALLOW, answer, chunks, parse, reply_header


def find_port() -> str | None:
    return next(iter(sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))), None)


def open_port(name: str) -> serial.Serial:
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = name, 115200, 0.1
    port.dtr = port.rts = False
    port.open()
    return port


def log(msg: str) -> None:
    sys.stderr.write(f"[usblink] {msg}\n")
    sys.stderr.flush()


class Lines:
    """Reads serial input line by line, printing non-frame lines (logs) as-is."""

    def __init__(self, port: serial.Serial):
        self.port = port
        self.buf = b""

    def frame(self, timeout: float | None = None) -> dict | None:
        """Next frame, or None after timeout seconds."""
        deadline = None if timeout is None else time.time() + timeout
        while True:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                start = line.find(b"\x1e")
                if start < 0:
                    sys.stdout.write(line.decode("utf-8", "replace") + "\n")
                    sys.stdout.flush()
                    continue
                if start > 0:  # frame started mid-log-line
                    sys.stdout.write(line[:start].decode("utf-8", "replace"))
                msg = parse(line[start:])
                if msg is None:
                    log("broken frame")
                    continue
                return msg
            if deadline is not None and time.time() > deadline:
                return None
            # Read only what is available: read(4096) blocks until 4096 bytes or timeout, stalling 0.1 s per ack
            self.buf += self.port.read(max(1, self.port.in_waiting))


def send_reply(port: serial.Serial, lines: Lines, header: dict, body: bytes) -> bool:
    """Sends a reply, its body in CHUNK-byte pieces, waiting for the device's ack after each."""
    port.write(reply_header(header, len(body)))
    sent = 0
    for part in chunks(body):
        port.write(part)
        port.flush()
        sent += len(part)
        while True:
            msg = lines.frame(timeout=5)
            if msg is None:
                log(f"no ack after {sent} bytes")
                return False
            if msg.get("op") == "ack" and msg.get("id") == header["id"]:
                break
            # Do not answer other frames (e.g. hello) mid-body: the device would read the reply as body bytes
    return True


def serve(port: serial.Serial, allow: tuple[str, ...], quiet_hello: bool = True) -> None:
    lines = Lines(port)
    while True:
        msg = lines.frame()
        if msg.get("op") == "ack":
            continue
        t0 = time.time()
        header, body = answer(msg, allow)
        ok = send_reply(port, lines, header, body)
        if msg.get("op") != "hello" or not quiet_hello:
            took = time.time() - t0
            log(f"{msg.get('m', '')} {msg.get('u', msg.get('op'))} -> {header.get('s')} {len(body)} bytes in {took:.2f}s"
                + ("" if ok else " (not delivered)"))


def keyboard(port: serial.Serial) -> None:
    for line in sys.stdin:
        port.write(line.rstrip("\n").encode())


def main() -> None:
    p = argparse.ArgumentParser(prog="usblink", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem* or /dev/ttyACM*)")
    p.add_argument("--allow", action="append", default=[], help="host to allow HTTP requests to (localhost is always allowed)")
    p.add_argument("--show-hello", action="store_true", help="also log hello frames")
    args = p.parse_args()

    name = args.port or find_port()
    if not name:
        sys.exit("no device found (connect it over USB and check /dev/cu.usbmodem*)")
    allow = tuple(DEFAULT_ALLOW) + tuple(args.allow)
    log(f"{name}, allow: {', '.join(allow)}")
    while True:
        try:
            port = open_port(name)
            threading.Thread(target=keyboard, args=(port,), daemon=True).start()
            serve(port, allow, quiet_hello=not args.show_hello)
        except KeyboardInterrupt:
            return
        except (serial.SerialException, OSError) as e:  # the port disappears while the device resets
            log(f"port lost ({e}). reconnecting...")
            time.sleep(1)
