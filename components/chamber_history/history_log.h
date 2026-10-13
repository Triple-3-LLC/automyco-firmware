#pragma once
// chamber_history's storage format and ring logic, with no ESP-IDF or ESPHome dependencies so it runs in
// host tests (esphome/tests/test_chamber_history.cpp). The component (chamber_history.cpp) supplies the
// flash through the Flash interface below and does the sampling and the HTTP side.
//
// Layout: a raw data partition (never a filesystem) split into 4 KiB sectors, used as a ring. Each sector
// holds one 16-byte header and 408 records of 10 bytes (16 + 408 * 10 = 4096, nothing wasted):
//
//   header  0  u32  magic 'AMYH' (bytes 41 4D 59 48)
//           4  u32  seq: +1 for every sector started; the highest valid one is the sector being written
//           8  u32  base: UTC epoch seconds of the sector's first record, rounded down to the minute
//          12  u8   format version (1)
//          13  u8   record size (10)
//          14  u16  CRC-16/CCITT-FALSE of bytes 0-13
//   record  0  u16  minutes after base (0xFFFF only in an erased slot)
//           2  7 B  the readings, bit-packed little-endian (pack() below), 56 bits
//           9  u8   CRC-8 (poly 0x07) of bytes 0-8, seeded with the sector's seq, so a torn write fails it
// All multi-byte fields little-endian.
//
// Power loss: a sector is erased before its header is written and its header is written before any record,
// so a cut at any point leaves either a sector with no valid header (ignored, erased again when the ring gets
// there) or a valid header with a torn last record (its CRC fails; the slot is skipped, never rewritten).
// Records are only ever appended into erased slots: NOR flash programs 1 -> 0 bits, so nothing earlier in
// the sector is touched. One erase per sector per lap (~34 h per sector at one record per 5 min), so even the
// 15-sector ESPHome partition sees an erase per sector every ~3 weeks: decades of flash life.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace history_log {

constexpr uint32_t SECTOR_SIZE = 4096;
constexpr uint32_t HEADER_SIZE = 16;
constexpr uint32_t RECORD_SIZE = 10;
constexpr uint32_t RECORDS_PER_SECTOR = (SECTOR_SIZE - HEADER_SIZE) / RECORD_SIZE;  // 408
constexpr uint32_t MAGIC = 0x48594D41u;  // "AMYH" as stored (little-endian)
constexpr uint8_t VERSION = 1;
constexpr uint32_t MIN_VALID_TIME = 1700000000u;  // 2023-11: anything earlier is a clock that was never set
constexpr uint16_t EMPTY_OFFSET = 0xFFFF;
constexpr uint16_t MAX_OFFSET = 0xFFFE;  // minutes after base a record can carry (~45 days)

static_assert(HEADER_SIZE + RECORDS_PER_SECTOR * RECORD_SIZE == SECTOR_SIZE, "sector layout");

// ---- Checksums -----------------------------------------------------------------------------------

inline uint8_t crc8(const uint8_t *data, size_t len, uint8_t crc = 0) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

inline uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

inline void put_u16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t get_u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// ---- One sample ----------------------------------------------------------------------------------

// What one record holds. NAN / UNKNOWN = not known for that interval.
struct Sample {
  static constexpr uint8_t UNKNOWN_PCT = 255;
  uint32_t time{0};          // UTC epoch seconds of the start of the interval
  float temperature{NAN};    // °F, mean over the interval (0.1 °F, -40.0 to 164.6)
  float humidity{NAN};       // %RH, mean (0.1 %)
  float temperature_target{NAN};  // °F setpoint at the end of the interval (whole degrees, 40-166)
  float humidity_target{NAN};     // % setpoint (whole percent, 0-126)
  uint8_t heater_pct{UNKNOWN_PCT};  // heater duty over the interval, 0-100 in 5 % steps
  uint8_t mist_pct{UNKNOWN_PCT};    // atomizer on-time over the interval, 0-100 in 5 % steps
  float co2{NAN};            // ppm, mean (10 ppm steps, 0-5100); NAN without a CO2 sensor
  uint8_t phase{0};          // 0 = Climate Control off, 1 Incubation, 2 Pinning, 3 Fruiting
};

