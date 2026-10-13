// Host tests for esphome/common/uvc_budget.h (UV-C: at most 60 lit minutes in any rolling 24 h).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_uvc_budget.cpp -o /tmp/t && /tmp/t
#include "uvc_budget.h"

#include <cstdio>
#include <cstring>

using namespace uvc_budget;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static constexpr uint32_t T0 = 1790000000UL;   // a valid wall-clock time (2026)

// Runs `secs` one-second ticks from `ms`, lamp lit or not. Returns the new millis().
static uint32_t run(Ledger &l, Clock &c, uint32_t ms, uint32_t epoch, uint32_t secs, bool lit) {
  for (uint32_t i = 0; i < secs; i++) {
    ms += 1000;
    tick(l, c, ms, epoch ? epoch + (ms / 1000) : 0, lit);
  }
  return ms;
}

static void test_fresh_budget() {
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  CHECK(remaining(l) == BUDGET_S);
  CHECK(run_seconds(l, 15 * 60) == 15 * 60);   // Auto-Off wins while there's budget
  CHECK(run_seconds(l, 60 * 60) == 60 * 60);
}

static void test_repeated_presses_cant_exceed() {
  // Four 15-minute runs use it up; the fifth press is refused, however the runs are split.
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  uint32_t ms = 0;
  for (int i = 0; i < 4; i++) {
    CHECK(run_seconds(l, 15 * 60) == 15 * 60);
    ms = run(l, c, ms, 0, 15 * 60, true);
    ms = run(l, c, ms, 0, 30, false);
  }
  CHECK(remaining(l) == 0);
  CHECK(run_seconds(l, 15 * 60) == 0);
  // 50 min used: the next run is cut to the 10 left, and under a minute left refuses.
  Ledger m{};
  Clock d;
  begin(m, d, 0);
  run(m, d, 0, 0, 50 * 60, true);
  CHECK(run_seconds(m, 15 * 60) == 10 * 60);
  Ledger n{};
  Clock e;
  begin(n, e, 0);
  run(n, e, 0, 0, BUDGET_S - 30, true);
  CHECK(run_seconds(n, 15 * 60) == 0);
}

static void test_rolls_off_after_24h_by_uptime() {
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  uint32_t ms = run(l, c, 0, 0, 60 * 60, true);   // hour 0: all 60 min
  CHECK(remaining(l) == 0);
  ms = run(l, c, ms, 0, 22 * 3600, false);         // 23 h after the start: still inside 24 h
  CHECK(remaining(l) == 0);
  ms = run(l, c, ms, 0, 3600 + 5, false);          // past 24 h: that hour has dropped out
  CHECK(remaining(l) == BUDGET_S);
}

static void test_blocked_loop_still_counts_lit_time() {
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  tick(l, c, 120000, 0, true);   // one tick after a 2-minute stall
  CHECK(used(l) == 120);
}

static void test_survives_reboot_with_no_clock() {
  // 60 min used, then a reboot that never gets the time: the restored ledger keeps it spent.
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  run(l, c, 0, T0, 60 * 60, true);
  Ledger saved;
  std::memcpy(&saved, &l, sizeof(saved));   // what flash holds
  Clock c2;
  begin(saved, c2, 0);
  run(saved, c2, 0, 0, 600, false);
  CHECK(remaining(saved) == 0);
  CHECK(run_seconds(saved, 60) == 0);
}

static void test_outage_hours_are_dropped_once_the_clock_is_known() {
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  run(l, c, 0, T0, 60 * 60, true);   // used at T0
  Ledger saved = l;
  // Back after 30 h off: the first wall-clock time drops everything.
  Clock c2;
  begin(saved, c2, 0);
  tick(saved, c2, 1000, T0 + 30 * 3600, false);
  CHECK(remaining(saved) == BUDGET_S);
  // Back after 2 h off: still spent.
  Ledger again = l;
  Clock c3;
  begin(again, c3, 0);
  tick(again, c3, 1000, T0 + 3600 + 2 * 3600, false);
  CHECK(remaining(again) == 0);
}

static void test_uptime_hours_not_counted_twice() {
  // Off 10 h, then 20 h of uptime without a clock (20 slots dropped by uptime), then the clock arrives:
  // 30 h since the use, so it's free; but the uptime hours mustn't be subtracted from an outage twice.
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  run(l, c, 0, T0, 60 * 60, true);
  const uint32_t saved_hour = l.epoch_hour;
  Clock c2;
  begin(l, c2, 0);
  uint32_t ms = run(l, c2, 0, 0, 20 * 3600, false);
  CHECK(c2.ms_hours == 20);
  CHECK(remaining(l) == 0);   // 20 uptime hours: not yet 24
  // The clock now says 10 h outage + 20 h = 30 h after saved_hour.
  tick(l, c2, ms + 1000, (saved_hour + 31) * 3600, false);
  CHECK(remaining(l) == BUDGET_S);
  // Same, but only 2 h off: 22 h in total, still spent.
  Ledger m{};
  Clock d;
  begin(m, d, 0);
  run(m, d, 0, T0, 60 * 60, true);
  const uint32_t h = m.epoch_hour;
  Clock d2;
  begin(m, d2, 0);
  ms = run(m, d2, 0, 0, 20 * 3600, false);
  tick(m, d2, ms + 1000, (h + 22) * 3600, false);
  CHECK(remaining(m) == 0);
}

static void test_clock_going_back_frees_nothing() {
  Ledger l{};
  Clock c;
  begin(l, c, 0);
  run(l, c, 0, T0, 60 * 60, true);
  tick(l, c, 3700000, T0 - 5 * 3600, false);
  CHECK(remaining(l) == 0);
}

static void test_garbage_from_flash_is_sanitised() {
  Ledger l;
  std::memset(&l, 0xFF, sizeof(l));
  Clock c;
  begin(l, c, 0);   // head out of range: start clean
  CHECK(remaining(l) == BUDGET_S);
  Ledger m{};
  m.used_s[3] = 50000;   // impossible slot value: clamped, so it can't overflow the sum
  begin(m, c, 0);
  CHECK(m.used_s[3] == BUDGET_S);
  CHECK(remaining(m) == 0);
}

static void test_millis_wrap() {
  Ledger l{};
  Clock c;
  begin(l, c, 0xFFFFF000u);
  uint32_t ms = 0xFFFFF000u;
  for (int i = 0; i < 20; i++) {
    ms += 1000;   // wraps through 0
    tick(l, c, ms, 0, true);
  }
  CHECK(used(l) == 20);
}

int main() {
  test_fresh_budget();
  test_repeated_presses_cant_exceed();
  test_rolls_off_after_24h_by_uptime();
  test_blocked_loop_still_counts_lit_time();
  test_survives_reboot_with_no_clock();
  test_outage_hours_are_dropped_once_the_clock_is_known();
  test_uptime_hours_not_counted_twice();
  test_clock_going_back_frees_nothing();
  test_garbage_from_flash_is_sanitised();
  test_millis_wrap();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("uvc_budget: all checks passed\n");
  return 0;
}
