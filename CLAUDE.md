# CLAUDE.md

USB communication library for M5Stack (ESP32-S3), with the device side in `arduino/` (C++) and the host side in `host/` (Python). See `README.md` for usage and `TODO.md` for planned work.

## Rules

- Keep the library generic: framing, serial, keyboard, USB host/device. Anything specific to one application, such as its API endpoints, board lookup tables, or what to type, belongs in that application.
- Do not break the UL1 protocol or the 4 KB acks; without acks the ESP32-S3 HWCDC silently drops data.
- Keep the host allowlist, localhost only by default.
- Release with git tags. Applications pin the git URL and tag in `lib_deps`, and use `symlink://` only while developing against a local checkout.
- Targets: CoreS3, PlatformIO espressif32@6.9.0 (Arduino 2.0.17), C++11, ArduinoJson 7; host on Python 3.11+ with uv.
