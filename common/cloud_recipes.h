#pragma once
// Cloud grow recipes (common/cloud-recipes.yaml): parse, check and cache the recipe index from
// ${cloud_base_url}/recipes/v1/index.json. Schema: site/public/recipes/v1/schema.json.
//
// The device never trusts the file: every value is checked again here, against the same limits the
// number entities enforce (terrashroom-core.yaml). A value just outside a limit is clamped (and noted);
// anything that can't be a sane setpoint (missing, not a number, RH outside 0-100, a temperature at or
// above max_safe_temp_f, bad stage order) rejects that recipe. site/scripts/publish_recipes.py refuses
// anything this file would clamp, so clamping is a second line of defence, not something to rely on.
// Recipes can't touch the UV-C lamp: no field maps to it and nothing here can switch it.
//
// Host-testable: site/test/recipes_test.py compiles this file with clang++ (TS_RECIPES_HOST_TEST) and
// esphome/test/cloud_recipes_host_test.cpp. Keep the limits in sync with the number entities and with
// LIMITS in publish_recipes.py; that test checks all three agree.
//
// Per-phase CO2 targets (recipe fields pinning_co2_ppm / fruiting_co2_ppm, schema revision 2) are read
// only when TS_RECIPES_CO2 is defined, which common/recipe-co2.yaml does with a build flag. That package
// needs common/scd41.yaml (CO2 Limit) as well. Without the flag the Set layout, the parse and the image
// are exactly what they were, so builds without a CO2 sensor (the public one) don't pay for it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef TS_RECIPES_HOST_TEST
#include <algorithm>
#include <atomic>
#include "cloud_post.h"   // esphome/cloud_post.h: the one in-flight slot every cloud request shares
#endif

#if __has_include(<ArduinoJson.h>)
#include <ArduinoJson.h>
#define TS_RECIPES_HAVE_JSON 1
#endif

namespace ts_recipes {

static constexpr int SCHEMA = 1;              // index.json "schema"; a breaking change moves to /recipes/v2/
static constexpr size_t MAX_RECIPES = 24;
static constexpr size_t MAX_STAGES = 4;
static constexpr size_t NAME_LEN = 24;        // bytes of UTF-8, without the terminator
static constexpr size_t MAX_BYTES = 8192;     // download buffer; a file this size or larger is refused
static constexpr int DAY_MAX = 365;
// Same limits as the number entities (and the stock idle-airflow range, with headroom).
static constexpr int TEMP_MIN = 55, TEMP_MAX = 85;          // Temperature Setpoint, °F
static constexpr int RH_MIN = 50, RH_MAX = 99;              // Humidity Setpoint, %
static constexpr int AIRFLOW_MIN = 1, AIRFLOW_MAX = 64;     // idle fan duty X out of 255 (stock 7-11)
static constexpr int AIRFLOW_DEFAULT = 7;                   // stock default ("Custom")
static constexpr int LIGHT_MIN = 0, LIGHT_MAX = 16;         // Light Hours, h
static constexpr int FAE_INT_MIN = 5, FAE_INT_MAX = 240, FAE_INT_STEP = 5;     // Fresh Air Interval, min
static constexpr int FAE_DUR_MIN = 10, FAE_DUR_MAX = 600, FAE_DUR_STEP = 10;   // Fresh Air Duration, s
static constexpr int CO2_MIN = 400, CO2_MAX = 5000, CO2_STEP = 100;           // CO2 Limit (scd41.yaml), ppm

struct Stage {
  uint16_t from_day;     // grow day this stage starts on (the first stage starts on day 1)
  uint8_t temp_f;
  uint8_t rh;
  uint8_t airflow;
  int8_t light_h;        // -1: the recipe leaves Light Hours alone
  uint8_t fae_int_5;     // 0: leave Fresh Air Interval alone; otherwise minutes / 5
  uint8_t fae_dur_10;    // 0: leave Fresh Air Duration alone; otherwise seconds / 10
};

struct Recipe {
  char name[NAME_LEN + 1];
  uint8_t n_stages;
  Stage stages[MAX_STAGES];
#ifdef TS_RECIPES_CO2
  uint8_t pinning_co2_100;    // CO2 Limit while Pinning, ppm / 100; 0: the recipe leaves CO2 Limit alone
  uint8_t fruiting_co2_100;   // the same while Fruiting
#endif
};

#ifdef TS_RECIPES_CO2
static constexpr uint32_t MAGIC = 0x54535243;   // "TSRC": the CO2 layout, so a cache from the other one is refused
#else
static constexpr uint32_t MAGIC = 0x54535231;   // "TSR1"; bump if the layout of Set changes
#endif

struct Set {
  uint32_t magic;
  uint32_t revision;
  uint8_t count;
  Recipe recipes[MAX_RECIPES];
};

// One stage as read from JSON. NAN = field absent or not a number.
struct RawStage {
  double from_day = NAN, temp_f = NAN, rh = NAN, airflow = NAN, light_h = NAN, fae_int = NAN, fae_dur = NAN;
};

inline int clamp_int(double v, int lo, int hi, int step, const char *what, std::string &notes) {
  // Into range before rounding: lround() of a value outside long's range (1e300 from the JSON) is undefined.
  // NaN never gets here (callers check isfinite), but it would land on lo.
  const double in_range = !(v >= lo) ? lo : (v > hi ? hi : v);
  long r = lround(in_range / step) * step;
  if (r < lo) r = lo;
  if (r > hi) r = hi;
  if (fabs((double) r - v) > 1e-6) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %g->%ld; ", what, v, r);
    notes += buf;
  }
  return (int) r;
}

