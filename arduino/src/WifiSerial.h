#pragma once
// WifiSerial: a Stream over Wi-Fi for logs and typed input, read by `m5-link --wifi <host>` on a computer.
// Use it instead of Serial while the USB port is busy with something else.
//
// The device listens on a TCP port; the computer connects, proves it knows the password (HMAC-SHA256
// challenge-response), and from then on bytes flow both ways unchanged. One computer at a time; a newer one
// that authenticates replaces the older. Nothing blocks: output is discarded while no computer is connected,
// and dropped when the send buffer is full. See README.md for the protocol.
// ESP32 only, as it uses lwIP sockets and ESP-IDF mbedtls. Use it from one task (normally loop()).
//
//   WifiSerial con;                      // port 2323
//   con.begin("long-random-password", "myapp");  // mDNS name myapp.local; Wi-Fi may come up later
//   con.poll();                          // every loop()
//   con.println("hello");                // print like Serial
//   int c = con.read();                  // typed input, or -1

#include <Arduino.h>
#include <IPAddress.h>

class WifiSerial : public Stream {
 public:
  static const uint16_t DEFAULT_PORT = 2323;

  explicit WifiSerial(uint16_t port = DEFAULT_PORT, size_t txBufferSize = 4096);
  ~WifiSerial();
  WifiSerial(const WifiSerial&) = delete;
  WifiSerial& operator=(const WifiSerial&) = delete;

  // Starts listening once Wi-Fi is connected (checked in poll()). password must not be empty.
  // If hostname is given, starts mDNS as <hostname>.local and advertises _m5link._tcp.
  // Returns false if password is empty or the send buffer cannot be allocated.
  bool begin(const char* password, const char* hostname = nullptr);
  // Closes all sockets. begin() may be called again.
  void end();
  // Call every loop(). Accepts and authenticates computers, sends buffered output, reads input.
  void poll();
  // True while an authenticated computer is connected.
  bool connected() const { return client >= 0; }
  // Address of the connected computer.
  IPAddress remoteIP() const { return remote; }
  // Bytes dropped so far because the send buffer was full.
  size_t dropped() const { return droppedBytes; }

  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t n) override;
  // Sends what the socket accepts now; never waits.
  void flush() override;
  using Print::write;

 private:
  uint16_t port;
  String password;
  String hostname;
  bool begun = false;
  int listener = -1;
  int client = -1;
  IPAddress remote;
  // Connection still proving the password
  int pending = -1;
  IPAddress pendingIP;
  uint32_t pendingSince = 0;
  char nonce[33];
  String pendingLine;
  uint32_t lockedUntil = 0;
  // Output waiting for the socket
  uint8_t* tx = nullptr;
  size_t txSize;
  size_t txStart = 0, txLen = 0;
  size_t droppedBytes = 0;
  // Input not read yet
  static const size_t RX = 256;
  uint8_t rx[RX];
  size_t rxStart = 0, rxLen = 0;

  bool listen();
  void accept();
  void handshake();
  void verify(const String& line, const uint8_t* rest, size_t restLen);
  void drain();
  void fill();
  void pushRx(const uint8_t* data, size_t n);
  void closeClient();
  void closePending();
};
