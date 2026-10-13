// Host tests for esphome/common/grow_climate.h (species presets, the humidity target per grow phase, and
// the mist-start band) together with common/mist_cycle.h.
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_grow_climate.cpp -o /tmp/tst && /tmp/tst
#include "grow_climate.h"
#include "mist_cycle.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace grow_climate;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A crude chamber for the short-cycling check: RH leaks toward the room at a fixed time constant, each
// second of atomizer time adds rise_per_s, and the drying fans pull it down a little faster. The controller
// part copies terrashroom-core.yaml: a started cycle runs to its end; a new one starts only via start();
// drying starts at target + 2 and stops at the target.
struct SimResult {
  int cycles = 0;
  uint32_t min_gap_ms = 0xFFFFFFFFu;
  float mean = 0, lo = 100, hi = 0;
  int dry_starts = 0;
};

template <typename StartFn>
static SimResult simulate(float target, float rise_per_s, StartFn start) {
  const mist_cycle::Timing tm = mist_cycle::timing(6000, 1500, 1000);   // the 1.2 defaults: 6 s in 1.5/1.0 pulses
  SimResult r;
  float h = target - 6.0f;
  uint32_t cycle_start = 0, last_start = 0;
  bool drying = false;
  double sum = 0;
  int n = 0;
  const uint32_t hours = 12;
  for (uint32_t now = 1000; now < hours * 3600000UL; now += 1000) {
    bool cycle = cycle_start != 0 && mist_cycle::elapsed(now, cycle_start) < tm.cycle_ms;
    if (!cycle && start(h, target)) {
      cycle_start = mist_cycle::stamp(now);
      cycle = true;
      if (last_start != 0) r.min_gap_ms = std::min(r.min_gap_ms, now - last_start);
      last_start = now;
      r.cycles++;
    }
    if (!cycle) cycle_start = 0;
    if (!drying && h >= target + 2.0f) { drying = true; r.dry_starts++; }
    if (drying && h <= target) drying = false;
    const bool mist = cycle && mist_cycle::phase(mist_cycle::elapsed(now, cycle_start), tm) == mist_cycle::Phase::MIST &&
                      mist_cycle::pulse_on(mist_cycle::elapsed(now, cycle_start), 1500, 1000);
    h += (70.0f - h) / 1800.0f;   // leak toward a 70 % room, tau 30 min
    if (mist) h += rise_per_s;
    if (drying && !cycle) h -= 0.01f;
    if (now > 3600000UL) {   // after an hour of settling
      sum += h;
      n++;
      r.lo = std::min(r.lo, h);
      r.hi = std::max(r.hi, h);
    }
  }
  r.mean = (float) (sum / n);
  return r;
}

