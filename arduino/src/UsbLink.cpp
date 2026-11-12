// UsbLink（UsbLink.h）。

#include "UsbLink.h"

#include <ArduinoJson.h>
#include <mbedtls/base64.h>

static const char PREFIX[] = "\x1eUL1 ";
static const size_t MAX_LINE = 4096;

void UsbLink::begin(Stream& s, uint32_t helloEveryMs) {
  io = &s;
  helloEvery = helloEveryMs;
}

bool UsbLink::connected() const { return helloGotAt && millis() - helloGotAt < helloEvery * 3; }

int64_t UsbLink::hostTime() const {
  if (!hostEpoch) return 0;
  return hostEpoch + (millis() - hostEpochAt) / 1000;
}

void UsbLink::pushChar(char c) {
  int next = (charHead + 1) % CHARS;
  if (next == charTail) return;  // あふれたら捨てる
  chars[charHead] = c;
  charHead = next;
}

int UsbLink::read() {
  if (io) pump();
  if (charHead == charTail) return -1;
  char c = chars[charTail];
  charTail = (charTail + 1) % CHARS;
  return (uint8_t)c;
}

void UsbLink::send(const String& json) {
  String frame = String(PREFIX) + json + "\n";
  io->write((const uint8_t*)frame.c_str(), frame.length());  // 一度に書く（ログの途中に割り込まれないように）
  io->flush();
}

void UsbLink::poll() {
  if (!io) return;
  if (!helloSentAt || millis() - helloSentAt >= helloEvery) {
    helloSentAt = millis();
    send(String("{\"v\":1,\"id\":") + nextId++ + ",\"op\":\"hello\"}");
  }
  pump();
}

// 届いているものを読む。0x1E からの一行はフレーム、ほかは文字としてためる。
void UsbLink::pump() {
  while (io->available()) {
    int c = io->read();
    if (c < 0) break;
    if (!inFrame) {
      if (c == 0x1E) {
        inFrame = true;
        line = "";
      } else {
        pushChar((char)c);
      }
      continue;
    }
    if (c == '\n') {
      inFrame = false;
      if (line.startsWith("UL1 ")) handleFrame(line.substring(4));
      line = "";
    } else if (line.length() < MAX_LINE) {
      line += (char)c;
    } else {
      inFrame = false;  // 長すぎる。壊れているので捨てる
    }
  }
}

// 本文を n バイト読む（buf が nullptr なら読み捨てる）。chunk バイトごとと最後に ack を返す。
// host は ack を待ってから次を送る。ESP32-S3 の USB シリアルは、受け取りが追いつかないと黙って捨てるので。
bool UsbLink::readBody(uint8_t* buf, size_t n, uint32_t id, size_t chunk, uint32_t timeoutMs) {
  uint8_t sink[256];
  size_t got = 0, acked = 0;
  uint32_t deadline = millis() + timeoutMs;
  while (got < n && (int32_t)(deadline - millis()) > 0) {
    int a = io->available();
    if (a <= 0) {
      delay(1);
      continue;
    }
    size_t want = min((size_t)a, n - got);
    if (chunk) want = min(want, chunk - (got - acked));
    if (!buf) want = min(want, sizeof sink);
    size_t k = io->readBytes(buf ? buf + got : sink, want);
    got += k;
    if (chunk && (got - acked == chunk || got == n)) {
      acked = got;
      send(String("{\"v\":1,\"id\":") + id + ",\"op\":\"ack\",\"n\":" + (unsigned)got + "}");
      deadline = millis() + timeoutMs;  // 次のかたまりを待つ
    }
  }
  return got == n;
}

void UsbLink::handleFrame(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  uint32_t id = doc["id"] | 0;
  size_t n = doc["n"] | 0;
  size_t chunk = doc["c"] | 0;
  if (doc["t"].is<int64_t>()) {
    hostEpoch = doc["t"].as<int64_t>();
    hostEpochAt = millis();
  }
  const char* op = doc["op"] | "";
  if (!strcmp(op, "hello")) helloGotAt = millis();
  // 本文（n バイト）は待っている返事のときだけ取っておき、ほかは読み捨てる
  bool mine = waitingId && id == waitingId && reply;
  uint8_t* body = nullptr;
  if (n) {
    body = mine ? (uint8_t*)ps_malloc(n) : nullptr;
    if (!readBody(body, n, id, chunk, 5000) && body) {  // 途中で切れた
      free(body);
      body = nullptr;
      mine = false;
    }
  }
  if (!mine) return;
  helloGotAt = millis();  // 返事が来たのも、つながっている証拠
  reply->status = doc["s"] | 0;
  reply->body = body;
  reply->length = body ? n : 0;
  reply->contentType = (const char*)(doc["h"]["content-type"] | "");
  gotReply = true;
}

bool UsbLink::request(const char* method, const String& url, const uint8_t* body, size_t length,
                      const char* contentType, Response& out, uint32_t timeoutMs) {
  if (!io) return false;
  out = Response();
  JsonDocument doc;
  uint32_t id = nextId++;
  doc["v"] = 1;
  doc["id"] = id;
  doc["op"] = "http";
  doc["m"] = method;
  doc["u"] = url;
  if (contentType) doc["h"]["content-type"] = contentType;
  if (body && length) {
    size_t outLen = 0;
    mbedtls_base64_encode(nullptr, 0, &outLen, body, length);
    char* b64 = (char*)ps_malloc(outLen + 1);
    if (!b64) return false;
    mbedtls_base64_encode((unsigned char*)b64, outLen + 1, &outLen, body, length);
    b64[outLen] = 0;
    doc["b"] = (const char*)b64;  // JsonDocument にコピーされる
    free(b64);
  }
  String json;
  serializeJson(doc, json);

  waitingId = id;
  gotReply = false;
  reply = &out;
  send(json);
  uint32_t deadline = millis() + timeoutMs;
  while (!gotReply && (int32_t)(deadline - millis()) > 0) {
    pump();
    if (!gotReply) delay(1);
  }
  waitingId = 0;
  reply = nullptr;
  return gotReply;
}
