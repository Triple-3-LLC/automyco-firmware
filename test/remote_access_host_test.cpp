// Host test for common/remote_access.h (the chamber side of remote access). Built and run by
// site/test/remote_test.py:
//   clang++ -std=c++17 -DTS_REMOTE_HOST_TEST remote_access_host_test.cpp && ./a.out
// It prints one sample body, which remote_test.py feeds through site/lambda/remote-core.mjs so the two
// ends are checked against each other.
#include "../common/remote_access.h"

#include <iostream>

using namespace ts_remote;

static int checks = 0, failures = 0;
#define CHECK(cond)                                                               \
  do {                                                                            \
    checks++;                                                                     \
    if (!(cond)) {                                                                \
      failures++;                                                                 \
      std::cerr << __FILE__ << ":" << __LINE__ << ": FAILED " #cond << std::endl; \
    }                                                                             \
  } while (0)

static Sample sample(uint32_t up) {
  Sample s;
  s.t = 1760000000 + up;
  s.up = up;
  s.temp_f = 70.44f;
  s.rh = 88.66f;
  s.sp_rh = 90;
  s.sp_t = 70;
  s.fan1 = 30;
  s.climate = s.atomizer = 1;
  s.alarms = A_LOW_WATER | A_FAN2;
  set_state(s, "Humidifying");
  return s;
}

static void test_ring() {
  Ring r;
  for (uint32_t i = 0; i < RING + 5; i++) r.push(sample(i));
  CHECK(r.size() == RING);
  CHECK(r.dropped() == 5);
  CHECK(r.at(0).up == 5);  // the oldest five went first
  CHECK(r.at(RING - 1).up == RING + 4);
  r.drop_front(10);
  CHECK(r.size() == RING - 10 && r.at(0).up == 15);
  r.drop_front(1000);
  CHECK(r.size() == 0);
  r.push(sample(1));
  CHECK(r.size() == 1 && r.at(0).up == 1);
}

static void test_json() {
  Sample s = sample(42);
  std::string j = sample_json(s);
  CHECK(j.find("\"t\":1760000042") != std::string::npos);
  CHECK(j.find("\"tf\":70.4,") != std::string::npos);
  CHECK(j.find("\"rh\":88.7,") != std::string::npos);
  CHECK(j.find("\"a\":65,") != std::string::npos);
  CHECK(j.find("\"st\":\"Humidifying\"") != std::string::npos);
  s.temp_f = NAN;
  s.t = 0;
  j = sample_json(s);
  CHECK(j.find("\"tf\":null") != std::string::npos);
  CHECK(j.find("\"t\":") == std::string::npos);
  set_state(s, "bad \"quote\\ and\nnewline that is far too long to keep");
  CHECK(strlen(s.state) == STATE_LEN - 1);
  CHECK(strchr(s.state, '"') == nullptr && strchr(s.state, '\\') == nullptr && strchr(s.state, '\n') == nullptr);
  char preset[33];
  json_safe(preset, sizeof(preset), "Oyster \"Blue\" \\ 2");
  CHECK(std::string(preset) == "Oyster ?Blue? ? 2");
  json_safe(preset, sizeof(preset), nullptr);
  CHECK(preset[0] == '\0');

  Ring r;
  for (uint32_t i = 0; i < 20; i++) r.push(sample(i));
  size_t taken = 0;
  std::string body = telemetry_json(r, 99, "\"v\":\"1.0.1\"", taken);
  CHECK(taken == MAX_PER_POST);
  CHECK(body.rfind("{\"schema\":1,\"up\":99,\"dropped\":0,\"v\":\"1.0.1\",\"s\":[", 0) == 0);
  CHECK(body.size() < 3000);
}

static void test_outcomes() {
  CHECK(classify(200) == OK && classify(204) == OK);
  CHECK(classify(403) == OFF);
  CHECK(classify(404) == UNPAIRED && classify(410) == UNPAIRED);
  CHECK(classify(401) == AUTH);
  CHECK(classify(400) == REJECTED && classify(413) == REJECTED);
  CHECK(classify(-1) == RETRY && classify(429) == RETRY && classify(503) == RETRY);
  CHECK(retry_delay_s(OK, 300, 0) == 300);
  CHECK(retry_delay_s(OFF, 300, 0) == 3600);
  CHECK(retry_delay_s(UNPAIRED, 300, 0) == 0);
  CHECK(retry_delay_s(RETRY, 300, 1) == 60);
  CHECK(retry_delay_s(RETRY, 300, 2) == 120);
  CHECK(retry_delay_s(RETRY, 300, 6) == 1920);
  CHECK(retry_delay_s(RETRY, 300, 7) == 3600);
  CHECK(retry_delay_s(RETRY, 300, 1000) == 3600);
}

static void test_camera_line() {
  const std::string on = camera_line("https://beezy.ninja", "0123456789abcdef0123456789abcdef", std::string(64, 'a').c_str());
  CHECK(on == "{\"cmd\":\"remote\",\"url\":\"https://beezy.ninja\",\"id\":\"0123456789abcdef0123456789abcdef\",\"key\":\"" +
                  std::string(64, 'a') + "\"}");
  CHECK(on.find('\n') == std::string::npos);  // one line on the wire
  CHECK(camera_line(nullptr, nullptr, nullptr) == "{\"cmd\":\"remote\",\"off\":true}");
}

