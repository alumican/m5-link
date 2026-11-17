# TODO

## Scope

- USB: serial, HID keyboard (`UsbDevice`), USB host (`UsbHost`), HTTP through the host computer
- Wi-Fi: joining saved and fallback networks, HTTP, serial (`WifiSerial`)
- BLE: HID keyboard, serial. The ESP32-S3 has BLE only, no Bluetooth Classic
- One API each for HTTP requests, keyboard typing, and serial exchange, whichever transport carries it
- No display or camera code, and no dependency on UI libraries

## Features

### Transport-independent API

- One interface each for HTTP, keyboard, and serial, with USB, Wi-Fi, and BLE implementations
- Let the application pick a transport or try them in order, e.g. Wi-Fi, then USB
- Run the UL1 protocol over Wi-Fi as well as USB; keep the 4 KB acks over USB and decide whether Wi-Fi needs them
- Apply the same host allowlist on every transport

### UsbLink

- Unity tests in `tests/` pass on a CoreS3 (v0.4.1): `UsbLink` over an in-memory `Stream` playing the host, including acks every 4096 bytes and overlong frames. The USB serial between the device and `m5-link` is not part of them

### WifiSerial

- Checked on a CoreS3 alongside ArduinoOTA with the same hostname: found over mDNS, authentication, logs, typed input, and reconnecting with logs continuing after an OTA upload and reboot
- Not yet checked on a device: reconnecting after a Wi-Fi drop, the listener surviving a Wi-Fi reconnect, and output under load (`dropped()`)
- Decide whether to keep recent output while no computer is connected, so boot logs show up on connect
- Discover devices from `m5-link` itself (`_m5link._tcp`) instead of naming one with `--wifi`

### Composite USB device: serial + keyboard

- `UsbDevice` built for CoreS3 with espressif32@6.9.0 (`ARDUINO_USB_MODE=0`, with `ARDUINO_USB_CDC_ON_BOOT` 0 and 1). Layout tables tested on a computer only
- Not yet checked on a device: enumeration on a Raspberry Pi as `/dev/ttyACM0` plus a keyboard, typing in the US and JP layouts, `mounted()` when unplugged and replugged (no VBUS sensing, so it relies on suspend), CDC JSON lines alongside typing, `UsbLink` over `serial()`, and the 1200-baud reboot as an upload method
- Keyboard LED state (Caps Lock) for the application
- Share `KeyLayout` with a BLE keyboard

### BLE

- HID keyboard
- Serial, e.g. Nordic UART Service
- Pairing state and bonded peers exposed for the application to show; no pairing screens

### USB host

- `UsbHost` built for CoreS3 with espressif32@6.9.0, alone and in one firmware with `UsbDevice`, using ESP-IDF 4.4 `usb_host` and its own CDC-ACM driver. Example in `examples/usb_host_serial`
- Not yet checked on a device: `usb_host` getting the USB-OTG port when `UsbDevice` is linked but not started, attaching an Arduino Uno R3 (2341:0043) and reading its strings, baud rate and DTR reset, data both ways, unplugging and replugging, and a device plugged in before `begin()`
- USB serial chips besides CDC-ACM: CH340, CP210x, FTDI
- Uploading sketches to an attached Arduino (STK500 over `UsbHostSerial`)
- Data bits, parity and stop bits other than 8N1; serial state notifications
- A device that never returns its transfers after unplugging keeps the next one from opening
- Not yet checked on a device: `port()`, `onConnect`/`onDisconnect`, and `onEnumerationFailed` with the stage taken from the library's log, e.g. with an iPod, which was reported as never attaching
- Configuration descriptors over 256 bytes cannot be read with the prebuilt ESP-IDF 4.4; would need a rebuilt SDK or core 3.x

## Open questions

- Whether to support uploading sketches to a connected board through the USB host
- Whether `WifiLink` should keep more than one saved network

## Order

1. Design the transport-independent API
2. Add BLE keyboard and serial
3. Extend USB host: more serial chips, sketch uploads
