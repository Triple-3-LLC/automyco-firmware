// Host tests for the per-phase CO2 targets in esphome/common/cloud_recipes.h (common/recipe-co2.yaml).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_recipe_co2.cpp -o /tmp/tst && /tmp/tst
// The JSON parse of the two fields is tested in test/cloud_recipes_host_test.cpp (site/test/recipes_test.py
// builds it with and without TS_RECIPES_CO2), because it needs ArduinoJson.
#define TS_RECIPES_HOST_TEST
#define TS_RECIPES_CO2
#include "cloud_recipes.h"

#include <cstdio>

using namespace ts_recipes;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static int hook_calls = 0;
static int hook_ppm = -2;

int main() {
  std::string notes;

  // --- check_co2: ppm / 100, clamped to CO2 Limit's range and noted; absent or not a number = 0.
  CHECK(check_co2(800, "pinning_co2_ppm", notes) == 8 && notes.empty());
  CHECK(check_co2(400, "x", notes) == 4 && check_co2(5000, "x", notes) == 50 && notes.empty());
  CHECK(check_co2(NAN, "x", notes) == 0 && check_co2(INFINITY, "x", notes) == 0 && notes.empty());
  CHECK(check_co2(300, "pinning_co2_ppm", notes) == 4 && notes.find("pinning_co2_ppm 300->400") != std::string::npos);
  notes.clear();
  CHECK(check_co2(9000, "fruiting_co2_ppm", notes) == 50 && notes.find("fruiting_co2_ppm") != std::string::npos);
  notes.clear();
  CHECK(check_co2(0, "x", notes) == 4 && !notes.empty());       // present but "off" is clamped up, never 0
  notes.clear();
  CHECK(check_co2(1249, "x", notes) == 12 && !notes.empty());   // rounded to the 100 ppm step
  notes.clear();
  CHECK(check_co2(-500, "x", notes) == 4);
  // Far outside long's range: clamped before rounding (lround of 1e300 is undefined behaviour).
  CHECK(check_co2(1e300, "x", notes) == 50);
  CHECK(check_co2(-1e300, "x", notes) == 4);
  CHECK(clamp_int(1e300, 60, 85, 1, "temperature_f", notes) == 85);
  CHECK(clamp_int(-1e19, 60, 85, 1, "temperature_f", notes) == 60);

  // --- co2_ok: the range the cached set may hold.
  CHECK(co2_ok(0) && co2_ok(4) && co2_ok(50));
  CHECK(!co2_ok(1) && !co2_ok(3) && !co2_ok(51) && !co2_ok(255));

  // --- co2_for_phase: Pinning and Fruiting only; Incubation and unknown phases leave CO2 Limit alone.
  CHECK(co2_for_phase(7, 10, "Pinning") == 700);
  CHECK(co2_for_phase(7, 10, "Fruiting") == 1000);
  CHECK(co2_for_phase(7, 10, "Incubation") == -1);
  CHECK(co2_for_phase(7, 10, "") == -1 && co2_for_phase(7, 10, "pinning") == -1);
  CHECK(co2_for_phase(0, 10, "Pinning") == -1 && co2_for_phase(7, 0, "Fruiting") == -1);   // no target: alone
  CHECK(co2_for_phase(200, 3, "Pinning") == -1 && co2_for_phase(200, 3, "Fruiting") == -1);   // corrupt: alone

  // --- sane: a cached set with a bad CO2 byte is refused; the CO2 layout has its own magic.
  Set s{};
  s.magic = MAGIC;
  s.count = 1;
  std::strcpy(s.recipes[0].name, "Shiitake");
  s.recipes[0].n_stages = 1;
  s.recipes[0].stages[0] = Stage{1, 70, 85, 8, -1, 0, 0};
  s.recipes[0].pinning_co2_100 = 7;
  s.recipes[0].fruiting_co2_100 = 10;
  CHECK(sane(s));
  s.recipes[0].fruiting_co2_100 = 0;
  CHECK(sane(s));
  s.recipes[0].pinning_co2_100 = 2;
  CHECK(!sane(s));
  s.recipes[0].pinning_co2_100 = 51;
  CHECK(!sane(s));
  s.recipes[0].pinning_co2_100 = 7;
  CHECK(MAGIC != 0x54535231);   // a cache written by a build without CO2 fails sane()
  s.magic = 0x54535231;
  CHECK(!sane(s));
  s.magic = MAGIC;

  // --- after_apply: nothing without a hook, the hook with one.
  after_apply(s.recipes[0]);
  g_co2_hook = [](const Recipe &r) {
    hook_calls++;
    hook_ppm = co2_for_phase(r.pinning_co2_100, r.fruiting_co2_100, "Pinning");
  };
  after_apply(s.recipes[0]);
  CHECK(hook_calls == 1 && hook_ppm == 700);
  g_co2_hook = nullptr;
  after_apply(s.recipes[0]);
  CHECK(hook_calls == 1);

  if (failures == 0) std::printf("test_recipe_co2: all passed\n");
  return failures == 0 ? 0 : 1;
}
