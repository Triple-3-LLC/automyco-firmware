// Host tests for esphome/common/co2_control.h (CO2-driven fresh-air exchange, issue #26).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_co2_control.cpp -o /tmp/tst && /tmp/tst
#include "co2_control.h"

#include <cstdio>

using namespace co2_control;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const uint32_t S = 1000, MIN = 60000;

// Defaults from common/scd41.yaml: 1500 ppm, 200 ppm hysteresis, 60 s Fresh Air Duration, 15 min max,
// 5 min gap.
static Config cfg() { return {1500.0f, 200.0f, 60 * S, 15 * MIN, 5 * MIN}; }

// A healthy chamber at t with CO2 ppm (fresh reading), climate on, nothing running.
static Inputs at(uint32_t t, float ppm) {
  Inputs in{};
  in.now = t;
  in.ppm = ppm;
  in.ppm_at = t | 1;   // the package stores the reading time | 1 (0 = never read)
  in.enabled = true;
  in.exchange_running = false;
  in.exchange_start = 0;
  in.humidify_cycle = false;
  in.rh = 88.0f;
  in.rh_target = 88.0f;
  return in;
}

// Simulates the core: a request becomes an exchange that started at `t`.
static Inputs running(uint32_t t, float ppm, uint32_t start) {
  Inputs in = at(t, ppm);
  in.exchange_running = true;
  in.exchange_start = start;
  return in;
}