// Field widths (bits), in packing order from bit 0.
constexpr int BITS_TEMP = 11, BITS_RH = 10, BITS_TSP = 7, BITS_RHSP = 7, BITS_HEAT = 5, BITS_MIST = 5,
              BITS_CO2 = 9, BITS_PHASE = 2;
static_assert(BITS_TEMP + BITS_RH + BITS_TSP + BITS_RHSP + BITS_HEAT + BITS_MIST + BITS_CO2 + BITS_PHASE == 56,
              "7 data bytes");

namespace detail {
inline uint32_t all_ones(int bits) { return (1u << bits) - 1u; }
// round(v * scale) + offset, clamped to [0, max-1]; the all-ones value means unknown.
inline uint32_t quantize(float v, float scale, float offset, int bits) {
  const uint32_t unknown = all_ones(bits);
  if (std::isnan(v))
    return unknown;
  float q = std::round(v * scale + offset);
  if (q < 0.0f)
    q = 0.0f;
  if (q > static_cast<float>(unknown - 1))
    q = static_cast<float>(unknown - 1);
  return static_cast<uint32_t>(q);
}
inline float dequantize(uint32_t raw, float scale, float offset, int bits) {
  if (raw == all_ones(bits))
    return NAN;
  return (static_cast<float>(raw) - offset) / scale;
}
inline uint32_t pct_raw(uint8_t pct) {
  if (pct == Sample::UNKNOWN_PCT)
    return all_ones(BITS_HEAT);
  const uint32_t p = pct > 100 ? 100 : pct;
  return (p + 2) / 5;  // nearest 5 %
}
inline uint8_t raw_pct(uint32_t raw) { return raw == all_ones(BITS_HEAT) ? Sample::UNKNOWN_PCT : static_cast<uint8_t>(raw > 20 ? 100 : raw * 5); }
}  // namespace detail

// The 56 data bits of a record (everything but the time).
inline uint64_t pack(const Sample &s) {
  using namespace detail;
  uint64_t v = 0;
  int at = 0;
  auto put = [&](uint32_t raw, int bits) {
    v |= static_cast<uint64_t>(raw & all_ones(bits)) << at;
    at += bits;
  };
  put(quantize(s.temperature, 10.0f, 400.0f, BITS_TEMP), BITS_TEMP);
  put(quantize(s.humidity, 10.0f, 0.0f, BITS_RH), BITS_RH);
  put(quantize(s.temperature_target, 1.0f, -40.0f, BITS_TSP), BITS_TSP);
  put(quantize(s.humidity_target, 1.0f, 0.0f, BITS_RHSP), BITS_RHSP);
  put(pct_raw(s.heater_pct), BITS_HEAT);
  put(pct_raw(s.mist_pct), BITS_MIST);
  put(quantize(s.co2, 0.1f, 0.0f, BITS_CO2), BITS_CO2);
  put(s.phase > 3 ? 0 : s.phase, BITS_PHASE);
  return v;
}

inline Sample unpack(uint64_t v, uint32_t time) {
  using namespace detail;
  Sample s;
  s.time = time;
  int at = 0;
  auto get = [&](int bits) {
    const uint32_t raw = static_cast<uint32_t>(v >> at) & all_ones(bits);
    at += bits;
    return raw;
  };
  s.temperature = dequantize(get(BITS_TEMP), 10.0f, 400.0f, BITS_TEMP);
  s.humidity = dequantize(get(BITS_RH), 10.0f, 0.0f, BITS_RH);
  s.temperature_target = dequantize(get(BITS_TSP), 1.0f, -40.0f, BITS_TSP);
  s.humidity_target = dequantize(get(BITS_RHSP), 1.0f, 0.0f, BITS_RHSP);
  s.heater_pct = raw_pct(get(BITS_HEAT));
  s.mist_pct = raw_pct(get(BITS_MIST));
  s.co2 = dequantize(get(BITS_CO2), 0.1f, 0.0f, BITS_CO2);
  s.phase = static_cast<uint8_t>(get(BITS_PHASE));
  return s;
}

inline void encode_record(uint8_t out[RECORD_SIZE], uint16_t offset_min, uint64_t data, uint32_t seq) {
  put_u16(out, offset_min);
  for (int i = 0; i < 7; i++)
    out[2 + i] = static_cast<uint8_t>(data >> (8 * i));
  out[9] = crc8(out, 9, static_cast<uint8_t>(seq));
}