// Checks one stage. Returns false (and why) if it must be rejected; otherwise fills `out`, clamping
// out-of-range values to the entity limits and noting each clamp in `notes`.
inline bool check_stage(const RawStage &in, float max_safe_temp_f, Stage &out, std::string &why, std::string &notes) {
  memset(&out, 0, sizeof(out));
  if (!std::isfinite(in.from_day) || in.from_day != floor(in.from_day) || in.from_day < 1 || in.from_day > DAY_MAX) {
    why = "from_day must be a whole number 1-365";
    return false;
  }
  if (!std::isfinite(in.temp_f)) { why = "temperature_f missing"; return false; }
  if (!std::isfinite(in.rh)) { why = "humidity_pct missing"; return false; }
  if (in.temp_f >= max_safe_temp_f) { why = "temperature_f at or above max_safe_temp_f"; return false; }
  if (in.rh < 0 || in.rh > 100) { why = "humidity_pct outside 0-100"; return false; }
  out.from_day = (uint16_t) in.from_day;
  out.temp_f = (uint8_t) clamp_int(in.temp_f, TEMP_MIN, TEMP_MAX, 1, "temperature_f", notes);
  out.rh = (uint8_t) clamp_int(in.rh, RH_MIN, RH_MAX, 1, "humidity_pct", notes);
  out.airflow = std::isfinite(in.airflow) ? (uint8_t) clamp_int(in.airflow, AIRFLOW_MIN, AIRFLOW_MAX, 1, "idle_airflow", notes)
                                          : (uint8_t) AIRFLOW_DEFAULT;
  out.light_h = std::isfinite(in.light_h) ? (int8_t) clamp_int(in.light_h, LIGHT_MIN, LIGHT_MAX, 1, "light_hours", notes) : -1;
  out.fae_int_5 = std::isfinite(in.fae_int)
                      ? (uint8_t) (clamp_int(in.fae_int, FAE_INT_MIN, FAE_INT_MAX, FAE_INT_STEP, "fresh_air_interval_min", notes) / FAE_INT_STEP)
                      : 0;
  out.fae_dur_10 = std::isfinite(in.fae_dur)
                       ? (uint8_t) (clamp_int(in.fae_dur, FAE_DUR_MIN, FAE_DUR_MAX, FAE_DUR_STEP, "fresh_air_duration_s", notes) / FAE_DUR_STEP)
                       : 0;
  return true;
}

// A recipe's CO2 target as ppm / 100, clamped to CO2 Limit's range (and noted); 0 when absent or not a
// number. It's optional, so a bad one never rejects the recipe: the chamber just leaves CO2 Limit alone.
inline uint8_t check_co2(double v, const char *what, std::string &notes) {
  if (!std::isfinite(v)) return 0;
  return (uint8_t) (clamp_int(v, CO2_MIN, CO2_MAX, CO2_STEP, what, notes) / CO2_STEP);
}

inline bool co2_ok(uint8_t v) { return v == 0 || (v >= CO2_MIN / CO2_STEP && v <= CO2_MAX / CO2_STEP); }

// CO2 Limit (ppm) for grow phase `phase` from a recipe's targets (ppm / 100), or -1 to leave it alone:
// Incubation ignores CO2 (scd41.yaml), and a recipe without a target for the phase sets nothing.
inline int co2_for_phase(uint8_t pinning_100, uint8_t fruiting_100, const std::string &phase) {
  uint8_t v = 0;
  if (phase == "Pinning") v = pinning_100;
  else if (phase == "Fruiting") v = fruiting_100;
  return (v != 0 && co2_ok(v)) ? v * CO2_STEP : -1;
}

