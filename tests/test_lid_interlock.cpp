// Host tests for esphome/common/lid_interlock.h (lid reed switch as a UV-C interlock, issue #28).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_lid_interlock.cpp -o /tmp/tst && /tmp/tst
#include "lid_interlock.h"

#include <cstdio>
#include <cstring>

using namespace lid_interlock;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

// Feeds one pin level every 50 ms (the watchdog's rate) from t0 to t1 inclusive.
static bool feed(Debounce &d, bool pin_high, uint32_t t0, uint32_t t1) {
  bool closed = d.closed;
  for (uint32_t t = t0; (int32_t) (t1 - t) >= 0; t += 50) closed = update(d, raw_closed(pin_high), t);
  return closed;
}

int main() {
  // --- Pin mapping: pull-up + reed to GND. LOW = closed; HIGH (lid open, or no switch at all) = open.
  CHECK(raw_closed(false));
  CHECK(!raw_closed(true));

  // --- Boot: starts open. A missing switch (pin floats HIGH on the pull-up) stays open forever.
  {
    Debounce d;
    CHECK(!d.closed);
    CHECK(!feed(d, true, 0, 60000));
  }

  // --- Closing takes CLOSE_MS of uninterrupted LOW; opening is immediate.
  {
    Debounce d;
    CHECK(!feed(d, false, 0, CLOSE_MS - 50));     // 450 ms: still open
    CHECK(feed(d, false, CLOSE_MS, CLOSE_MS));    // 500 ms: closed
    CHECK(!update(d, raw_closed(true), CLOSE_MS + 10));   // one HIGH reading: open at once
    CHECK(!feed(d, false, CLOSE_MS + 50, 2 * CLOSE_MS));  // closing starts over (450 ms)
    CHECK(feed(d, false, 2 * CLOSE_MS + 50, 2 * CLOSE_MS + 50));
  }

  // --- A bouncing contact while the lid comes down never reads closed until it settles.
  {
    Debounce d;
    for (uint32_t t = 0; t < 3000; t += 50) CHECK(!update(d, raw_closed((t / 50) % 3 == 0), t));
    CHECK(!update(d, raw_closed(true), 2990));   // last bounce
    CHECK(!feed(d, false, 3000, 3450));
    CHECK(feed(d, false, 3500, 3500));
  }

  // --- Lead cut while closed: the pull-up reads HIGH -> open -> cut.
  {
    Debounce d;
    feed(d, false, 0, 1000);
    CHECK(d.closed);
    CHECK(!update(d, raw_closed(true), 1050));
    CHECK(must_cut(true, d.closed));
  }

  // --- Cut decision.
  CHECK(must_cut(true, false));    // lamp on, lid open: cut
  CHECK(!must_cut(true, true));    // lamp on, lid shut: fine
  CHECK(!must_cut(false, false));  // lamp off: nothing to cut

  // --- Turn-on gate: low water first (the core's existing message), then the lid.
  CHECK(std::strcmp(refusal(true, false), "UV-C blocked: reservoir low/empty") == 0);
  CHECK(std::strcmp(refusal(true, true), "UV-C blocked: reservoir low/empty") == 0);
  CHECK(std::strcmp(refusal(false, false), "UV-C blocked: lid open") == 0);
  CHECK(refusal(false, true) == nullptr);

  // --- Across the 49-day millis() wrap.
  {
    Debounce d;
    const uint32_t t0 = 0xFFFFFFFFu - 200;
    CHECK(!feed(d, false, t0, t0 + CLOSE_MS - 50));
    CHECK(feed(d, false, t0 + CLOSE_MS, t0 + CLOSE_MS));
  }

  if (failures) {
    std::printf("test_lid_interlock: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_lid_interlock: all passed\n");
  return 0;
}
