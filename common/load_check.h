#pragma once
// "Is this load actually plugged in?" Every load on the TS-01 runs off the 24 V input, which the INA219
// at 0x45 measures, so a load that turns on but adds no current is unplugged or broken.
//
// Two users, one set of numbers:
//  - the self-test (terrashroom-core.yaml) runs each load alone and compares its jump with `ok_a`;
//  - the passive Watcher below never runs anything. It waits for the firmware (or a person) to turn ONE
//    load on while nothing else on the rail changes, compares Load Current before and after, and only
//    then gives a verdict.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_load_check.cpp can test it on a Mac.
#include <cmath>
#include <cstdint>

namespace load_check {

enum Load : uint8_t { FAN1 = 0, FAN2, BLOWER, ATOMIZER, HEATER, COUNT };

// Not called bit(): Arduino.h #defines that.
constexpr uint8_t flag_of(Load l) { return (uint8_t) (1u << l); }

struct Spec {
  const char *name;     // as the self-test report spells it ("Fan1 OK 41mA;")
  const char *label;    // as the entities and the event log spell it ("Fan 1")
  float ok_a;           // a jump of at least this means the load is there (the self-test's thresholds)
  float miss_a;         // passive only: a jump below this counts as a miss; in between proves nothing
  float max_spread_a;   // passive only: readings spread wider than this are too noisy to judge
  float min_level;      // passive only: judge only at or above this output level (0-1)
  uint8_t tolerate_any;     // passive: these loads may switch or change level without spoiling the check
  uint8_t tolerate_level;   // passive: these may change level while staying on (not switch on or off)
  bool quiet = false;       // draws too little to prove it's missing: the self-test says OK or UNSURE, never FAIL
};

// Bench numbers (LOG.md): idle rail 11-16 mA; Fan 1 +41 mA, Fan 2 +39-40 mA (50 % and 100 %);
// Blower +6 mA (100 %); Heater +141 mA (22 % stage); atomizer + blower ~0.20 A together.
//  - Fans: their current barely follows the duty (41 mA at 50 % and at 100 %), so only off -> on says
//    anything, and only at >= 50 %, the lowest speed measured. The idle airflow (3-4 %) is never judged.
//  - Blower: +6 mA against a 5 mA threshold is tight. It's judged only at 100 %, and it's `quiet`: the
//    self-test and the passive check can confirm it, but never call it missing (a running blower has
//    read -2 mA), and readings spread over 3 mA make the check inconclusive rather than wrong.
//  - Atomizer and heater dwarf the blower (<= 6 mA) and the fans' level steps, so the controller turning
//    those on in the same tick (heater + "never heat still air" blower) doesn't spoil their check. A fan
//    switching on (+40 mA) still does.
static const Spec SPECS[COUNT] = {
    {"Fan1", "Fan 1", 0.020f, 0.010f, 0.010f, 0.50f, 0, 0},
    {"Fan2", "Fan 2", 0.020f, 0.010f, 0.010f, 0.50f, 0, 0},
    // quiet: the blower runs off 5 V and adds ~6 mA on the 24 V rail, about the INA219's noise. The
    // 2026-10-09 boot and button self-tests read -2 mA while the owner heard it running.
    {"Blower", "Blower", 0.005f, 0.002f, 0.003f, 1.00f, 0, 0, true},
    {"Atomizer", "Atomizer", 0.050f, 0.025f, 0.025f, 0.0f, flag_of(BLOWER), (uint8_t) (flag_of(FAN1) | flag_of(FAN2))},
    {"Heater", "Heater", 0.030f, 0.015f, 0.020f, 0.0f, flag_of(BLOWER), (uint8_t) (flag_of(FAN1) | flag_of(FAN2))},
};

// What the rail looks like right now.
struct Snapshot {
  bool on[COUNT] = {};
  float level[COUNT] = {};   // output level while on, 0-1 (heater: its PWM stage)
  uint32_t others = 0;       // fingerprint of everything else on the rail: UV-C, both lights, camera
  bool quiet = true;         // false while a light effect or transition makes the current jumpy
};

// FNV-1a step, for building Snapshot::others.
inline uint32_t mix(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 16777619u;
  }
  return h;
}
constexpr uint32_t MIX_SEED = 2166136261u;

struct Verdict {
  enum Result : uint8_t { NONE, DETECTED, MISSED, UNSURE };
  Result result = NONE;
  Load load = COUNT;
  float delta_a = 0;   // after minus before, A
};

class Watcher {
 public:
  static constexpr uint32_t SETTLE_MS = 1500;   // after any change, readings only count after this
  static constexpr uint32_t READ_GAP_MS = 300;  // between extra one-shot reads (INA219: 68 ms per conversion)
  static constexpr uint32_t GIVE_UP_MS = 10000; // no readings this long after a change: drop the check
  static constexpr uint8_t AFTER_SAMPLES = 3;
  static constexpr uint8_t RING = 4;

  // A fresh Load Current reading. Call it from the sensor's on_value, including extra one-shot reads.
  void sample(float amps, uint32_t now) {
    if (std::isnan(amps) || !have_last_ || (uint32_t) (now - changed_at_) < SETTLE_MS) return;
    ring_[head_] = amps;
    head_ = (uint8_t) ((head_ + 1) % RING);
    if (count_ < RING) count_++;
  }

