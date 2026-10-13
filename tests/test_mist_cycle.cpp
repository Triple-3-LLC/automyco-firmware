// Host tests for esphome/common/mist_cycle.h (the humidify cycle's clock and the mist pulser).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_mist_cycle.cpp -o /tmp/tst && /tmp/tst
#include "mist_cycle.h"

#include <cstdio>

using namespace mist_cycle;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const uint32_t S = 1000;

int main() {
  // --- elapsed(): a stamp taken this tick is 0 ms old, even on an even millisecond.
  CHECK(elapsed(1000, stamp(1000)) == 0);   // stamp 1001: 1 ms ahead
  CHECK(elapsed(1001, stamp(1001)) == 0);
  CHECK(elapsed(1001, stamp(1000)) == 0);
  CHECK(elapsed(2000, stamp(1000)) == 999);
  CHECK(elapsed(5, stamp(0xFFFFFFF0u)) == 20);   // across the millis() wrap
  CHECK(stamp(0) != 0);                          // 0 means "no cycle"

  // --- timing(): the owner's settings and the defaults.
  {
    const Timing t = timing(8 * S, 1500, 1000);   // Mist Burst 8 s, Pulse On 1.5 s, Off 1.0 s
    CHECK(t.mist_ms == 6 * 2500 - 1000);          // 6 pulses: 14 s first-on to last-off
    CHECK(t.cycle_ms == 60 * S);
  }
  {
    const Timing t = timing(6 * S, 1500, 1000);   // the 1.2 default: 4 pulses, 9 s
    CHECK(t.mist_ms == 9 * S);
    CHECK(t.cycle_ms == 60 * S);
  }
  CHECK(timing(10 * S, 1500, 0).mist_ms == 10 * S);    // Pulse Off 0: one continuous burst (stock)
  CHECK(timing(10 * S, 0, 1000).mist_ms == 10 * S);    // Pulse On 0: the same
  CHECK(timing(20 * S, 500, 5000).mist_ms == 40 * 5500 - 5000);   // 40 pulses, 215 s ...
  CHECK(timing(20 * S, 500, 5000).cycle_ms == 235 * S);           // ... so the cycle grows past 60 s

  // --- phase(): mist, then 15 s of blower, then rest to the end of the cycle.
  {
    const Timing t = timing(6 * S, 1500, 1000);
    CHECK(phase(0, t) == Phase::MIST);
    CHECK(phase(t.mist_ms - 1, t) == Phase::MIST);
    CHECK(phase(t.mist_ms, t) == Phase::BLOWER);
    CHECK(phase(t.mist_ms + BLOWER_MS - 1, t) == Phase::BLOWER);
    CHECK(phase(t.mist_ms + BLOWER_MS, t) == Phase::REST);
    CHECK(phase(t.cycle_ms - 1, t) == Phase::REST);
  }

  // --- pulse_on(): 1.5 s on, 1.0 s off, from the start of the mist phase.
  CHECK(pulse_on(0, 1500, 1000));
  CHECK(pulse_on(1499, 1500, 1000));
  CHECK(!pulse_on(1500, 1500, 1000));
  CHECK(!pulse_on(2499, 1500, 1000));
  CHECK(pulse_on(2500, 1500, 1000));
  CHECK(pulse_on(123456, 1500, 0));   // Pulse Off 0: always on
  CHECK(pulse_on(123456, 0, 1000));   // Pulse On 0: never pulsed, always on

  // --- The 1.2.2 bug: a cycle started on an even millisecond read as 49 days old on its first tick.
  {
    const Timing t = timing(6 * S, 1500, 1000);
    for (uint32_t now : {1000u, 1001u, 0xFFFFFFFEu, 7u}) {
      const uint32_t start = stamp(now);
      // What the controller and the 250 ms pulser did up to 1.2.2 on the tick that started the cycle:
      const uint32_t legacy = now - start;
      const bool legacy_rested = !(legacy < t.mist_ms);
      const bool legacy_pulse = (legacy % 2500) < 1500;
      if ((now & 1u) == 0) {
        CHECK(legacy == 0xFFFFFFFFu);   // "resting" ...
        CHECK(legacy_rested);
        CHECK(!legacy_pulse);           // ... and the atomizer held off
      } else {
        CHECK(legacy == 0 && !legacy_rested && legacy_pulse);   // odd ms were fine
      }
      // Now: the first tick is 0 ms in, misting, atomizer on, whatever the parity.
      const uint32_t e = elapsed(now, start);
      CHECK(e == 0);
      CHECK(phase(e, t) == Phase::MIST);
      CHECK(pulse_on(e, 1500, 1000));
      // And the pulser's own millis() calls in that first second agree.
      for (uint32_t dt = 0; dt < 1000; dt += 250) {
        const uint32_t ee = elapsed(now + dt, start);
        CHECK(ee == (dt == 0 ? 0 : dt - (start - now)));
        CHECK(phase(ee, t) == Phase::MIST);
        CHECK(pulse_on(ee, 1500, 1000));
      }
      // A cycle is still over when it is over.
      CHECK(elapsed(now + t.cycle_ms, start) >= t.cycle_ms - 1);
      CHECK(elapsed(now + t.cycle_ms + 1, start) >= t.cycle_ms);
    }
  }

  if (failures == 0) std::printf("test_mist_cycle: all checks passed\n");
  else std::printf("test_mist_cycle: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
