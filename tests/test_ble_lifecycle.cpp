// Host tests for esphome/common/ble_lifecycle.h (when the public build runs Bluetooth for BLE Improv, and
// when it gives the stack's RAM back).
//   c++ -std=c++17 -Wall -Wextra -Werror -I esphome/common esphome/tests/test_ble_lifecycle.cpp -o /tmp/tbl && /tmp/tbl
#include "ble_lifecycle.h"

#include <cstdio>

using namespace ts_ble;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

// The chamber, one tick a second like the interval in terrashroom-public.yaml.
struct Sim {
  Lifecycle lc;
  Inputs in{false, false, false, true, 1000};
  bool ble_on = false;   // what ESPHome's esp32_ble would be doing
  int enables = 0, disables = 0, releases = 0;

  Action tick() {
    in.controller_idle = !ble_on;   // ESPHome tears the stack down within a loop of disable()
    const Action a = lc.step(in);
    switch (a) {
      case Action::ENABLE: enables++; ble_on = true; break;
      case Action::DISABLE: disables++; ble_on = false; break;
      case Action::RELEASE: releases++; CHECK(!ble_on); break;
      case Action::NONE: break;
    }
    in.now_ms += 1000;
    return a;
  }
  void run(int seconds) {
    for (int i = 0; i < seconds; i++) tick();
  }
};

// A chamber that already has Wi-Fi: Bluetooth never starts; its RAM is released 30 s after Wi-Fi connects.
static void test_saved_wifi_never_starts_ble() {
  std::printf("saved Wi-Fi: Bluetooth never starts\n");
  Sim s;
  s.run(5);   // connecting
  CHECK(s.lc.stage() == Stage::OFF);
  s.in.wifi_connected = true;
  s.run(29);
  CHECK(s.releases == 0);
  s.run(2);
  CHECK(s.enables == 0);
  CHECK(s.disables == 0);
  CHECK(s.releases == 1);
  CHECK(s.lc.stage() == Stage::RELEASED);
  s.run(3600);
  CHECK(s.releases == 1);
}

// First boot, no network: Improv wants Bluetooth straight away; it goes once the phone has set Wi-Fi up.
static void test_unprovisioned_improv_then_release() {
  std::printf("no Wi-Fi saved: Bluetooth for Improv, released after setup\n");
  Sim s;
  s.in.improv_wanted = true;
  CHECK(s.tick() == Action::ENABLE);
  CHECK(s.ble_on);
  s.run(300);   // the owner finds the app, presses the rear button
  CHECK(s.enables == 1);
  CHECK(s.ble_on);
  s.in.wifi_connected = true;   // Improv sent the network; Improv still telling the phone (10 s)
  s.run(10);
  CHECK(s.ble_on);
  s.in.improv_wanted = false;   // Improv stopped
  s.run(29);
  CHECK(s.ble_on);
  s.run(2);
  CHECK(!s.ble_on);
  CHECK(s.disables == 1);
  CHECK(s.releases == 0);   // the teardown runs in ESPHome's loop; released on the next tick
  s.run(1);
  CHECK(s.releases == 1);
  CHECK(s.lc.stage() == Stage::RELEASED);
}

// The saved network is gone (new router): Wi-Fi fails for 90 s, Improv asks, Bluetooth starts.
static void test_new_router_improv_comes_back() {
  std::printf("saved network unreachable: Improv after 90 s\n");
  Sim s;
  s.run(90);
  CHECK(s.enables == 0);
  CHECK(s.releases == 0);   // never connected: nothing released yet
  s.in.improv_wanted = true;
  CHECK(s.tick() == Action::ENABLE);
  s.in.wifi_connected = true;
  s.in.improv_wanted = false;
  s.run(32);
  CHECK(s.disables == 1);
  CHECK(s.releases == 1);
}

// A connection that drops before it settled doesn't cost Bluetooth for the rest of the boot.
static void test_flapping_wifi_keeps_ble_available() {
  std::printf("Wi-Fi that drops within 30 s: nothing released\n");
  Sim s;
  for (int i = 0; i < 5; i++) {
    s.in.wifi_connected = true;
    s.run(20);
    s.in.wifi_connected = false;
    s.run(5);
  }
  CHECK(s.releases == 0);
  CHECK(s.lc.stage() == Stage::OFF);
  s.in.improv_wanted = true;
  CHECK(s.tick() == Action::ENABLE);
}

// Claim window closed with no network: Improv has stopped for good, so Bluetooth goes too.
static void test_window_closed_releases() {
  std::printf("claim window closed, never connected: released\n");
  Sim s;
  s.in.improv_wanted = true;
  s.tick();
  CHECK(s.ble_on);
  s.run(1800);
  s.in.window_closed = true;    // ESPHome stops Improv (10 s later it reports stopped)
  s.run(10);
  CHECK(s.ble_on);
  s.in.improv_wanted = false;
  s.run(32);
  CHECK(!s.ble_on);
  CHECK(s.releases == 1);
}

// Closed window, never started: a late Improv request (Wi-Fi lost) must not start Bluetooth.
static void test_no_start_after_window_closed() {
  std::printf("claim window closed: Bluetooth never starts\n");
  Sim s;
  s.in.window_closed = true;
  s.in.improv_wanted = true;
  s.run(5);
  CHECK(s.enables == 0);
}

// After the release, nothing ever starts the stack again this boot (its RAM is the heap's now).
static void test_released_is_final() {
  std::printf("released: final until power-on\n");
  Sim s;
  s.in.wifi_connected = true;
  s.run(31);
  CHECK(s.lc.stage() == Stage::RELEASED);
  s.in.wifi_connected = false;
  s.in.improv_wanted = true;    // Wi-Fi lost for 90 s inside the window
  s.run(600);
  CHECK(s.enables == 0);
  CHECK(s.releases == 1);
}

// The teardown is asynchronous: no release until the controller reports idle.
static void test_release_waits_for_idle() {
  std::printf("release waits for the controller to go idle\n");
  Lifecycle lc;
  Inputs in{true, false, false, true, 1000};
  CHECK(lc.step(in) == Action::ENABLE);
  in.improv_wanted = false;
  in.wifi_connected = true;
  in.controller_idle = false;
  Action a = Action::NONE;
  for (int i = 0; i < 31 && a == Action::NONE; i++) {
    in.now_ms += 1000;
    a = lc.step(in);
  }
  CHECK(a == Action::DISABLE);
  for (int i = 0; i < 5; i++) {
    in.now_ms += 1000;
    CHECK(lc.step(in) == Action::NONE);   // ESPHome hasn't torn it down yet
  }
  in.controller_idle = true;
  in.now_ms += 1000;
  CHECK(lc.step(in) == Action::RELEASE);
  in.now_ms += 1000;
  CHECK(lc.step(in) == Action::NONE);
}

// millis() wraps after ~49.7 days; the settle timer must not care.
static void test_millis_wrap() {
  std::printf("millis() wrap\n");
  Sim s;
  s.in.now_ms = 0xFFFFFFFFu - 10000;
  s.in.wifi_connected = true;
  s.run(29);
  CHECK(s.releases == 0);
  s.run(2);
  CHECK(s.releases == 1);
}

int main() {
  test_saved_wifi_never_starts_ble();
  test_unprovisioned_improv_then_release();
  test_new_router_improv_comes_back();
  test_flapping_wifi_keeps_ble_available();
  test_window_closed_releases();
  test_no_start_after_window_closed();
  test_released_is_final();
  test_release_waits_for_idle();
  test_millis_wrap();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