inline bool is_erased(const uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (p[i] != 0xFF)
      return false;
  }
  return true;
}

// A record that was completely written, or false (erased slot, torn write, garbage).
inline bool decode_record(const uint8_t in[RECORD_SIZE], uint32_t seq, uint32_t base, Sample *out) {
  const uint16_t offset = get_u16(in);
  if (offset == EMPTY_OFFSET || crc8(in, 9, static_cast<uint8_t>(seq)) != in[9])
    return false;
  uint64_t data = 0;
  for (int i = 0; i < 7; i++)
    data |= static_cast<uint64_t>(in[2 + i]) << (8 * i);
  *out = unpack(data, base + static_cast<uint32_t>(offset) * 60u);
  return true;
}

// ---- Sector header -------------------------------------------------------------------------------

struct Header {
  uint32_t seq{0};
  uint32_t base{0};
};

inline void encode_header(uint8_t out[HEADER_SIZE], const Header &h) {
  put_u32(out, MAGIC);
  put_u32(out + 4, h.seq);
  put_u32(out + 8, h.base);
  out[12] = VERSION;
  out[13] = RECORD_SIZE;
  put_u16(out + 14, crc16(out, 14));
}

inline bool decode_header(const uint8_t in[HEADER_SIZE], Header *out) {
  if (get_u32(in) != MAGIC || in[12] != VERSION || in[13] != RECORD_SIZE || crc16(in, 14) != get_u16(in + 14))
    return false;
  out->seq = get_u32(in + 4);
  out->base = get_u32(in + 8);
  return out->seq != 0 && out->base >= MIN_VALID_TIME;
}

// ---- The ring ------------------------------------------------------------------------------------

// The partition, as the ring sees it. Offsets are relative to the partition start.
class Flash {
 public:
  virtual ~Flash() = default;
  virtual bool read(uint32_t offset, void *dst, size_t len) = 0;
  virtual bool write(uint32_t offset, const void *src, size_t len) = 0;
  virtual bool erase_sector(uint32_t offset) = 0;  // one SECTOR_SIZE-aligned sector
};

// Days a partition of `size` bytes always holds at one record per `interval_s`: every sector but the one
// being reused is full.
inline float capacity_days(uint32_t size, uint32_t interval_s) {
  const uint32_t sectors = size / SECTOR_SIZE;
  if (sectors < 2)
    return 0.0f;
  return static_cast<float>((sectors - 1) * RECORDS_PER_SECTOR) * interval_s / 86400.0f;
}

class Ring {
 public:
  // `size`: the partition size; only whole sectors are used. Needs at least 2.
  Ring(Flash *flash, uint32_t size) : flash_(flash), sectors_(size / SECTOR_SIZE) {}

  uint32_t sectors() const { return this->sectors_; }
  bool usable() const { return this->sectors_ >= 2; }

  // Finds the sector being written and the first free slot in it. Reads 16 bytes per sector plus one
  // sector of records. Sectors without a valid header (erased, torn, someone else's data) are skipped
  // and get erased when the ring reaches them.
  bool begin() {
    if (!this->usable())
      return false;
    bool found = false;
    uint32_t best = 0;
    Header best_h;
    uint8_t buf[HEADER_SIZE];
    for (uint32_t i = 0; i < this->sectors_; i++) {
      Header h;
      if (!this->flash_->read(i * SECTOR_SIZE, buf, HEADER_SIZE) || !decode_header(buf, &h))
        continue;
      if (!found || static_cast<int32_t>(h.seq - best_h.seq) > 0) {
        found = true;
        best = i;
        best_h = h;
      }
    }
    this->valid_ = found;
    if (!found) {
      // Nothing of ours yet (fresh partition, or the stock firmware's old data): start at sector 0.
      this->cur_ = this->sectors_ - 1;
      this->seq_ = 0;
      this->next_slot_ = RECORDS_PER_SECTOR;
      this->last_time_ = 0;
      return true;
    }
    this->cur_ = best;
    this->seq_ = best_h.seq;
    this->base_ = best_h.base;
    this->last_time_ = best_h.base;
    // First slot after the last one that isn't erased (written or torn alike: a torn slot is never reused).
    uint32_t next = 0;
    uint8_t rec[RECORD_SIZE * 24];
    for (uint32_t slot = 0; slot < RECORDS_PER_SECTOR; slot += 24) {
      const uint32_t n = (RECORDS_PER_SECTOR - slot) < 24 ? (RECORDS_PER_SECTOR - slot) : 24;
      if (!this->flash_->read(this->record_offset_(best, slot), rec, n * RECORD_SIZE))
        return false;
      for (uint32_t k = 0; k < n; k++) {
        const uint8_t *r = rec + k * RECORD_SIZE;
        if (is_erased(r, RECORD_SIZE))
          continue;
        next = slot + k + 1;
        Sample s;
        if (decode_record(r, best_h.seq, best_h.base, &s))
          this->last_time_ = s.time;
      }
    }
    this->next_slot_ = next;
    return true;
  }

