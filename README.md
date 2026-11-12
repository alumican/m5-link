# m5-usblink

Lets an ESP32 make HTTP requests through a computer connected over USB serial. Useful where the ESP32 cannot join Wi-Fi, such as venues with 5 GHz networks only. The device can call internet APIs or servers running on the computer, and read the computer's clock.

The same serial port carries `Serial.print` logs. The host prints them and forwards typed input to the device, so it also works as a serial monitor.

- `arduino/`: `UsbLink`, the device library for ESP32, using ArduinoJson and ESP-IDF mbedtls
- `host/`: `usblink`, the host command in Python, using pyserial

## Usage

Device, in `platformio.ini`, with this repository cloned next to your project:

```ini
lib_deps =
  symlink://../m5-usblink/arduino
```

```cpp
#include "UsbLink.h"

UsbLink usb;

void setup() {
  Serial.setRxBufferSize(8192);  // room for reply bodies
  Serial.begin(115200);
  usb.begin(Serial);
}

void loop() {
  usb.poll();           // sends hello every 3 s and reads input
  int c = usb.read();   // use instead of Serial.read(); returns non-frame bytes only
  if (usb.connected()) {
    UsbLink::Response r;
    if (usb.request("GET", "https://api.open-meteo.com/v1/forecast?...", nullptr, 0, nullptr, r)) {
      // r.status, r.body, r.length
      free(r.body);
    }
  }
}
```

Host:

```sh
uv tool install ./m5-usblink/host
usblink --allow api.open-meteo.com
```

- The host only makes requests to `localhost` and to hosts given with `--allow`
- `localhost` on the device refers to the computer
- Opening the port may reset the device; the host reconnects whenever the port reappears

## Protocol v1

Each frame is a single line of JSON prefixed with `0x1E` `UL1 `, which keeps it distinct from log output.

Device to host:

```
0x1E UL1 {"v":1,"id":1,"op":"hello"}\n
0x1E UL1 {"v":1,"id":2,"op":"http","m":"POST","u":"http://localhost:8000/api/hello","h":{"content-type":"text/plain"},"b":"<base64 body>"}\n
0x1E UL1 {"v":1,"id":2,"op":"ack","n":4096}\n
```

Host to device:

```
0x1E UL1 {"v":1,"id":1,"op":"hello","t":1793900000,"s":200,"n":0,"c":4096}\n
0x1E UL1 {"v":1,"id":2,"op":"http","t":1793900000,"s":200,"h":{"content-type":"audio/wav"},"n":514018,"c":4096}\n<n-byte body>
```

- `hello`: sent by the device every 3 s by default. The device counts as connected while replies keep arriving within three intervals. `t` is the host's UNIX time in seconds.
- `http`: `s` is the HTTP status, 0 if the server was unreachable, 403 if the host is not allowed.
- Bodies are sent in `c`-byte chunks. The device acks every `c` bytes and at the end, and the host waits for each ack before sending more. The ESP32-S3 USB serial (arduino-esp32 HWCDC) silently drops data when the reader falls behind.
- The host answers no other frames while a body is in flight, since the reply would corrupt the body.

## Tests

```sh
cd host
uv run --group dev pytest
```

## License

[MIT](LICENSE)
