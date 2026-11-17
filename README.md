# m5-link

Transport library for M5Stack (ESP32-S3). The goal is one way to make HTTP requests, type as a keyboard, and exchange serial data, whichever transport is available: USB, Wi-Fi, or BLE. See [TODO.md](TODO.md) for what is not built yet.

Available now:

- `UsbLink`: HTTP requests through a computer connected over USB serial, for places where the ESP32 cannot join Wi-Fi, such as venues with 5 GHz networks only. Also provides the computer's clock. `Serial.print` logs share the same port, and the host prints them and forwards typed input, so it doubles as a serial monitor.
- `WifiLink`: joins Wi-Fi, trying the network saved in NVS first and then fallbacks supplied by the application.
- `WifiSerial`: a password-protected `Stream` over Wi-Fi for logs and typed input, read with `m5-link --wifi`. For when the USB port is busy with something else.
- `UsbDevice`: the device as a composite USB device, CDC serial plus HID keyboard, so a host such as a Raspberry Pi sees `/dev/ttyACM0` and a keyboard at once. The keyboard types text without blocking, in the US or Japanese layout.
- `UsbHost`: the device as a USB host. Reads the attached device's identity and talks to CDC-ACM serial devices, such as an Arduino Uno R3, with baud rate, DTR and RTS. One firmware can contain both `UsbDevice` and `UsbHost` and start one of them at boot.

`UsbLink` and `WifiSerial` are both `Stream`s with `poll()` and `connected()`, so logging and command code can take a `Stream&` and work over either. `UsbDevice::serial()` and `UsbHost::serial()` are `Stream`s too.

The library has no display or camera code. Screens for entering Wi-Fi credentials or pairing belong in the application or a separate library, which passes the result to `WifiLink::save()`.

- `arduino/src/`: device library, using ArduinoJson, ESP-IDF mbedtls and USB Host Library, and the ESP32 Arduino core
- `examples/`: PlatformIO projects using the library
- `host/`: `m5-link`, the host command in Python, using pyserial

## Install

Device, in `platformio.ini`:

```ini
lib_deps =
  https://github.com/alumican/m5-link.git#v0.6.0
```

Host:

```sh
uv tool install "git+https://github.com/alumican/m5-link@v0.6.0#subdirectory=host"
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

## UsbDevice

Serial and keyboard over the ESP32-S3's USB-OTG port, using TinyUSB. Needs these build flags:

```ini
build_flags =
  -DARDUINO_USB_MODE=0
  -DARDUINO_USB_CDC_ON_BOOT=0
build_unflags =
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
```

```cpp
#include "UsbDevice.h"

UsbDevice usb;  // global: its USB interfaces must be registered before USB starts

void setup() {
  usb.begin("My Device");                 // product name shown by lsusb
  usb.keyboard.setLayout(KeyLayout::JP);  // the layout the host uses
  usb.keyboard.setInterval(20);           // ms per key
}