  // Appends one sample (its time must be a real clock: >= MIN_VALID_TIME). Starts a new sector when the
  // current one is full, the clock went back more than a minute, or the time is too far past the base.
  // False on a flash error or an unset clock.
  bool append(const Sample &s) {
    if (!this->usable() || s.time < MIN_VALID_TIME)
      return false;
    const bool backwards = s.time + 60 < this->last_time_;
    // (last_time_ >= base_ in a valid sector, so a time that isn't `backwards` is at most a minute before
    // base_, and the clamp below lifts it to last_time_.)
    if (!this->valid_ || this->next_slot_ >= RECORDS_PER_SECTOR || backwards ||
        (s.time > this->base_ && (s.time - this->base_) / 60 > MAX_OFFSET)) {
      if (!this->start_sector_(s.time))
        return false;
    }
    // A clock that stepped back by under a minute keeps the previous record's minute.
    const uint32_t t = s.time < this->last_time_ ? this->last_time_ : s.time;
    const uint16_t offset = static_cast<uint16_t>((t - this->base_) / 60);
    uint8_t rec[RECORD_SIZE];
    encode_record(rec, offset, pack(s), this->seq_);
    const uint32_t slot = this->next_slot_++;  // used up even if the write fails: never program a slot twice
    if (!this->flash_->write(this->record_offset_(this->cur_, slot), rec, RECORD_SIZE))
      return false;
    this->last_time_ = t;
    this->count_written_++;
    return true;
  }

  // Calls fn(const Sample &) for every intact record with time > since, oldest sector first (ring order
  // starting after the sector being written, which comes last). Only reads flash, so it can run on another
  // task while append() runs: a record caught mid-write fails its CRC and is skipped, and a sector the
  // writer erases meanwhile just yields fewer (or newer) records. Returns false on a read error.
  // `buf`/`buf_len`: scratch space, a multiple of RECORD_SIZE and at least one record.
  template<typename F> bool for_each(uint32_t since, uint8_t *buf, size_t buf_len, F &&fn) const {
    if (!this->usable() || buf_len < RECORD_SIZE)
      return false;
    const uint32_t per_read = static_cast<uint32_t>(buf_len / RECORD_SIZE);
    const uint32_t cur = this->cur_.load();
    for (uint32_t step = 1; step <= this->sectors_; step++) {
      const uint32_t sector = (cur + step) % this->sectors_;
      uint8_t hb[HEADER_SIZE];
      Header h;
      if (!this->flash_->read(sector * SECTOR_SIZE, hb, HEADER_SIZE))
        return false;
      if (!decode_header(hb, &h))
        continue;
      // A whole sector spans at most MAX_OFFSET minutes after its base; skip it without reading if it
      // can't hold anything newer than `since`.
      if (static_cast<uint64_t>(h.base) + static_cast<uint64_t>(MAX_OFFSET) * 60u <= since)
        continue;
      for (uint32_t slot = 0; slot < RECORDS_PER_SECTOR; slot += per_read) {
        const uint32_t n = (RECORDS_PER_SECTOR - slot) < per_read ? (RECORDS_PER_SECTOR - slot) : per_read;
        if (!this->flash_->read(record_offset_(sector, slot), buf, n * RECORD_SIZE))
          return false;
        bool any = false;
        for (uint32_t k = 0; k < n; k++) {
          const uint8_t *r = buf + k * RECORD_SIZE;
          if (is_erased(r, RECORD_SIZE))
            continue;
          any = true;
          Sample s;
          if (decode_record(r, h.seq, h.base, &s) && s.time > since)
            fn(s);
        }
        if (!any)
          break;  // an all-erased run: the rest of the sector is unwritten
      }
    }
    return true;
  }

