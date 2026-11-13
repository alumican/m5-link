# m5-link

Transport library for M5Stack (ESP32-S3). The goal is one way to make HTTP requests, type as a keyboard, and exchange serial data, whichever transport is available: USB, Wi-Fi, or BLE. See [TODO.md](TODO.md) for what is not built yet.

Available now:

- `UsbLink`: HTTP requests through a computer connected over USB serial, for places where the ESP32 cannot join Wi-Fi, such as venues with 5 GHz networks only. Also provides the computer's clock. `Serial.print` logs share the same port, and the host prints them and forwards typed input, so it doubles as a serial monitor.
- `WifiLink`: joins Wi-Fi, trying the network saved in NVS first and then fallbacks supplied by the application.

The library has no display or camera code. Screens for entering Wi-Fi credentials or pairing belong in the application or a separate library, which passes the result to `WifiLink::save()`.

- `arduino/src/`: device library, using ArduinoJson, ESP-IDF mbedtls, and the ESP32 Arduino core
- `host/`: `m5-link`, the host command in Python, using pyserial

## Install

Device, in `platformio.ini`:

```ini
lib_deps =
  https://github.com/alumican/m5-link.git#v0.2.0
```

Host:

```sh
uv tool install "git+https://github.com/alumican/m5-link@v0.2.0#subdirectory=host"
```

## UsbLink

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

On the computer:

```sh
m5-link --allow api.open-meteo.com
```

- The host only makes requests to `localhost` and to hosts given with `--allow`
- `localhost` on the device refers to the computer
- Opening the port may reset the device; the host reconnects whenever the port reappears

## WifiLink

```cpp
#include "WifiLink.h"

WifiLink wifi("myapp-wifi");  // NVS namespace, up to 15 characters

void setup() {
  const WifiLink::Network spare[] = {{"backup-ssid", "backup-password"}};
  if (wifi.connect(spare, 1)) {
    // wifi.ssid() is the network that worked
  }
}

// After the user enters credentials:
//   wifi.save(ssid, password);
//   wifi.connect();
```

| Method | |
|---|---|
| `bool saved(Network& out) const` | Saved network, false if none |
| `bool save(const String& ssid, const String& password)` | Replaces the saved network; empty password for open networks |
| `void forget()` | Erases the saved network |
| `bool connect(const Network* fallbacks = nullptr, size_t count = 0, uint32_t timeoutMs = 10000)` | Tries the saved network, then each fallback, skipping empty and repeated SSIDs |
| `bool connect(const Network& network, uint32_t timeoutMs = 10000)` | Joins one network |
| `void disconnect()` | |
| `bool connected() const` | |
| `const String& ssid() const` | Network of the last successful connect |

## UsbLink protocol v1

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
