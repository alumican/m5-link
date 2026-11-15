"""m5-link: prints a device's logs and sends it typed input, over USB serial or Wi-Fi.
Over USB it also performs HTTP requests for UsbLink.

    m5-link                                   # first /dev/cu.usbmodem*; allows localhost only
    m5-link --allow api.open-meteo.com        # allow another host (repeatable)
    m5-link --port /dev/cu.usbmodem101
    m5-link --wifi myapp.local                # WifiSerial on port 2323; or HOST:PORT

Typed lines are sent to the device on Enter (e.g. dev commands); lines typed while not connected
are sent once connected. Ctrl-C quits.
Opening the USB port may reset the device once.
Over Wi-Fi the password is read from M5LINK_PASSWORD, or asked for.
"""

from __future__ import annotations

import argparse
import getpass
import glob
import os
import sys
import threading
import time

import serial

from . import wifi
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
    sys.stderr.write(f"[m5-link] {msg}\n")
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


class Keyboard:
    """Sends typed lines, without the newline, to whichever connection is current.

    Lines typed while there is none (before authentication, while the device resets) are queued and sent,
    in order, as soon as the next connection is attached.
    """

    ERRORS = (OSError, serial.SerialException)

    def __init__(self, lines=None) -> None:
        self._send = None
        self._queue: list[bytes] = []
        self._lock = threading.Lock()
        threading.Thread(target=self._run, args=(sys.stdin if lines is None else lines,), daemon=True).start()

    def _run(self, lines) -> None:
        for line in lines:
            self.type(line)

    def type(self, line: str) -> None:
        data = line.rstrip("\n").encode()
        with self._lock:
            if self._send is not None:
                try:
                    self._send(data)
                    return
                except self.ERRORS:
                    self._send = None
            self._queue.append(data)
        log(f"not connected; will send on connect: {data.decode(errors='replace')!r}")

    def attach(self, send) -> None:
        """Sends the queued lines, then routes new ones to send."""
        with self._lock:
            while self._queue:
                try:
                    send(self._queue[0])
                except self.ERRORS:
                    return  # connection already gone; keep the rest queued
                self._queue.pop(0)
                log("sent a queued line")
            self._send = send

    def detach(self) -> None:
        with self._lock:
            self._send = None


def run_wifi(target: str, password: str) -> None:
    host, port = wifi.parse_target(target)
    keys = Keyboard()
    waiting = False
    while True:
        try:
            sock, first = wifi.connect(host, port, password)
        except wifi.AuthError:
            sys.exit(f"{host}:{port} rejected the password (M5LINK_PASSWORD)")
        except OSError as e:  # not on the network yet, or resetting
            if not waiting:
                log(f"waiting for {host}:{port} ({e})")
                waiting = True
            time.sleep(1)
            continue
        waiting = False
        log(f"connected to {host}:{port}")
        keys.attach(sock.sendall)
        try:
            wifi.pump(sock, first, sys.stdout)
        except OSError:
            pass
        finally:
            keys.detach()
            sock.close()
        log("connection lost. reconnecting...")


def wifi_password() -> str:
    password = os.environ.get("M5LINK_PASSWORD", "")
    if not password and sys.stdin.isatty():
        password = getpass.getpass("WifiSerial password: ")
    if not password:
        sys.exit("set M5LINK_PASSWORD to the device's WifiSerial password")
    return password


def main() -> None:
    p = argparse.ArgumentParser(prog="m5-link", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem* or /dev/ttyACM*)")
    p.add_argument("--allow", action="append", default=[], help="host to allow HTTP requests to (localhost is always allowed)")
    p.add_argument("--show-hello", action="store_true", help="also log hello frames")
    p.add_argument("--wifi", metavar="HOST[:PORT]", help=f"connect to WifiSerial instead of USB (port {wifi.DEFAULT_PORT} by default)")
    args = p.parse_args()

    if args.wifi:
        password = wifi_password()
        try:
            run_wifi(args.wifi, password)
        except KeyboardInterrupt:
            pass
        return

    name = args.port or find_port()
    if not name:
        sys.exit("no device found (connect it over USB and check /dev/cu.usbmodem*)")
    allow = tuple(DEFAULT_ALLOW) + tuple(args.allow)
    log(f"{name}, allow: {', '.join(allow)}")
    keys = Keyboard()
    while True:
        try:
            port = open_port(name)
            keys.attach(port.write)
            serve(port, allow, quiet_hello=not args.show_hello)
        except KeyboardInterrupt:
            return
        except (serial.SerialException, OSError) as e:  # the port disappears while the device resets
            keys.detach()
            log(f"port lost ({e}). reconnecting...")
            time.sleep(1)