// A name is 1-NAME_LEN bytes, no control characters, and not "None" (the select's off option).
inline bool check_name(const char *name, std::string &why) {
  if (name == nullptr) { why = "name missing"; return false; }
  size_t n = strlen(name);
  if (n == 0 || n > NAME_LEN) { why = "name must be 1-24 bytes"; return false; }
  for (size_t i = 0; i < n; i++) {
    if ((unsigned char) name[i] < 0x20 || name[i] == 0x7f) { why = "name has a control character"; return false; }
  }
  if (strcmp(name, "None") == 0) { why = "name \"None\" is reserved"; return false; }
  return true;
}

// Stages must start on day 1 and their from_day must strictly increase.
inline bool check_order(const Recipe &r, std::string &why) {
  if (r.n_stages == 0 || r.n_stages > MAX_STAGES) { why = "needs 1-4 stages"; return false; }
  if (r.stages[0].from_day != 1) { why = "first stage must start on day 1"; return false; }
  for (size_t i = 1; i < r.n_stages; i++) {
    if (r.stages[i].from_day <= r.stages[i - 1].from_day) { why = "stage from_day must increase"; return false; }
  }
  return true;
}

// Index of the stage in force on grow day `day` (1-based): the last one whose from_day <= day.
inline int stage_for_day(const Recipe &r, int day) {
  int idx = 0;
  for (int i = 0; i < r.n_stages; i++) {
    if (r.stages[i].from_day <= day) idx = i;
  }
  return idx;
}

inline const Recipe *find(const Set &s, const char *name) {
  if (name == nullptr || name[0] == '\0') return nullptr;
  for (size_t i = 0; i < s.count && i < MAX_RECIPES; i++) {
    if (strncmp(s.recipes[i].name, name, NAME_LEN + 1) == 0) return &s.recipes[i];
  }
  return nullptr;
}

// A cached set is usable if its header and every recipe pass the structural checks (flash could hold
// garbage from an older layout).
inline bool sane(const Set &s) {
  if (s.magic != MAGIC || s.count == 0 || s.count > MAX_RECIPES) return false;
  std::string why;
  for (size_t i = 0; i < s.count; i++) {
    const Recipe &r = s.recipes[i];
    if (memchr(r.name, 0, sizeof(r.name)) == nullptr || !check_name(r.name, why) || !check_order(r, why)) return false;
#ifdef TS_RECIPES_CO2
    if (!co2_ok(r.pinning_co2_100) || !co2_ok(r.fruiting_co2_100)) return false;
#endif
    for (size_t j = 0; j < r.n_stages; j++) {
      const Stage &st = r.stages[j];
      if (st.temp_f < TEMP_MIN || st.temp_f > TEMP_MAX || st.rh < RH_MIN || st.rh > RH_MAX ||
          st.airflow < AIRFLOW_MIN || st.airflow > AIRFLOW_MAX || st.light_h > LIGHT_MAX || st.light_h < -1 ||
          st.fae_int_5 > FAE_INT_MAX / FAE_INT_STEP || st.fae_dur_10 > FAE_DUR_MAX / FAE_DUR_STEP)
        return false;
    }
  }
  return true;
}

#ifdef TS_RECIPES_HAVE_JSON
inline double num(JsonVariantConst v) {
  if (v.is<bool>() || !(v.is<double>() || v.is<long>())) return NAN;
  return v.as<double>();
}