void loop() {
  usb.poll();                             // types queued keys, one press or release per call
  Stream& io = usb.serial();
  if (io.available()) { /* JSON lines from the host program */ }
  if (usb.mounted() && !usb.keyboard.busy() && startRequested) {
    usb.keyboard.tap(KeyMod::Ctrl | KeyMod::Alt, 't');  // e.g. open a terminal
    usb.keyboard.type("python3 ~/agent.py\n");
  }
  if (usb.keyboard.busy()) {
    // show "typing" with usb.keyboard.typed() of usb.keyboard.total()
  }
}
```

| `UsbDevice` | |
|---|---|
| `bool begin(const char* productName = nullptr, const char* manufacturer = nullptr, size_t rxBufferSize = 2048)` | Starts USB |
| `void poll()` | Call every `loop()` |
| `bool mounted() const` | A host has configured the device and is not suspended. Unplugging shows as suspended |
| `USBCDC& serial()` | The CDC port, a `Stream`. `bool(usb.serial())` is true while a host program has it open |
| `UsbKeyboard keyboard` | |

| `UsbKeyboard` | |
|---|---|
| `void setLayout(KeyLayout layout)` | `KeyLayout::US` (default) or `KeyLayout::JP` |
| `void setInterval(uint16_t ms)` | Time per key, half held and half released; default 20 |
| `bool type(const char* text)` | Queues text: printable ASCII, `\n` (Enter), `\t` (Tab), `\b` (Backspace), `\x1b` (Esc). Other characters, including `\r`, are skipped; false if any were |
| `bool tap(uint8_t modifiers, char c)` | Queues one key with modifiers, e.g. `tap(KeyMod::Ctrl, 'c')` |
| `void tapKey(uint8_t modifiers, uint8_t usage)` | Queues a key by HID usage ID: `Key::Enter`, `Key::Esc`, `Key::Up`, `Key::F1`, ... |
| `void cancel()` | Drops keys not typed yet |
| `bool busy() const` | Keys are queued or one is held down |
| `size_t typed() const`, `size_t total() const`, `size_t skipped() const` | Progress of the current batch, which starts when keys are queued while not busy; kept after it finishes |

Behavior:

- Typing never blocks the loop: each `poll()` sends at most one key press or release, waiting up to 20 ms for the host to take it. A key therefore needs two `poll()` calls, so call it often while typing. While the host is not there, keys stay queued.
- The device sends key positions, not characters, so the layout must match the host's. `KeyLayout::JP` is the JIS 106/109 layout, which Raspberry Pi OS uses when Japan is chosen (`jp106`); symbols such as `@ : ' " _ \ |` are on different keys from US. On a Japanese host, `\` is typed with the "ro" key, which Linux treats as backslash.
- An input method in Japanese mode, or Caps Lock, changes what the host makes of the keys; switch to direct input first.
- The serial port does not block while no host program has it open: writes return 0. While a program has it open but is not reading, each write waits up to 250 ms (`serial().setTxTimeoutMs()`).
- `UsbLink` works over `usb.serial()`, with `m5-link --port /dev/ttyACM0` on the host.

Things to know about `ARDUINO_USB_MODE=0`:

- The ESP32-S3's USB-Serial/JTAG port is gone while the application runs, so `Serial` is the hardware UART (GPIO 43/44), not USB. Log over `WifiSerial` instead.
- Uploading over USB with the usual reset no longer works. Use OTA, or put the CoreS3 in download mode by holding its reset button for about 2 s until the green LED lights, upload, then press reset. The core's CDC also reboots into download mode when a host opens it at 1200 baud; this has not been tried as an upload method here. `usb.serial().enableReboot(false)` turns that off.
- With `ARDUINO_USB_CDC_ON_BOOT=1` instead, the core starts USB before `setup()`: `serial()` is `Serial`, `Serial.print` logs go to the host program, and `begin()` cannot set the names.
- Some Linux hosts run ModemManager, which may send AT commands to a new `/dev/ttyACM*` for a few seconds after it appears. Raspberry Pi OS does not install it by default.

## UsbHost

The USB-OTG port as a USB host, using ESP-IDF's USB Host Library (`usb_host`, ESP-IDF 4.4 in Arduino core 2.0.17). CDC-ACM is implemented in this library, as ESP-IDF 4.4 does not include a CDC-ACM host driver. Build flags, the same as for `UsbDevice`:

```ini
build_flags =
  -DARDUINO_USB_MODE=0
  -DARDUINO_USB_CDC_ON_BOOT=0
build_unflags =
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
```

```cpp
#include <M5Unified.h>
#include "UsbHost.h"

UsbHost usb;

void setup() {
  M5.begin();
  M5.Power.setUsbOutput(true);           // 5 V on the port: the attached device is powered from here
  usb.serial().setBaud(115200);
  usb.onAttach([](const UsbHost::Device& d) {
    // d.vid, d.pid, d.manufacturer, d.product, d.serialNumber, d.cdcAcm
  });
  usb.onDetach([](const UsbHost::Device& d) {});
  usb.begin();
}

void loop() {
  usb.poll();
  UsbHostSerial& port = usb.serial();
  if (port) {                            // a CDC-ACM device is ready
    port.println("hello");
    while (port.available()) handle(port.read());
  }
}
```

