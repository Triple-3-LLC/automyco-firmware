#pragma once
// When the public build's provisioning window (Home Assistant adoption: the native API with no key, BLE
// Improv) may open: terrashroom-public.yaml "provisioning". ESPHome 2026.9 opens it at every boot and keeps
// its closed state in RAM only, so any software restart (the Restart button, an OTA update, a crash) would
// reopen it for 30 minutes, and a keyless API accepts the all-zero key and lets anyone on the LAN set one.
// So it opens only after:
//   - a real power-on (ESP_RST_POWERON: the power was cut, or the EN pin was pulsed by a USB flasher), or
//   - one software restart asked for by "Open Adoption Window" inside the rear-button Pair window. That
//     button leaves a magic word in RTC memory (kept through a software restart, lost on power loss), and
//     the next boot takes it, once.
// Everything else (software restart, OTA, watchdog, panic, brownout) boots with the window closed, unless
// the API already has its key: ESPHome then counts the API as provisioned, and closing the window would
// only lock Home Assistant out (a closed window refuses every API client, keyed or not, until power-off).
//
// decide() is pure (esphome/tests/test_adopt_window.cpp fakes POWERON / SW / panic resets on the host).
#include <cstdint>

#ifndef TS_ADOPT_HOST_TEST
#include <esp_attr.h>
#include <esp_system.h>
#endif

namespace ts_adopt {

inline constexpr uint32_t MAGIC = 0x41444f50;   // "ADOP"

enum class Reset : uint8_t { POWER_ON, SOFTWARE, OTHER };   // OTHER: watchdog, panic, brownout, deep sleep...

struct Decision {
  bool window_open;   // leave ESPHome's window as configured (open for provisioning_window)
  bool close_now;     // set its timeout to 1 ms: it closes on the first loop
  bool adopt_boot;    // this boot came from Open Adoption Window
};

// `magic` is the RTC word; it is always cleared, so a request is good for one boot only.
inline Decision decide(Reset reset, uint32_t &magic, bool api_has_key) {
  const bool asked = reset == Reset::SOFTWARE && magic == MAGIC;
  magic = 0;
  Decision d{};
  d.window_open = reset == Reset::POWER_ON || asked;
  d.adopt_boot = asked;
  d.close_now = !d.window_open && !api_has_key;
  return d;
}

// The AutoMyco camera (camera-public.yaml): its reset reason proves nothing (the chamber's LAN-reachable
// Camera Power Cycle is a real power-on to it), so its window opens only on the boot after the chamber sent
// {"cmd":"open_api"}, which the camera saved as `asked` (a restored flag). Always cleared: one boot only.
inline Decision decide_camera(bool &asked, bool api_has_key) {
  Decision d{};
  d.window_open = asked;
  d.adopt_boot = asked;
  d.close_now = !asked && !api_has_key;
  asked = false;
  return d;
}

// The camera, on {"cmd":"open_api"}: restart into a window only if it has no API key yet.
inline bool camera_should_restart_for_window(bool api_has_key) { return !api_has_key; }

#ifndef TS_ADOPT_HOST_TEST
// RTC slow memory, not zeroed by a software restart; garbage after power-on (hence the magic word).
static RTC_NOINIT_ATTR uint32_t g_adopt_magic;

// Called by the Open Adoption Window button just before it restarts the chamber.
inline void arm() { g_adopt_magic = MAGIC; }
inline bool armed() { return g_adopt_magic == MAGIC; }

inline Reset reset_kind(esp_reset_reason_t r) {
  return r == ESP_RST_POWERON ? Reset::POWER_ON : r == ESP_RST_SW ? Reset::SOFTWARE : Reset::OTHER;
}

// Once per boot, after the API has loaded its key.
inline Decision decide_now(bool api_has_key) { return decide(reset_kind(esp_reset_reason()), g_adopt_magic, api_has_key); }
#endif

}  // namespace ts_adopt
