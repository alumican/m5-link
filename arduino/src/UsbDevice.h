#pragma once
// UsbDevice: the M5Stack as a composite USB device, CDC serial plus HID keyboard, through the ESP32-S3's
// USB-OTG port (TinyUSB). A host such as a Raspberry Pi sees /dev/ttyACM0 and a keyboard at the same time.
//
// Needs build_flags -DARDUINO_USB_MODE=0, which disables the USB-Serial/JTAG port: see README.md.
// Declare the UsbDevice globally: its USB interfaces must be registered before USB starts.
//
//   UsbDevice usb;
//   usb.begin("My Device");                 // in setup()
//   usb.keyboard.setLayout(KeyLayout::JP);
//   usb.poll();                             // every loop()
//   usb.serial().println("{\"hello\":1}");  // a Stream
//   if (usb.mounted() && !usb.keyboard.busy()) usb.keyboard.type("ls\n");

#include <Arduino.h>

#include "KeyLayout.h"

#if CONFIG_TINYUSB_ENABLED && defined(ARDUINO_USB_MODE) && !ARDUINO_USB_MODE

#include <USB.h>
#include <USBHID.h>
#include <USBHIDKeyboard.h>

#include <vector>

// Types text without blocking: type() queues keys and poll() sends them one at a time.
class UsbKeyboard {
 public:
  // Layout the host uses. Default US.
  void setLayout(KeyLayout layout) { keyLayout = layout; }
  KeyLayout layout() const { return keyLayout; }
  // Milliseconds per key, half held down and half released. Default 20.
  void setInterval(uint16_t ms) { interval = ms; }

  // Queues text: printable ASCII, '\n' (Enter), '\t' (Tab), '\b' (Backspace), 0x1B (Esc).
  // Other characters, including '\r' and non-ASCII, are skipped and counted in skipped(); returns false if any were.
  bool type(const char* text);
  bool type(const String& text) { return type(text.c_str()); }
  // Queues the key for c with extra modifiers, e.g. tap(KeyMod::Ctrl, 'c'). False if c has no key.
  bool tap(uint8_t modifiers, char c);
  // Queues a key by HID usage ID, e.g. tapKey(0, Key::Up).
  void tapKey(uint8_t modifiers, uint8_t usage);
  // Drops the keys not typed yet.
  void cancel();

  // Called by UsbDevice::poll(). Sends at most one key press or release.
  void poll();
  // True while keys are queued or one is held down.
  bool busy() const { return pos < keys.size() || down; }
  // Progress of the current batch, which starts when keys are queued while not busy: keys typed so far,
  // keys queued in all, and characters skipped. Kept after the batch finishes, until the next one.
  size_t typed() const { return typedCount; }
  size_t total() const { return totalCount; }
  size_t skipped() const { return skippedCount; }

 private:
  friend class UsbDevice;
  USBHIDKeyboard hidKeyboard;  // registers the keyboard report descriptor
  USBHID hid;
  KeyLayout keyLayout = KeyLayout::US;
  uint16_t interval = 20;
  std::vector<uint16_t> keys;  // modifiers << 8 | usage
  size_t pos = 0;
  bool down = false;
  uint32_t nextAt = 0;
  size_t typedCount = 0, totalCount = 0, skippedCount = 0;

  void begin();
  void startBatch();
  void queue(uint8_t modifiers, uint8_t usage);
  bool send(uint8_t modifiers, uint8_t usage);
};

class UsbDevice {
 public:
  UsbKeyboard keyboard;

  // Starts USB. productName and manufacturer are shown by the host (lsusb); they cannot be changed with
  // ARDUINO_USB_CDC_ON_BOOT=1, since USB is then started before setup(). rxBufferSize applies to the serial.
  bool begin(const char* productName = nullptr, const char* manufacturer = nullptr, size_t rxBufferSize = 2048);
  // Call every loop(). Types queued keys.
  void poll() { keyboard.poll(); }
  // True while a host has configured the device and is not suspended. Unplugging shows as suspended.
  bool mounted() const;
  // The CDC serial port. bool(serial()) is true while a program on the host has it open (DTR set).
  // With ARDUINO_USB_CDC_ON_BOOT=1 this is Serial.
  USBCDC& serial();

 private:
#if !ARDUINO_USB_CDC_ON_BOOT
  USBCDC cdc;
#endif
};

#elif !defined(M5LINK_USB_DEVICE_IMPL)
#error "UsbDevice needs an ESP32-S3 with build_flags -DARDUINO_USB_MODE=0; see README.md"
#endif
