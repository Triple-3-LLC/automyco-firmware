// Host tests for esphome/components/chamber_history/history_log.h (on-chamber climate history ring).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_chamber_history.cpp -o /tmp/tch && /tmp/tch
// The flash is simulated as NOR: erase sets a sector to 0xFF, programming can only clear bits, and a
// "power cut" programs only the first N bytes of a write (or a mangled erase leaves garbage).
#include "components/chamber_history/history_log.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace history_log;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

struct NorFlash : Flash {
  std::vector<uint8_t> mem;
  int erases = 0;
  long cut_after_bytes = -1;  // >= 0: the next write programs only this many bytes, then "power is lost"
  bool fail_writes = false;
  explicit NorFlash(uint32_t size, uint8_t fill = 0xFF) : mem(size, fill) {}
  bool read(uint32_t off, void *dst, size_t len) override {
    if (off + len > mem.size())
      return false;
    std::memcpy(dst, mem.data() + off, len);
    return true;
  }
  bool write(uint32_t off, const void *src, size_t len) override {
    if (fail_writes || off + len > mem.size())
      return false;
    const auto *s = static_cast<const uint8_t *>(src);
    size_t n = len;
    if (cut_after_bytes >= 0) {
      n = static_cast<size_t>(cut_after_bytes) < len ? static_cast<size_t>(cut_after_bytes) : len;
      cut_after_bytes = -1;
    }
    for (size_t i = 0; i < n; i++)
      mem[off + i] &= s[i];  // NOR: 1 -> 0 only
    return n == len;
  }
  bool erase_sector(uint32_t off) override {
    if (off % SECTOR_SIZE != 0 || off + SECTOR_SIZE > mem.size())
      return false;
    std::memset(mem.data() + off, 0xFF, SECTOR_SIZE);
    erases++;
    return true;
  }
};

static const uint32_t T0 = 1791500000;  // October 2026; T0 % 60 == 20, so minute rounding shows

static Sample make(uint32_t t, float temp = 72.4f, float rh = 88.1f) {
  Sample s;
  s.time = t;
  s.temperature = temp;
  s.humidity = rh;
  s.temperature_target = 70;
  s.humidity_target = 88;
  s.heater_pct = 35;
  s.mist_pct = 10;
  s.phase = 3;
  return s;
}

static std::vector<Sample> all(const Ring &ring, uint32_t since = 0) {
  std::vector<Sample> out;
  uint8_t buf[RECORD_SIZE * 17];
  CHECK(ring.for_each(since, buf, sizeof(buf), [&](const Sample &s) { out.push_back(s); }));
  return out;
}

static void test_layout_and_capacity() {
  CHECK(RECORDS_PER_SECTOR == 408);
  // Stock TS-01 'storage' partition (512 KiB, 128 sectors) and ESPHome's 'spiffs' (60 KiB, 15 sectors).
  CHECK(near(capacity_days(0x80000, 300), 179.9f, 0.1f));
  CHECK(near(capacity_days(0xF000, 300), 19.8f, 0.1f));
  CHECK(capacity_days(SECTOR_SIZE, 300) == 0.0f);
}

static void test_pack_roundtrip() {
  Sample s = make(T0);
  s.co2 = 1234;
  const Sample r = unpack(pack(s), T0);
  CHECK(near(r.temperature, 72.4f, 0.051f));
  CHECK(near(r.humidity, 88.1f, 0.051f));
  CHECK(r.temperature_target == 70 && r.humidity_target == 88);
  CHECK(r.heater_pct == 35 && r.mist_pct == 10);
  CHECK(near(r.co2, 1230, 0.5f));
  CHECK(r.phase == 3);

  // Unknowns stay unknown; out-of-range values clamp instead of wrapping into "unknown".
  Sample u;
  u.time = T0;
  const Sample ru = unpack(pack(u), T0);
  CHECK(std::isnan(ru.temperature) && std::isnan(ru.humidity) && std::isnan(ru.co2));
  CHECK(std::isnan(ru.temperature_target) && std::isnan(ru.humidity_target));
  CHECK(ru.heater_pct == Sample::UNKNOWN_PCT && ru.mist_pct == Sample::UNKNOWN_PCT && ru.phase == 0);
  Sample c = make(T0, 400.0f, 150.0f);
  c.temperature_target = 300;
  c.heater_pct = 250;  // > 100 but not UNKNOWN_PCT
  c.co2 = 99999;
  const Sample rc = unpack(pack(c), T0);
  CHECK(near(rc.temperature, 164.6f, 0.01f) && near(rc.humidity, 102.2f, 0.01f));
  CHECK(rc.temperature_target == 166 && rc.heater_pct == 100 && near(rc.co2, 5100, 0.5f));
  Sample cold = make(T0, -80.0f, -5.0f);
  const Sample rcold = unpack(pack(cold), T0);
  CHECK(near(rcold.temperature, -40.0f, 0.01f) && rcold.humidity == 0.0f);
  // Duty rounds to the nearest 5 %.
  Sample d = make(T0);
  d.heater_pct = 2;
  d.mist_pct = 98;
  const Sample rd = unpack(pack(d), T0);
  CHECK(rd.heater_pct == 0 && rd.mist_pct == 100);
  d.heater_pct = 3;
  CHECK(unpack(pack(d), T0).heater_pct == 5);
}

