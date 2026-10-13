#pragma once
// Public build only (terrashroom-public.yaml): run Bluetooth only while BLE Improv needs it, then give its
// RAM back to the heap. The decisions are in common/ble_lifecycle.h (host-tested); this is the ESPHome and
// ESP-IDF side. Called every second from an interval.
//
// Memory, ESP32 classic, BLE-only Bluedroid, ESP-IDF 5.5 (numbers from the 1.2.2 build's linker map and
// components/bt/include/esp32/include/esp_bt.h):
//   - Bluedroid + controller at runtime: their tasks, queues and control blocks on the heap. Never
//     allocated on a chamber that already has Wi-Fi saved; freed by ESPHome's teardown otherwise.
//   - esp_bt_controller_mem_release(ESP_BT_MODE_BTDM): the controller's reserved DRAM. ~41 KB when the
//     stack never started (6.2 KB ROM data + 25.5 KB exchange memory in one piece + 6.7 KB ROM bss + the
//     controller library's own 2.9 KB), ~26 KB after it ran (ESPHome already handed back the Classic BT
//     part when it started the stack).
// The host's own .bss/.data (esp_bt_mem_release) stays reserved: ESPHome's dump_config still asks
// Bluedroid for the BLE MAC, which reads those statics.
//
// A build that adds the Bluetooth proxy (common/bluetooth-proxy.yaml) needs the stack all the time, so it
// is simply started at boot and left alone.
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_improv/esp32_improv_component.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_PROVISIONING
#include "esphome/components/provisioning/provisioning.h"
#endif

#include "ble_lifecycle.h"

namespace ts_ble {

inline Lifecycle g_lifecycle;
inline bool g_keep = false;          // the proxy build: started once, never touched again
inline bool g_late_warned = false;   // "needs a power cycle" logged once

inline void tick() {
  using namespace esphome;
  auto *ble = esp32_ble::global_ble;
  auto *improv = esp32_improv::global_improv_component;
  if (ble == nullptr || improv == nullptr || ble->is_failed() || g_keep) return;
#ifdef USE_ESP32_BLE_TRACKER
  ble->enable();
  g_keep = true;
  ESP_LOGI("ble", "Bluetooth stays on (a BLE scanner or proxy is part of this build)");
#else

  Inputs in{};
  in.improv_wanted = improv->should_start() || improv->is_active();
  in.wifi_connected = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#ifdef USE_PROVISIONING
  in.window_closed = provisioning::global_provisioning_manager != nullptr &&
                     provisioning::global_provisioning_manager->closed();
#endif
  in.controller_idle = !ble->is_active() && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
  in.now_ms = millis();

  switch (g_lifecycle.step(in)) {
    case Action::ENABLE:
      ESP_LOGI("ble", "Starting Bluetooth for BLE Improv (Wi-Fi setup; the rear button authorises it)");
      ble->enable();
      break;
    case Action::DISABLE:
      ESP_LOGI("ble", "Wi-Fi is set up: stopping Bluetooth (free heap %u B)",
               (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
      ble->disable();   // ESPHome deinitialises Bluedroid and the controller in its next loop
      break;
    case Action::RELEASE: {
      const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      const esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
      const size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      if (err != ESP_OK) {
        ESP_LOGW("ble", "Bluetooth memory release failed: %s", esp_err_to_name(err));
      } else {
        ESP_LOGI("ble", "Bluetooth off until the next power-on; %u B of its RAM returned (free %u B, largest block %u B)",
                 (unsigned) (after - before), (unsigned) after,
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      }
      break;
    }
    case Action::NONE:
      break;
  }

  if (g_lifecycle.stage() == Stage::RELEASED && in.improv_wanted && !g_late_warned) {
    g_late_warned = true;
    ESP_LOGW("ble", "Wi-Fi lost: BLE Improv needs a power cycle (Bluetooth was released after setup). "
                    "Serial Improv still works.");
  }
#endif  // USE_ESP32_BLE_TRACKER
}

}  // namespace ts_ble