  // Diagnostics.
  uint32_t current_sector() const { return this->cur_.load(); }
  uint32_t next_slot() const { return this->next_slot_; }
  uint32_t seq() const { return this->seq_; }
  uint32_t written() const { return this->count_written_; }
  bool has_data() const { return this->valid_; }

 protected:
  static uint32_t record_offset_(uint32_t sector, uint32_t slot) {
    return sector * SECTOR_SIZE + HEADER_SIZE + slot * RECORD_SIZE;
  }

  bool start_sector_(uint32_t time) {
    const uint32_t next = (this->cur_.load() + 1) % this->sectors_;
    const Header h{this->seq_ + 1 == 0 ? 1 : this->seq_ + 1, time - time % 60};
    // The sector stops counting as the oldest data before it is erased: readers skip it from here on.
    if (!this->flash_->erase_sector(next * SECTOR_SIZE)) {
      this->valid_ = false;  // try the sector after it next time
      this->cur_ = next;
      this->next_slot_ = RECORDS_PER_SECTOR;
      return false;
    }
    uint8_t hb[HEADER_SIZE];
    encode_header(hb, h);
    this->cur_ = next;
    this->seq_ = h.seq;
    this->base_ = h.base;
    this->next_slot_ = 0;
    this->last_time_ = h.base;
    if (!this->flash_->write(next * SECTOR_SIZE, hb, HEADER_SIZE)) {
      this->valid_ = false;
      this->next_slot_ = RECORDS_PER_SECTOR;
      return false;
    }
    this->valid_ = true;
    return true;
  }

  Flash *flash_;
  uint32_t sectors_;
  std::atomic<uint32_t> cur_{0};  // sector being written (read by for_each on the web server's task)
  uint32_t seq_{0};
  uint32_t base_{0};
  uint32_t next_slot_{RECORDS_PER_SECTOR};
  uint32_t last_time_{0};
  uint32_t count_written_{0};
  bool valid_{false};
};

// GET /history is served on the web server's only task, and a full stream from a stock-layout chamber
// (~180 days, a few MB) keeps that task busy for seconds, so the app and the dashboard wait behind it. One
// stream at a time, and after each one a cooldown of twice its duration (at least MIN_GAP_MS, at most
// MAX_GAP_MS): a client asking in a loop gets 429 + Retry-After and the web server stays at least ~2/3
// free. The app asks at most once per 5 minutes per chamber, so it never meets it.
class StreamGate {
 public:
  static constexpr uint32_t MIN_GAP_MS = 2000;
  static constexpr uint32_t MAX_GAP_MS = 60000;

  // True if a stream may start now (it is then running). Otherwise false, with retry_after_s set.
  bool admit(uint32_t now_ms, uint32_t &retry_after_s) {
    bool expected = false;
    if (!this->busy_.compare_exchange_strong(expected, true)) {
      retry_after_s = 1;
      return false;
    }
    const int32_t wait = static_cast<int32_t>(this->next_ok_ms_ - now_ms);
    if (this->started_once_ && wait > 0) {
      this->busy_ = false;
      retry_after_s = (static_cast<uint32_t>(wait) + 999) / 1000;
      return false;
    }
    this->start_ms_ = now_ms;
    return true;
  }
  // The admitted stream is done (or failed).
  void finish(uint32_t now_ms) {
    uint32_t gap = 2 * (now_ms - this->start_ms_);
    if (gap < MIN_GAP_MS)
      gap = MIN_GAP_MS;
    if (gap > MAX_GAP_MS)
      gap = MAX_GAP_MS;
    this->next_ok_ms_ = now_ms + gap;
    this->started_once_ = true;
    this->busy_ = false;
  }

 protected:
  std::atomic<bool> busy_{false};
  bool started_once_{false};
  uint32_t start_ms_{0};
  uint32_t next_ok_ms_{0};
};

}  // namespace history_log
