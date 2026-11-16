// UsbLink over an in-memory Stream that plays the m5-link host: frames, logs, replies and acks.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <unity.h>

#include <string>
#include <vector>

#include "UsbLink.h"

static const std::string PREFIX = "\x1eUL1 ";

// A Stream whose input is a byte string, recording everything written. If a responder is set, it sees each
// complete line written and may append input, as the host would reply.
class FakeHost : public Stream {
 public:
  std::string in;
  size_t pos = 0;
  std::string out;
  std::vector<std::string> lines;
  void (*responder)(FakeHost&, const std::string& line) = nullptr;

  int available() override { return in.size() - pos; }
  int read() override { return pos < in.size() ? (uint8_t)in[pos++] : -1; }
  int peek() override { return pos < in.size() ? (uint8_t)in[pos] : -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t n) override {
    out.append((const char*)data, n);
    for (size_t i; (i = out.find('\n', lineStart)) != std::string::npos; lineStart = i + 1) {
      lines.push_back(out.substr(lineStart, i - lineStart));
      if (responder) responder(*this, lines.back());
    }
    return n;
  }
  void flush() override {}
  using Print::write;

  // Frames written by the device, parsed
  std::vector<JsonDocument> frames() const {
    std::vector<JsonDocument> docs;
    for (const std::string& l : lines) {
      if (l.compare(0, PREFIX.size(), PREFIX)) continue;
      JsonDocument doc;
      deserializeJson(doc, l.c_str() + PREFIX.size());
      docs.push_back(doc);
    }
    return docs;
  }

 private:
  size_t lineStart = 0;
};

static std::string hostFrame(const char* json) { return PREFIX + json + "\n"; }

static std::string replyFrame(uint32_t id, int status, size_t n, const char* contentType) {
  char buf[200];
  snprintf(buf, sizeof buf,
           "{\"v\":1,\"id\":%u,\"op\":\"http\",\"t\":1793900000,\"s\":%d,\"h\":{\"content-type\":\"%s\"},\"n\":%u,"
           "\"c\":4096}",
           (unsigned)id, status, contentType, (unsigned)n);
  return hostFrame(buf);
}

static std::string pattern(size_t n) {
  std::string s(n, 0);
  for (size_t i = 0; i < n; i++) s[i] = (char)(i * 7 + i / 256);
  return s;
}

static FakeHost* host;
static UsbLink* usb;

void setUp() {
  host = new FakeHost();
  usb = new UsbLink();
  usb->begin(*host);
}

void tearDown() {
  delete usb;
  delete host;
}

void test_poll_sends_hello_once_per_interval() {
  usb->poll();
  usb->poll();
  TEST_ASSERT_EQUAL_STRING("\x1eUL1 {\"v\":1,\"id\":1,\"op\":\"hello\"}\n", host->out.c_str());
}

void test_not_connected_until_the_host_replies() {
  usb->poll();
  TEST_ASSERT_FALSE(usb->connected());
  TEST_ASSERT_EQUAL(0, (int)usb->hostTime());
  host->in = hostFrame("{\"v\":1,\"id\":1,\"op\":\"hello\",\"t\":1793900000,\"s\":200,\"n\":0,\"c\":4096}");
  usb->poll();
  TEST_ASSERT_TRUE(usb->connected());
  TEST_ASSERT_TRUE(usb->hostTime() >= 1793900000 && usb->hostTime() < 1793900010);
}

void test_connection_expires_after_three_intervals() {
  usb->begin(*host, 20);
  host->in = hostFrame("{\"v\":1,\"id\":1,\"op\":\"hello\",\"s\":200,\"n\":0}");
  usb->poll();
  TEST_ASSERT_TRUE(usb->connected());
  delay(70);
  TEST_ASSERT_FALSE(usb->connected());
}

