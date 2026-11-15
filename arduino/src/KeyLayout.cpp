#include "KeyLayout.h"

#include <string.h>

#define S(u) (0x200 | (u))  // with Shift: modifiers in the high byte, usage ID in the low byte

static const char SYMBOLS[] = " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";

static const uint16_t US_KEYS[] = {
    0x2C,     S(0x1E), S(0x34), S(0x20), S(0x21), S(0x22), S(0x24), 0x34,    S(0x26), S(0x27), S(0x25),
    S(0x2E),  0x36,    0x2D,    0x37,    0x38,    S(0x33), 0x33,    S(0x36), 0x2E,    S(0x37), S(0x38),
    S(0x1F),  0x2F,    0x31,    0x30,    S(0x23), S(0x2D), 0x35,    S(0x2F), S(0x31), S(0x30), S(0x35),
};

// 0x87 is the "ro" key (backslash, underscore) and 0x89 the yen key (backslash, bar on Linux).
// 0x32 is the key left of Enter (] and }).
static const uint16_t JP_KEYS[] = {
    0x2C,     S(0x1E), S(0x1F), S(0x20), S(0x21), S(0x22), S(0x23), S(0x24), S(0x25), S(0x26), S(0x34),
    S(0x33),  0x36,    0x2D,    0x37,    0x38,    0x34,    0x33,    S(0x36), S(0x2D), S(0x37), S(0x38),
    0x2F,     0x30,    0x87,    0x32,    0x2E,    S(0x87), S(0x2F), S(0x30), S(0x89), S(0x32), S(0x2E),
};

static_assert(sizeof SYMBOLS - 1 == sizeof US_KEYS / sizeof US_KEYS[0], "US table size");
static_assert(sizeof SYMBOLS - 1 == sizeof JP_KEYS / sizeof JP_KEYS[0], "JP table size");

bool keyFor(KeyLayout layout, char c, uint8_t& modifiers, uint8_t& usage) {
  uint16_t key = 0;
  if (c >= 'a' && c <= 'z') {
    key = 0x04 + (c - 'a');
  } else if (c >= 'A' && c <= 'Z') {
    key = S(0x04 + (c - 'A'));
  } else if (c >= '1' && c <= '9') {
    key = 0x1E + (c - '1');
  } else if (c == '0') {
    key = 0x27;
  } else if (c == '\n') {
    key = 0x28;
  } else if (c == '\t') {
    key = 0x2B;
  } else if (c == '\b') {
    key = 0x2A;
  } else if (c == 0x1B) {
    key = 0x29;
  } else if (c) {
    const char* p = strchr(SYMBOLS, c);
    if (!p) return false;
    key = (layout == KeyLayout::JP ? JP_KEYS : US_KEYS)[p - SYMBOLS];
  }
  if (!key) return false;
  modifiers = key >> 8;
  usage = key & 0xFF;
  return true;
}

#undef S
