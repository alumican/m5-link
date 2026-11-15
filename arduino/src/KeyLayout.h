#pragma once
// Keyboard layouts: which HID key (usage ID and modifiers) types an ASCII character on the host.
// Plain C++ with no Arduino dependency, so it can be tested on a computer (arduino/test/).

#include <stdint.h>

enum class KeyLayout : uint8_t {
  US,  // ANSI US
  JP,  // JIS 106/109; "jp106" on Linux, the layout Raspberry Pi OS uses when Japan is chosen
};

// HID modifier bits, as in the first byte of a boot keyboard report
namespace KeyMod {
const uint8_t Ctrl = 0x01;
const uint8_t Shift = 0x02;
const uint8_t Alt = 0x04;
const uint8_t Gui = 0x08;  // Windows / Command / Super
}  // namespace KeyMod

// HID usage IDs of some keys without a character. (Arduino's USBHIDKeyboard.h defines KEY_* macros with other
// values, hence the namespace.)
namespace Key {
const uint8_t Enter = 0x28;
const uint8_t Esc = 0x29;
const uint8_t Backspace = 0x2A;
const uint8_t Tab = 0x2B;
const uint8_t F1 = 0x3A;  // F2..F12 follow in order
const uint8_t Delete = 0x4C;
const uint8_t Right = 0x4F;
const uint8_t Left = 0x50;
const uint8_t Down = 0x51;
const uint8_t Up = 0x52;
}  // namespace Key

// Finds the key for c: printable ASCII, '\n' (Enter), '\t' (Tab), '\b' (Backspace), 0x1B (Esc).
// Returns false for anything else, including '\r' and non-ASCII bytes.
bool keyFor(KeyLayout layout, char c, uint8_t& modifiers, uint8_t& usage);
