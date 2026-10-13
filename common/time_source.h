#pragma once
// Where the chamber's wall-clock time comes from, for the Time Source diagnostic (issue #29,
// docs/time-and-schedule.md). Host tests: tests/test_time_source.cpp.
//
//   sntp  synced from the internet within the last hour (SNTP polls every 15 min)
//   held  the clock is valid but no SNTP sync in the last hour, or none since a software restart that kept
//         the clock. The ESP32 keeps counting from its crystal after a sync, so the light schedule, Grow
//         Day and the phase timer carry on; drift is a few seconds a day.
//   rtc   set from the optional DS3231 at boot (common/rtc-ds3231.yaml) and no SNTP sync yet
//   none  no valid time: powered on with no internet and no RTC. Light schedule, Grow Day, Phase Day and
//         the phase check wait for the clock.
//
// Nothing safety-related reads the wall clock: heater cut-offs, the UV-C auto-off and the low-water
// lockout all run on millis().
#include <cstdint>

namespace time_source {

static const uint32_t STALE_MS = 60UL * 60UL * 1000UL;   // no SNTP sync for this long: "held"
static const uint32_t NEVER = 0;      // synced_at: no SNTP sync since boot
static const uint32_t LONG_AGO = 1;   // synced_at: synced, but longer ago than STALE_MS

// The value to keep in synced_at at an SNTP sync: millis() with bit 1 set, so it is never NEVER or LONG_AGO.
inline uint32_t stamp(uint32_t now_ms) { return now_ms | 2u; }

// clock_valid: sntp_time.now().is_valid(). synced_at: the stamp() of the last SNTP sync (or NEVER/LONG_AGO).
// from_rtc: the DS3231 set the clock at this boot. Ages synced_at to LONG_AGO once it is stale, so millis()
// wrapping after 49.7 days can't make an old sync look fresh; call it at least once an hour (the sensor
// updates every minute).
inline const char *classify(bool clock_valid, uint32_t &synced_at, uint32_t now_ms, bool from_rtc) {
  if (synced_at > LONG_AGO && (uint32_t) (now_ms - synced_at) >= STALE_MS) synced_at = LONG_AGO;
  if (!clock_valid) return "none";
  if (synced_at > LONG_AGO) return "sntp";
  if (synced_at == NEVER && from_rtc) return "rtc";
  return "held";
}

}  // namespace time_source