void test_read_returns_only_bytes_outside_frames() {
  host->in = "ab" + hostFrame("{\"v\":1,\"id\":1,\"op\":\"hello\",\"s\":200,\"n\":0}") + "cd";
  std::string got;
  for (int c; (c = usb->read()) >= 0;) got += (char)c;
  TEST_ASSERT_EQUAL_STRING("abcd", got.c_str());
  TEST_ASSERT_TRUE(usb->connected());
  TEST_ASSERT_EQUAL(-1, usb->peek());
  TEST_ASSERT_EQUAL(0, usb->available());
}

void test_lines_starting_0x1e_without_ul1_are_dropped() {
  host->in = "\x1eXYZ {\"op\":\"hello\"}\nok";
  TEST_ASSERT_EQUAL(2, usb->available());
  TEST_ASSERT_EQUAL('o', usb->read());
  TEST_ASSERT_EQUAL('k', usb->read());
  TEST_ASSERT_FALSE(usb->connected());
}

void test_broken_json_is_ignored() {
  host->in = hostFrame("{\"op\":\"hello\"") + hostFrame("{\"v\":1,\"id\":1,\"op\":\"hello\",\"s\":200,\"n\":0}");
  usb->poll();
  TEST_ASSERT_TRUE(usb->connected());
}

void test_overlong_frames_are_dropped_whole() {
  host->in = "a" + PREFIX + std::string(5000, 'x') + "\n" + "b" +
             hostFrame("{\"v\":1,\"id\":1,\"op\":\"hello\",\"s\":200,\"n\":0}") + "c";
  usb->poll();
  TEST_ASSERT_TRUE(usb->connected());
  std::string got;
  for (int c; (c = usb->read()) >= 0;) got += (char)c;
  TEST_ASSERT_EQUAL_STRING("abc", got.c_str());  // nothing of the overlong line reaches read()
}

void test_overlong_valid_frame_is_not_handled() {
  // A hello padded past the limit is broken, even though it starts like a valid frame
  host->in = hostFrame(("{\"v\":1,\"id\":1,\"op\":\"hello\",\"s\":200,\"n\":0,\"pad\":\"" + std::string(5000, 'x') +
                        "\"}").c_str());
  usb->poll();
  TEST_ASSERT_FALSE(usb->connected());
  TEST_ASSERT_EQUAL(0, usb->available());
}

void test_write_goes_straight_to_the_port() {
  usb->print("temp ");
  usb->println(18);
  TEST_ASSERT_EQUAL_STRING("temp 18\r\n", host->out.c_str());
}

// Answers each http request with a 10000-byte body, preceded by a reply for another id that must be discarded.
static void replyWithBody(FakeHost& h, const std::string& line) {
  if (line.compare(0, PREFIX.size(), PREFIX)) return;
  JsonDocument doc;
  deserializeJson(doc, line.c_str() + PREFIX.size());
  if (strcmp(doc["op"] | "", "http")) return;
  uint32_t id = doc["id"];
  h.in += replyFrame(id + 100, 200, 5, "text/plain") + "stray";
  h.in += replyFrame(id, 200, 10000, "audio/wav") + pattern(10000);
}

void test_request_round_trip_with_acks() {
  host->responder = replyWithBody;
  const char body[] = "It's 18\xc2\xb0" "C.";
  UsbLink::Response r;
  TEST_ASSERT_TRUE(usb->request("POST", "http://localhost:8000/api/hello", (const uint8_t*)body, strlen(body),
                                "text/plain", r, 2000));
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_EQUAL_STRING("audio/wav", r.contentType.c_str());
  TEST_ASSERT_EQUAL(10000, (int)r.length);
  TEST_ASSERT_EQUAL_MEMORY(pattern(10000).data(), r.body, 10000);
  free(r.body);
  TEST_ASSERT_TRUE(usb->connected());  // a reply proves the link too
  TEST_ASSERT_EQUAL(0, usb->available());

  std::vector<JsonDocument> frames = host->frames();
  TEST_ASSERT_EQUAL(5, (int)frames.size());
  JsonDocument& req = frames[0];
  uint32_t id = req["id"];
  TEST_ASSERT_EQUAL_STRING("http", req["op"]);
  TEST_ASSERT_EQUAL_STRING("POST", req["m"]);
  TEST_ASSERT_EQUAL_STRING("http://localhost:8000/api/hello", req["u"]);
  TEST_ASSERT_EQUAL_STRING("text/plain", req["h"]["content-type"]);
  TEST_ASSERT_EQUAL_STRING("SXQncyAxOMKwQy4=", req["b"]);  // base64 of the body
  // The stray reply is acked and discarded; the awaited one is acked every 4096 bytes and at the end
  const uint32_t ids[] = {id + 100, id, id, id};
  const int ns[] = {5, 4096, 8192, 10000};
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL_STRING("ack", frames[i + 1]["op"]);
    TEST_ASSERT_EQUAL(ids[i], (uint32_t)frames[i + 1]["id"]);
    TEST_ASSERT_EQUAL(ns[i], (int)frames[i + 1]["n"]);
  }
}