static void test_record_crc() {
  uint8_t rec[RECORD_SIZE];
  encode_record(rec, 5, pack(make(T0)), 7);
  Sample out;
  CHECK(decode_record(rec, 7, T0 - T0 % 60, &out) && out.time == T0 - T0 % 60 + 300);
  CHECK(!decode_record(rec, 8, T0, &out));  // another sector's seq: refused
  for (int bit = 0; bit < 80; bit++) {     // any single flipped bit is caught
    uint8_t bad[RECORD_SIZE];
    std::memcpy(bad, rec, sizeof(bad));
    bad[bit / 8] ^= static_cast<uint8_t>(1 << (bit % 8));
    CHECK(!decode_record(bad, 7, T0, &out));
  }
  uint8_t erased[RECORD_SIZE];
  std::memset(erased, 0xFF, sizeof(erased));
  CHECK(!decode_record(erased, 7, T0, &out));
  // Header: a torn header never counts.
  uint8_t hb[HEADER_SIZE];
  encode_header(hb, Header{3, T0});
  Header h;
  CHECK(decode_header(hb, &h) && h.seq == 3 && h.base == T0);
  hb[9] ^= 0x10;
  CHECK(!decode_header(hb, &h));
  uint8_t zero[HEADER_SIZE] = {};
  CHECK(!decode_header(zero, &h));
}

static void test_append_and_read() {
  NorFlash f(4 * SECTOR_SIZE);
  Ring ring(&f, f.mem.size());
  CHECK(ring.begin() && !ring.has_data());
  CHECK(!ring.append(make(1000)));  // never with an unset clock
  for (int i = 0; i < 10; i++)
    CHECK(ring.append(make(T0 + 300 * i, 70.0f + i)));
  CHECK(ring.current_sector() == 0 && ring.next_slot() == 10 && f.erases == 1);
  auto got = all(ring);
  CHECK(got.size() == 10);
  // Times come back to the minute: the sector base is T0 rounded down to the minute.
  CHECK(got[0].time == T0 - T0 % 60 && got[9].time == T0 - T0 % 60 + 2700);
  CHECK(near(got[9].temperature, 79.0f, 0.051f));
  // since: strictly newer.
  CHECK(all(ring, got[4].time).size() == 5);
  CHECK(all(ring, got[9].time).empty());

  // A reboot finds the same write position and carries on in the same sector.
  Ring again(&f, f.mem.size());
  CHECK(again.begin() && again.has_data() && again.current_sector() == 0 && again.next_slot() == 10);
  CHECK(again.append(make(T0 + 3000)));
  CHECK(all(again).size() == 11 && f.erases == 1);
}

static void test_wraps_and_keeps_order() {
  NorFlash f(3 * SECTOR_SIZE);
  Ring ring(&f, f.mem.size());
  CHECK(ring.begin());
  const uint32_t total = RECORDS_PER_SECTOR * 5 + 7;  // laps the 3-sector ring
  for (uint32_t i = 0; i < total; i++)
    CHECK(ring.append(make(T0 + 300 * i)));
  auto got = all(ring);
  // Holds the 2 full sectors before the current one, plus the 7 in the current one.
  CHECK(got.size() == RECORDS_PER_SECTOR * 2 + 7);
  bool ordered = true;
  for (size_t i = 1; i < got.size(); i++)
    ordered = ordered && got[i].time > got[i - 1].time;
  CHECK(ordered);
  const uint32_t last = T0 + 300 * (total - 1);
  CHECK(got.back().time == last - last % 60);  // every time comes back rounded down to its minute
  CHECK(f.erases == 6);
  Ring again(&f, f.mem.size());
  CHECK(again.begin() && again.current_sector() == ring.current_sector() && again.next_slot() == 7);
  CHECK(again.seq() == 6);
}

static void test_torn_record() {
  NorFlash f(2 * SECTOR_SIZE);
  Ring ring(&f, f.mem.size());
  CHECK(ring.begin());
  CHECK(ring.append(make(T0)));
  CHECK(ring.append(make(T0 + 300)));
  f.cut_after_bytes = 4;  // power lost after 4 of 10 bytes
  CHECK(!ring.append(make(T0 + 600)));
  // After the reboot: the torn slot is skipped (never reprogrammed), the two good records survive.
  Ring again(&f, f.mem.size());
  CHECK(again.begin() && again.next_slot() == 3);
  CHECK(again.append(make(T0 + 900)));
  auto got = all(again);
  CHECK(got.size() == 3);
  CHECK(got.size() == 3 && got[2].time / 60 == (T0 + 900) / 60);
}

