# CLAUDE.md

m5-link, a transport library for M5Stack (ESP32-S3) covering USB, Wi-Fi, and BLE. The device side is in `arduino/src/` (C++) with `library.json` at the repository root, and the host side is in `host/` (Python). See `README.md` for usage and `TODO.md` for scope and planned work.

## Rules

- Write everything in this repository in English: code, comments, docs, CLI messages, commit messages, issues and pull requests.
- Keep the library generic: transports and protocols only. Anything specific to one application, such as its API endpoints, board lookup tables, or what to type, belongs in that application.
- No display or camera code, and no dependency on UI libraries. Screens for entering Wi-Fi credentials or pairing belong in a separate setup library that calls this one.
- Do not break the UL1 protocol or the 4 KB acks; without acks the ESP32-S3 HWCDC silently drops data.
- Keep the host allowlist, localhost only by default.
- Release with git tags, bumping the version in `library.json` and `host/pyproject.toml`. Applications pin `https://github.com/alumican/m5-link.git#<tag>` in `lib_deps`, and use `symlink://` only while developing against a local checkout.
- Targets: CoreS3, PlatformIO espressif32@6.9.0 (Arduino 2.0.17), C++11, ArduinoJson 7; host on Python 3.11+ with uv. The ESP32-S3 has BLE only, no Bluetooth Classic.
