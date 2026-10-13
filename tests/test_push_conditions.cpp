// Host tests for esphome/common/push_conditions.h (the sensor_stuck and climate_out_of_band push alerts).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_push_conditions.cpp -o /tmp/tst && /tmp/tst
#include "push_conditions.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace push_cond;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const uint32_t S = 1000, MIN = 60 * S, H = 60 * MIN;

// The 1.2.2 logic, verbatim from push-alerts.yaml, to show the false alarm it raised.
struct Legacy {
  float t = NAN, h = NAN;
  uint32_t changed = 0, inband_at = 0;
  bool stuck(uint32_t now, float nt, float nh, bool climate_on) {
    if (!std::isnan(nt) && !std::isnan(nh) && (nt != t || nh != h || changed == 0)) {
      t = nt; h = nh; changed = now | 1;
    }
    return climate_on && changed != 0 && (uint32_t) (now - changed) > 2UL * 3600000UL;
  }
  bool oob(uint32_t now, bool judged_out) {
    if (!judged_out) inband_at = now | 1;
    return inband_at != 0 && (uint32_t) (now - inband_at) > 3UL * 3600000UL;
  }
};

// The owner's chamber on 2026-10-11: T flips 71.846 <-> 71.864 °F (inside the tolerance on its own), RH
// wanders 84-89 % with the misting cycle. A new reading every 10 s.
static float jitter_t(uint32_t i) { return (i / 10) % 2 ? 71.864f : 71.846f; }
static float jitter_h(uint32_t i) { return 84.0f + (float) ((i / 10) % 50) * 0.1f; }

// The 1 s interval, as push-alerts.yaml runs it: rising edges of the condition, and how many would
// get past the 30-min per-kind throttle.
struct Edges {
  bool prev = false;
  uint32_t rises = 0, pushes = 0, last_push = 0;
  void sample(uint32_t now, bool flag) {
    if (flag && !prev) {
      rises++;
      if (last_push == 0 || now - last_push >= 30 * MIN) { pushes++; last_push = now; }
    }
    prev = flag;
  }
};

