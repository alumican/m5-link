#pragma once
// UsbLink: USB のシリアル越しに、つないだパソコン（usblink の host）に HTTP を頼む。
// Wi-Fi がない場所でも、パソコンがネットにつながっていれば、ネットの API を呼んだり、パソコンの上のサーバーを
// 呼んだりできる。パソコンの時計ももらえる。ログ（Serial.print）は同じシリアルにそのまま流してよい。
//
// フレームは 0x1E "UL1 " で始まる一行の JSON（README.md）。ログと混ざっても見分けられる。
// ESP32 用（ESP-IDF の mbedtls の base64 と、ArduinoJson を使う）。
//
//   UsbLink link;
//   link.begin(Serial);                     // setup()。Serial.setRxBufferSize() は大きめに（4096 など）
//   link.poll();                            // loop() で毎回
//   int c = link.read();                    // フレームでない受け取った文字（開発用のコマンドなど）
//   UsbLink::Response r;
//   if (link.connected() && link.request("GET", "https://api.example.com/x", nullptr, 0, nullptr, r)) {
//     ... r.status, r.body, r.length ...
//     free(r.body);
//   }

#include <Arduino.h>

class UsbLink {
 public:
  struct Response {
    int status = 0;           // HTTP の status。host まで届かなければ 0
    uint8_t* body = nullptr;  // ps_malloc。使ったら free()
    size_t length = 0;
    String contentType;
  };

  void begin(Stream& io, uint32_t helloEveryMs = 3000);
  // loop() から毎回呼ぶ。あいさつを送り、届いたものを読む
  void poll();
  // host から最近（あいさつの間隔の 3 倍以内）返事があったか
  bool connected() const;
  // host の時計（UNIX 時刻、秒）。まだ分からなければ 0
  int64_t hostTime() const;
  // フレームでない受け取った一文字。なければ -1。poll() と request() のあいだに届いたものもためておく
  int read();
  // HTTP を頼んで、返事を待つ。返事が来れば true（status は out.status）
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
  // 受け取りの途中
  String line;
  bool inFrame = false;
  // フレームでない文字
  static const int CHARS = 64;
  char chars[CHARS];
  int charHead = 0, charTail = 0;
  // request() が待っている返事
  uint32_t waitingId = 0;
  bool gotReply = false;
  Response* reply = nullptr;

  void send(const String& json);
  void pump();
  void handleFrame(const String& json);
  bool readBody(uint8_t* buf, size_t n, uint32_t id, size_t chunk, uint32_t timeoutMs);
  void pushChar(char c);
};
