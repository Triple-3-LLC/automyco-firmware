#pragma once
// CO2-driven fresh-air exchange (common/scd41.yaml, issue #26, docs/co2-fresh-air.md): when to start,
// hold and end a fresh-air exchange from the SCD41's CO2 reading.
//
// The core controller (common/terrashroom-core.yaml) owns the fans. Its exchange runs while
// fresh_air_until is in the future, and it pauses misting for as long as that lasts. This logic never
// touches a fan: it asks the core for an exchange (START: the package sets next_fresh_air to
// now, which the next 1 s tick turns into an exchange of Fresh Air Duration and which also restarts
// the timer schedule), then stretches that exchange (HOLD: fresh_air_until = start + max) until CO2 is
// back below the limit minus the hysteresis, and then ends it (STOP: fresh_air_until = now).
//
// Arbitration with the humidity controller:
//   - Never starts during a humidify cycle (60 s at most); it waits for the cycle to finish.
//   - Runs at least Fresh Air Duration (the minimum on time), so an exchange always moves real air.
//   - Ends early once that minimum is met if RH has fallen more than RH_GUARD below the humidity target:
//     fresh air is drying the chamber faster than it is clearing CO2.
//   - Waits at least min_gap after the end of its own exchange before starting another, so the
//     humidifier gets time to recover. Its duty cycle is at most max_on / (max_on + min_gap).
//   - Ends at max_on whatever the CO2 reading says.
// A scheduled exchange (or Fresh Air Now) that starts while CO2 is over the limit is adopted: it is
// held open like one of ours. The timer schedule keeps running regardless, so the chamber always gets
// a floor of fresh air, and with the sensor missing, failed or stale the logic does nothing and the
// schedule alone runs (exactly the build without the SCD41).
//
// Plain C++ with no ESPHome types, so esphome/tests/test_co2_control.cpp can test it on a Mac. All
// times are millis() values; differences are taken unsigned, so the 49-day wrap is harmless.
#include <cmath>
#include <cstdint>

