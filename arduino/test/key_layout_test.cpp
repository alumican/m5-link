// Tests KeyLayout on a computer:
//   c++ -std=c++11 -Wall -I arduino/src arduino/test/key_layout_test.cpp arduino/src/KeyLayout.cpp -o /tmp/key_layout_test && /tmp/key_layout_test

#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "KeyLayout.h"

static int failures = 0;

static void expect(KeyLayout layout, char c, uint8_t modifiers, uint8_t usage) {
  uint8_t m = 0xFF, u = 0xFF;
  if (!keyFor(layout, c, m, u) || m != modifiers || u != usage) {
    printf("FAIL %s 0x%02x: got mod 0x%02x usage 0x%02x, want 0x%02x 0x%02x\n", layout == KeyLayout::JP ? "JP" : "US",
           (unsigned char)c, m, u, modifiers, usage);
    failures++;
  }
}

static void none(char c) {
  uint8_t m, u;
  if (keyFor(KeyLayout::US, c, m, u) || keyFor(KeyLayout::JP, c, m, u)) {
    printf("FAIL 0x%02x should have no key\n", (unsigned char)c);
    failures++;
  }
}

int main() {
  const uint8_t SH = KeyMod::Shift;
  for (KeyLayout l : {KeyLayout::US, KeyLayout::JP}) {
    expect(l, 'a', 0, 0x04);
    expect(l, 'z', 0, 0x1D);
    expect(l, 'A', SH, 0x04);
    expect(l, '1', 0, 0x1E);
    expect(l, '0', 0, 0x27);
    expect(l, ' ', 0, 0x2C);
    expect(l, '\n', 0, Key::Enter);
    expect(l, '\t', 0, Key::Tab);
    expect(l, '-', 0, 0x2D);
    expect(l, ',', 0, 0x36);
    expect(l, '/', 0, 0x38);
    expect(l, '?', SH, 0x38);
    expect(l, ';', 0, 0x33);
    expect(l, '!', SH, 0x1E);
  }
  // Keys that differ
  const char* chars = "\"&'()*+:=@[\\]^_`{|}~";
  const uint8_t us[][2] = {{SH, 0x34}, {SH, 0x24}, {0, 0x34}, {SH, 0x26}, {SH, 0x27}, {SH, 0x25}, {SH, 0x2E},
                           {SH, 0x33}, {0, 0x2E},  {SH, 0x1F}, {0, 0x2F},  {0, 0x31},  {0, 0x30},  {SH, 0x23},
                           {SH, 0x2D}, {0, 0x35},  {SH, 0x2F}, {SH, 0x31}, {SH, 0x30}, {SH, 0x35}};
  const uint8_t jp[][2] = {{SH, 0x1F}, {SH, 0x23}, {SH, 0x24}, {SH, 0x25}, {SH, 0x26}, {SH, 0x34}, {SH, 0x33},
                           {0, 0x34},  {SH, 0x2D}, {0, 0x2F},  {0, 0x30},  {0, 0x87},  {0, 0x32},  {0, 0x2E},
                           {SH, 0x87}, {SH, 0x2F}, {SH, 0x30}, {SH, 0x89}, {SH, 0x32}, {SH, 0x2E}};
  for (size_t i = 0; i < strlen(chars); i++) {
    expect(KeyLayout::US, chars[i], us[i][0], us[i][1]);
    expect(KeyLayout::JP, chars[i], jp[i][0], jp[i][1]);
  }
  // Every printable character has a key
  for (int c = 0x20; c < 0x7F; c++) {
    uint8_t m, u;
    if (!keyFor(KeyLayout::US, c, m, u) || !keyFor(KeyLayout::JP, c, m, u)) {
      printf("FAIL no key for '%c'\n", c);
      failures++;
    }
  }
  none('\r');
  none(0);
  none((char)0xE3);
  none(0x7F);
  printf(failures ? "%d failures\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