  // Call often (every 100-250 ms) with the current snapshot. Sets *want_read when an extra one-shot
  // reading would help (the caller then reads the INA219, which comes back through sample()).
  Verdict poll(const Snapshot &s, uint32_t now, bool *want_read) {
    Verdict v;
    *want_read = false;
    if (!have_last_) {
      start(s, now);
      return v;
    }
    if (!same(s, last_)) {
      on_change(s, now);
      return v;
    }
    const uint32_t since = now - changed_at_;
    if (since < SETTLE_MS) return v;
    if (pending_ == COUNT && count_ > 0) {
      // Freeze the baseline here, while the old state certainly still held. A reading that lands
      // between a load switching and the next poll would otherwise leak into "before".
      float lo, hi;
      base_avg_ = average(count_, &lo, &hi);
      base_spread_ = hi - lo;
      base_n_ = count_;
    }
    if (pending_ != COUNT) {
      if (count_ >= AFTER_SAMPLES) return judge();
      if (since > GIVE_UP_MS) {
        pending_ = COUNT;
        return v;
      }
      if (count_ < AFTER_SAMPLES) *want_read = read_due(now);
    } else if (count_ == 0) {
      *want_read = read_due(now);   // a baseline now, so a load switched on in a moment can be judged
    }
    return v;
  }

  // Forget everything, e.g. while the self-test runs its own measurements.
  void reset() {
    have_last_ = false;
    pending_ = COUNT;
    count_ = 0;
    base_n_ = 0;
  }

  bool pending() const { return pending_ != COUNT; }

 private:
  void start(const Snapshot &s, uint32_t now) {
    last_ = s;
    have_last_ = true;
    changed_at_ = now;
    count_ = 0;
    base_n_ = 0;
    pending_ = COUNT;
  }

  void on_change(const Snapshot &s, uint32_t now) {
    uint8_t switched = 0, stepped = 0;
    for (int i = 0; i < COUNT; i++) {
      if (s.on[i] != last_.on[i]) switched |= flag_of((Load) i);
      else if (s.on[i] && s.level[i] != last_.level[i]) stepped |= flag_of((Load) i);
    }
    const bool rail_ok = s.others == last_.others && s.quiet && last_.quiet;

    if (pending_ != COUNT) {
      // Already judging a load: keep going only if what changed is too small to matter for it.
      // The baseline stays; the after-readings start over.
      if (!rail_ok || !tolerated(pending_, switched, stepped)) pending_ = COUNT;
    } else if (rail_ok && base_n_ > 0) {
      // The one load that switched on, where everything else that changed is too small to matter
      // for it (the heater with its "never heat still air" blower). Two such loads: judge neither.
      int candidate = -1, found = 0;
      for (int i = 0; i < COUNT; i++) {
        const Load l = (Load) i;
        if (!last_.on[l] && s.on[l] && tolerated(l, switched & ~flag_of(l), stepped)) {
          candidate = i;
          found++;
        }
      }
      if (found == 1) {
        const Load l = (Load) candidate;
        const Spec &spec = SPECS[l];
        if (s.level[l] >= spec.min_level && base_spread_ <= spec.max_spread_a) {
          pending_ = l;
          base_ = base_avg_;
        }
      }
    }
    last_ = s;
    changed_at_ = now;
    count_ = 0;
    base_n_ = 0;
  }

  bool tolerated(Load l, uint8_t switched, uint8_t stepped) const {
    const Spec &spec = SPECS[l];
    if (switched & ~spec.tolerate_any) return false;
    return (stepped & ~(spec.tolerate_any | spec.tolerate_level)) == 0;
  }

  Verdict judge() {
    Verdict v;
    const Spec &spec = SPECS[pending_];
    float lo, hi;
    const float after = average(AFTER_SAMPLES, &lo, &hi);
    v.load = pending_;
    v.delta_a = after - base_;
    if (hi - lo > spec.max_spread_a) v.result = Verdict::UNSURE;
    else if (v.delta_a >= spec.ok_a) v.result = Verdict::DETECTED;
    // A quiet load (the 5 V blower, ~6 mA on the 24 V rail, inside the INA219's noise) is never called
    // missing: a running blower read -2 mA on 2026-10-09. It can only be confirmed, or stay unsure.
    else if (v.delta_a < spec.miss_a) v.result = spec.quiet ? Verdict::UNSURE : Verdict::MISSED;
    else v.result = Verdict::UNSURE;
    pending_ = COUNT;
    return v;
  }

  // Mean of the newest n readings (n <= count_), with their min and max.
  float average(uint8_t n, float *lo, float *hi) const {
    float sum = 0;
    *lo = INFINITY;
    *hi = -INFINITY;
    for (uint8_t k = 0; k < n; k++) {
      const float x = ring_[(head_ + RING - 1 - k) % RING];
      sum += x;
      if (x < *lo) *lo = x;
      if (x > *hi) *hi = x;
    }
    return sum / n;
  }

  bool read_due(uint32_t now) {
    if (asked_ && (uint32_t) (now - asked_at_) < READ_GAP_MS) return false;
    asked_ = true;
    asked_at_ = now;
    return true;
  }

  static bool same(const Snapshot &a, const Snapshot &b) {
    if (a.others != b.others || a.quiet != b.quiet) return false;
    for (int i = 0; i < COUNT; i++)
      if (a.on[i] != b.on[i] || (a.on[i] && a.level[i] != b.level[i])) return false;
    return true;
  }

  Snapshot last_;
  bool have_last_ = false;
  uint32_t changed_at_ = 0;
  float ring_[RING] = {};
  uint8_t head_ = 0, count_ = 0;
  Load pending_ = COUNT;
  float base_ = 0;                 // baseline of the load being judged
  float base_avg_ = 0, base_spread_ = 0;   // baseline frozen at the last poll
  uint8_t base_n_ = 0;
  bool asked_ = false;
  uint32_t asked_at_ = 0;
};

// The firmware's one watcher. A function-local static, because ESPHome declares `globals:` before it
// pulls in `includes:`, so a global of type Watcher won't compile.
inline Watcher &watcher() {
  static Watcher w;
  return w;
}

}  // namespace load_check
