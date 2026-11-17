// UsbHost example: the CoreS3 hosts a CDC-ACM device such as an Arduino Uno R3 on its USB port, and bridges
// it to a computer over Wi-Fi. On the computer:
//
//   M5LINK_PASSWORD=... m5-link --wifi usbhost-example.local
//
// shows the attached device's identity and everything it prints; typed lines go to it. Touching the screen
// pulses DTR, which resets an Uno.
//
// The USB port is a host here, so this firmware is updated over Wi-Fi (ArduinoOTA) after the first upload.

#include <ArduinoOTA.h>
#include <M5Unified.h>

#include "UsbHost.h"
#include "WifiLink.h"
#include "WifiSerial.h"

static const char NAME[] = "usbhost-example";

WifiLink wifi("usbhost-ex");
WifiSerial console;
UsbHost usb;

static void show(const char* what, const UsbHost::Device& d) {
  console.printf("%s %04x:%04x \"%s\" \"%s\" serial \"%s\"%s\n", what, d.vid, d.pid, d.manufacturer.c_str(),
                 d.product.c_str(), d.serialNumber.c_str(), d.cdcAcm ? ", CDC-ACM" : "");
  M5.Display.printf("%s %04x:%04x %s\n", what, d.vid, d.pid, d.product.c_str());
}

void setup() {
  M5.begin();
  M5.Display.setTextSize(2);

  const WifiLink::Network fallback = {WIFI_SSID, WIFI_PASSWORD};
  wifi.connect(&fallback, 1);
  console.begin(CONSOLE_PASSWORD, NAME);
  ArduinoOTA.setHostname(NAME);
  ArduinoOTA.begin();

  // Power the port first, then start the host. Only do this when the port is not plugged into a computer.
  M5.Power.setUsbOutput(true);
  usb.serial().setBaud(115200);
  usb.onAttach([](const UsbHost::Device& d) { show("attached", d); });
  usb.onDetach([](const UsbHost::Device& d) { show("detached", d); });
  if (!usb.begin()) M5.Display.println("UsbHost failed to start");
}

void loop() {
  M5.update();
  ArduinoOTA.handle();
  console.poll();
  usb.poll();

  UsbHostSerial& port = usb.serial();
  uint8_t buf[256];
  int n = 0;
  while (n < (int)sizeof buf && port.available()) buf[n++] = port.read();
  if (n) console.write(buf, n);
  while (console.available() && port.availableForWrite()) port.write(console.read());

  if (port && M5.Touch.getCount() && M5.Touch.getDetail().wasPressed()) {
    port.setDtr(false);
    delay(50);
    port.setDtr(true);  // the Uno resets on this edge
    console.println("DTR pulsed");
  }
}
