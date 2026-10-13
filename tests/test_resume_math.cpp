// Host tests for esphome/common/resume_math.h (resume or pause after a power cut).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_resume_math.cpp -o /tmp/tst && /tmp/tst
#include "resume_math.h"

#include <cstdio>

using namespace resume_math;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const uint32_t T0 = 1791500000;   // an October 2026 timestamp
static const uint32_t H = 3600;
static const uint32_t HALF = MAX_STALE_S / 2;   // best estimate = the most it can be, less this

int main() {
  // Unknown last-alive (first boot, an install over stock, or a run that never had the clock): resume.
  CHECK(judge(0, T0, 30, true, 72).action == Action::NONE);

  // A 3 h cut mid-grow: resume, and the uptime before the clock arrived isn't counted as time off.
  // last_alive can be up to MAX_STALE_S old, so 3 h is the most it can have been; the estimate is the middle.
  Verdict v = judge(T0, T0 + 3 * H + 45, 45, true, 72);
  CHECK(v.action == Action::RESUME);
  CHECK(v.off_max_s == 3 * H);
  CHECK(v.off_s == 3 * H - HALF);

  // The 72 h rule uses the best estimate: exactly 72 h estimated still resumes; a second more pauses.
  CHECK(judge(T0, T0 + 72 * H + HALF + 20, 20, true, 72).action == Action::RESUME);
  CHECK(judge(T0, T0 + 72 * H + HALF + 21, 20, true, 72).action == Action::PAUSE);
  CHECK(judge(T0, T0 + 10 * 24 * H, 20, true, 72).action == Action::PAUSE);

  // A cut shorter than half the save interval: the estimate floors at 0, the bound is kept.
  v = judge(T0, T0 + 300 + 20, 20, true, 72);
  CHECK(v.action == Action::RESUME && v.off_s == 0 && v.off_max_s == 300);

  // Climate Control off: never pause or resume, only log an outage of a minute or more.
  CHECK(judge(T0, T0 + 10 * 24 * H, 20, false, 72).action == Action::LOG);
  CHECK(judge(T0, T0 + 50, 20, false, 72).action == Action::NONE);

  // The clock arrives 10 min or more after boot: no judgement (that runtime isn't time off).
  CHECK(judge(T0, T0 + 100 * H, 600, true, 72).action == Action::NONE);
  CHECK(judge(T0, T0 + 100 * H, 599, true, 72).action == Action::PAUSE);

  // A clock behind last-alive (bad first sync) or a reboot with no time off: nothing to judge.
  CHECK(judge(T0, T0 - 100, 20, true, 72).action == Action::NONE);
  CHECK(judge(T0, T0 + 20, 20, true, 72).action == Action::NONE);

  // Reset reasons (2026-10-09: a 15 s OTA reboot was logged as "power was off 0 h 11 m").
  CHECK(!is_restart(reset::POWERON));
  CHECK(!is_restart(reset::BROWNOUT));
  CHECK(!is_restart(reset::UNKNOWN));   // unknown: judged as a power cut, as before
  CHECK(!is_restart(14));               // a reason newer chips add (power glitch)
  for (int r : {reset::SW, reset::PANIC, reset::INT_WDT, reset::TASK_WDT, reset::WDT, reset::DEEPSLEEP,
                reset::EXT, reset::SDIO})
    CHECK(is_restart(r));
  CHECK(std::string(reset_name(reset::SW)) == "software");
  CHECK(std::string(reset_name(reset::TASK_WDT)) == "watchdog");
  CHECK(std::string(reset_name(reset::PANIC)) == "crash");
  CHECK(std::string(reset_name(99)) == "unknown");

  // A restart never measures an outage and never pauses, however stale last_alive is.
  v = judge(T0, T0 + 11 * 60 + 15, 15, true, 72, true);
  CHECK(v.action == Action::RESUME && v.off_s == 0 && v.off_max_s == 0);
  CHECK(judge(T0, T0 + 10 * 24 * H, 20, true, 72, true).action == Action::RESUME);
  CHECK(judge(0, T0, 20, true, 72, true).action == Action::RESUME);
  CHECK(judge(T0, T0 + 10 * 24 * H, 20, false, 72, true).action == Action::NONE);
  CHECK(judge(T0, T0 + 10 * 24 * H, 600, true, 72, true).action == Action::NONE);

  CHECK(duration(3 * H + 10 * 60) == "3 h 10 m");
  CHECK(duration(59) == "0 h 0 m");
  CHECK(duration(4 * 24 * H + 2 * H + 59) == "4 d 2 h");

  // The outage as a range: short ones as "up to", longer ones from the earliest to the latest it can be.
  CHECK(outage(11 * 60) == "up to 0 h 11 m");
  CHECK(outage(MAX_STALE_S + 60) == "up to 0 h 17 m");
  CHECK(outage(3 * H + 10 * 60) == "2 h 53 m to 3 h 10 m");
  CHECK(outage(4 * 24 * H + 2 * H) == "4 d 1 h to 4 d 2 h");

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("resume_math: all passed\n");
  return 0;
}
