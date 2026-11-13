# TODO

## Scope

- USB: serial, HID keyboard, USB host and device, HTTP through the host computer
- Wi-Fi: joining saved and fallback networks, HTTP
- BLE: HID keyboard, serial. The ESP32-S3 has BLE only, no Bluetooth Classic
- One API each for HTTP requests, keyboard typing, and serial exchange, whichever transport carries it
- No display or camera code, and no dependency on UI libraries

## Features

### Transport-independent API

- One interface each for HTTP, keyboard, and serial, with USB, Wi-Fi, and BLE implementations
- Let the application pick a transport or try them in order, e.g. Wi-Fi, then USB
- Run the UL1 protocol over Wi-Fi as well as USB; keep the 4 KB acks over USB and decide whether Wi-Fi needs them
- Apply the same host allowlist on every transport

### Composite USB device: serial + keyboard

- Present the device as both CDC serial and HID keyboard, e.g. to a Raspberry Pi or a PC
- Keep the keyboard usable regardless of the serial state
- Keep `UsbLink` and logs working on the serial side

### BLE

- HID keyboard
- Serial, e.g. Nordic UART Service
- Pairing state and bonded peers exposed for the application to show; no pairing screens

### USB host

- Act as a USB host and read/write the USB serial of a connected board such as an Arduino
- Report the connected device's VID/PID
- Decide which USB serial chips to support: CDC-ACM, CH340, CP210x, FTDI

## Open questions

- USB host needs either ESP-IDF `usb_host` directly or Arduino core 3.x; the current target is core 2.0.17 (espressif32@6.9.0)
- Whether to support uploading sketches to a connected board through the USB host
- Whether `WifiLink` should keep more than one saved network

## Order

1. Design the transport-independent API
2. Choose the USB host approach
3. Add the composite USB device and BLE keyboard and serial
4. Add USB host
