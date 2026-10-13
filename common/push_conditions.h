#pragma once
// The two slow push-alert conditions (common/push-alerts.yaml), sampled once a second:
//   sensor_stuck         neither temperature nor humidity has moved more than STUCK_TOLERANCE for STUCK_MS
//                        while Climate Control is on (a dry pulse holds the readings by design, so it
//                        restarts the wait)
//   climate_out_of_band  the climate has been outside its bands for OUT_OF_BAND_MS
//
// Timestamps are millis() values stored | 1, so 0 can mean "none". That makes a stamp taken on an even
// millisecond 1 ms *ahead* of now, and up to 1.2.2 the same tick then computed (uint32_t) (now - stamp) =
// 0xFFFFFFFF, "49 days ago": both alerts came on for one second whenever a reading changed (or, for
// out_of_band, on any in-band second) on an even millisecond, and went out every 30 min, the per-kind
// throttle, from boot on. elapsed() reads a stamp in the future as 0.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_push_conditions.cpp can test it on a Mac.
#include <cmath>
#include <cstdint>

namespace push_cond {

constexpr uint32_t STUCK_MS = 2UL * 3600000UL;
constexpr uint32_t OUT_OF_BAND_MS = 3UL * 3600000UL;
// An HDC1080 resolves 0.005 °F and 0.002 %RH and its last digits always wander, so two hours inside
// 0.05 °F and 0.05 %RH means the readings are frozen (or the sensor has stopped publishing).
constexpr float STUCK_TOLERANCE = 0.05f;

// A millis() stamp that is never 0.
inline uint32_t stamp(uint32_t now) { return now | 1u; }

// Milliseconds since a stamp(), wrap-safe. A stamp up to a second ahead (the | 1 above) reads as 0.
inline uint32_t elapsed(uint32_t now, uint32_t since) {
  const uint32_t d = now - since;
  return d > 0xFFFFFFFFu - 1000u ? 0 : d;
}

// sensor_stuck. ref_t / ref_h are the readings the wait is measured from (a move past the tolerance
// re-anchors them, so a slow drift counts too) and since is when it started (0 = no reading yet).
inline bool sensor_stuck(float &ref_t, float &ref_h, uint32_t &since, uint32_t now, float t, float h, bool climate_on,
                         bool heating) {
  // A missing reading doesn't move anything: a sensor that stops publishing is stuck too.
  const bool valid = !std::isnan(t) && !std::isnan(h);
  if (valid && (since == 0 || std::isnan(ref_t) || std::isnan(ref_h) || std::fabs(t - ref_t) > STUCK_TOLERANCE ||
                std::fabs(h - ref_h) > STUCK_TOLERANCE)) {
    ref_t = t;
    ref_h = h;
    since = stamp(now);
    return false;
  }
  if (since == 0) return false;   // no reading yet
  if (!climate_on || heating) {   // not judged: the wait starts again when it is
    since = stamp(now);
    return false;
  }
  return elapsed(now, since) > STUCK_MS;
}

// climate_out_of_band. inband_at is when the climate was last inside its bands or not being judged
// (0 = never sampled). judged_out: Climate Control on, no dry pulse, both readings present, and at least one
// outside its band.
inline bool out_of_band(uint32_t &inband_at, uint32_t now, bool judged_out) {
  if (!judged_out || inband_at == 0) {
    inband_at = stamp(now);
    return false;
  }
  return elapsed(now, inband_at) > OUT_OF_BAND_MS;
}

}  // namespace push_cond