int main() {
  const Config c = cfg();

  // --- Below the limit, or at it: nothing.
  {
    State s;
    CHECK(step(s, c, at(10 * S, 1400)).action == Do::NOTHING);
    CHECK(step(s, c, at(11 * S, 1500)).action == Do::NOTHING);   // strictly above starts
    CHECK(!s.pending && !s.active);
  }

  // --- Full cycle: over -> request -> core starts -> held -> cleared below limit - hysteresis -> end.
  {
    State s;
    uint32_t t = 100 * S;
    Result r = step(s, c, at(t, 1600));
    CHECK(r.action == Do::START);
    CHECK(s.pending);
    // Same tick again (the core hasn't run yet): no second request.
    CHECK(step(s, c, at(t + 500, 1600)).action == Do::NOTHING);
    // Core started it at t + 1 s.
    t += S;
    r = step(s, c, running(t, 1600, t | 1));
    CHECK(r.action == Do::HOLD);
    CHECK(r.why == Why::OVER_LIMIT);
    CHECK(r.until == (t | 1) + 15 * MIN);
    CHECK(s.active && !s.pending);
    // Same millisecond again: the start stamp (t | 1) is 1 ms "ahead"; that reads as 0 ms on, not max.
    CHECK(step(s, c, running(t, 1600, t | 1)).action == Do::HOLD);
    // Below the limit but inside the hysteresis band: keep going.
    r = step(s, c, running(t + 2 * MIN, 1400, t | 1));
    CHECK(r.action == Do::HOLD);
    // At limit - hysteresis: end.
    r = step(s, c, running(t + 3 * MIN, 1300, t | 1));
    CHECK(r.action == Do::STOP);
    CHECK(r.why == Why::CLEARED);
    CHECK(!s.active);
  }

  // --- Minimum on time: CO2 clears at once, the exchange still runs Fresh Air Duration.
  {
    State s;
    step(s, c, at(0, 1600));
    step(s, c, running(S, 1600, S | 1));
    CHECK(step(s, c, running(30 * S, 900, S | 1)).action == Do::HOLD);
    CHECK(step(s, c, running(62 * S, 900, S | 1)).action == Do::STOP);
  }

  // --- Maximum on time: CO2 never clears, the exchange ends at max.
  {
    State s;
    step(s, c, at(0, 3000));
    step(s, c, running(S, 3000, S | 1));
    CHECK(step(s, c, running(14 * MIN, 3000, S | 1)).action == Do::HOLD);
    Result r = step(s, c, running(15 * MIN + 2 * S, 3000, S | 1));
    CHECK(r.action == Do::STOP);
    CHECK(r.why == Why::MAX_ON);
    // Minimum gap: still over the limit, but no new request for 5 min after the end.
    const uint32_t end = 15 * MIN + 2 * S;
    CHECK(step(s, c, at(end + S, 3000)).action == Do::NOTHING);
    CHECK(step(s, c, at(end + 4 * MIN, 3000)).action == Do::NOTHING);
    CHECK(step(s, c, at(end + 5 * MIN + S, 3000)).action == Do::START);
  }

  // --- Arbitration: never starts during a humidify cycle; starts once it's over.
  {
    State s;
    Inputs in = at(0, 2000);
    in.humidify_cycle = true;
    CHECK(step(s, c, in).action == Do::NOTHING);
    CHECK(!s.pending);
    in = at(60 * S, 2000);
    CHECK(step(s, c, in).action == Do::START);
  }

  // --- Arbitration: RH guard ends the exchange (after the minimum) when RH falls > 10 % below target.
  {
    State s;
    step(s, c, at(0, 2000));
    step(s, c, running(S, 2000, S | 1));
    Inputs in = running(30 * S, 2000, S | 1);
    in.rh = 70.0f;   // 18 % under, but inside the minimum on time
    CHECK(step(s, c, in).action == Do::HOLD);
    in = running(90 * S, 2000, S | 1);
    in.rh = 78.5f;   // 9.5 % under: fine
    CHECK(step(s, c, in).action == Do::HOLD);
    in.now = 91 * S; in.ppm_at = in.now | 1;
    in.rh = 77.0f;   // 11 % under: end
    Result r = step(s, c, in);
    CHECK(r.action == Do::STOP);
    CHECK(r.why == Why::RH_LOW);
    // An RH fault (NAN) doesn't trip the guard.
    State s2;
    step(s2, c, at(0, 2000));
    step(s2, c, running(S, 2000, S | 1));
    in = running(90 * S, 2000, S | 1);
    in.rh = NAN;
    CHECK(step(s2, c, in).action == Do::HOLD);
  }

  // --- Fallback: sensor missing (NAN), never read, or stale -> never starts; a lost sensor ends ours.
  {
    State s;
    Inputs in = at(10 * S, NAN);
    CHECK(step(s, c, in).action == Do::NOTHING);
    in = at(10 * S, 2000);
    in.ppm_at = 0;   // never read
    CHECK(step(s, c, in).action == Do::NOTHING);
    in = at(10 * MIN, 2000);
    in.ppm_at = 10 * MIN - STALE_MS - 1;   // last reading 2 min + 1 ms ago
    CHECK(step(s, c, in).action == Do::NOTHING);
    in.ppm_at = 10 * MIN - STALE_MS;       // exactly 2 min: still trusted
    CHECK(step(s, c, in).action == Do::START);

    State h;
    step(h, c, at(0, 2000));
    step(h, c, running(S, 2000, S | 1));
    in = running(2 * MIN, 2000, S | 1);
    in.ppm = NAN;
    Result r = step(h, c, in);
    CHECK(r.action == Do::STOP);
    CHECK(r.why == Why::SENSOR_LOST);
  }

  // --- Off: limit below 400 ppm (or NAN), Climate Control off / Incubation (enabled = false).
  {
    State s;
    Config off = c;
    off.limit_ppm = 300;
    CHECK(step(s, off, at(0, 4000)).action == Do::NOTHING);
    off.limit_ppm = NAN;
    CHECK(step(s, off, at(0, 4000)).action == Do::NOTHING);
    Inputs in = at(0, 4000);
    in.enabled = false;
    CHECK(step(s, c, in).action == Do::NOTHING);
    // A held exchange ends when Climate Control goes off, even inside the minimum on time.
    State h;
    step(h, c, at(0, 2000));
    step(h, c, running(S, 2000, S | 1));
    in = running(5 * S, 2000, S | 1);
    in.enabled = false;
    Result r = step(h, c, in);
    CHECK(r.action == Do::STOP);
    CHECK(r.why == Why::CLIMATE_OFF);
    // Limit switched off mid-exchange: ends once the minimum is met.
    Config later = c;
    later.limit_ppm = 0;
    State h2;
    step(h2, c, at(0, 2000));
    step(h2, c, running(S, 2000, S | 1));
    CHECK(step(h2, later, running(30 * S, 2000, S | 1)).action == Do::HOLD);
    CHECK(step(h2, later, running(70 * S, 2000, S | 1)).action == Do::STOP);
  }

  // --- Adoption: a scheduled exchange running while CO2 is high is held; while CO2 is fine it isn't.
  {
    State s;
    CHECK(step(s, c, running(10 * S, 1200, 9 * S | 1)).action == Do::NOTHING);
    CHECK(!s.active);
    Result r = step(s, c, running(11 * S, 1700, 9 * S | 1));
    CHECK(r.action == Do::HOLD);
    CHECK(r.why == Why::ADOPTED);
    CHECK(r.until == (9 * S | 1) + 15 * MIN);
  }

  // --- During the gap after our exchange, a running exchange (the exhaust half a Two-phase STOP leaves,
  // or a scheduled one) is not adopted, even with CO2 still high.
  {
    State s;
    step(s, c, at(0, 3000));
    step(s, c, running(S, 3000, S | 1));
    CHECK(step(s, c, running(15 * MIN + 2 * S, 3000, S | 1)).action == Do::STOP);   // max on
    CHECK(step(s, c, running(15 * MIN + 3 * S, 3000, S | 1)).action == Do::NOTHING);  // tail: not adopted
    CHECK(!s.active);
    const uint32_t later = 15 * MIN + 2 * S + 5 * MIN + S;
    CHECK(step(s, c, running(later, 3000, later | 1)).action == Do::HOLD);          // after the gap: adopted
  }

  // --- Someone else ended our exchange (Climate Control toggled, etc.): drop it and start the gap.
  {
    State s;
    step(s, c, at(0, 2000));
    step(s, c, running(S, 2000, S | 1));
    CHECK(step(s, c, at(90 * S, 2000)).action == Do::NOTHING);
    CHECK(!s.active);
    CHECK(s.ended != 0);
    CHECK(step(s, c, at(90 * S + 4 * MIN, 2000)).action == Do::NOTHING);   // gap
    CHECK(step(s, c, at(90 * S + 5 * MIN + S, 2000)).action == Do::START);
  }

  // --- A request the core never served (Climate Control off between ticks) expires after 5 s.
  {
    State s;
    CHECK(step(s, c, at(0, 2000)).action == Do::START);
    CHECK(step(s, c, at(4 * S, 2000)).action == Do::NOTHING);
    CHECK(step(s, c, at(6 * S, 2000)).action == Do::START);
  }

  // --- Across the 49-day millis() wrap.
  {
    State s;
    const uint32_t t0 = 0xFFFFFFFFu - 30 * S;
    CHECK(step(s, c, at(t0, 2000)).action == Do::START);
    step(s, c, running(t0 + S, 2000, (t0 + S) | 1));
    CHECK(step(s, c, running(t0 + 50 * S, 2000, (t0 + S) | 1)).action == Do::HOLD);   // wrapped
    CHECK(step(s, c, running(t0 + 50 * S, 1000, (t0 + S) | 1)).action == Do::HOLD);   // < min on
    CHECK(step(s, c, running(t0 + 70 * S, 1000, (t0 + S) | 1)).action == Do::STOP);
  }

  CHECK(why_name(Why::CLEARED)[0] != '\0');
  CHECK(why_name(Why::NONE)[0] == '\0');

  if (failures) {
    std::printf("test_co2_control: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_co2_control: all passed\n");
  return 0;
}
