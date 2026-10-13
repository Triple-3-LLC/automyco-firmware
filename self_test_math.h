#pragma once
// The self-test's arithmetic: one bracketed measurement per load and the verdict it earns.
//
// The 2026-10-09 boot self-test called the blower missing ("Blower FAIL -2 mA") while the owner heard it
// run. The blower adds ~6 mA to the 24 V rail against a 5 mA threshold, and the old test compared it with
// ONE baseline taken 25 s earlier, from 3 readings, with nothing checking that the rail had stayed quiet.
// A camera still booting, or the bottom light's Status effect pulsing its brightness, drifts the rail by
// more than the blower's whole signal. So each load is now bracketed:
//
//   before = SAMPLES one-shot readings just before the load switches on
//   on     = SAMPLES readings once it has run for its settle time
//   after  = SAMPLES readings once it is off again
//   delta  = on - mean(before, after)
//
// and the verdict is UNSURE, never FAIL, when the readings spread too wide, when the baselines disagree,
// or when anything else on the rail (UV-C, lights, camera power: the load_check "others" fingerprint)
// changed between the two baselines. FAIL needs a quiet rail AND a delta below the load's threshold.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_self_test_math.cpp can test it on a Mac.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "load_check.h"

namespace self_test_math {

constexpr uint8_t SAMPLES = 8;   // one-shot INA219 readings per measurement (was 3)

// One measurement: the mean and spread of up to SAMPLES readings, plus what the rail looked like.
struct Measure {
  float sum = 0, lo = INFINITY, hi = -INFINITY;
  uint8_t n = 0;
  uint32_t others = 0;   // load_check "others" fingerprint when the measurement started
  bool quiet = true;     // false if a light effect or transition was running during it

  void clear() { *this = Measure(); }
  void add(float amps) {
    if (std::isnan(amps)) return;
    sum += amps;
    n++;
    if (amps < lo) lo = amps;
    if (amps > hi) hi = amps;
  }
  bool empty() const { return n == 0; }
  float mean() const { return n ? sum / n : NAN; }
  float spread() const { return n ? hi - lo : 0; }
};

struct Bracket {
  Measure before, on, after;
  void clear() { *this = Bracket(); }
};

enum class Result : uint8_t { OK, FAIL, UNSURE, SKIPPED };

struct Verdict {
  Result result = Result::SKIPPED;
  float delta_a = NAN;    // on minus the bracketed baseline, A
  float spread_a = 0;     // the widest spread among the three measurements and the baseline drift, A
  const char *why = "";   // UNSURE/SKIPPED: "noisy rail", "rail changed", "listen for it", "no current reading"
};

// mean(before, after); whichever exists if only one does; NAN if neither.
inline float baseline(const Bracket &b) {
  if (!b.before.empty() && !b.after.empty()) return (b.before.mean() + b.after.mean()) / 2;
  if (!b.before.empty()) return b.before.mean();
  if (!b.after.empty()) return b.after.mean();
  return NAN;
}

// The rail changed under the test: a different "others" fingerprint between any two measurements, or a
// light effect/transition running during one.
inline bool rail_changed(const Bracket &b) {
  const Measure *ms[3] = {&b.before, &b.on, &b.after};
  const Measure *ref = nullptr;
  for (auto *m : ms) {
    if (m->empty()) continue;
    if (!m->quiet) return true;
    if (ref == nullptr) ref = m;
    else if (m->others != ref->others) return true;
  }
  return false;
}

inline Verdict judge(const load_check::Spec &spec, const Bracket &b) {
  Verdict v;
  const float base = baseline(b);
  if (b.on.empty() || std::isnan(base)) {
    v.why = "no current reading";
    return v;
  }
  v.delta_a = b.on.mean() - base;
  v.spread_a = std::fmax(std::fmax(b.before.spread(), b.on.spread()), b.after.spread());
  if (!b.before.empty() && !b.after.empty())
    v.spread_a = std::fmax(v.spread_a, std::fabs(b.before.mean() - b.after.mean()));   // baseline drift
  if (rail_changed(b)) {
    v.result = Result::UNSURE;
    v.why = "rail changed";
    return v;
  }
  const bool noisy = v.spread_a > spec.max_spread_a;
  // Clearly there even at the noisy end: the delta beats the threshold with the whole spread taken off.
  if (v.delta_a >= spec.ok_a && (!noisy || v.delta_a - v.spread_a >= spec.ok_a)) {
    v.result = Result::OK;
    return v;
  }
  v.result = Result::UNSURE;
  if (noisy) {
    v.why = "noisy rail";
  } else if (spec.quiet) {
    v.why = "listen for it";   // too small a load to tell from the sensor's noise; never called missing
  } else {
    v.result = Result::FAIL;
  }
  return v;
}

// One item of the Self-Test Result text, in the form the apps parse ("<Load> <VERDICT> <detail>; "):
//   "Fan1 OK 41mA; "   "Heater FAIL 2mA; "   "Blower UNSURE (noisy rail) -2mA spread 4mA; "
//   "Blower UNSURE (listen for it) -2mA; "   "Fan1 SKIPPED (no current reading); "
inline std::string report(const load_check::Spec &spec, const Verdict &v) {
  char buf[72];
  const float ma = v.delta_a * 1000.0f;
  switch (v.result) {
    case Result::OK:
      snprintf(buf, sizeof(buf), "%s OK %.0fmA; ", spec.name, ma);
      break;
    case Result::FAIL:
      snprintf(buf, sizeof(buf), "%s FAIL %.0fmA; ", spec.name, ma);
      break;
    case Result::UNSURE:
      if (v.why[0] == 'l')   // listen for it: the rail was quiet, the spread adds nothing
        snprintf(buf, sizeof(buf), "%s UNSURE (%s) %.0fmA; ", spec.name, v.why, ma);
      else
        snprintf(buf, sizeof(buf), "%s UNSURE (%s) %.0fmA spread %.0fmA; ", spec.name, v.why, ma, v.spread_a * 1000.0f);
      break;
    case Result::SKIPPED:
      snprintf(buf, sizeof(buf), "%s SKIPPED (%s); ", spec.name, v.why);
      break;
  }
  return buf;
}

// The firmware's self-test state. A function-local static, because ESPHome declares `globals:` before it
// pulls in `includes:`, so a global of these types won't compile (same trick as load_check::watcher()).
struct Session {
  Measure cur;            // what st_measure is filling right now
  Bracket bracket;        // the load under test
  uint32_t light_effect[2] = {0, 0};   // effects paused for the test (top, bottom), restored after it
};

inline Session &session() {
  static Session s;
  return s;
}

}  // namespace self_test_math
