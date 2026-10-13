// Host test for common/cloud_recipes.h (the chamber-side recipe checks). Built and run by
// site/test/recipes_test.py:
//   clang++ -std=c++17 -DTS_RECIPES_HOST_TEST [-I<ArduinoJson src>] cloud_recipes_host_test.cpp
//   ./a.out site/public/recipes/v1/index.json
// Without ArduinoJson on the include path only the value checks run (the JSON tests are skipped).
#include "../common/cloud_recipes.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace ts_recipes;

static int checks = 0, failures = 0;
#define CHECK(cond)                                                                     \
  do {                                                                                  \
    checks++;                                                                           \
    if (!(cond)) {                                                                      \
      failures++;                                                                       \
      std::cerr << __FILE__ << ":" << __LINE__ << ": FAILED " #cond << std::endl;       \
    }                                                                                   \
  } while (0)

static const float MAX_SAFE = 88;

static RawStage raw(double day, double t, double rh) {
  RawStage r;
  r.from_day = day;
  r.temp_f = t;
  r.rh = rh;
  return r;
}

static void test_stage_checks() {
  Stage s;
  std::string why, notes;
  CHECK(check_stage(raw(1, 70, 82), MAX_SAFE, s, why, notes));
  CHECK(s.temp_f == 70 && s.rh == 82 && s.airflow == AIRFLOW_DEFAULT && s.light_h == -1 && s.fae_int_5 == 0 &&
        s.fae_dur_10 == 0 && notes.empty());

  // Hard rejects: unsafe or meaningless.
  CHECK(!check_stage(raw(1, 88, 80), MAX_SAFE, s, why, notes));     // at max_safe_temp_f
  CHECK(!check_stage(raw(1, 120, 80), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(1, NAN, 80), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(1, 70, NAN), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(1, 70, 101), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(1, 70, -1), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(0, 70, 80), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(1.5, 70, 80), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(366, 70, 80), MAX_SAFE, s, why, notes));
  CHECK(!check_stage(raw(NAN, 70, 80), MAX_SAFE, s, why, notes));
  // A lower max_safe_temp_f build rejects what the default accepts.
  CHECK(!check_stage(raw(1, 80, 80), 80, s, why, notes));

  // Clamps to the number entities' limits, and notes them.
  notes.clear();
  CHECK(check_stage(raw(1, 87, 100), MAX_SAFE, s, why, notes));
  CHECK(s.temp_f == TEMP_MAX && s.rh == RH_MAX && !notes.empty());
  notes.clear();
  CHECK(check_stage(raw(1, 40, 20), MAX_SAFE, s, why, notes));
  CHECK(s.temp_f == TEMP_MIN && s.rh == RH_MIN);
  RawStage r = raw(1, 70, 85);
  r.airflow = 200;
  r.light_h = 24;
  r.fae_int = 7;
  r.fae_dur = 1000;
  notes.clear();
  CHECK(check_stage(r, MAX_SAFE, s, why, notes));
  CHECK(s.airflow == AIRFLOW_MAX && s.light_h == LIGHT_MAX && s.fae_int_5 * FAE_INT_STEP == 5 &&
        s.fae_dur_10 * FAE_DUR_STEP == FAE_DUR_MAX);
  r.airflow = 0;
  r.light_h = 0;
  r.fae_int = 240;
  r.fae_dur = 10;
  CHECK(check_stage(r, MAX_SAFE, s, why, notes));
  CHECK(s.airflow == AIRFLOW_MIN && s.light_h == 0 && s.fae_int_5 * FAE_INT_STEP == 240 && s.fae_dur_10 * FAE_DUR_STEP == 10);
}

static void test_names_and_order() {
  std::string why;
  CHECK(check_name("Lion's Mane", why));
  CHECK(!check_name("", why));
  CHECK(!check_name(nullptr, why));
  CHECK(!check_name("None", why));
  CHECK(!check_name("abcdefghijklmnopqrstuvwxy", why));   // 25 bytes
  CHECK(check_name("abcdefghijklmnopqrstuvwx", why));     // 24
  CHECK(!check_name("tab\there", why));

  Recipe rc{};
  rc.n_stages = 3;
  rc.stages[0].from_day = 1;
  rc.stages[1].from_day = 15;
  rc.stages[2].from_day = 22;
  CHECK(check_order(rc, why));
  CHECK(stage_for_day(rc, 1) == 0 && stage_for_day(rc, 14) == 0 && stage_for_day(rc, 15) == 1 &&
        stage_for_day(rc, 21) == 1 && stage_for_day(rc, 22) == 2 && stage_for_day(rc, 300) == 2);
  rc.stages[2].from_day = 15;
  CHECK(!check_order(rc, why));
  rc.stages[2].from_day = 22;
  rc.stages[0].from_day = 2;
  CHECK(!check_order(rc, why));
  rc.n_stages = 0;
  CHECK(!check_order(rc, why));
}

