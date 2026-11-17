#pragma once
// UsbHost: the M5Stack as a USB host on the ESP32-S3's USB-OTG port, using ESP-IDF's USB Host Library.
// Reads the attached device's identity (VID, PID, strings) and talks to CDC-ACM serial devices, such as an
// Arduino Uno R3, through a Stream with baud rate, DTR and RTS.
//
// One device at a time; no hubs. The application must supply 5 V on the port (with M5Unified on a CoreS3:
// M5.Power.setUsbOutput(true)), and only while it is the host. UsbHost and UsbDevice cannot both run: pick one
// at boot. See README.md.
//
//   UsbHost usb;
//   usb.begin();                       // in setup()
//   usb.poll();                        // every loop()
//   if (usb.attached()) ... usb.device().vid, usb.device().product ...
//   if (usb.serial()) usb.serial().println("hello");
//   int c = usb.serial().read();

#include <Arduino.h>

#include <functional>

#include "UsbRole.h"

#if CONFIG_USB_OTG_SUPPORTED

#include <usb/usb_host.h>

class UsbHost;

// The CDC-ACM serial port of the attached device. Never blocks, except flush().
class UsbHostSerial : public Stream {
 public:
  // True while a CDC-ACM device is attached and ready.
  explicit operator bool() const { return ready; }

  // Line settings, sent now if a device is attached and again on every attach. Default 115200 baud, 8N1,
  // DTR and RTS on (as a computer opening the port; an Arduino Uno resets when DTR turns on).
  void setBaud(uint32_t baud);
  uint32_t baud() const { return lineBaud; }
  void setDtr(bool on);
  void setRts(bool on);
  bool dtr() const { return lineDtr; }
  bool rts() const { return lineRts; }

  // Input bytes dropped so far because the receive buffer was full.
  size_t dropped() const { return droppedBytes; }

  int available() override;
  int read() override;
  int peek() override;
  // Queues output. Returns the bytes queued: 0 while no device is ready, fewer than n when the buffer is full.
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t n) override;
  int availableForWrite() override;
  // Waits until queued output has been sent, the device detaches, or 1 s passes.
  void flush() override;
  using Print::write;

 private:
  friend class UsbHost;
  struct Ring {
    uint8_t* buf = nullptr;
    size_t size = 0, start = 0, len = 0;
    size_t push(const uint8_t* data, size_t n);
    size_t pop(uint8_t* data, size_t n);
  };

  UsbHost* host = nullptr;
  Ring rx, tx;
  size_t droppedBytes = 0;
  uint32_t lineBaud = 115200;
  bool lineDtr = true, lineRts = true;
  bool ready = false;
  // Endpoints and interfaces of the attached device
  uint8_t controlInterface = 0, dataInterface = 0, epIn = 0, epOut = 0;
  size_t inLength = 0;  // a multiple of the packet size
  usb_transfer_t* in = nullptr;
  usb_transfer_t* out = nullptr;
  bool claimed = false;
  bool inBusy = false, outBusy = false;
  uint32_t inRetryAt = 0;
  bool inRetry = false;
  // Control requests still to send
  bool sendCoding = false, sendState = false;

  void attach(const usb_config_desc_t* config);
  void detach();
  bool idle() const { return !inBusy && !outBusy; }
  void release();
  void service();
  void submitIn();
  void kickOut();
  static void inDone(usb_transfer_t* t);
  static void outDone(usb_transfer_t* t);
};

class UsbHost {
 public:
  // Identity of the attached device. Strings are ASCII; other characters become '?'.
  struct Device {
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t deviceClass = 0;
    String manufacturer;
    String product;
    String serialNumber;
    bool cdcAcm = false;  // has a CDC-ACM serial interface, usable through serial()
  };

  explicit UsbHost(size_t rxBufferSize = 2048, size_t txBufferSize = 1024);

  // Starts the USB Host Library on the USB-OTG port. Returns false if UsbDevice has the port, if TinyUSB was
  // started at boot (ARDUINO_USB_CDC_ON_BOOT=1 with ARDUINO_USB_MODE=0), or if ESP-IDF fails. Runs once per boot.
  bool begin();
  // Call every loop(). Handles attach and detach, and moves serial data.
  void poll();
  // True while a device is attached and its descriptors have been read.
  bool attached() const { return dev != nullptr && !closing; }
  // The attached device; empty while none.
  const Device& device() const { return info; }
  UsbHostSerial& serial() { return cdc; }
  // Called from poll() with the device that attached or detached. A detach is reported before an attach that
  // follows it.
  void onAttach(std::function<void(const Device&)> callback) { attachCallback = callback; }
  void onDetach(std::function<void(const Device&)> callback) { detachCallback = callback; }

 private:
  friend class UsbHostSerial;
  size_t rxSize, txSize;
  bool begun = false;
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t dev = nullptr;
  bool closing = false;
  Device info, goneInfo;
  UsbHostSerial cdc;
  usb_transfer_t* control = nullptr;
  bool controlBusy = false;
  bool attachEvent = false, detachEvent = false;
  // Recorded by clientEvent() and acted on in pump()
  uint8_t newAddress = 0;
  bool devGone = false;
  std::function<void(const Device&)> attachCallback, detachCallback;

  void pump();
  void open(uint8_t address);
  void gone();
  void finishClose();
  void openWaiting();
  void kickControl();
  static void clientEvent(const usb_host_client_event_msg_t* msg, void* arg);
  static void controlDone(usb_transfer_t* t);
};

#elif !defined(M5LINK_USB_HOST_IMPL)
#error "UsbHost needs an ESP32-S2 or ESP32-S3 (USB-OTG); see README.md"
#endif
