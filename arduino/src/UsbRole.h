#pragma once
// The ESP32-S3 has one USB-OTG port: UsbDevice and UsbHost cannot both run. Each claims the port in begin().

#include <stdint.h>

enum class UsbRole : uint8_t { None, Device, Host };

// The role claimed so far in this boot.
inline UsbRole& currentUsbRole() {
  static UsbRole role = UsbRole::None;
  return role;
}

// Claims the port for role. False if the other role already has it; switching needs a restart.
inline bool claimUsbRole(UsbRole role) {
  UsbRole& current = currentUsbRole();
  if (current != UsbRole::None && current != role) return false;
  current = role;
  return true;
}