static void replyEmpty(FakeHost& h, const std::string& line) {
  if (line.find("\"op\":\"http\"") == std::string::npos) return;
  JsonDocument doc;
  deserializeJson(doc, line.c_str() + PREFIX.size());
  h.in += replyFrame(doc["id"], 403, 0, "text/plain");
}

void test_request_without_body_or_content_type() {
  host->responder = replyEmpty;
  UsbLink::Response r;
  TEST_ASSERT_TRUE(usb->request("GET", "https://example.com/", nullptr, 0, nullptr, r, 2000));
  TEST_ASSERT_EQUAL(403, r.status);
  TEST_ASSERT_NULL(r.body);
  TEST_ASSERT_EQUAL(0, (int)r.length);
  std::vector<JsonDocument> frames = host->frames();
  TEST_ASSERT_EQUAL(1, (int)frames.size());  // no ack for an empty body
  TEST_ASSERT_FALSE(frames[0]["b"].is<const char*>());
  TEST_ASSERT_FALSE(frames[0]["h"].is<JsonObject>());
}

void test_request_times_out_without_a_reply() {
  UsbLink::Response r;
  uint32_t t0 = millis();
  TEST_ASSERT_FALSE(usb->request("GET", "http://localhost/", nullptr, 0, nullptr, r, 50));
  TEST_ASSERT_TRUE(millis() - t0 >= 50);
  TEST_ASSERT_EQUAL(0, r.status);
  TEST_ASSERT_NULL(r.body);
}

void test_request_ids_increase() {
  host->responder = replyEmpty;
  UsbLink::Response r;
  usb->poll();  // hello takes id 1
  usb->request("GET", "http://localhost/a", nullptr, 0, nullptr, r, 2000);
  usb->request("GET", "http://localhost/b", nullptr, 0, nullptr, r, 2000);
  std::vector<JsonDocument> frames = host->frames();
  TEST_ASSERT_EQUAL(3, (int)frames.size());
  for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL(i + 1, (int)frames[i]["id"]);
}

void setup() {
  delay(2000);  // lets the USB serial come up so the first results are not lost
  UNITY_BEGIN();
  RUN_TEST(test_poll_sends_hello_once_per_interval);
  RUN_TEST(test_not_connected_until_the_host_replies);
  RUN_TEST(test_connection_expires_after_three_intervals);
  RUN_TEST(test_read_returns_only_bytes_outside_frames);
  RUN_TEST(test_lines_starting_0x1e_without_ul1_are_dropped);
  RUN_TEST(test_broken_json_is_ignored);
  RUN_TEST(test_overlong_frames_are_dropped_whole);
  RUN_TEST(test_overlong_valid_frame_is_not_handled);
  RUN_TEST(test_write_goes_straight_to_the_port);
  RUN_TEST(test_request_round_trip_with_acks);
  RUN_TEST(test_request_without_body_or_content_type);
  RUN_TEST(test_request_times_out_without_a_reply);
  RUN_TEST(test_request_ids_increase);
  UNITY_END();
}

void loop() {}
