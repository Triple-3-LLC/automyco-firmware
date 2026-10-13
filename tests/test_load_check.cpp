// Host tests for esphome/common/load_check.h (the passive "load not connected" check).
//   c++ -std=c++17 -Wall -Wextra -I esphome/common esphome/tests/test_load_check.cpp -o /tmp/tlc && /tmp/tlc
// Simulates the chamber: a 200 ms poll, the INA219's own 2 s readings, and the one-shot reads the
// watcher asks for. Current numbers are the bench ones from LOG.md.
#include "load_check.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace load_check;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

// A rail whose current is the idle draw plus whatever each running load really draws.
struct Rail {
  Snapshot s;
  float idle = 0.012f;
  float draw[COUNT] = {0.041f, 0.039f, 0.006f, 0.190f, 0.141f};   // set to 0 to "unplug" a load
  float noise = 0;      // added to alternate readings (+/-)
  float extra = 0;      // other things on the rail (a light)
  uint32_t now = 0;
  int flip = 0;
  Watcher w;
  std::vector<Verdict> verdicts;

  float read() {
    float a = idle + extra;
    for (int i = 0; i < COUNT; i++)
      if (s.on[i]) a += draw[i];
    flip = !flip;
    return a + (flip ? noise : -noise);
  }

  // Runs for `ms`, polling every 200 ms, with the INA219's own reading every 2 s.
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
      now += 100;
      if (now % 2000 == 0) w.sample(read(), now);
      if (now % 200 == 0) {
        bool want = false;
        Verdict v = w.poll(s, now, &want);
        if (v.result != Verdict::NONE) verdicts.push_back(v);
        if (want) w.sample(read(), now);
      }
    }
  }

  void set(Load l, bool on, float level = 1.0f) {
    s.on[l] = on;
    s.level[l] = on ? level : 0;
  }

  const Verdict *last() const { return verdicts.empty() ? nullptr : &verdicts.back(); }
};

