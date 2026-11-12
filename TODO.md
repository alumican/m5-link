# TODO

## Features

### Same API over USB and Wi-Fi

- Run `UsbLink` (UL1 frames, `hello`, `http`) over either USB serial or Wi-Fi, for setups where the USB port is taken by another device
- Keep the 4 KB acks over USB; decide whether Wi-Fi needs them
- Apply the same host allowlist on both transports

### Composite USB device: serial + keyboard

- Present the device as both CDC serial and HID keyboard, e.g. to a Raspberry Pi or a PC
- Keep the keyboard usable regardless of the serial state
- Keep `UsbLink` and logs working on the serial side

### USB host

- Act as a USB host and read/write the USB serial of a connected board such as an Arduino
- Report the connected device's VID/PID
- Decide which USB serial chips to support: CDC-ACM, CH340, CP210x, FTDI

## Open questions

- USB host needs either ESP-IDF `usb_host` directly or Arduino core 3.x; the current target is core 2.0.17 (espressif32@6.9.0)
- `library.json` lives in `arduino/`; move it to the repository root so PlatformIO can install from the git URL, or keep requiring the `arduino/` path
- Whether to support uploading sketches to a connected board through the USB host

## Order

1. Settle the `library.json` location and confirm installing `v0.1.0` by git URL and tag
2. Choose the USB host approach
3. Design the shared USB/Wi-Fi layer
4. Add the composite device, then USB host