static void test_torn_header_and_foreign_data() {
  // A stock 'storage' partition full of someone else's bytes: nothing reads as ours, writing starts at
  // sector 0 and erases each sector only when it gets there.
  NorFlash f(4 * SECTOR_SIZE, 0x5A);
  Ring ring(&f, f.mem.size());
  CHECK(ring.begin() && !ring.has_data());
  CHECK(all(ring).empty());
  CHECK(ring.append(make(T0)));
  CHECK(ring.current_sector() == 0 && f.erases == 1);
  CHECK(f.mem[2 * SECTOR_SIZE] == 0x5A);  // untouched until the ring reaches it
  CHECK(all(ring).size() == 1);

  // Power cut while writing the next sector's header: that sector is ignored and the ring carries on.
  for (uint32_t i = 1; i < RECORDS_PER_SECTOR; i++)
    CHECK(ring.append(make(T0 + 300 * i)));
  f.cut_after_bytes = 6;
  CHECK(!ring.append(make(T0 + 300 * RECORDS_PER_SECTOR)));
  Ring again(&f, f.mem.size());
  CHECK(again.begin() && again.current_sector() == 0 && again.next_slot() == RECORDS_PER_SECTOR);
  CHECK(all(again).size() == RECORDS_PER_SECTOR);
  CHECK(again.append(make(T0 + 300 * (RECORDS_PER_SECTOR + 1))));
  CHECK(again.current_sector() == 1 && all(again).size() == RECORDS_PER_SECTOR + 1);
}

static void test_clock_steps() {
  NorFlash f(4 * SECTOR_SIZE);
  Ring ring(&f, f.mem.size());
  CHECK(ring.begin());
  CHECK(ring.append(make(T0 + 600)));
  // A 30 s step back keeps the previous minute (same sector).
  CHECK(ring.append(make(T0 + 570)));
  CHECK(ring.current_sector() == 0);
  // An hour back starts a new sector with its own base.
  CHECK(ring.append(make(T0 - 3600)));
  CHECK(ring.current_sector() == 1);
  // More than ~45 days later (offset overflow) also starts a new sector.
  CHECK(ring.append(make(T0 + 50u * 86400u)));
  CHECK(ring.current_sector() == 2);
  auto got = all(ring);
  CHECK(got.size() == 4);
  // since skips whole old sectors without losing newer ones.
  CHECK(all(ring, T0 + 40u * 86400u).size() == 1);
}

static void test_write_failure_skips_slot() {
  NorFlash f(2 * SECTOR_SIZE);
  Ring ring(&f, f.mem.size());
  CHECK(ring.begin());
  CHECK(ring.append(make(T0)));
  f.fail_writes = true;
  CHECK(!ring.append(make(T0 + 300)));
  f.fail_writes = false;
  CHECK(ring.append(make(T0 + 600)));
  CHECK(ring.next_slot() == 3 && all(ring).size() == 2);
}

// /history: one stream at a time, then a cooldown of twice its duration (2-60 s), 429 + Retry-After meanwhile.
static void test_stream_gate() {
  StreamGate g;
  uint32_t retry = 0;
  CHECK(g.admit(1000, retry));               // the first request always goes
  CHECK(!g.admit(1500, retry));              // a second one while the first streams: busy
  CHECK(retry == 1);
  g.finish(6000);                            // streamed for 5 s
  CHECK(!g.admit(10000, retry));             // cooldown 10 s: until 16000
  CHECK(retry == 6);
  CHECK(g.admit(16000, retry));
  g.finish(16100);                           // a short one: the 2 s minimum
  CHECK(!g.admit(17000, retry));
  CHECK(g.admit(18100, retry));
  g.finish(18100 + 200000);                  // a pathological 200 s stream: capped at 60 s
  CHECK(!g.admit(18100 + 200000 + 59000, retry));
  CHECK(g.admit(18100 + 200000 + 60000, retry));
  g.finish(0xFFFFFF00u);                     // across the millis() wrap
  CHECK(!g.admit(0x00000100u, retry));
  CHECK(retry >= 1 && retry <= 60);
}

int main() {
  test_layout_and_capacity();
  test_pack_roundtrip();
  test_record_crc();
  test_append_and_read();
  test_wraps_and_keeps_order();
  test_torn_record();
  test_torn_header_and_foreign_data();
  test_clock_steps();
  test_write_failure_skips_slot();
  test_stream_gate();
  if (failures) {
    std::printf("chamber_history: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("chamber_history: all tests passed\n");
  return 0;
}