#ifdef TS_RECIPES_HAVE_JSON
static bool parse(const std::string &json, Set &out, std::string &report) {
  return parse_index(json.data(), json.size(), MAX_SAFE, out, report);
}

static void test_parse_canonical(const char *path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  Set s;
  std::string report;
  CHECK(parse(ss.str(), s, report));
  CHECK(report.empty());
  CHECK(s.count == 13 && s.revision >= 1 && s.magic == MAGIC);
  CHECK(sane(s));
  const Recipe *r = find(s, "Golden Oyster");
  CHECK(r != nullptr && r->n_stages == 1 && r->stages[0].temp_f == 73 && r->stages[0].rh == 91 && r->stages[0].airflow == 10);
  CHECK(r != nullptr && r->stages[0].light_h == -1 && r->stages[0].fae_int_5 == 0 && r->stages[0].fae_dur_10 == 0);
  CHECK(find(s, "Lion's Mane") != nullptr && find(s, "None") == nullptr && find(s, "") == nullptr);
}

static void test_parse_cases() {
  Set s;
  std::string report;
  const std::string multi = R"({"schema":1,"revision":7,"recipes":[
    {"name":"Test Oyster","description":"ignored","stages":[
      {"name":"Colonisation","from_day":1,"temperature_f":75,"humidity_pct":80,"idle_airflow":4,"light_hours":0},
      {"name":"Pinning","from_day":15,"temperature_f":65,"humidity_pct":95,"fresh_air_interval_min":30,"fresh_air_duration_s":120},
      {"name":"Fruiting","from_day":19,"temperature_f":68,"humidity_pct":90,"light_hours":12,"uv_minutes":60,"future_field":1}]},
    {"name":"Too Hot","stages":[{"from_day":1,"temperature_f":90,"humidity_pct":80}]},
    {"name":"Bad Order","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":80},{"from_day":1,"temperature_f":70,"humidity_pct":80}]},
    {"name":"Test Oyster","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":80}]},
    {"name":"None","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":80}]},
    {"name":"No Stages","stages":[]},
    {"name":"Five","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":80},{"from_day":2,"temperature_f":70,"humidity_pct":80},
      {"from_day":3,"temperature_f":70,"humidity_pct":80},{"from_day":4,"temperature_f":70,"humidity_pct":80},{"from_day":5,"temperature_f":70,"humidity_pct":80}]},
    {"name":"Strings","stages":[{"from_day":"1","temperature_f":"70","humidity_pct":80}]},
    {"name":"Bools","stages":[{"from_day":1,"temperature_f":true,"humidity_pct":80}]},
    {"name":"Clamped","stages":[{"from_day":1,"temperature_f":86,"humidity_pct":80}]}
  ]})";
  CHECK(parse(multi, s, report));
  CHECK(s.count == 2 && s.revision == 7);
  const Recipe *r = find(s, "Test Oyster");
  CHECK(r != nullptr && r->n_stages == 3);
  if (r != nullptr) {
    CHECK(r->stages[0].temp_f == 75 && r->stages[0].airflow == 4 && r->stages[0].light_h == 0);
    CHECK(r->stages[1].from_day == 15 && r->stages[1].fae_int_5 * FAE_INT_STEP == 30 && r->stages[1].fae_dur_10 * FAE_DUR_STEP == 120 &&
          r->stages[1].light_h == -1 && r->stages[1].airflow == AIRFLOW_DEFAULT);
    CHECK(r->stages[2].light_h == 12);
    CHECK(stage_for_day(*r, 18) == 1 && stage_for_day(*r, 19) == 2);
  }
  CHECK(find(s, "Too Hot") == nullptr && find(s, "Bad Order") == nullptr && find(s, "Five") == nullptr &&
        find(s, "Strings") == nullptr && find(s, "Bools") == nullptr && find(s, "No Stages") == nullptr);
  const Recipe *c = find(s, "Clamped");
  CHECK(c != nullptr && c->stages[0].temp_f == TEMP_MAX);
  CHECK(report.find("Too Hot") != std::string::npos && report.find("duplicate") != std::string::npos &&
        report.find("clamped") != std::string::npos);
  CHECK(sane(s));

  // Whole-file rejects keep nothing.
  CHECK(!parse(R"({"schema":2,"revision":1,"recipes":[{"name":"A","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":80}]}]})", s, report));
  CHECK(!parse(R"({"schema":1,"revision":1,"recipes":[{"name":"A","stages":[{"from_day":1,"temperature_f":95,"humidity_pct":80}]}]})", s, report));
  CHECK(!parse(R"({"schema":1,"revision":1,"recipes":[]})", s, report));
  CHECK(!parse(R"({"schema":1,"revision":1})", s, report));
  CHECK(!parse("<html>CloudFront error</html>", s, report));
  CHECK(!parse(R"({"schema":1,"revision":1,"recipes":[{"name":"A","stages":[{"from_day":1,"temperature_f":70,"humidi)", s, report));
  CHECK(!parse(std::string(MAX_BYTES, ' '), s, report));
  CHECK(s.count == 0 && s.magic == 0);

  // More than 24 recipes: the first 24 are kept.
  std::string many = R"({"schema":1,"revision":2,"recipes":[)";
  for (int i = 0; i < 30; i++) {
    if (i) many += ",";
    many += R"({"name":"R)" + std::to_string(i) + R"(","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":80}]})";
  }
  many += "]}";
  CHECK(parse(many, s, report));
  CHECK(s.count == MAX_RECIPES && find(s, "R23") != nullptr && find(s, "R24") == nullptr);

  // A cached set that went bad in flash is refused.
  Set bad = s;
  bad.recipes[3].stages[0].temp_f = 99;
  CHECK(!sane(bad));
  bad = s;
  bad.magic = 0;
  CHECK(!sane(bad));
  bad = s;
  memset(bad.recipes[0].name, 'x', sizeof(bad.recipes[0].name));
  CHECK(!sane(bad));
}