int main() {
  // --- elapsed(): a stamp taken this tick is 0 ms old, even on an even millisecond.
  CHECK(elapsed(1000, stamp(1000)) == 0);   // stamp 1001: 1 ms ahead
  CHECK(elapsed(1001, stamp(1001)) == 0);
  CHECK(elapsed(1001, stamp(1000)) == 0);
  CHECK(elapsed(2000, stamp(1000)) == 999);
  CHECK(elapsed(5, stamp(0xFFFFFFF0u)) == 20);   // across the millis() wrap
  CHECK(elapsed(0, stamp(0)) == 0);

  // --- Reproduction: 1.2.2 on the owner's chamber (booted at ~07:29, readings moving, in band).
  {
    Legacy old;
    Edges stuck, oob;
    const uint32_t boot = 2 * S;
    for (uint32_t i = 0; i < 5 * 3600; i++) {
      const uint32_t now = boot + i * S + (i % 3 == 0 ? 1 : 0);   // the 1 s interval lands on odd and even ms
      stuck.sample(now, old.stuck(now, jitter_t(i), jitter_h(i), true));
      oob.sample(now, old.oob(now, false));
    }
    CHECK(stuck.rises > 100);   // a one-second "stuck" whenever a new reading lands on an even ms
    CHECK(oob.rises > 1000);    // and "out of band" on every in-band second that does
    CHECK(stuck.pushes == 10);  // one every 30 min from boot, as the Lambda logged (07:28:55, 07:59:01, ...)
    CHECK(oob.pushes == 10);
  }

  // --- 1. Normal jitter: no alert in 12 h, on even and odd milliseconds alike.
  for (uint32_t boot : {2 * S, 2 * S + 1, 0xFFFFFFFFu - 3 * H}) {   // the last one crosses the millis() wrap
    float rt = NAN, rh = NAN;
    uint32_t since = 0, inband = 0;
    Edges stuck, oob;
    for (uint32_t i = 0; i < 12 * 3600; i++) {
      const uint32_t now = boot + i * S + (i % 3 == 0 ? 1 : 0);   // the interval drifts by a millisecond now and then
      stuck.sample(now, sensor_stuck(rt, rh, since, now, jitter_t(i), jitter_h(i), true, false));
      oob.sample(now, out_of_band(inband, now, false));
    }
    CHECK(stuck.rises == 0);
    CHECK(oob.rises == 0);
  }

  // A slow drift (0.01 per reading, under the tolerance each step) still counts as moving.
  {
    float rt = NAN, rh = NAN;
    uint32_t since = 0;
    bool any = false;
    for (uint32_t i = 0; i < 6 * 3600; i++)
      any |= sensor_stuck(rt, rh, since, 2 * S + i * S, 70.0f + (float) (i / 10) * 0.01f, 88.0f, true, false);
    CHECK(!any);
  }

  // --- 2. Frozen readings: the alert comes on after 2 h, not before, and goes off when they move.
  {
    float rt = NAN, rh = NAN;
    uint32_t since = 0;
    const uint32_t boot = 2 * S;
    Edges stuck;
    uint32_t first_on = 0;
    for (uint32_t i = 0; i <= 2 * 3600 + 5; i++) {
      const uint32_t now = boot + i * S;
      const bool f = sensor_stuck(rt, rh, since, now, 71.8f, 86.2f, true, false);
      if (f && !first_on) first_on = now;
      stuck.sample(now, f);
    }
    CHECK(first_on == boot + 2 * H + S);   // first sample more than 2 h after the first reading
    CHECK(stuck.rises == 1);
    // Frozen within the tolerance (one LSB of noise) is frozen too.
    CHECK(sensor_stuck(rt, rh, since, boot + 2 * H + 10 * S, 71.83f, 86.17f, true, false));
    // A real move clears it at once.
    CHECK(!sensor_stuck(rt, rh, since, boot + 2 * H + 20 * S, 71.9f, 86.2f, true, false));
  }

  // A sensor that stops publishing (NaN) after a last value is stuck after 2 h as well.
  {
    float rt = NAN, rh = NAN;
    uint32_t since = 0;
    CHECK(!sensor_stuck(rt, rh, since, 10 * S, 71.8f, 86.2f, true, false));
    CHECK(!sensor_stuck(rt, rh, since, 10 * S + 2 * H, NAN, NAN, true, false));
    CHECK(sensor_stuck(rt, rh, since, 11 * S + 2 * H, NAN, NAN, true, false));
    // No reading at all yet: nothing to judge.
    float nt = NAN, nh = NAN;
    uint32_t ns = 0;
    CHECK(!sensor_stuck(nt, nh, ns, 3 * H, NAN, NAN, true, false));
  }

  // Climate Control off: never stuck, and turning it on starts a fresh 2 h.
  {
    float rt = NAN, rh = NAN;
    uint32_t since = 0;
    bool any = false;
    for (uint32_t i = 0; i < 4 * 3600; i++) any |= sensor_stuck(rt, rh, since, 2 * S + i * S, 71.8f, 86.2f, false, false);
    CHECK(!any);
    const uint32_t on_at = 2 * S + 4 * H;
    CHECK(!sensor_stuck(rt, rh, since, on_at + 2 * H - S, 71.8f, 86.2f, true, false));
    CHECK(sensor_stuck(rt, rh, since, on_at + 2 * H + S, 71.8f, 86.2f, true, false));
  }

  // --- 3. Dry pulses: the readings are held (30 s heat + 3 min cool-down), which isn't a stuck sensor.
  {
    float rt = NAN, rh = NAN;
    uint32_t since = 0;
    const uint32_t boot = 2 * S;
    bool during = false, after = false;
    // Readings already unchanged for 1 h 58 min, then a dry pulse that would carry them past 2 h.
    for (uint32_t i = 0; i < 118 * 60; i++) sensor_stuck(rt, rh, since, boot + i * S, 71.8f, 86.2f, true, false);
    for (uint32_t i = 118 * 60; i < 118 * 60 + 210; i++)
      during |= sensor_stuck(rt, rh, since, boot + i * S, 71.8f, 86.2f, true, true);
    CHECK(!during);
    // The wait restarts after the pulse: still nothing 1 h 59 min later.
    for (uint32_t i = 118 * 60 + 210; i < 118 * 60 + 210 + 119 * 60; i++)
      after |= sensor_stuck(rt, rh, since, boot + i * S, 71.8f, 86.2f, true, false);
    CHECK(!after);
    // Pulsing every 6 h on a moving sensor never alerts either.
    float pt = NAN, ph = NAN;
    uint32_t ps = 0;
    bool any = false;
    for (uint32_t i = 0; i < 24 * 3600; i++) {
      const bool heating = i % (6 * 3600) < 210;
      any |= sensor_stuck(pt, ph, ps, boot + i * S, jitter_t(i), jitter_h(i), true, heating);
    }
    CHECK(!any);
  }

  // --- climate_out_of_band: 3 h out, not before; back in band clears it.
  {
    uint32_t inband = 0;
    const uint32_t boot = 2 * S;
    CHECK(!out_of_band(inband, boot, false));
    CHECK(!out_of_band(inband, boot + 3 * H, true));   // just left the band: the 3 h starts from the last in-band second
    inband = 0;
    CHECK(!out_of_band(inband, boot, false));
    CHECK(!out_of_band(inband, boot + S, true));
    CHECK(!out_of_band(inband, boot + 3 * H, true));
    CHECK(out_of_band(inband, boot + 3 * H + S, true));
    CHECK(!out_of_band(inband, boot + 3 * H + 2 * S, false));
    CHECK(!out_of_band(inband, boot + 3 * H + 3 * S, true));
    // Out of band from the first sample after boot: 3 h from then.
    uint32_t b2 = 0;
    CHECK(!out_of_band(b2, boot, true));
    CHECK(!out_of_band(b2, boot + 3 * H, true));
    CHECK(out_of_band(b2, boot + 3 * H + S, true));
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("push_conditions: all tests passed\n");
  return 0;
}