[examples/usb_host_serial](examples/usb_host_serial) bridges the attached device to `m5-link --wifi`, with OTA for updates.

| `UsbHost` | |
|---|---|
| `UsbHost(size_t rxBufferSize = 2048, size_t txBufferSize = 1024)` | |
| `bool begin()` | Starts the host. False if `UsbDevice` has started, if TinyUSB was started at boot (`ARDUINO_USB_CDC_ON_BOOT=1`), or if ESP-IDF fails |
| `void poll()` | Call every `loop()`. Attach, detach, and serial data happen here |
| `bool attached() const` | A device is attached and its descriptors have been read |
| `const Device& device() const` | `vid`, `pid`, `deviceClass`, `manufacturer`, `product`, `serialNumber`, `cdcAcm`; empty while none |
| `void onAttach(std::function<void(const Device&)>)`, `void onDetach(...)` | Called from `poll()`. A detach is reported before an attach that follows it |
| `UsbHostSerial& serial()` | |
| `Port port() const`, `bool connected() const` | The port as the controller sees it, enumerated or not: `connected` (something pulls D+ or D- up), `enabled`, `speed` (`UsbSpeed::Low`, `Full`, ...), `dPlus`, `dMinus` (line levels) |
| `void onConnect(std::function<void(UsbSpeed)>)`, `void onDisconnect(std::function<void()>)` | Called from `poll()` when something is plugged in or unplugged, before any enumeration |
| `void onEnumerationFailed(std::function<void(const EnumerationFailure&)>)` | Called from `poll()`, once per connection, when a connected device does not attach |
| `bool enumerationFailed() const`, `const EnumerationFailure& failure() const` | The failure for the current connection: `speed`, `stage` (e.g. `GET_FULL_CONFIG_DESC`), `message` (the library's error lines), `timedOut`. Cleared on unplug |
| `void setEnumerationTimeout(uint32_t ms)` | How long a connected device may take to attach before it counts as failed; default 5000 |

| `UsbHostSerial` | |
|---|---|
| `explicit operator bool() const` | A CDC-ACM device is attached and ready |
| `void setBaud(uint32_t)`, `void setDtr(bool)`, `void setRts(bool)` | Sent now if a device is ready, and on every attach. Default 115200 baud, 8N1, DTR and RTS on |
| `read`, `available`, `peek` | As any `Stream`. Input arriving faster than it is read is dropped beyond the receive buffer (`dropped()`) |
| `write`, `print`, `availableForWrite` | Queues output and returns the bytes queued: 0 while no device is ready, fewer than asked when the buffer is full |
| `void flush()` | Waits until queued output is sent, the device detaches, or 1 s passes. The only call that waits |

Order and power:

1. `M5.begin()`, then `M5.Power.setUsbOutput(true)` to put 5 V on the port. The library does not control power.
2. Optionally `serial().setBaud()`, `setDtr()`, `onAttach()`; then `begin()`.
3. `poll()` every `loop()`. A device plugged in before or after `begin()` is reported by `poll()`.

- Turn the 5 V output on only while the port is a host with a device attached. Never when it is plugged into a computer or a Raspberry Pi, which supply 5 V themselves: start `UsbDevice` with the output off.
- On a CoreS3, the 5 V output setting lives in the power chips (AW9523, AXP2101), not the ESP32, so it survives a restart. With it still on, a computer on a USB-C port sees 5 V already there and does not act as a host, so `UsbDevice` never mounts. Turn it off early in `setup()` (`M5.Power.setUsbOutput(false)`), wait about a second for the line to settle, and only then decide the role, e.g. by checking for outside power.
- While it is a host, the CoreS3 cannot be charged through the port; it runs on its battery or other supply, which also powers the attached device (an Uno R3 draws about 50 mA).
- With DTR on, which is the default, an Arduino Uno resets when it attaches, as when a computer opens its port, and its bootloader runs for up to about a second before the sketch starts. Turning DTR off and on again resets it again, which is how a later sketch upload (STK500) would start.

What can be known about a plugged-in device:

| What happens | What the application sees |
|---|---|
| The device enumerates | `onConnect`, then `onAttach` with its identity; `attached()` |
| It pulls D+ or D- up but enumeration fails | `onConnect` with its speed, then `onEnumerationFailed` with the stage that failed and the library's error lines |
| It pulls a line up but nothing is logged and it never attaches | `onConnect`, then `onEnumerationFailed` with `timedOut` after the timeout |
| No data lines (charge-only cable), or the device never pulls a line up (it only takes power, is off or asleep, or waits for something the port does not offer) | Nothing: `connected()` stays false, the same as an empty port. The USB controller cannot see power draw |

- ESP-IDF 4.4 does not tell clients about failed enumerations, but its hub driver logs them as errors, e.g. `Stage failed: GET_FULL_CONFIG_DESC`. `begin()` installs an `esp_log_set_vprintf()` hook that copies the USB Host Library's error lines and passes all output on unchanged. Without them, for example when the application installs its own hook after `begin()` or lowers the log level below errors, failures are reported only by the timeout, with an empty `stage`.
- After a failed enumeration, the library does not retry until the device is unplugged and plugged in again.
- A high-speed device runs at full speed here: the ESP32-S3 host is full speed only.
- Limits of ESP-IDF 4.4 that make some devices fail: a configuration descriptor longer than 256 bytes (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` in the core's prebuilt SDK) fails with "Configuration descriptor larger than control transfer max length"; only the first configuration is used; one device and no hubs.

Using `UsbDevice` and `UsbHost` in one firmware:

- With `ARDUINO_USB_MODE=0` and `ARDUINO_USB_CDC_ON_BOOT=0`, the Arduino core starts TinyUSB only in `USB.begin()`, which `UsbDevice::begin()` calls. Declaring a `UsbDevice` registers its USB descriptors and nothing else, so while `UsbDevice::begin()` is not called, `UsbHost::begin()` gets the USB-OTG port. This was read from the core's source and built; it has not yet been tried on a device.
- `begin()` of one fails once the other has started. Switching roles needs a restart.
- With `ARDUINO_USB_CDC_ON_BOOT=1`, TinyUSB starts before `setup()` and `UsbHost::begin()` returns false.
- `UsbHost` also builds with the default `ARDUINO_USB_MODE=1`, but starting it takes the USB PHY from the USB-Serial/JTAG port that `Serial` and uploads use; this has not been tried.

Limits: CDC-ACM only (no CH340, CP210x, FTDI yet); 8N1 only; one device and no hubs; serial state notifications are ignored. Uploads over USB need download mode or OTA, as with `UsbDevice`.

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

Host: the UL1 replies and chunked acks over a fake serial port, the allowlist, HTTP requests against a local server, and WifiSerial authentication against a fake device.

```sh
cd host
uv run --group dev pytest
```

`tests`: Unity tests that run on a connected CoreS3. They cover `UsbLink` over an in-memory `Stream` that plays the host: hello and the connection state, logs kept apart from frames, requests, and acks every 4096 bytes.

```sh
cd tests
pio test                                        # build, upload and run
pio test --without-uploading --without-testing  # build only
```

Keyboard layouts, on the computer:

```sh
c++ -std=c++11 -Wall -I arduino/src arduino/test/key_layout_test.cpp arduino/src/KeyLayout.cpp -o /tmp/key_layout_test && /tmp/key_layout_test
```

## License

[MIT](LICENSE)
