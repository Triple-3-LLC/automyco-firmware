// Host tests for esphome/self_test_math.h (the self-test's bracketed delta, spread and UNSURE rules).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_self_test_math.cpp -o /tmp/tst && /tmp/tst
// Current numbers are the bench ones from LOG.md: idle rail ~12 mA, Fan 1 +41 mA, Blower +6 mA, Heater
// +141 mA; and the 2026-10-09 false alarm, where the blower read -2 mA against a baseline taken 25 s
// earlier while the camera was still booting.
#include "self_test_math.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace self_test_math;
using load_check::SPECS;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

// SAMPLES readings alternating around `amps` by +/- noise, with the rail fingerprint `others`.
static Measure readings(float amps, float noise = 0, uint32_t others = 1, bool quiet = true) {
  Measure m;
  m.others = others;
  m.quiet = quiet;
  for (int i = 0; i < SAMPLES; i++) m.add(amps + ((i % 2) ? noise : -noise));
  return m;
}

static Bracket bracket(Measure before, Measure on, Measure after) {
  Bracket b;
  b.before = before;
  b.on = on;
  b.after = after;
  return b;
}

static void test_measure_stats() {
  Measure m;
  CHECK(m.empty());
  CHECK(std::isnan(m.mean()));
  m.add(NAN);   // a missing reading is ignored, not counted
  CHECK(m.empty());
  for (float a : {0.010f, 0.014f, 0.012f}) m.add(a);
  CHECK(m.n == 3);
  CHECK(near(m.mean(), 0.012f));
  CHECK(near(m.spread(), 0.004f));
  m.clear();
  CHECK(m.empty() && m.quiet);
}

static void test_delta_is_bracketed() {
  // The rail drifts down 4 mA during the step (camera finishing its boot): one old baseline would read
  // the blower at +2 mA; the bracket reads it at +6 mA, between the two baselines.
  Bracket b = bracket(readings(0.016f), readings(0.018f), readings(0.012f));
  CHECK(near(baseline(b), 0.014f));
  Verdict v = judge(SPECS[load_check::BLOWER], b);
  CHECK(near(v.delta_a, 0.004f));   // 18 - 14, not the +2 mA one stale baseline would give
  CHECK(v.result == Result::UNSURE);   // and a 4 mA drift is too much rail noise for a 6 mA load anyway
  // With a genuine +6 mA step on that 4 mA drift, the bracketed delta is exactly the step.
  b = bracket(readings(0.016f), readings(0.014f + 0.006f), readings(0.012f));
  v = judge(SPECS[load_check::BLOWER], b);
  CHECK(near(v.delta_a, 0.006f));
  // The baseline drift of 4 mA counts as spread; it's wider than the blower's 3 mA limit...
  CHECK(near(v.spread_a, 0.004f));
  // ...and a +6 mA delta minus 4 mA of spread doesn't clear 5 mA, so this stays UNSURE, not FAIL.
  CHECK(v.result == Result::UNSURE);
  CHECK(std::string(v.why) == "noisy rail");
}

static void test_one_baseline_missing() {
  Bracket b = bracket(readings(0.012f), readings(0.053f), Measure());
  CHECK(near(baseline(b), 0.012f));
  CHECK(judge(SPECS[load_check::FAN1], b).result == Result::OK);
  b = bracket(Measure(), readings(0.053f), readings(0.012f));
  CHECK(judge(SPECS[load_check::FAN1], b).result == Result::OK);
  // No baseline at all, or no "on" reading: skipped, never a verdict.
  b = bracket(Measure(), readings(0.053f), Measure());
  Verdict v = judge(SPECS[load_check::FAN1], b);
  CHECK(v.result == Result::SKIPPED);
  CHECK(std::string(v.why) == "no current reading");
  b = bracket(readings(0.012f), Measure(), readings(0.012f));
  CHECK(judge(SPECS[load_check::FAN1], b).result == Result::SKIPPED);
}

static void test_bench_numbers_pass() {
  CHECK(judge(SPECS[load_check::FAN1], bracket(readings(0.012f), readings(0.053f), readings(0.012f))).result == Result::OK);
  CHECK(judge(SPECS[load_check::FAN2], bracket(readings(0.012f), readings(0.051f), readings(0.012f))).result == Result::OK);
  CHECK(judge(SPECS[load_check::BLOWER], bracket(readings(0.012f), readings(0.018f), readings(0.012f))).result == Result::OK);
  CHECK(judge(SPECS[load_check::ATOMIZER], bracket(readings(0.012f), readings(0.196f), readings(0.012f))).result == Result::OK);
  CHECK(judge(SPECS[load_check::HEATER], bracket(readings(0.012f), readings(0.153f), readings(0.012f))).result == Result::OK);
}

