// Host tests for esphome/common/water_level.h (non-contact water-level sensor retrofit, issue #27).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_water_level.cpp -o /tmp/tst && /tmp/tst
#include "water_level.h"

#include <cstdio>

using namespace water_level;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const uint32_t S = 1000;

// Feeds the same reading every 500 ms (the package's rate) from t0 to t1 inclusive; returns the last
// debounced "dry".
static bool feed(Debounce &d, bool wet, uint32_t t0, uint32_t t1, bool splashing = false) {
  bool dry = d.dry;
  for (uint32_t t = t0; (int32_t) (t1 - t) >= 0; t += 500) dry = update(d, wet, splashing, t);
  return dry;
}

int main() {
  // --- Polarity and pulls: a disconnected input always reads "dry".
  // Active low (NPN / open collector: LOW = water): pull-up, so a floating line reads HIGH = dry.
  CHECK(pull_up(true));
  CHECK(!raw_wet(true, true));    // HIGH (pulled up, nothing driving it) = dry
  CHECK(raw_wet(false, true));    // LOW = water
  // Active high (HIGH = water): pull-down, so a floating line reads LOW = dry.
  CHECK(!pull_up(false));
  CHECK(!raw_wet(false, false));  // LOW (pulled down) = dry
  CHECK(raw_wet(true, false));
  // Misconfigured as active-high with an open-collector sensor: the line is pulled down and the sensor
  // can only pull it lower, so it reads dry forever -> locked out (safe, if annoying).
  CHECK(!raw_wet(false, false));

  // --- Boot: fails closed until WET_MS of wet readings.
  {
    Debounce d;
    CHECK(d.dry);
    CHECK(feed(d, true, 0, WET_MS - 500));   // 5.5 s of wet: still dry
    CHECK(!feed(d, true, WET_MS, WET_MS));   // 6 s: clears
  }

  // --- Unplugged at boot (reads dry): stays dry forever.
  {
    Debounce d;
    CHECK(feed(d, false, 0, 60 * S));
  }

  // --- Going dry takes DRY_MS of uninterrupted dry readings; one wet reading restarts it.
  {
    Debounce d;
    feed(d, true, 0, 10 * S);
    CHECK(!d.dry);
    const uint32_t t = 20 * S;
    CHECK(!feed(d, false, t, t + DRY_MS - 500));   // 9.5 s dry: still has water
    CHECK(!feed(d, true, t + DRY_MS, t + DRY_MS));   // one wet reading
    CHECK(!feed(d, false, t + DRY_MS + 500, t + 2 * DRY_MS));   // the streak restarted
    CHECK(feed(d, false, t + 2 * DRY_MS + 500, t + 2 * DRY_MS + 500));   // 10 s since the restart
  }

  // --- Wire cut / connector pulled while running: the pull makes it read dry, Low Water after 10 s.
  {
    Debounce d;
    feed(d, raw_wet(false, true), 0, 10 * S);   // active-low sensor, water: LOW
    CHECK(!d.dry);
    CHECK(feed(d, raw_wet(true, true), 11 * S, 11 * S + DRY_MS));   // pulled up: HIGH = dry
  }

  // --- Flicker near the threshold (sloshing): alternating readings never clear Low Water.
  {
    Debounce d;
    for (uint32_t t = 0; t < 120 * S; t += 500) update(d, (t / 500) % 2 == 0, false, t);
    CHECK(d.dry);
  }

  // --- Splashing: wet readings during spray never clear Low Water ...
  {
    Debounce d;
    CHECK(feed(d, true, 0, 30 * S, true));
    // ... and a wet streak that began before the spray is cancelled by it.
    feed(d, true, 31 * S, 34 * S);          // 3 s wet, not splashing
    update(d, true, true, 34500);            // spray
    CHECK(feed(d, true, 35 * S, 40 * S));   // 5 s more wet: not 6 s since the spray
    CHECK(!feed(d, true, 40500, 41 * S));   // 6 s after the spray: clears
  }
  // ... while dry readings during spray still count toward Low Water.
  {
    Debounce d;
    feed(d, true, 0, 10 * S);
    CHECK(!d.dry);
    CHECK(feed(d, false, 11 * S, 11 * S + DRY_MS, true));
  }
  // ... and spray-wet readings in the middle of a dry streak don't reset it (the tank can't refill
  // during a mist pulse).
  {
    Debounce d;
    feed(d, true, 0, 10 * S);
    update(d, false, false, 11 * S);           // dry streak starts at 11 s
    feed(d, true, 11500, 15 * S, true);        // spray: ignored
    CHECK(feed(d, false, 15500, 21 * S));      // 10 s after 11 s: Low Water
  }

  // --- Across the 49-day millis() wrap.
  {
    Debounce d;
    const uint32_t t0 = 0xFFFFFFFFu - 2 * S;
    CHECK(!feed(d, true, t0, t0 + WET_MS + 500));
  }

  // --- Low Water per Water Sensing mode.
  CHECK(low_water(Mode::PROBE, true, false, false));
  CHECK(!low_water(Mode::PROBE, false, true, false));           // the level sensor is ignored in Probe
  CHECK(!low_water(Mode::HUMIDITY_RESPONSE, true, true, false));
  CHECK(low_water(Mode::HUMIDITY_RESPONSE, false, false, true));
  CHECK(low_water(Mode::LEVEL_SENSOR, false, true, false));
  CHECK(!low_water(Mode::LEVEL_SENSOR, true, false, false));     // the probe is ignored
  CHECK(low_water(Mode::LEVEL_SENSOR, false, false, true));      // Tank Empty Alert still counts
  // An unknown mode with the package absent: the core's level_dry global starts true -> locked out.
  CHECK(low_water(Mode::LEVEL_SENSOR, false, Debounce{}.dry, false));
  // The Humidity Response stall check backs up the level sensor.
  CHECK(!humidity_check(Mode::PROBE));
  CHECK(humidity_check(Mode::HUMIDITY_RESPONSE));
  CHECK(humidity_check(Mode::LEVEL_SENSOR));

  if (failures) {
    std::printf("test_water_level: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_water_level: all passed\n");
  return 0;
}