namespace co2_control {

constexpr float OUTDOOR_PPM = 400.0f;   // a limit below outdoor air can never be met: CO2 control off
constexpr float RH_GUARD = 10.0f;       // end early when RH < humidity target - this (once min on is met)
constexpr uint32_t STALE_MS = 120000;   // no CO2 reading for 2 min (4 missed 30 s updates): sensor lost

struct Config {
  float limit_ppm;       // CO2 Limit; < OUTDOOR_PPM = off
  float hysteresis_ppm;  // end once CO2 <= limit - hysteresis
  uint32_t min_on_ms;    // Fresh Air Duration: an exchange never ends earlier (bar Climate Control off)
  uint32_t max_on_ms;    // CO2 Exchange Max: never held longer
  uint32_t min_gap_ms;   // CO2 Exchange Gap: rest after our exchange ends before the next CO2 start
};

struct Inputs {
  uint32_t now;
  float ppm;                // NAN when the SCD41 is missing or has never read
  uint32_t ppm_at;          // millis() of the last CO2 reading (0 = never)
  bool enabled;             // Climate Control on, no self-test, phase not Incubation
  bool exchange_running;    // fresh_air_until is in the future (ours, scheduled, or Fresh Air Now)
  uint32_t exchange_start;  // last_fresh_air: when the running (or last) exchange started (0 = none)
  bool humidify_cycle;      // a humidify cycle is in progress (hum_cycle_start != 0)
  float rh;                 // chamber RH (NAN on a sensor fault: the RH guard is skipped)
  float rh_target;          // the humidity target in force
};

struct State {
  bool active = false;     // we are holding an exchange open
  bool pending = false;    // we asked the core for an exchange and it hasn't started yet
  uint32_t started = 0;    // when the held exchange started
  uint32_t ended = 0;      // when our last exchange ended (0 = never)
  uint32_t asked = 0;      // when we asked (a request the core never served expires after 5 s)
};

// Names chosen not to collide with Arduino macros (DISABLED, ...) or esphome::Action inside a lambda.
enum class Do {
  NOTHING,  // nothing to do
  START,    // ask the core for an exchange (next_fresh_air = now)
  HOLD,     // keep the exchange open: fresh_air_until = started + max_on_ms
  STOP,     // end the exchange now
};

enum class Why { NONE, OVER_LIMIT, ADOPTED, CLEARED, MAX_ON, RH_LOW, SENSOR_LOST, CLIMATE_OFF };

struct Result {
  Do action = Do::NOTHING;
  Why why = Why::NONE;
  uint32_t until = 0;   // for HOLD: the fresh_air_until to set
};

// Milliseconds since a millis() stamp stored | 1, wrap-safe. A stamp up to a second "ahead" (the | 1 on
// an even millisecond) reads as 0, not 49 days (the bug common/mist_cycle.h describes).
inline uint32_t age(uint32_t now, uint32_t stamp) {
  const uint32_t d = now - stamp;
  return d > 0xFFFFFFFFu - 1000u ? 0 : d;
}

inline bool sensor_ok(const Inputs &in) {
  return !std::isnan(in.ppm) && in.ppm_at != 0 && age(in.now, in.ppm_at) <= STALE_MS;
}

inline bool limit_on(const Config &c) { return !std::isnan(c.limit_ppm) && c.limit_ppm >= OUTDOOR_PPM; }

inline Result step(State &s, const Config &c, const Inputs &in) {
  Result r;
  const bool ok = sensor_ok(in);
  if (s.active) {
    if (!in.exchange_running) {   // ended by someone else (or Climate Control went off and it lapsed)
      s.active = false;
      s.ended = in.now | 1u;
      return r;
    }
    const uint32_t on = age(in.now, s.started);
    Why end = Why::NONE;
    if (!in.enabled) end = Why::CLIMATE_OFF;
    else if (on >= c.max_on_ms) end = Why::MAX_ON;
    else if (on >= c.min_on_ms) {
      if (!ok || !limit_on(c)) end = Why::SENSOR_LOST;
      else if (in.ppm <= c.limit_ppm - c.hysteresis_ppm) end = Why::CLEARED;
      else if (!std::isnan(in.rh) && !std::isnan(in.rh_target) && in.rh < in.rh_target - RH_GUARD) end = Why::RH_LOW;
    }
    if (end != Why::NONE) {
      s.active = false;
      s.ended = in.now | 1u;
      r.action = Do::STOP;
      r.why = end;
      return r;
    }
    r.action = Do::HOLD;
    r.until = s.started + c.max_on_ms;
    return r;
  }

  const bool over = in.enabled && ok && limit_on(c) && in.ppm > c.limit_ppm;
  const bool resting = s.ended != 0 && age(in.now, s.ended) < c.min_gap_ms;
  if (in.exchange_running) {
    // Ours starting (pending), or a scheduled / Fresh Air Now exchange: hold it open while CO2 is high.
    // Not while resting after our own exchange: that includes the exhaust half a Two-phase STOP leaves
    // running, which would otherwise be adopted straight back.
    if (s.pending || (over && !resting)) {
      const bool ours = s.pending;
      s.pending = false;
      s.active = true;
      s.started = in.exchange_start != 0 ? in.exchange_start : in.now;
      r.action = Do::HOLD;
      r.why = ours ? Why::OVER_LIMIT : Why::ADOPTED;
      r.until = s.started + c.max_on_ms;
    }
    return r;
  }
  if (s.pending) {
    if (age(in.now, s.asked) < 5000u) return r;   // the core's next 1 s tick picks it up
    s.pending = false;                                      // never served (Climate Control off?): drop it
  }
  if (!over || resting) return r;
  if (in.humidify_cycle) return r;   // let the cycle finish (it can't start while we run)
  s.pending = true;
  s.asked = in.now;
  r.action = Do::START;
  r.why = Why::OVER_LIMIT;
  return r;
}

inline const char *why_name(Why w) {
  switch (w) {
    case Why::OVER_LIMIT: return "over limit";
    case Why::ADOPTED: return "scheduled exchange held";
    case Why::CLEARED: return "cleared";
    case Why::MAX_ON: return "max time";
    case Why::RH_LOW: return "RH too low";
    case Why::SENSOR_LOST: return "sensor lost";
    case Why::CLIMATE_OFF: return "climate control off";
    default: return "";
  }
}

}  // namespace co2_control
