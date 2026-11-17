#define M5LINK_USB_DEVICE_IMPL  // compiled into every application; empty unless ARDUINO_USB_MODE=0
#include "UsbDevice.h"

#if CONFIG_TINYUSB_ENABLED && defined(ARDUINO_USB_MODE) && !ARDUINO_USB_MODE

#include "tusb.h"

static const uint32_t REPORT_TIMEOUT_MS = 20;

void UsbKeyboard::begin() { hidKeyboard.begin(); }

void UsbKeyboard::startBatch() {
  if (busy()) return;
  keys.clear();
  pos = 0;
  typedCount = totalCount = skippedCount = 0;
}

void UsbKeyboard::queue(uint8_t modifiers, uint8_t usage) {
  keys.push_back((uint16_t)modifiers << 8 | usage);
  totalCount++;
}

bool UsbKeyboard::type(const char* text) {
  startBatch();
  bool all = true;
  for (const char* p = text; p && *p; p++) {
    uint8_t modifiers, usage;
    if (keyFor(keyLayout, *p, modifiers, usage)) {
      queue(modifiers, usage);
    } else {
      skippedCount++;
      all = false;
    }
  }
  return all;
}

bool UsbKeyboard::tap(uint8_t modifiers, char c) {
  uint8_t extra, usage;
  if (!keyFor(keyLayout, c, extra, usage)) return false;
  startBatch();
  queue(modifiers | extra, usage);
  return true;
}

void UsbKeyboard::tapKey(uint8_t modifiers, uint8_t usage) {
  startBatch();
  queue(modifiers, usage);
}

void UsbKeyboard::cancel() {
  size_t keep = pos + (down ? 1 : 0);  // a key held down is still released, and counted, by poll()
  totalCount -= keys.size() - keep;
  keys.resize(keep);
}

bool UsbKeyboard::send(uint8_t modifiers, uint8_t usage) {
  hid_keyboard_report_t report;
  memset(&report, 0, sizeof report);
  report.modifier = modifiers;
  report.keycode[0] = usage;
  return hid.SendReport(HID_REPORT_ID_KEYBOARD, &report, sizeof report, REPORT_TIMEOUT_MS);
}

void UsbKeyboard::poll() {
  if (!busy() || (int32_t)(millis() - nextAt) < 0) return;
  if (!USB || tud_suspended() || !hid.ready()) return;  // wait for the host; keys stay queued
  if (down) {
    if (!send(0, 0)) return;
    down = false;
    pos++;
    typedCount++;
    if (pos >= keys.size()) {  // batch done: free the queue
      std::vector<uint16_t>().swap(keys);
      pos = 0;
    }
    nextAt = millis() + (interval - interval / 2);
    return;
  }
  uint16_t key = keys[pos];
  if (!send(key >> 8, key & 0xFF)) return;
  down = true;
  nextAt = millis() + interval / 2;
}

bool UsbDevice::begin(const char* productName, const char* manufacturer, size_t rxBufferSize) {
  if (!claimUsbRole(UsbRole::Device)) return false;  // UsbHost has the port
  if (productName) USB.productName(productName);
  if (manufacturer) USB.manufacturerName(manufacturer);
  keyboard.begin();
#if !ARDUINO_USB_CDC_ON_BOOT
  cdc.setRxBufferSize(rxBufferSize);
  cdc.begin();
#else
  (void)rxBufferSize;
#endif
  return USB.begin();
}

bool UsbDevice::mounted() const { return USB && !tud_suspended(); }

USBCDC& UsbDevice::serial() {
#if ARDUINO_USB_CDC_ON_BOOT
  return Serial;
#else
  return cdc;
#endif
}

#endif
