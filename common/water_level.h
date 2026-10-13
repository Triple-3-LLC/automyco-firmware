#pragma once
// Non-contact water-level sensor retrofit (common/water-level-sensor.yaml, issue #27,
// docs/water-level-sensor.md): turning the sensor's digital output into "the tank has water" for the
// core's Low Water (and so the atomizer lockout and the UV-C cut).
//
// Fail-safe by construction:
//   - The pin's pull always goes to the sensor's "no water" level: pull-up for an active-low sensor
//     (NPN / open-collector, LOW = water), pull-down for an active-high one. A cut wire, an unplugged
//     connector or an unpowered sensor therefore reads "dry", and Low Water locks the atomizer out.
//   - The debounce starts dry (Low Water ON at boot) and only clears after WET_MS of uninterrupted wet
//     readings taken outside the splash window. Going dry takes DRY_MS of uninterrupted dry readings.
//   - While the atomizer runs, and for SPLASH_MS after, a wet reading can't count toward clearing
//     (spray on the tank wall can look like water at the sensor's height). A dry reading still counts:
//     splashing can only make the sensor read wetter than the truth, never drier.
// What it can't catch: a sensor whose output is stuck at "water". The core keeps the Humidity Response
// check running in this mode (misting that doesn't raise RH engages the Hard Stop) as a second layer.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_water_level.cpp can test it on a Mac. Times are
// millis() values; differences are taken unsigned, so the 49-day wrap is harmless.
#include <cstdint>

namespace water_level {

constexpr uint32_t DRY_MS = 10000;    // continuous dry before Low Water (the probe takes ~16 s)
constexpr uint32_t WET_MS = 6000;     // continuous wet before it clears (the probe: 3 reads, 6 s)
constexpr uint32_t SPLASH_MS = 3000;  // after the atomizer stops, wet readings still don't count (as the probe)

// The pin mode that makes a disconnected input read "dry": true = pull-up, false = pull-down.
inline bool pull_up(bool active_low) { return active_low; }

// Whether a pin level means water. active_low: the sensor pulls the line LOW when it sees water.
inline bool raw_wet(bool level_high, bool active_low) { return active_low ? !level_high : level_high; }

struct Debounce {
  bool dry = true;          // debounced state: true = no water (Low Water). Starts dry: fails closed
  bool cand = true;         // the reading the current streak is made of
  uint32_t cand_since = 0;  // when that streak started
  bool started = false;     // false until the first reading
};

// Feeds one reading; returns the debounced "dry" state. splashing: the atomizer is on or stopped less
// than SPLASH_MS ago (the caller decides).
inline bool update(Debounce &d, bool wet, bool splashing, uint32_t now) {
  const bool reading_dry = !wet;
  if (!reading_dry && splashing) {
    // A wet reading during spray proves nothing. It cancels a wet streak (water has to be seen again
    // after the spray settles) but leaves a dry streak running: the tank can't really have refilled
    // during a mist pulse. The state holds.
    if (d.dry && !d.cand) {   // a wet streak toward clearing Low Water: cancel it
      d.cand = true;
      d.cand_since = now;
    }
    return d.dry;
  }
  if (!d.started || reading_dry != d.cand) {
    d.cand = reading_dry;
    d.cand_since = now;
    d.started = true;
  }
  if (d.cand != d.dry) {
    const uint32_t need = d.cand ? DRY_MS : WET_MS;
    if ((uint32_t) (now - d.cand_since) >= need) d.dry = d.cand;
  }
  return d.dry;
}

// The core's Water Sensing modes. Probe and Humidity Response are the core's; anything else (the
// package's "Level Sensor", or an option the core doesn't know) uses this sensor's verdict, which is
// "dry" until the package has proven otherwise. So an unknown mode locks out rather than ignoring water.
enum class Mode { PROBE, HUMIDITY_RESPONSE, LEVEL_SENSOR };

// Low Water as the core computes it (common/terrashroom-core.yaml, the 2 s water interval).
inline bool low_water(Mode m, bool probe_low, bool level_dry, bool tank_empty_alert) {
  switch (m) {
    case Mode::PROBE: return probe_low || tank_empty_alert;
    case Mode::HUMIDITY_RESPONSE: return tank_empty_alert;
    default: return level_dry || tank_empty_alert;
  }
}

// Whether the Humidity Response stall check runs: in every mode but Probe, so it backs up the level
// sensor too.
inline bool humidity_check(Mode m) { return m != Mode::PROBE; }

}  // namespace water_level
