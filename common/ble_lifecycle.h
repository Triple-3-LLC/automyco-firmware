// When the public build's Bluetooth stack runs, and when its memory goes back to the heap
// (terrashroom-public.yaml "Bluetooth only while Improv needs it", esphome/ble_release.h).
// Kept free of ESPHome and ESP-IDF so the host test can run it: tests/test_ble_lifecycle.cpp.
//
// Why. Bluetooth is only there for BLE Improv (Wi-Fi setup from a phone). Left running, Bluedroid and the
// controller held so much heap that public 1.2.0 sat at ~30 KB free (lowest 120 B) and no TLS handshake
// could start: update checks, recipes and remote access all failed (LOG.md 2026-10-11).
//
// The rule:
//   OFF       the stack has never been started this boot (esp32_ble enable_on_boot: false).
//             Improv asks for it (no saved network, or the saved one failed for 90 s, and the claim
//             window is still open)                                         -> ENABLE, stage ON.
//             Settled without ever needing it                               -> RELEASE, stage RELEASED.
//   ON        the stack is up for Improv. Settled                           -> DISABLE, stage STOPPING.
//   STOPPING  ESPHome tears Bluedroid and the controller down in its loop. Once the controller is idle
//                                                                           -> RELEASE, stage RELEASED.
//   RELEASED  the controller's reserved RAM is heap now. Bluetooth can't start again until the next
//             power-on, which is also the only time Improv can reopen (the claim window).
// "Settled" = Improv doesn't want the stack, and Wi-Fi is connected or the claim window has closed,
// continuously for SETTLE_MS. The wait lets Improv finish telling the phone it worked, and keeps a
// connection that drops straight away from costing Bluetooth for the rest of the boot.
#pragma once

#include <cstdint>

namespace ts_ble {

enum class Stage : uint8_t { OFF, ON, STOPPING, RELEASED };
enum class Action : uint8_t { NONE, ENABLE, DISABLE, RELEASE };

struct Inputs {
  bool improv_wanted;    // Improv is running or has been asked to start
  bool wifi_connected;
  bool window_closed;    // the provisioning (claim) window expired; Improv can't start again
  bool controller_idle;  // the BT controller is deinitialised and the BLE component is off
  uint32_t now_ms;       // millis()
};

static constexpr uint32_t SETTLE_MS = 30000;

class Lifecycle {
 public:
  Stage stage() const { return this->stage_; }

  Action step(const Inputs &in) {
    const bool settled_now = !in.improv_wanted && (in.wifi_connected || in.window_closed);
    if (!settled_now) {
      this->settling_ = false;
    } else if (!this->settling_) {
      this->settling_ = true;
      this->settled_since_ = in.now_ms;
    }
    const bool settled = this->settling_ && (uint32_t) (in.now_ms - this->settled_since_) >= SETTLE_MS;

    switch (this->stage_) {
      case Stage::OFF:
        if (in.improv_wanted && !in.window_closed) {
          this->stage_ = Stage::ON;
          return Action::ENABLE;
        }
        if (settled) {
          this->stage_ = Stage::RELEASED;  // never started, so the controller is already idle
          return Action::RELEASE;
        }
        return Action::NONE;
      case Stage::ON:
        if (settled) {
          this->stage_ = Stage::STOPPING;
          return Action::DISABLE;
        }
        return Action::NONE;
      case Stage::STOPPING:
        if (in.controller_idle) {
          this->stage_ = Stage::RELEASED;
          return Action::RELEASE;
        }
        return Action::NONE;
      case Stage::RELEASED:
        break;
    }
    return Action::NONE;
  }

 private:
  Stage stage_{Stage::OFF};
  bool settling_{false};
  uint32_t settled_since_{0};
};

}  // namespace ts_ble