// Parses index.json into `out` (zeroed first). Bad recipes are dropped and listed in `report`; the
// whole file is rejected (returns false) if it isn't JSON, has the wrong schema, or has no good recipe.
inline bool parse_index(const char *body, size_t len, float max_safe_temp_f, Set &out, std::string &report) {
  memset(&out, 0, sizeof(out));
  if (len >= MAX_BYTES) { report = "file too large (or cut off)"; return false; }
  // Keep only the fields the device uses, so descriptions and notes never take RAM.
  JsonDocument filter;
  filter["schema"] = true;
  filter["revision"] = true;
  JsonObject fr = filter["recipes"].add<JsonObject>();
  fr["name"] = true;
#ifdef TS_RECIPES_CO2
  fr["pinning_co2_ppm"] = true;
  fr["fruiting_co2_ppm"] = true;
#endif
  JsonObject fs = fr["stages"].add<JsonObject>();
  for (const char *k : {"from_day", "temperature_f", "humidity_pct", "idle_airflow", "light_hours",
                        "fresh_air_interval_min", "fresh_air_duration_s"})
    fs[k] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, len, DeserializationOption::Filter(filter));
  if (err) { report = std::string("not valid JSON: ") + err.c_str(); return false; }
  if (num(doc["schema"]) != SCHEMA) { report = "unsupported schema (this firmware reads schema 1)"; return false; }
  const double rev = num(doc["revision"]);
  out.revision = (std::isfinite(rev) && rev >= 0 && rev < 4e9) ? (uint32_t) rev : 0;
  JsonArrayConst recipes = doc["recipes"].as<JsonArrayConst>();
  if (recipes.isNull()) { report = "no recipes list"; return false; }
  std::string notes;
  for (JsonObjectConst jr : recipes) {
    const char *name = jr["name"].as<const char *>();
    std::string why;
    std::string label = name != nullptr ? std::string(name).substr(0, NAME_LEN) : std::string("?");
    if (out.count >= MAX_RECIPES) { notes += label + ": over 24 recipes, skipped; "; continue; }
    if (!check_name(name, why)) { notes += label + ": " + why + "; "; continue; }
    if (find(out, name) != nullptr) { notes += label + ": duplicate name; "; continue; }
    Recipe &r = out.recipes[out.count];
    memset(&r, 0, sizeof(r));
    JsonArrayConst stages = jr["stages"].as<JsonArrayConst>();
    bool ok = !stages.isNull() && stages.size() >= 1 && stages.size() <= MAX_STAGES;
    if (!ok) why = "needs 1-4 stages";
    std::string clamped;
    for (JsonObjectConst js : stages) {
      if (!ok) break;
      RawStage raw;
      raw.from_day = num(js["from_day"]);
      raw.temp_f = num(js["temperature_f"]);
      raw.rh = num(js["humidity_pct"]);
      raw.airflow = num(js["idle_airflow"]);
      raw.light_h = num(js["light_hours"]);
      raw.fae_int = num(js["fresh_air_interval_min"]);
      raw.fae_dur = num(js["fresh_air_duration_s"]);
      ok = check_stage(raw, max_safe_temp_f, r.stages[r.n_stages], why, clamped);
      if (ok) r.n_stages++;
    }
    if (ok) ok = check_order(r, why);
#ifdef TS_RECIPES_CO2
    if (ok) {
      r.pinning_co2_100 = check_co2(num(jr["pinning_co2_ppm"]), "pinning_co2_ppm", clamped);
      r.fruiting_co2_100 = check_co2(num(jr["fruiting_co2_ppm"]), "fruiting_co2_ppm", clamped);
    }
#endif
    if (!ok) {
      notes += label + ": " + why + "; ";
      memset(&r, 0, sizeof(r));
      continue;
    }
    if (!clamped.empty()) notes += label + ": clamped " + clamped;
    strncpy(r.name, name, NAME_LEN);
    out.count++;
  }
  report = notes;
  if (out.count == 0) {
    if (report.empty()) report = "no recipes";
    return false;
  }
  out.magic = MAGIC;
  return true;
}
#endif  // TS_RECIPES_HAVE_JSON

// Called by cloud-recipes.yaml after it applies a recipe's stage. With TS_RECIPES_CO2, common/recipe-co2.yaml
// sets g_co2_hook to write CO2 Limit for the current phase; without it this compiles to nothing.
#ifdef TS_RECIPES_CO2
inline void (*g_co2_hook)(const Recipe &) = nullptr;
#endif
inline void after_apply(const Recipe &r) {
#ifdef TS_RECIPES_CO2
  if (g_co2_hook != nullptr) g_co2_hook(r);
#else
  (void) r;
#endif
}

#ifndef TS_RECIPES_HOST_TEST
// ---- Device side: the cached set, its flash slot and the "Cloud Recipe" select's options. ----
inline Set g_set{};
inline esphome::ESPPreferenceObject g_pref;
inline const char *const NONE = "None";

// Loads the last good set from flash (empty if there's none or it fails the checks).
inline void load() {
#ifdef TS_RECIPES_CO2
  g_pref = esphome::global_preferences->make_preference<Set>(esphome::fnv1_hash("ts_cloud_recipes_co2"), true);   // bigger Set, own slot
#else
  g_pref = esphome::global_preferences->make_preference<Set>(esphome::fnv1_hash("ts_cloud_recipes_v1"), true);
#endif
  if (!g_pref.load(&g_set) || !sane(g_set)) memset(&g_set, 0, sizeof(g_set));
}