int main() {
  // --- Presets: the audited values (recipe-research.md rec. 3) and the unchanged ones.
  struct Want { const char *name; float t, rh; int x; float pin; };
  const Want want[] = {
      {"Shiitake", 70, 85, 8, 94},       {"Lion's Mane", 70, 87, 11, 92},  {"Reishi", 72, 87, 7, 94},
      {"Pink Oyster", 75, 85, 10, 94},   {"Golden Oyster", 73, 91, 10, 94}, {"Blue Oyster", 64, 85, 10, 94},
      {"Chestnut", 70, 90, 9, 94},       {"King Trumpet", 60, 86, 10, 94}, {"Italian Oyster", 73, 89, 10, 94},
      {"Snow Oyster", 60, 90, 9, 94},    {"Black King", 70, 92, 9, 92},    {"Pioppino", 70, 93, 10, 94},
      {"Nameko", 58, 90, 8, 94},
  };
  CHECK(sizeof(PRESETS) / sizeof(PRESETS[0]) == sizeof(want) / sizeof(want[0]));
  for (const auto &w : want) {
    const Preset *p = find_preset(w.name);
    CHECK(p != nullptr);
    if (p == nullptr) continue;
    CHECK(near(p->temp_f, w.t));
    CHECK(near(p->rh, w.rh));
    CHECK(p->airflow == w.x);
    CHECK(near(p->pinning_rh, w.pin));
    // Inside the number entities' limits (Temperature Setpoint 55-85 °F, Humidity Setpoint 50-99 %) and
    // never above the pinning cap.
    CHECK(p->temp_f >= 55 && p->temp_f <= 85);
    CHECK(p->rh >= 50 && p->rh <= 99);
    CHECK(p->pinning_rh <= PINNING_RH_CAP);
  }
  CHECK(find_preset("Custom") == nullptr);
  CHECK(find_preset("lion's mane") == nullptr);   // exact select option names only
  CHECK(find_preset(nullptr) == nullptr);

  // --- Boot never applies a preset: the select's restore at boot leaves the restored setpoints alone, so an
  // update to new preset values doesn't touch running grows. Only a pick after boot applies.
  for (const auto &w : want) {
    CHECK(preset_to_apply(w.name, false) == nullptr);
    CHECK(preset_to_apply(w.name, true) == find_preset(w.name));
  }
  CHECK(preset_to_apply("Custom", true) == nullptr);

  // --- Pinning target.
  // Known species: max(setpoint, species pinning RH), capped at 94.
  CHECK(near(pinning_target(87, 92), 92));   // Lion's Mane preset: 90 before, 92 now
  CHECK(near(pinning_target(92, 92), 92));   // Black King: 94 before, 92 now
  CHECK(near(pinning_target(91, 94), 94));   // Golden Oyster
  CHECK(near(pinning_target(85, 94), 94));   // Shiitake: 88 before
  CHECK(near(pinning_target(93, 92), 93));   // the owner raised Lion's Mane to 93: the higher setpoint wins
  CHECK(near(pinning_target(98, 92), 94));   // never above the cap
  CHECK(near(pinning_target(80, 92), 92));   // the species value is a floor
  // No species (Custom picked by hand, unknown cloud recipe): the 1.2.6 rule.
  CHECK(near(pinning_target(87, 0), 90));
  CHECK(near(pinning_target(92, 0), 94));
  CHECK(near(pinning_target(95, 0), 94));
  CHECK(std::isnan(pinning_target(NAN, 92)));   // no setpoint yet: no target (the controller idles)

  // --- Which species value is in force.
  CHECK(near(pinning_rh_for("Lion's Mane", 0), 92));   // updated from 1.2.6 with a species selected: works at once
  CHECK(near(pinning_rh_for("Lion's Mane", 94), 92));  // the preset's own value beats a stale kept one
  CHECK(near(pinning_rh_for("Custom", 92), 92));       // a setpoint hand edit kept Lion's Mane's value
  CHECK(near(pinning_rh_for("Custom", 0), 0));         // Custom by hand: old rule
  CHECK(near(species_pinning_rh("Black King"), 92));
  CHECK(near(species_pinning_rh("My Recipe"), 0));

  // --- Humidity target per phase.
  CHECK(phase_from("Incubation") == Phase::INCUBATION);
  CHECK(phase_from("Pinning") == Phase::PINNING);
  CHECK(phase_from("Fruiting") == Phase::FRUITING);
  CHECK(phase_from("") == Phase::FRUITING);
  CHECK(phase_from(nullptr) == Phase::FRUITING);
  CHECK(near(humidity_target(Phase::PINNING, 87, 92), 92));
  CHECK(near(humidity_target(Phase::FRUITING, 87, 92), 87));    // Fruiting runs the setpoint as it is
  CHECK(near(humidity_target(Phase::INCUBATION, 87, 92), 87));

  // --- Mist-start band: 2 under the target in both phases (Fruiting was 5 up to 1.2.6).
  {
    const float fruit = humidity_target(Phase::FRUITING, 87, 92);   // 87
    CHECK(mist_may_start(85.0f, fruit));
    CHECK(!mist_may_start(85.1f, fruit));
    CHECK(!mist_may_start(86.0f, fruit));   // stock waited until 82 here
    CHECK(in_top_up_band(85.1f, fruit));
    CHECK(in_top_up_band(87.0f, fruit));
    CHECK(!in_top_up_band(85.0f, fruit));
    CHECK(!in_top_up_band(87.1f, fruit));
    const float pin = humidity_target(Phase::PINNING, 87, 92);      // 92
    CHECK(mist_may_start(90.0f, pin));
    CHECK(!mist_may_start(90.5f, pin));
    CHECK(in_top_up_band(91.0f, pin));
    // The 97 % hard ceiling holds whatever the target (Fruiting setpoint 99).
    CHECK(mist_may_start(96.9f, 99));
    CHECK(!mist_may_start(97.0f, 99));
    // No reading / no setpoint: never start.
    CHECK(!mist_may_start(NAN, 87));
    CHECK(!mist_may_start(80, NAN));
  }

  // --- Short-cycling: with the narrower band, cycles still start at least a whole cycle (60 s) apart, the
  // drying fans don't fight every cycle at the default burst, and the realised RH moves up to the target instead of sitting
  // ~4 under it. Compared against the stock -5 band on the same crude chamber.
  {
    const auto stock = [](float h, float t) { return h <= t - 5.0f && h < 97.0f; };
    const auto now_band = [](float h, float t) { return mist_may_start(h, t); };
    for (float rise : {0.5f, 0.9f}) {   // 3 % and 5.4 % per 6 s cycle, the two measured rises (LOG.md 10-11)
      const SimResult old_r = simulate(87, rise, stock);
      const SimResult new_r = simulate(87, rise, now_band);
      std::printf("  rise %.1f %%/s: stock mean %.1f (%.1f-%.1f) %d cycles | -2 band mean %.1f (%.1f-%.1f) %d cycles, "
                  "min gap %u s, %d dry starts\n",
                  rise, old_r.mean, old_r.lo, old_r.hi, old_r.cycles, new_r.mean, new_r.lo, new_r.hi, new_r.cycles,
                  (unsigned) (new_r.min_gap_ms / 1000), new_r.dry_starts);
      CHECK(new_r.cycles > 0);
      CHECK(new_r.min_gap_ms >= 60000u);           // a cycle always finishes before the next can start
      CHECK(new_r.mean > old_r.mean + 1.5f);       // the point of the change
      CHECK(new_r.mean > 85.0f && new_r.mean < 89.0f);
      CHECK(new_r.hi < 95.0f);                     // well clear of the soft ceiling
      // The drying fans (8-13 % duty) never wake more than once per cycle. With the default 6 s burst
      // (~3 % per cycle) peaks stay under target + 2 and they never wake; with a big 8 s / 65 % burst
      // (~5.4 %) each peak briefly crosses target + 2, as in Pinning since 1.2.3 (LOG.md: 6-7 s bursts fit
      // the band better than 8 s).
      CHECK(new_r.dry_starts <= new_r.cycles);
      if (rise < 0.6f) CHECK(new_r.dry_starts == 0);
    }
  }

  if (failures == 0) std::printf("grow_climate: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
