#pragma once
// UV-C daily budget (common/terrashroom-core.yaml "UV-C Lamp"): the lamp can be lit for at most BUDGET_S
// seconds in any rolling 24 hours, however many times it is switched on. UV-C Auto-Off (1-60 min) bounds one
// run; this bounds the sum, so re-pressing UV-C Lamp, a script or a stolen key can't keep it lit all day.
//
// How: 24 hourly slots of lit seconds (a ring). Each hour the oldest slot is dropped. The ring is saved in
// flash (Store, below), so a reboot doesn't reset it. Time is counted by millis() within a boot. Across a reboot, the
// first wall-clock time of the new boot (SNTP, the held RTC timer or the DS3231) says how many hours the
// chamber was off, and those slots are dropped too. Until then the outage counts as zero hours: unknown time
// keeps the budget spent, never frees it. Nothing here can make the lamp run longer than Auto-Off allows.
//
// Pure C++17, no ESPHome: esphome/tests/test_uvc_budget.cpp checks it on the host (-Wall -Wextra -Werror).
#include <cstdint>
#include <cstring>

#ifdef USE_ESP32
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#endif

namespace uvc_budget {

inline constexpr uint32_t BUDGET_S = 60UL * 60UL;   // lit seconds allowed per rolling 24 h
inline constexpr uint8_t SLOTS = 24;                // one per hour
inline constexpr uint32_t HOUR_MS = 3600UL * 1000UL;
inline constexpr uint32_t MIN_RUN_S = 60;           // less than this left: UV-C on is refused
inline constexpr uint32_t MIN_VALID_EPOCH = 1700000000UL;   // 2023-11: anything earlier is an unset clock

// Saved (restore_value global). Trivially copyable: ESPHome stores it as one preference blob.
struct Ledger {
  uint16_t used_s[SLOTS];   // lit seconds in each of the last 24 hours; [head] is the current hour
  uint8_t head;
  uint32_t epoch_hour;      // the wall-clock hour (epoch / 3600) [head] stands for; 0 = not known yet
};

// RAM only, per boot.
struct Clock {
  uint32_t hour_start_ms{0};   // millis() the current uptime hour began
  uint32_t last_ms{0};         // millis() of the previous tick
  uint32_t carry_ms{0};        // lit milliseconds not yet counted as a whole second
  uint32_t ms_hours{0};        // hours advanced by uptime before the wall clock was known
  bool synced{false};          // the wall clock has been seen this boot
};

// A restored ledger from flash (or garbage from a format change): keep it only if it makes sense.
inline void sanitize(Ledger &l) {
  if (l.head >= SLOTS) {
    l = Ledger{};
    return;
  }
  for (auto &s : l.used_s)
    if (s > BUDGET_S)
      s = BUDGET_S;
}

inline uint32_t used(const Ledger &l) {
  uint32_t sum = 0;
  for (auto s : l.used_s)
    sum += s;
  return sum;
}

inline uint32_t remaining(const Ledger &l) {
  const uint32_t u = used(l);
  return u >= BUDGET_S ? 0 : BUDGET_S - u;
}

// `hours` new hours have begun: the oldest slots fall out of the window.
inline void advance(Ledger &l, uint32_t hours) {
  if (hours > SLOTS)
    hours = SLOTS;
  for (uint32_t i = 0; i < hours; i++) {
    l.head = (uint8_t) ((l.head + 1) % SLOTS);
    l.used_s[l.head] = 0;
  }
}

// Call once at boot, after the ledger has been restored.
inline void begin(Ledger &l, Clock &c, uint32_t now_ms) {
  sanitize(l);
  c = Clock{};
  c.hour_start_ms = now_ms;
  c.last_ms = now_ms;
}

// Call about once a second. `epoch_s` is the wall-clock time, or 0 (or anything before MIN_VALID_EPOCH) when
// it isn't known. `lit` = the lamp was on since the previous tick.
inline void tick(Ledger &l, Clock &c, uint32_t now_ms, uint32_t epoch_s, bool lit) {
  // Lit time, from millis() rather than tick counts, so a blocked loop doesn't lose seconds.
  if (lit) {
    c.carry_ms += now_ms - c.last_ms;
    const uint32_t secs = c.carry_ms / 1000;
    c.carry_ms %= 1000;
    const uint32_t room = 0xFFFFu - l.used_s[l.head];
    l.used_s[l.head] = (uint16_t) (l.used_s[l.head] + (secs < room ? secs : room));
  } else {
    c.carry_ms = 0;
  }
  c.last_ms = now_ms;

  const bool clock_ok = epoch_s >= MIN_VALID_EPOCH;
  if (clock_ok) {
    c.hour_start_ms = now_ms;   // should the clock ever go away again, uptime counting restarts from here
    const uint32_t hour = epoch_s / 3600;
    if (!c.synced) {
      // First wall-clock time this boot: drop the hours the chamber was off. The uptime hours already
      // dropped this boot are part of that gap.
      if (l.epoch_hour != 0 && hour > l.epoch_hour) {
        const uint32_t gap = hour - l.epoch_hour;
        advance(l, gap > c.ms_hours ? gap - c.ms_hours : 0);
      }
      l.epoch_hour = hour;
      c.synced = true;
    } else if (hour > l.epoch_hour) {
      advance(l, hour - l.epoch_hour);
      l.epoch_hour = hour;
    } else if (hour < l.epoch_hour) {
      l.epoch_hour = hour;   // the clock went back: follow it, drop nothing
    }
    return;
  }
  // No wall clock yet: hours by uptime.
  while (now_ms - c.hour_start_ms >= HOUR_MS) {
    c.hour_start_ms += HOUR_MS;
    advance(l, 1);
    c.ms_hours++;
  }
}

// Seconds the next run may last: Auto-Off's length, cut to what's left of the budget. 0 = refuse.
inline uint32_t run_seconds(const Ledger &l, uint32_t auto_off_s) {
  const uint32_t left = remaining(l);
  if (left < MIN_RUN_S)
    return 0;
  return auto_off_s < left ? auto_off_s : left;
}

#ifdef USE_ESP32
// On the chamber: the ledger lives here, saved as its own preference (ESPHome globals are declared before
// the includes, so they can't hold a type from this header). Key: fnv1("uvc_budget_ledger"), new in this
// release, so it can't collide with anything older units saved. Written through the same preferences buffer
// as the restored globals: on flash within a minute (flash_write_interval), and at a clean shutdown.
struct Store {
  Ledger ledger{};
  Clock clock{};
  Ledger saved{};
  esphome::ESPPreferenceObject pref;

  void begin(uint32_t now_ms) {
    this->pref = esphome::global_preferences->make_preference<Ledger>(esphome::fnv1_hash("uvc_budget_ledger"));
    if (!this->pref.load(&this->ledger))
      this->ledger = Ledger{};
    uvc_budget::begin(this->ledger, this->clock, now_ms);
    std::memcpy(&this->saved, &this->ledger, sizeof(Ledger));
  }
  void tick(uint32_t now_ms, uint32_t epoch_s, bool lit) {
    uvc_budget::tick(this->ledger, this->clock, now_ms, epoch_s, lit);
    if (std::memcmp(&this->ledger, &this->saved, sizeof(Ledger)) != 0) {
      this->pref.save(&this->ledger);
      std::memcpy(&this->saved, &this->ledger, sizeof(Ledger));
    }
  }
};
inline Store g_store;
#endif

}  // namespace uvc_budget
