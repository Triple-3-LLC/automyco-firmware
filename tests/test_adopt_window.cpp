// Host tests for esphome/adopt_window.h: when Home Assistant adoption (ESPHome's provisioning window) may
// open on the public chamber and camera. Fakes POWERON / software / panic resets.
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_adopt_window.cpp -o /tmp/t && /tmp/t
#define TS_ADOPT_HOST_TEST
#include "adopt_window.h"

#include <cstdio>
#include <initializer_list>

using namespace ts_adopt;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static void test_power_on_opens() {
  // A real power-on (or the USB flasher's EN pulse): HA adoption works for 30 minutes, as before.
  uint32_t magic = 0xDEADBEEF;   // RTC garbage after power loss
  const Decision d = decide(Reset::POWER_ON, magic, false);
  CHECK(d.window_open);
  CHECK(!d.close_now);
  CHECK(!d.adopt_boot);
  CHECK(magic == 0);
}

static void test_software_restart_closes() {
  // Restart button, OTA update, LAN-triggered restart: closed at once.
  uint32_t magic = 0;
  const Decision d = decide(Reset::SOFTWARE, magic, false);
  CHECK(!d.window_open);
  CHECK(d.close_now);
}

static void test_other_resets_close() {
  for (uint32_t m0 : {0u, MAGIC}) {   // even with the magic word: only a software restart may use it
    uint32_t magic = m0;
    const Decision d = decide(Reset::OTHER, magic, false);
    CHECK(!d.window_open);
    CHECK(d.close_now);
    CHECK(magic == 0);
  }
}

static void test_open_adoption_window_once() {
  uint32_t magic = MAGIC;   // armed by the button, then esp_restart()
  const Decision d = decide(Reset::SOFTWARE, magic, false);
  CHECK(d.window_open);
  CHECK(d.adopt_boot);
  CHECK(!d.close_now);
  CHECK(magic == 0);
  // The next software restart (say, the window expired and someone presses Restart) is closed again.
  const Decision again = decide(Reset::SOFTWARE, magic, false);
  CHECK(!again.window_open);
  CHECK(again.close_now);
}

static void test_keyed_api_is_never_closed() {
  // A chamber Home Assistant already adopted: closing the window would refuse HA itself (ESPHome rejects every
  // client once closed), so the key holder must never be locked out by a software restart.
  for (Reset r : {Reset::POWER_ON, Reset::SOFTWARE, Reset::OTHER}) {
    uint32_t magic = 0;
    CHECK(!decide(r, magic, true).close_now);
  }
}

static void test_camera() {
  bool asked = false;
  Decision d = decide_camera(asked, false);   // ordinary boot, incl. a Camera Power Cycle (a real power-on)
  CHECK(!d.window_open);
  CHECK(d.close_now);
  asked = true;   // the chamber sent open_api, the camera saved it and restarted
  d = decide_camera(asked, false);
  CHECK(d.window_open);
  CHECK(!d.close_now);
  CHECK(!asked);   // one boot only
  d = decide_camera(asked, false);
  CHECK(d.close_now);
  bool none = false;
  CHECK(!decide_camera(none, true).close_now);   // adopted camera: never closed
  CHECK(camera_should_restart_for_window(false));
  CHECK(!camera_should_restart_for_window(true));   // has a key: open_api doesn't restart it
}

int main() {
  test_power_on_opens();
  test_software_restart_closes();
  test_other_resets_close();
  test_open_adoption_window_once();
  test_keyed_api_is_never_closed();
  test_camera();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("adopt_window: all checks passed\n");
  return 0;
}