static void test(const char *name, const std::function<void()> &fn) {
  const int before = failures;
  fn();
  std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main() {
  test("a fan switched on alone is detected", [] {
    Rail r;
    r.run(5000);
    r.set(FAN1, true, 0.5f);
    r.run(5000);
    CHECK(r.verdicts.size() == 1);
    CHECK(r.last() && r.last()->load == FAN1 && r.last()->result == Verdict::DETECTED);
    CHECK(r.last() && std::fabs(r.last()->delta_a - 0.041f) < 0.002f);
  });

  test("an unplugged fan is a miss", [] {
    Rail r;
    r.draw[FAN2] = 0;
    r.run(5000);
    r.set(FAN2, true, 1.0f);
    r.run(5000);
    CHECK(r.last() && r.last()->load == FAN2 && r.last()->result == Verdict::MISSED);
  });

  test("fans below 50 % are never judged", [] {
    Rail r;
    r.draw[FAN1] = 0;
    r.run(5000);
    r.set(FAN1, true, 0.03f);   // the idle airflow
    r.run(8000);
    CHECK(r.verdicts.empty());
  });

  test("a speed change on a running fan is not judged", [] {
    Rail r;
    r.set(FAN1, true, 0.5f);
    r.run(5000);
    r.verdicts.clear();
    r.set(FAN1, true, 1.0f);
    r.run(5000);
    CHECK(r.verdicts.empty());
  });

  test("two loads switched on together are not judged", [] {
    Rail r;
    r.draw[FAN2] = 0;
    r.run(5000);
    r.set(FAN1, true, 1.0f);
    r.set(FAN2, true, 1.0f);
    r.run(8000);
    CHECK(r.verdicts.empty());
  });

  test("a second load within the window cancels the check", [] {
    Rail r;
    r.draw[FAN1] = 0;
    r.run(5000);
    r.set(FAN1, true, 1.0f);
    r.run(600);
    r.set(FAN2, true, 1.0f);
    r.run(8000);
    CHECK(r.verdicts.empty());
  });

  test("heater + blower in one tick still judges the heater", [] {
    Rail r;
    r.set(FAN1, true, 0.03f);
    r.run(5000);
    r.set(HEATER, true, 0.22f);
    r.set(BLOWER, true, 0.30f);   // "never heat still air"
    r.s.level[FAN1] = 0.08f;      // and the controller steps the idle fan up
    r.run(5000);
    CHECK(r.last() && r.last()->load == HEATER && r.last()->result == Verdict::DETECTED);
  });

  test("an unplugged heater is a miss even with the blower coming on", [] {
    Rail r;
    r.draw[HEATER] = 0;
    r.run(5000);
    r.set(HEATER, true, 0.22f);
    r.run(500);
    r.set(BLOWER, true, 0.30f);   // the interlock, one 1 s tick later
    r.run(5000);
    CHECK(r.last() && r.last()->load == HEATER && r.last()->result == Verdict::MISSED);
  });

  test("a fan switching on alongside the heater spoils its check", [] {
    Rail r;
    r.draw[HEATER] = 0;
    r.run(5000);
    r.set(HEATER, true, 0.22f);
    r.set(FAN2, true, 0.5f);
    r.run(8000);
    CHECK(r.verdicts.empty());
  });

  test("the blower at 100 % is detected, and never called missing (it's quiet)", [] {
    Rail r;
    r.run(5000);
    r.set(BLOWER, true, 1.0f);
    r.run(5000);
    CHECK(r.last() && r.last()->load == BLOWER && r.last()->result == Verdict::DETECTED);
    Rail u;
    u.draw[BLOWER] = 0;
    u.run(5000);
    u.set(BLOWER, true, 1.0f);
    u.run(5000);
    CHECK(u.last() && u.last()->load == BLOWER && u.last()->result == Verdict::UNSURE);
    // 2026-10-09: the owner heard it running while the rail read 2 mA lower than before.
    Rail h;
    h.draw[BLOWER] = -0.002f;
    h.run(5000);
    h.set(BLOWER, true, 1.0f);
    h.run(5000);
    for (const auto &v : h.verdicts) CHECK(v.result != Verdict::MISSED);
  });

  test("a noisy rail is inconclusive for the blower, never a miss", [] {
    Rail r;
    r.draw[BLOWER] = 0.006f;
    r.noise = 0.0025f;   // 5 mA peak to peak, like the 11-16 mA idle readings
    r.run(5000);
    r.set(BLOWER, true, 1.0f);
    r.run(5000);
    for (const auto &v : r.verdicts) CHECK(v.result != Verdict::MISSED);
  });

  test("a light change or effect blocks judging", [] {
    Rail r;
    r.draw[ATOMIZER] = 0;
    r.run(5000);
    r.s.others = mix(MIX_SEED, 1);   // top light switched on
    r.extra = 0.046f;
    r.set(ATOMIZER, true);
    r.run(8000);
    CHECK(r.verdicts.empty());
    r.s.quiet = false;   // an effect starts
    r.run(5000);
    r.set(ATOMIZER, false);
    r.run(5000);
    r.set(ATOMIZER, true);
    r.run(5000);
    CHECK(r.verdicts.empty());
  });

  test("a reading taken just after the switch doesn't leak into the baseline", [] {
    Rail r;
    r.run(6000);
    r.set(ATOMIZER, true);
    r.w.sample(r.read(), r.now + 50);   // an INA219 reading lands before the next poll sees the switch
    r.run(6000);
    CHECK(r.last() && r.last()->load == ATOMIZER && r.last()->result == Verdict::DETECTED);
    CHECK(r.last() && std::fabs(r.last()->delta_a - 0.190f) < 0.002f);
  });

  test("a load switched off is never judged", [] {
    Rail r;
    r.set(ATOMIZER, true);
    r.run(5000);
    r.verdicts.clear();
    r.set(ATOMIZER, false);
    r.run(5000);
    CHECK(r.verdicts.empty());
  });

  test("reset forgets a pending check", [] {
    Rail r;
    r.run(5000);
    r.set(FAN1, true, 1.0f);
    r.run(400);
    r.w.reset();
    r.draw[FAN1] = 0;
    r.run(5000);
    CHECK(r.verdicts.empty());
  });

  test("with no baseline yet, a switch is not judged", [] {
    Rail r;
    r.run(400);   // still settling from the first poll
    r.set(FAN1, true, 1.0f);
    r.run(5000);
    CHECK(r.verdicts.empty());
  });

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