static void test_unplugged_fails_only_on_a_quiet_rail() {
  // Fan 1 unplugged, rail quiet (spread 1 mA): FAIL.
  Verdict v = judge(SPECS[load_check::FAN1], bracket(readings(0.012f, 0.0005f), readings(0.013f, 0.0005f), readings(0.012f, 0.0005f)));
  CHECK(v.result == Result::FAIL);
  CHECK(near(v.delta_a, 0.001f));
  CHECK(near(v.spread_a, 0.001f));
  // Same numbers with a 12 mA spread (fan limit 10 mA): UNSURE (noisy rail), not FAIL.
  v = judge(SPECS[load_check::FAN1], bracket(readings(0.012f, 0.006f), readings(0.013f, 0.006f), readings(0.012f, 0.006f)));
  CHECK(v.result == Result::UNSURE);
  CHECK(std::string(v.why) == "noisy rail");
  CHECK(near(v.spread_a, 0.012f));
  // A load that clearly beats its threshold passes even on a noisy rail: atomizer +184 mA, 40 mA spread.
  v = judge(SPECS[load_check::ATOMIZER], bracket(readings(0.012f, 0.020f), readings(0.196f, 0.020f), readings(0.012f, 0.020f)));
  CHECK(v.result == Result::OK);
  // But a borderline one doesn't: blower +6 mA with a 4 mA spread is UNSURE.
  v = judge(SPECS[load_check::BLOWER], bracket(readings(0.012f, 0.002f), readings(0.018f, 0.002f), readings(0.012f, 0.002f)));
  CHECK(v.result == Result::UNSURE);
  CHECK(std::string(v.why) == "noisy rail");
}

static void test_quiet_blower_never_fails() {
  // 2026-10-09: a running blower read -2 mA on a quiet rail. UNSURE (listen for it), never FAIL.
  Verdict v = judge(SPECS[load_check::BLOWER], bracket(readings(0.012f), readings(0.010f), readings(0.012f)));
  CHECK(v.result == Result::UNSURE);
  CHECK(std::string(v.why) == "listen for it");
  CHECK(near(v.delta_a, -0.002f));
  // The same -2 mA from the heater (not quiet) is a FAIL.
  v = judge(SPECS[load_check::HEATER], bracket(readings(0.012f), readings(0.010f), readings(0.012f)));
  CHECK(v.result == Result::FAIL);
}

static void test_rail_change_is_unsure() {
  // Camera power toggled (fingerprint 1 -> 2) between the baselines: UNSURE even with a perfect delta.
  Verdict v = judge(SPECS[load_check::FAN1], bracket(readings(0.012f, 0, 1), readings(0.053f, 0, 1), readings(0.012f, 0, 2)));
  CHECK(v.result == Result::UNSURE);
  CHECK(std::string(v.why) == "rail changed");
  // A light effect running during the "on" reading: the same.
  v = judge(SPECS[load_check::FAN1], bracket(readings(0.012f), readings(0.053f, 0, 1, false), readings(0.012f)));
  CHECK(v.result == Result::UNSURE);
  CHECK(std::string(v.why) == "rail changed");
  // A missing after-baseline can't disagree with anything.
  v = judge(SPECS[load_check::FAN1], bracket(readings(0.012f, 0, 1), readings(0.053f, 0, 1), Measure()));
  CHECK(v.result == Result::OK);
}

static void test_report_format() {
  const auto &fan = SPECS[load_check::FAN1];
  const auto &blower = SPECS[load_check::BLOWER];
  CHECK(report(fan, judge(fan, bracket(readings(0.012f), readings(0.053f), readings(0.012f)))) == "Fan1 OK 41mA; ");
  CHECK(report(fan, judge(fan, bracket(readings(0.012f), readings(0.014f), readings(0.012f)))) == "Fan1 FAIL 2mA; ");
  CHECK(report(blower, judge(blower, bracket(readings(0.012f), readings(0.010f), readings(0.012f)))) ==
        "Blower UNSURE (listen for it) -2mA; ");
  CHECK(report(blower, judge(blower, bracket(readings(0.012f, 0.002f), readings(0.010f, 0.002f), readings(0.012f, 0.002f)))) ==
        "Blower UNSURE (noisy rail) -2mA spread 4mA; ");
  CHECK(report(fan, judge(fan, bracket(readings(0.012f, 0, 1), readings(0.053f, 0, 1), readings(0.012f, 0, 2)))) ==
        "Fan1 UNSURE (rail changed) 41mA spread 0mA; ");
  CHECK(report(fan, judge(fan, bracket(Measure(), Measure(), Measure()))) == "Fan1 SKIPPED (no current reading); ");
}

int main() {
  test_measure_stats();
  test_delta_is_bracketed();
  test_one_baseline_missing();
  test_bench_numbers_pass();
  test_unplugged_fails_only_on_a_quiet_rail();
  test_quiet_blower_never_fails();
  test_rail_change_is_unsure();
  test_report_format();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("self_test_math: all tests passed\n");
  return 0;
}
