#include "WifiSerial.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <mbedtls/md.h>

static const char PREFIX[] = "\x1eUL1 ";
static const uint32_t AUTH_TIMEOUT_MS = 5000;
static const uint32_t LOCKOUT_MS = 1000;  // after a wrong password, refuse connections this long
static const size_t MAX_AUTH_LINE = 256;

static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }

static void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

// Notices a computer that vanished (sleep, Wi-Fi loss) within about 11 s, so a new one is not needed to clear it.
static void keepAlive(int fd) {
  int one = 1, idle = 5, interval = 2, count = 3;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
}

// Sends a short frame on a socket whose send buffer is still empty.
static void sendFrame(int fd, const String& json) {
  String frame = String(PREFIX) + json + "\n";
  send(fd, frame.c_str(), frame.length(), MSG_DONTWAIT);
}

static void hex(const uint8_t* in, size_t n, char* out) {
  static const char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = DIGITS[in[i] >> 4];
    out[i * 2 + 1] = DIGITS[in[i] & 15];
  }
  out[n * 2] = 0;
}

WifiSerial::WifiSerial(uint16_t port, size_t txBufferSize) : port(port), txSize(txBufferSize) { nonce[0] = 0; }

WifiSerial::~WifiSerial() {
  end();
  free(tx);
}

bool WifiSerial::begin(const char* pw, const char* name) {
  end();
  if (!pw || !*pw) return false;
  if (!tx && txSize) {
    tx = (uint8_t*)malloc(txSize);
    if (!tx) return false;
  }
  password = pw;
  hostname = name ? name : "";
  begun = true;
  return true;
}

void WifiSerial::end() {
  closeClient();
  closePending();
  if (listener >= 0) close(listener);
  listener = -1;
  begun = false;
  rxStart = rxLen = 0;
}

bool WifiSerial::listen() {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 || ::listen(fd, 2) < 0) {
    close(fd);
    return false;
  }
  setNonBlocking(fd);
  listener = fd;
  if (hostname.length()) {
    MDNS.begin(hostname.c_str());  // may already be running, e.g. started by ArduinoOTA with the same name
    MDNS.addService("m5link", "tcp", port);
  }
  return true;
}

void WifiSerial::poll() {
  if (!begun) return;
  if (listener < 0 && (WiFi.status() != WL_CONNECTED || !listen())) return;
  accept();
  if (pending >= 0) handshake();
  drain();
  fill();
}

void WifiSerial::accept() {
  struct sockaddr_in addr;
  socklen_t len = sizeof addr;
  int fd = ::accept(listener, (struct sockaddr*)&addr, &len);
  if (fd < 0) return;
  if ((int32_t)(lockedUntil - millis()) > 0) {
    close(fd);
    return;
  }
  closePending();  // the newest connection gets to try
  setNonBlocking(fd);
  keepAlive(fd);
  pending = fd;
  pendingIP = IPAddress(addr.sin_addr.s_addr);
  pendingSince = millis();
  pendingLine = "";
  uint8_t bytes[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(bytes + i, &r, 4);
  }
  hex(bytes, sizeof bytes, nonce);
  sendFrame(fd, String("{\"v\":1,\"op\":\"auth\",\"nonce\":\"") + nonce + "\"}");
}

void WifiSerial::handshake() {
  if (millis() - pendingSince > AUTH_TIMEOUT_MS) {
    closePending();
    return;
  }
  uint8_t buf[64];
  int n = recv(pending, buf, sizeof buf, MSG_DONTWAIT);
  if (n == 0 || (n < 0 && !wouldBlock())) {
    closePending();
    return;
  }
  for (int i = 0; i < n; i++) {
    if (buf[i] == '\n') {
      verify(pendingLine, buf + i + 1, n - i - 1);
      return;
    }
    if (pendingLine.length() >= MAX_AUTH_LINE) {
      closePending();
      return;
    }
    pendingLine += (char)buf[i];
  }
}

void WifiSerial::verify(const String& line, const uint8_t* rest, size_t restLen) {
  bool ok = false;
  JsonDocument doc;
  if (line.startsWith(PREFIX) && !deserializeJson(doc, line.c_str() + sizeof PREFIX - 1)) {
    const char* mac = doc["mac"] | "";
    uint8_t digest[32];
    char expected[65];
    const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!mbedtls_md_hmac(sha256, (const uint8_t*)password.c_str(), password.length(), (const uint8_t*)nonce,
                         strlen(nonce), digest)) {
      hex(digest, sizeof digest, expected);
      if (strlen(mac) == 64) {
        uint8_t diff = 0;  // constant time
        for (int i = 0; i < 64; i++) diff |= mac[i] ^ expected[i];
        ok = !diff;
      }
    }
  }
  if (!ok) {
    sendFrame(pending, "{\"v\":1,\"op\":\"auth\",\"s\":401}");
    closePending();
    lockedUntil = millis() + LOCKOUT_MS;
    return;
  }
  // Output still buffered for a previous computer goes to the new one
  if (client >= 0) close(client);
  client = pending;
  remote = pendingIP;
  pending = -1;
  pendingLine = "";
  sendFrame(client, "{\"v\":1,\"op\":\"auth\",\"s\":200}");
  pushRx(rest, restLen);
}

void WifiSerial::closeClient() {
  if (client >= 0) close(client);
  client = -1;
  txStart = txLen = 0;
}

void WifiSerial::closePending() {
  if (pending >= 0) close(pending);
  pending = -1;
  pendingLine = "";
}

void WifiSerial::drain() {
  while (client >= 0 && txLen) {
    size_t chunk = min(txLen, txSize - txStart);
    int k = send(client, tx + txStart, chunk, MSG_DONTWAIT);
    if (k > 0) {
      txStart = (txStart + k) % txSize;
      txLen -= k;
    } else {
      if (k < 0 && !wouldBlock()) closeClient();
      return;
    }
  }
}

void WifiSerial::flush() { drain(); }

size_t WifiSerial::write(const uint8_t* data, size_t n) {
  if (client < 0) return n;  // nobody listening: discard
  drain();
  size_t sent = 0;
  if (!txLen) {  // nothing queued: try the socket first
    int k = send(client, data, n, MSG_DONTWAIT);
    if (k > 0) {
      sent = k;
    } else if (k < 0 && !wouldBlock()) {
      closeClient();
      return n;
    }
  }
  while (sent < n && txLen < txSize) {
    size_t end = (txStart + txLen) % txSize;
    size_t chunk = min(n - sent, min(txSize - txLen, txSize - end));
    memcpy(tx + end, data + sent, chunk);
    txLen += chunk;
    sent += chunk;
  }
  droppedBytes += n - sent;
  return n;
}

void WifiSerial::pushRx(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n && rxLen < RX; i++) {
    rx[(rxStart + rxLen) % RX] = data[i];
    rxLen++;
  }
}

void WifiSerial::fill() {
  while (client >= 0 && rxLen < RX) {
    uint8_t buf[64];
    int k = recv(client, buf, min(sizeof buf, RX - rxLen), MSG_DONTWAIT);
    if (k > 0) {
      pushRx(buf, k);
    } else {
      if (k == 0 || !wouldBlock()) closeClient();
      return;
    }
  }
}

int WifiSerial::available() {
  fill();
  return rxLen;
}

int WifiSerial::read() {
  fill();
  if (!rxLen) return -1;
  uint8_t c = rx[rxStart];
  rxStart = (rxStart + 1) % RX;
  rxLen--;
  return c;
}

int WifiSerial::peek() {
  fill();
  return rxLen ? rx[rxStart] : -1;
}