static void test_acks_and_clamp() {
  Acks q;
  size_t n = 0;
  CHECK(q.take_for_post(n).empty() && n == 0);
  q.add("0011aabb", "ok");
  q.add("bad id", "refused: \"quoted\" \\ text");
  std::string part = q.take_for_post(n);
  CHECK(n == 2);
  CHECK(part == ",\"ack\":[{\"i\":\"0011aabb\",\"r\":\"ok\"},{\"i\":\"00000000\",\"r\":\"refused: ?quoted? ? text\"}]");
  q.add("deadbeef", "clamped to 99");  // arrives while the post is in flight
  q.delivered(n);                     // the post carrying the first two was accepted
  part = q.take_for_post(n);
  CHECK(n == 1 && part == ",\"ack\":[{\"i\":\"deadbeef\",\"r\":\"clamped to 99\"}]");
  for (int k = 0; k < 20; k++) q.add("0000000a", "ok");
  CHECK(q.size() == MAX_ACKS);  // bounded: the oldest go
  bool cl = false;
  CHECK(clamp_to(120, 50, 99, cl) == 99 && cl);
  CHECK(clamp_to(70, 55, 85, cl) == 70 && !cl);
  CHECK(clamp_to(-5, 0, 100, cl) == 0 && cl);
  CHECK(is_cmd_id("0011aabb") && !is_cmd_id("0011AABB") && !is_cmd_id("0011aab"));
}

static void test_codes() {
  CHECK(normalise_code("abcd-efgh") == "ABCDEFGH");
  CHECK(normalise_code(" 2345 6789 ") == "23456789");
  CHECK(normalise_code("ABCDEFG").empty());     // too short
  CHECK(normalise_code("ABCDEFGHJ").empty());   // too long
  CHECK(normalise_code("ABCDEF0O").empty());    // 0 and O aren't in the alphabet
  CHECK(normalise_code("ABCDEF1I").empty());
  CHECK(normalise_code("ABC\"EFGH").empty());
}

static void test_events() {
  char log[600] = {0};
  // Newest first, as log_event writes it.
  strcpy(log, "10-09 14:05 low water\n10-09 14:00 mist on\n10-09 13:00 grow started: Bob's \"secret\" tub (Blue Oyster)\n+12s boot\n");
  std::string o;
  uint32_t mark = append_events(o, log, sizeof(log), 0);
  CHECK(o == ",\"ev\":[\"10-09 14:05 low water\",\"10-09 14:00 mist on\",\"10-09 13:00 grow started\",\"+12s boot\"]");
  CHECK(mark == line_hash("10-09 14:05 low water", 21));
  // Nothing new: nothing appended, the marker stays.
  o.clear();
  CHECK(append_events(o, log, sizeof(log), mark) == mark);
  CHECK(o.empty());
  // One new line on top: only it goes.
  char log2[600] = {0};
  snprintf(log2, sizeof(log2), "10-09 14:10 grow ended: Bob's tub\n%s", log);
  o.clear();
  uint32_t mark2 = append_events(o, log2, sizeof(log2), mark);
  CHECK(o == ",\"ev\":[\"10-09 14:10 grow ended\"]");
  CHECK(mark2 != mark && mark2 != 0);
  // At most EV_MAX lines, each at most EV_LEN characters, JSON-safe; a full (unterminated) buffer is fine.
  std::string big;
  for (int i = 0; i < 15; i++) big += "+" + std::to_string(i) + "s " + std::string(100, 'x') + "\\\"\n";
  char full[600];
  memcpy(full, big.data(), sizeof(full));
  o.clear();
  append_events(o, full, sizeof(full), 0);
  size_t lines = 0;
  for (size_t p = o.find(",\"", 6); p != std::string::npos; p = o.find(",\"", p + 1)) lines++;
  CHECK(lines + 1 <= EV_MAX);
  CHECK(o.find('\\') == std::string::npos);
  CHECK(o.find(std::string(EV_LEN, 'x')) == std::string::npos);
  o.clear();
  CHECK(append_events(o, nullptr, 0, 7) == 7 && o.empty());
  CHECK(line_hash("", 0) != 0);
}

int main(int argc, char **argv) {
  test_ring();
  test_events();
  test_json();
  test_outcomes();
  test_codes();
  test_camera_line();
  test_acks_and_clamp();
  if (argc > 1 && std::string(argv[1]) == "--sample") {
    Ring r;
    r.push(sample(1));
    Sample s = sample(2);
    s.t = 0;
    s.rh = NAN;
    r.push(s);
    size_t taken = 0;
    std::string info = "\"v\":\"1.0.1\",\"preset\":\"Blue Oyster\",\"day\":3,\"rssi\":-61";
    append_events(info, "10-09 14:05 low water\n10-09 14:00 mist on\n", 600, 0);
    std::cout << telemetry_json(r, 120, info, taken)
              << std::endl;
  }
  std::cerr << checks << " checks, " << failures << " failures" << std::endl;
  return failures ? 1 : 0;
}
