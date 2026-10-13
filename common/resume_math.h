#pragma once
// Resume after a power cut: what the chamber does once its clock is valid after a boot
// (docs/setup-simplification.md, section 3a).
//
// Climate Control and the grow are saved, so they come back as they were. This only judges why the chamber
// booted and, after a real power cut, how long the power was off:
//   - a restart (software or OTA, crash, watchdog, deep sleep): the power never went off. Nothing is
//     measured and nothing is paused; a running Climate Control is logged as resumed.
//   - Climate Control was off: nothing to resume; the outage is logged if it was a minute or more.
//   - up to max_hours off: carry on (Climate Control already came back on).
//   - longer: pause. Climate Control goes off and Grow Paused comes on until the owner turns it back on
//     or ends the grow. A chamber taken out of storage shouldn't start heating and misting by itself.
// last_alive is saved every 15 min while the clock is valid (and at every clean shutdown), and cleared at
// boot, so it is 0 ("unknown") after a first boot or a run that never had the clock; then nothing is judged
// and the chamber resumes. A power cut gives no warning, so last_alive can be up to MAX_STALE_S old when it
// happens: the outage lies between off_max_s - MAX_STALE_S and off_max_s. off_s, the midpoint, is the best
// estimate, and it is what the 72 h rule and Last Power Outage use.
// Judging only within 10 min of boot keeps a clock that arrives hours later from counting that runtime
// as time off.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_resume_math.cpp can test it on a Mac.
#include <cstdint>
#include <cstdio>
#include <string>

namespace resume_math {

// ESP-IDF's esp_reset_reason_t values (esp_system.h), mirrored so the host tests can use them. On the
// device the static_assert at the end of this file checks that they still match.
namespace reset {
constexpr int UNKNOWN = 0, POWERON = 1, EXT = 2, SW = 3, PANIC = 4, INT_WDT = 5, TASK_WDT = 6, WDT = 7,
              DEEPSLEEP = 8, BROWNOUT = 9, SDIO = 10;
}

// True when the chip restarted with the power on: software (a restart button, an OTA update), a crash, a
// watchdog, deep sleep, the reset pin or SDIO. Power-on and brownout are power cuts. So is any reason this
// doesn't know (UNKNOWN, and reasons newer chips add): those are judged by last_alive as before, which
// can't pause a chamber that was running moments ago, since its last_alive is fresh.
inline bool is_restart(int reason) {
  switch (reason) {
    case reset::SW: case reset::PANIC: case reset::INT_WDT: case reset::TASK_WDT: case reset::WDT:
    case reset::DEEPSLEEP: case reset::EXT: case reset::SDIO:
      return true;
    default:
      return false;
  }
}

// Short name for the event log: "restarted (watchdog): resumed Pinning day 1".
inline const char *reset_name(int reason) {
  switch (reason) {
    case reset::POWERON: return "power on";
    case reset::BROWNOUT: return "brownout";
    case reset::SW: return "software";
    case reset::PANIC: return "crash";
    case reset::INT_WDT: case reset::TASK_WDT: case reset::WDT: return "watchdog";
    case reset::DEEPSLEEP: return "deep sleep";
    case reset::EXT: return "reset pin";
    case reset::SDIO: return "SDIO";
    default: return "unknown";
  }
}

enum class Action { NONE, LOG, RESUME, PAUSE };

struct Verdict {
  Action action;
  uint32_t off_s;       // best estimate of the seconds the power was off (0 when not judged, or a restart)
  uint32_t off_max_s;   // the most it can have been (0 when not judged, or a restart)
};

constexpr uint32_t JUDGE_WITHIN_S = 600;
// How old last_alive can be when the power goes: rewritten 15 min (900 s) after the last write, checked
// every 10 s, then up to 60 s in ESPHome's flash-write queue, plus a little slack.
constexpr uint32_t MAX_STALE_S = 975;

// prev_alive: last_alive as found at boot (0 = unknown). now: UTC epoch seconds. uptime_s: since boot.
// restart: is_restart(esp_reset_reason()).
inline Verdict judge(uint32_t prev_alive, uint32_t now, uint32_t uptime_s, bool climate_on, uint32_t max_hours,
                     bool restart = false) {
  if (uptime_s >= JUDGE_WITHIN_S) return {Action::NONE, 0, 0};
  if (restart) return {climate_on ? Action::RESUME : Action::NONE, 0, 0};   // a restart never pauses
  if (prev_alive == 0 || now <= prev_alive + uptime_s) return {Action::NONE, 0, 0};
  const uint32_t off_max = now - uptime_s - prev_alive;
  const uint32_t off = off_max > MAX_STALE_S / 2 ? off_max - MAX_STALE_S / 2 : 0;
  if (!climate_on) return {off_max >= 60 ? Action::LOG : Action::NONE, off, off_max};
  return {off > max_hours * 3600UL ? Action::PAUSE : Action::RESUME, off, off_max};
}

// "3 h 10 m", or "4 d 2 h" from a day up.
inline std::string duration(uint32_t s) {
  char b[24];
  if (s >= 86400) std::snprintf(b, sizeof(b), "%lu d %lu h", (unsigned long) (s / 86400), (unsigned long) (s % 86400 / 3600));
  else std::snprintf(b, sizeof(b), "%lu h %lu m", (unsigned long) (s / 3600), (unsigned long) (s % 3600 / 60));
  return b;
}

// The outage as the range it is known to lie in, from Verdict::off_max_s: "up to 0 h 11 m" for a short
// one, "2 h 54 m to 3 h 10 m" otherwise.
inline std::string outage(uint32_t off_max_s) {
  if (off_max_s <= MAX_STALE_S + 60) return "up to " + duration(off_max_s);
  return duration(off_max_s - MAX_STALE_S) + " to " + duration(off_max_s);
}

}  // namespace resume_math

#ifdef ESP_PLATFORM
#include <esp_system.h>
static_assert(ESP_RST_UNKNOWN == resume_math::reset::UNKNOWN && ESP_RST_POWERON == resume_math::reset::POWERON &&
                  ESP_RST_EXT == resume_math::reset::EXT && ESP_RST_SW == resume_math::reset::SW &&
                  ESP_RST_PANIC == resume_math::reset::PANIC && ESP_RST_INT_WDT == resume_math::reset::INT_WDT &&
                  ESP_RST_TASK_WDT == resume_math::reset::TASK_WDT && ESP_RST_WDT == resume_math::reset::WDT &&
                  ESP_RST_DEEPSLEEP == resume_math::reset::DEEPSLEEP &&
                  ESP_RST_BROWNOUT == resume_math::reset::BROWNOUT && ESP_RST_SDIO == resume_math::reset::SDIO,
              "resume_math::reset must match esp_reset_reason_t");
#endif