#ifdef TS_RECIPES_CO2
// Per-phase CO2 targets (common/recipe-co2.yaml). Built with -DTS_RECIPES_CO2 by site/test/recipes_test.py.
static void test_parse_co2(const char *path) {
  Set s;
  std::string report;
  if (path != nullptr) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(parse(ss.str(), s, report) && report.empty());
    const Recipe *r = find(s, "King Trumpet");
    CHECK(r != nullptr && r->pinning_co2_100 * CO2_STEP == 2500 && r->fruiting_co2_100 * CO2_STEP == 2000);
    r = find(s, "Shiitake");
    CHECK(r != nullptr && co2_for_phase(r->pinning_co2_100, r->fruiting_co2_100, "Pinning") == 700 &&
          co2_for_phase(r->pinning_co2_100, r->fruiting_co2_100, "Fruiting") == 1000 &&
          co2_for_phase(r->pinning_co2_100, r->fruiting_co2_100, "Incubation") == -1);
  }
  const std::string doc = R"({"schema":1,"revision":3,"recipes":[
    {"name":"Both","pinning_co2_ppm":800,"fruiting_co2_ppm":1200,"stages":[{"from_day":1,"temperature_f":70,"humidity_pct":85}]},
    {"name":"None Set","stages":[{"from_day":1,"temperature_f":70,"humidity_pct":85}]},
    {"name":"Clamped","pinning_co2_ppm":9000,"fruiting_co2_ppm":250,"stages":[{"from_day":1,"temperature_f":70,"humidity_pct":85}]},
    {"name":"Junk","pinning_co2_ppm":"800","fruiting_co2_ppm":true,"stages":[{"from_day":1,"temperature_f":70,"humidity_pct":85}]},
    {"name":"Bad Stage","pinning_co2_ppm":800,"stages":[{"from_day":1,"temperature_f":95,"humidity_pct":85}]}
  ]})";
  CHECK(parse(doc, s, report));
  CHECK(s.count == 4 && s.magic == MAGIC && sane(s));
  const Recipe *r = find(s, "Both");
  CHECK(r != nullptr && r->pinning_co2_100 == 8 && r->fruiting_co2_100 == 12);
  r = find(s, "None Set");
  CHECK(r != nullptr && r->pinning_co2_100 == 0 && r->fruiting_co2_100 == 0);
  r = find(s, "Clamped");
  CHECK(r != nullptr && r->pinning_co2_100 * CO2_STEP == CO2_MAX && r->fruiting_co2_100 * CO2_STEP == CO2_MIN);
  CHECK(report.find("pinning_co2_ppm 9000->5000") != std::string::npos);
  r = find(s, "Junk");   // a bad optional CO2 field leaves CO2 Limit alone; the recipe still loads
  CHECK(r != nullptr && r->pinning_co2_100 == 0 && r->fruiting_co2_100 == 0);
  CHECK(find(s, "Bad Stage") == nullptr);
}
#endif
#endif

int main(int argc, char **argv) {
  test_stage_checks();
  test_names_and_order();
#ifdef TS_RECIPES_HAVE_JSON
  if (argc > 1) test_parse_canonical(argv[1]);
  test_parse_cases();
#ifdef TS_RECIPES_CO2
  test_parse_co2(argc > 1 ? argv[1] : nullptr);
  std::cout << "co2: yes" << std::endl;
#endif
  std::cout << "json: yes" << std::endl;
#else
  (void) argc;
  (void) argv;
  std::cout << "json: skipped (no ArduinoJson on the include path)" << std::endl;
#endif
  std::cout << checks - failures << "/" << checks << " checks passed" << std::endl;
  return failures == 0 ? 0 : 1;
}
