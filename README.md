# m5-link

Transport library for M5Stack (ESP32-S3). The goal is one way to make HTTP requests, type as a keyboard, and exchange serial data, whichever transport is available: USB, Wi-Fi, or BLE. See [TODO.md](TODO.md) for what is not built yet.

Available now:

- `UsbLink`: HTTP requests through a computer connected over USB serial, for places where the ESP32 cannot join Wi-Fi, such as venues with 5 GHz networks only. Also provides the computer's clock. `Serial.print` logs share the same port, and the host prints them and forwards typed input, so it doubles as a serial monitor.
- `WifiLink`: joins Wi-Fi, trying the network saved in NVS first and then fallbacks supplied by the application.
- `WifiSerial`: a password-protected `Stream` over Wi-Fi for logs and typed input, read with `m5-link --wifi`. For when the USB port is busy with something else.

`UsbLink` and `WifiSerial` are both `Stream`s with `poll()` and `connected()`, so logging and command code can take a `Stream&` and work over either.

The library has no display or camera code. Screens for entering Wi-Fi credentials or pairing belong in the application or a separate library, which passes the result to `WifiLink::save()`.

- `arduino/src/`: device library, using ArduinoJson, ESP-IDF mbedtls, and the ESP32 Arduino core
- `host/`: `m5-link`, the host command in Python, using pyserial

## Install

Device, in `platformio.ini`:

```ini
lib_deps =
  https://github.com/alumican/m5-link.git#v0.3.1
```

Host:

```sh
uv tool install "git+https://github.com/alumican/m5-link@v0.3.1#subdirectory=host"
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

## WifiSerial

```cpp
#include "WifiLink.h"
#include "WifiSerial.h"

WifiLink wifi("myapp-wifi");
WifiSerial con;  // TCP port 2323; WifiSerial con(port, txBufferSize) to change

void setup() {
  wifi.connect();
  con.begin("long-random-password", "myapp");  // advertises myapp.local; Wi-Fi may connect later
}

void loop() {
  con.poll();               // accepts the computer, sends buffered output, reads input
  int c = con.read();       // typed input, or -1
  if (c == 'r') con.println("reset requested");
}
```

On the computer:

```sh
export M5LINK_PASSWORD=long-random-password   # asked for if unset
m5-link --wifi myapp.local                     # or an IP address, HOST:PORT for another port
```

The command prints everything the device writes, sends each typed line (without the newline) on Enter, and reconnects by itself when the device resets or leaves the network. Lines typed while not connected, such as before authentication finishes or while the device resets, are queued and sent in order once connected, over USB as well. It exits if the password is wrong. `dns-sd -B _m5link._tcp` lists devices advertising WifiSerial.

| Method | |
|---|---|
| `WifiSerial(uint16_t port = 2323, size_t txBufferSize = 4096)` | |
| `bool begin(const char* password, const char* hostname = nullptr)` | Starts listening once Wi-Fi is connected. False if the password is empty. With a hostname, starts mDNS and advertises `_m5link._tcp` |
| `void end()` | Closes all sockets |
| `void poll()` | Call every `loop()` |
| `bool connected() const` | An authenticated computer is connected |
| `IPAddress remoteIP() const` | Its address |
| `size_t dropped() const` | Bytes dropped because the send buffer was full |
| `print`, `write`, `read`, `available`, `peek`, `flush` | As any `Stream`. `flush()` never waits |

Using either transport:

```cpp
UsbLink usb;
WifiSerial net;
Stream* con = &usb;  // or &net while the USB port is in use

void loop() {
  usb.poll();
  net.poll();
  con->println("tick");
  int c = con->read();
}
```

Behavior:

- Never blocks. Output is discarded while no computer is connected, queued in the send buffer while the network is slow, and dropped once the buffer is full (`dropped()` counts it). Input is buffered up to 256 bytes until read.
- One computer at a time. A newer one that authenticates replaces the older, so a computer that went to sleep does not lock you out. TCP keepalive also notices a vanished computer within about 11 s.
- Use it from one task, normally `loop()`.
- With ArduinoOTA, give both the same hostname; ArduinoOTA starts mDNS too, and a second `MDNS.begin` with the same name is harmless.

### Design choices

- TCP, not UDP: logs and commands need every byte in order, and a connection tells the device whether anyone is listening, so it can discard output instead of sending it nowhere.
- The device listens and the computer connects: the device does not need to know the computer's address, any computer can attach, and reconnecting is the computer's job, so the device does nothing while nobody is there.
- mDNS: `<hostname>.local` resolves without looking up the device's IP, which DHCP may change. macOS resolves `.local` names natively; on Linux, use the IP or install nss-mdns.
- Password with challenge-response: anyone on the same Wi-Fi can reach the port, and typed input drives the application. The device sends a random nonce, and the computer answers with HMAC-SHA256 of it keyed by the password, so the password never crosses the network and a recorded answer cannot be replayed. The device refuses connections for 1 s after a wrong answer, which slows guessing. There is no unauthenticated mode.
- No encryption: after authentication, logs and input are plain TCP, readable by anyone who can capture the network's traffic. Do not log secrets. TLS on the device would cost tens of KB of RAM per connection.
- Use a long random password: a captured nonce and answer allow offline guessing of short ones.

### WifiSerial protocol

The handshake uses the UL1 frame format; after it, bytes pass through unchanged in both directions.

```
device: 0x1E UL1 {"v":1,"op":"auth","nonce":"<32 hex digits>"}\n
host:   0x1E UL1 {"v":1,"op":"auth","mac":"<64 hex digits: HMAC-SHA256(key=password, message=nonce as ASCII)>"}\n
device: 0x1E UL1 {"v":1,"op":"auth","s":200}\n
```

On a wrong answer the device sends `"s":401` and closes. The host must answer within 5 s.

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
