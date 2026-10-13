#pragma once
// DS3231 real-time clock: register codec and the "can this time be trusted?" check, for the opt-in
// common/rtc-ds3231.yaml package (issue #29, docs/time-and-schedule.md). Host tests: tests/test_time_source.cpp.
//
// The RTC holds UTC in 24-hour mode, registers 0x00-0x06 (seconds, minutes, hours, weekday, date,
// month/century, year), BCD. Register 0x0F bit 7 (OSF) is set when the oscillator has stopped (no battery
// through a power cut, first power-up): the time is then garbage and must not set the clock.
#include <cstdint>
#include <ctime>
#include <sys/time.h>   // settimeofday(), used by the package

namespace rtc_ds3231 {

static const uint8_t ADDR = 0x68;
static const uint8_t REG_TIME = 0x00;
static const uint8_t REG_STATUS = 0x0F;
static const uint8_t OSF = 0x80;
// Earlier than any build of this firmware (2026-08-29): an RTC reading before it was never set.
static const uint32_t MIN_EPOCH = 1788000000UL;
// last_alive is the system clock, and the RTC was written from SNTP: allow a little disagreement.
static const uint32_t BACKWARDS_SLACK_S = 120;

inline bool bcd_ok(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }
inline uint8_t bcd2bin(uint8_t v) { return (uint8_t) ((v >> 4) * 10 + (v & 0x0F)); }
inline uint8_t bin2bcd(uint8_t v) { return (uint8_t) (((v / 10) << 4) | (v % 10)); }

inline bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
inline int days_in_month(int y, int m) {
  static const uint8_t D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : D[m - 1];
}

// Days since 1970-01-01 (H. Hinnant's days_from_civil).
inline int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Registers 0x00-0x06 to UTC epoch seconds; 0 if they don't hold a valid 24-hour time.
inline uint32_t decode(const uint8_t r[7]) {
  if (r[2] & 0x40) return 0;   // 12-hour mode: never written by this firmware
  const uint8_t s = r[0] & 0x7F, mi = r[1] & 0x7F, h = r[2] & 0x3F, d = r[4] & 0x3F, mo = r[5] & 0x1F, y = r[6];
  if (!bcd_ok(s) || !bcd_ok(mi) || !bcd_ok(h) || !bcd_ok(d) || !bcd_ok(mo) || !bcd_ok(y)) return 0;
  const int sec = bcd2bin(s), min = bcd2bin(mi), hour = bcd2bin(h), day = bcd2bin(d), month = bcd2bin(mo);
  const int year = 2000 + bcd2bin(y) + ((r[5] & 0x80) ? 100 : 0);
  if (sec > 59 || min > 59 || hour > 23 || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month))
    return 0;
  const int64_t t = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
  return t > 0 && t <= 0xFFFFFFFFLL ? (uint32_t) t : 0;
}

// UTC epoch seconds to registers 0x00-0x06 (24-hour mode, weekday 1 = Sunday, century bit for 2100+).
inline void encode(uint32_t epoch, uint8_t r[7]) {
  const int64_t days = epoch / 86400;
  const uint32_t rem = epoch % 86400;
  // H. Hinnant's civil_from_days
  const int64_t z = days + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int d = (int) (doy - (153 * mp + 2) / 5 + 1);
  const int m = (int) (mp < 10 ? mp + 3 : mp - 9);
  const int y = (int) (yoe + era * 400 + (m <= 2));
  r[0] = bin2bcd(rem % 60);
  r[1] = bin2bcd((rem / 60) % 60);
  r[2] = bin2bcd(rem / 3600);
  r[3] = (uint8_t) ((days + 4) % 7 + 1);   // 1970-01-01 was a Thursday (5)
  r[4] = bin2bcd(d);
  r[5] = (uint8_t) (bin2bcd(m) | (y >= 2100 ? 0x80 : 0));
  r[6] = bin2bcd((uint8_t) (y % 100));
}

// Whether a decoded RTC time may set the clock: the oscillator never stopped, it was set by this firmware
// era, and it isn't earlier than the last time the chamber knew it was alive (last_alive, 0 = unknown).
inline bool plausible(uint32_t rtc_epoch, bool osf, uint32_t last_alive) {
  if (osf || rtc_epoch < MIN_EPOCH) return false;
  if (last_alive != 0 && rtc_epoch + BACKWARDS_SLACK_S < last_alive) return false;
  return true;
}

}  // namespace rtc_ds3231
