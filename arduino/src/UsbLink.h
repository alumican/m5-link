#pragma once
// UsbLink: HTTP requests over USB serial, made by a computer running the m5-link host.
// Reaches internet APIs and servers on the computer without Wi-Fi, and provides the computer's clock.
// Serial.print logs can share the port: frames are single JSON lines prefixed with 0x1E "UL1 ".
// UsbLink is a Stream: print() goes to the port and read() returns typed input, so code written against
// Stream& works with WifiSerial as well.
// ESP32 only, as it uses ESP-IDF mbedtls for base64. See README.md for the protocol.
//
//   UsbLink usb;
//   usb.begin(Serial);                   // in setup(), after enlarging the RX buffer
//   usb.poll();                          // every loop()
//   int c = usb.read();                  // non-frame input
//   usb.println("log");                  // same as Serial.println
//   UsbLink::Response r;
//   if (usb.connected() && usb.request("GET", "https://api.example.com/x", nullptr, 0, nullptr, r)) {
//     ... r.status, r.body, r.length ...
//     free(r.body);
//   }

#include <Arduino.h>

class UsbLink : public Stream {
 public:
  struct Response {
    int status = 0;           // HTTP status; 0 if the host could not reach the server
    uint8_t* body = nullptr;  // ps_malloc'd; free() when done
    size_t length = 0;
    String contentType;
  };

  void begin(Stream& io, uint32_t helloEveryMs = 3000);
  // Call every loop(). Sends hello and reads input.
  void poll();
  // True if the host replied within 3 hello intervals.
  bool connected() const;
  // Host clock (UNIX time, seconds); 0 if unknown.
  int64_t hostTime() const;
  // Next non-frame input byte, or -1. Bytes received during poll() and request() are buffered.
  int read() override;
  // Non-frame input bytes waiting.
  int available() override;
  int peek() override;
  // Output goes straight to the port, like Serial.print.
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t n) override;
  void flush() override;
  using Print::write;
  // Sends an HTTP request and waits for the reply. Returns true if a reply arrived; the HTTP status is in out.status.
  bool request(const char* method, const String& url, const uint8_t* body, size_t length, const char* contentType,
               Response& out, uint32_t timeoutMs = 20000);

 private:
  Stream* io = nullptr;
  uint32_t helloEvery = 3000;
  uint32_t helloSentAt = 0;
  uint32_t helloGotAt = 0;
  int64_t hostEpoch = 0;
  uint32_t hostEpochAt = 0;
  uint32_t nextId = 1;
  // Frame being received
  String line;
  bool inFrame = false;
  // Non-frame input
  static const int CHARS = 64;
  char chars[CHARS];
  int charHead = 0, charTail = 0;
  // Reply awaited by request()
  uint32_t waitingId = 0;
  bool gotReply = false;
  Response* reply = nullptr;

  void send(const String& json);
  void pump();
  void handleFrame(const String& json);
  bool readBody(uint8_t* buf, size_t n, uint32_t id, size_t chunk, uint32_t timeoutMs);
  void pushChar(char c);
};