inline void save() { g_pref.save(&g_set); }

// Rebuilds the select's options ("None" + every cached recipe, pointing into g_set) and shows `name`
// if it's still there. Returns false if `name` was given but is gone. Publishing the state runs the
// select's on_value; the caller guards against applying anything from it.
inline bool set_options(esphome::select::Select *sel, const char *name) {
  esphome::FixedVector<const char *> opts;
  opts.init(g_set.count + 1);
  opts.push_back(NONE);
  size_t want = 0;
  for (size_t i = 0; i < g_set.count; i++) {
    opts.push_back(g_set.recipes[i].name);
    if (name != nullptr && strncmp(g_set.recipes[i].name, name, NAME_LEN + 1) == 0) want = i + 1;
  }
  sel->traits.set_options(opts);
  sel->publish_state(want);   // always a valid index right after the options change
  return want != 0 || name == nullptr || name[0] == '\0';
}

// ---- Background fetch: the GET, the TLS handshake and the parse run in their own FreeRTOS task, so the
// climate controller never waits on the network. The main loop polls fetch_state() and takes the result.

struct FetchResult {
  int status = -1;       // HTTP status; -1 = no response (DNS, TLS, timeout)
  bool ok = false;       // parsed, and at least one good recipe
  std::string report;    // why it was rejected, or what was skipped/clamped
  Set set{};
};

// 0 idle, 1 running, 2 result waiting for take_result().
inline std::atomic<int> g_fetch_state{0};
inline FetchResult *g_fetch_result = nullptr;

struct FetchJob {
  esphome::http_request::HttpRequestComponent *http;
  std::string url;
  float max_safe_temp_f;
};

inline void fetch_task(void *arg) {
  auto *job = static_cast<FetchJob *>(arg);
  auto *res = new FetchResult();
  {
    auto c = job->http->get(job->url);
    if (c != nullptr) {
      res->status = c->status_code;
      if (c->status_code == 200) {
        std::string body;
        body.resize(MAX_BYTES);
        size_t got = 0;
        uint32_t last = millis();
        bool failed = false;
        while (got < MAX_BYTES) {
          int n = c->read(reinterpret_cast<uint8_t *>(&body[got]), std::min<size_t>(MAX_BYTES - got, 512));
          auto r = esphome::http_request::http_read_loop_result(n, last, job->http->get_timeout(), c->is_read_complete());
          if (r == esphome::http_request::HttpReadLoopResult::RETRY) continue;
          if (r != esphome::http_request::HttpReadLoopResult::DATA) {
            failed = r != esphome::http_request::HttpReadLoopResult::COMPLETE;
            break;
          }
          got += n;
        }
        body.resize(got);
        if (failed) res->report = "download interrupted";
        else res->ok = parse_index(body.data(), body.size(), job->max_safe_temp_f, res->set, res->report);
      }
      c->end();
    }
  }
  delete job;
  g_fetch_result = res;
  g_fetch_state = 2;
  ts_cloud::release();
  vTaskDelete(nullptr);
}

// Starts a fetch; false if one is running or waiting, another cloud request is in flight, or there isn't
// heap for a TLS session (cloud_post.h holds the one slot every cloud request shares).
// Heap the fetch keeps besides the TLS session itself (common/tls_budget.h): the MAX_BYTES download
// buffer, then the filtered JsonDocument, the filter and the parsed Set (~1.4 KB) while it parses.
static constexpr size_t FETCH_EXTRA_FREE = MAX_BYTES + 8 * 1024;

inline bool fetch_async(esphome::http_request::HttpRequestComponent *http, const std::string &url, float max_safe_temp_f) {
  if (g_fetch_state != 0) return false;
  if (!ts_cloud::acquire(FETCH_EXTRA_FREE)) return false;
  auto *job = new FetchJob{http, url, max_safe_temp_f};
  g_fetch_state = 1;
  if (xTaskCreate(fetch_task, "ts01_recipes", ts_cloud::TASK_STACK, job, 1, nullptr) != pdPASS) {
    delete job;
    g_fetch_state = 0;
    ts_cloud::release();
    return false;
  }
  return true;
}

// The finished fetch's result (caller deletes it), or nullptr while none is waiting.
inline FetchResult *take_result() {
  if (g_fetch_state != 2) return nullptr;
  FetchResult *r = g_fetch_result;
  g_fetch_result = nullptr;
  g_fetch_state = 0;
  return r;
}
#endif  // TS_RECIPES_HOST_TEST

}  // namespace ts_recipes
