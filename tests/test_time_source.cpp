// Host tests for esphome/common/time_source.h (Time Source diagnostic) and esphome/common/rtc_ds3231.h
// (DS3231 codec and plausibility check), issue #29.
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_time_source.cpp -o /tmp/tst && /tmp/tst
#include "rtc_ds3231.h"
#include "time_source.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static bool is(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

static void test_time_source() {
  using namespace time_source;
  uint32_t s = NEVER;
  // Power-on, no internet, no RTC.
  CHECK(is(classify(false, s, 5000, false), "none"));
  // The DS3231 set the clock at boot.
  CHECK(is(classify(true, s, 5000, true), "rtc"));
  // A software restart kept the clock (ESP32 RTC timer), no sync yet.
  CHECK(is(classify(true, s, 5000, false), "held"));

  // SNTP synced at 20 s.
  s = stamp(20000);
  CHECK(s != NEVER && s != LONG_AGO);
  CHECK(stamp(0) != NEVER && stamp(0) != LONG_AGO && stamp(1) != LONG_AGO);
  CHECK(is(classify(true, s, 20000 + 59 * 60 * 1000, false), "sntp"));
  CHECK(is(classify(true, s, 20000 + 59 * 60 * 1000, true), "sntp"));   // SNTP outranks the RTC
  // Internet lost: an hour without a sync is "held", and stays held (ages out to LONG_AGO).
  CHECK(is(classify(true, s, 20002 + STALE_MS, false), "held"));
  CHECK(s == LONG_AGO);
  CHECK(is(classify(true, s, 20000 + STALE_MS + 1000, true), "held"));   // RTC at boot, then SNTP, then lost
  // millis() wraps 49.7 days later: still held, not "sntp".
  CHECK(is(classify(true, s, 20000 + 5000, false), "held"));
  // A new sync is fresh again.
  s = stamp(123456);
  CHECK(is(classify(true, s, 123456 + 1000, false), "sntp"));
  // Fresh across the millis() wrap.
  s = stamp(0xFFFFF000u);
  CHECK(is(classify(true, s, 0x00001000u, false), "sntp"));
  // A clock that went invalid reads none whatever the stamp says.
  CHECK(is(classify(false, s, 0x00001000u, false), "none"));
}

static void test_codec() {
  using namespace rtc_ds3231;
  // 2026-10-11 14:05:09 UTC = 1791727509, a Sunday.
  const uint32_t T = 1791727509UL;
  uint8_t r[7];
  encode(T, r);
  CHECK(r[0] == 0x09 && r[1] == 0x05 && r[2] == 0x14);
  CHECK(r[3] == 1);   // Sunday
  CHECK(r[4] == 0x11 && r[5] == 0x10 && r[6] == 0x26);
  CHECK(decode(r) == T);

  // Epoch 0 is a Thursday (5); round trips across leap days, month and year ends.
  encode(0, r);
  CHECK(r[3] == 5 && r[4] == 0x01 && r[5] == 0x01 && r[6] == 0x70);
  const uint32_t samples[] = {951782400UL /* 2000-02-29 */, 1709208000UL /* 2024-02-29 12:00 */,
                              1798761599UL /* 2026-12-31 23:59:59 */, 1798761600UL /* 2027-01-01 */,
                              4102444800UL /* 2100-01-01, century bit */, 4294967295UL};
  for (uint32_t t : samples) {
    encode(t, r);
    if (t >= 946684800UL) CHECK(decode(r) == t);   // the DS3231 counts from 2000
  }
  encode(4102444800UL, r);
  CHECK(r[5] & 0x80);

  // Invalid registers decode to 0.
  const uint8_t reset[7] = {0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00};   // power-up default 2000-01-01
  CHECK(decode(reset) == 946684800UL);   // valid, but below MIN_EPOCH (see plausible)
  uint8_t bad[7];
  std::memcpy(bad, reset, 7);
  bad[0] = 0x5A;   // not BCD
  CHECK(decode(bad) == 0);
  std::memcpy(bad, reset, 7);
  bad[2] = 0x40 | 0x12;   // 12-hour mode
  CHECK(decode(bad) == 0);
  std::memcpy(bad, reset, 7);
  bad[4] = 0x30; bad[5] = 0x02; bad[6] = 0x26;   // 2026-02-30
  CHECK(decode(bad) == 0);
  bad[4] = 0x29;   // 2026-02-29 (not a leap year)
  CHECK(decode(bad) == 0);
  bad[6] = 0x28;   // 2028-02-29
  CHECK(decode(bad) != 0);
  const uint8_t floating[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // bus floating
  CHECK(decode(floating) == 0);
}

static void test_plausible() {
  using namespace rtc_ds3231;
  const uint32_t T = 1791727509UL;
  CHECK(plausible(T, false, 0));                       // last_alive unknown
  CHECK(plausible(T, false, T - 3 * 3600));            // 3 h power cut
  CHECK(plausible(T, false, T + BACKWARDS_SLACK_S));   // within the slack
  CHECK(!plausible(T, false, T + BACKWARDS_SLACK_S + 1));   // earlier than the chamber was last alive
  CHECK(!plausible(T, true, 0));                       // oscillator stopped
  CHECK(!plausible(946684800UL, false, 0));            // never set (2000-01-01)
  CHECK(!plausible(0, false, 0));                      // didn't decode
}

int main() {
  test_time_source();
  test_codec();
  test_plausible();
  if (failures) {
    std::printf("time_source: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("time_source: all passed\n");
  return 0;
}
