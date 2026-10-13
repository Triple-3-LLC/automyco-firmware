#pragma once

#include <memory>
#include <string>

#include "camera_link_machine.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace camera_link {

using ::camera_install::InstallMachine;

// The main board's end of the camera link: reads lines, feeds them to the install state machine
// (camera_link_machine.h, host-tested), and keeps an AutoMyco camera on the chamber's Wi-Fi.
class CameraLink : public Component, public uart::UARTDevice {
 public:
  void set_camera_power(switch_::Switch *power) { this->power_ = power; }
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_sensor_ = s; }
  void set_firmware_sensor(text_sensor::TextSensor *s) { this->firmware_sensor_ = s; }
  void set_address_sensor(text_sensor::TextSensor *s) { this->address_sensor_ = s; }
  void set_reset_window(uint32_t ms) { this->reset_window_ms_ = ms; }
  void set_log_lines(bool log_lines) { this->log_lines_ = log_lines; }
  void set_passive(bool passive) { this->passive_ = passive; }

  // "<version> <sha256>" from the Camera Install text entity.
  void start_install(const std::string &request);
  // Camera Reset Login button.
  void reset_login();
  // The rear-button Pair window (public build): Camera Reset Login is also accepted for `ms` from now,
  // besides the window after power-on. Nothing calls it in the personal build.
  void open_pair_window(uint32_t ms) { this->pair_until_ms_ = (millis() + ms) | 1; }
  // "Open Adoption Window" (public build): asks an AutoMyco camera to restart once with its own Home Assistant
  // provisioning window open (camera-public.yaml, {"cmd":"open_api"}). False, and nothing sent, unless an
  // AutoMyco camera is up and no install is running. Stock cameras never get it.
  bool open_camera_api();
  // True while a camera firmware install is running (other packages keep off the link then).
  bool installing() const { return this->machine_ && this->machine_->installing(); }
  // Camera Power Cycle button: camera power off 10 s, then on (a soft camera reboot can't un-wedge the
  // sensor). The camera's own frame watchdog asks for the same, with a backoff.
  void power_cycle(const std::string &reason);
  // The chamber's self-test: hold off power cycles (automatic and the button) while it measures the
  // 24 V rail, and release them afterwards. A dropped watchdog request is asked again by the camera.
  void set_defer_power_cycles(bool defer) {
    if (this->machine_)
      this->machine_->defer_power_cycles(defer);
  }
  // The camera has sent its ready line since it was last powered on (an AutoMyco camera at boot, or in
  // answer to the status query). False while it is off or still booting. The boot self-test waits for
  // this, because a booting camera draws a changing current from the rail the self-test measures.
  bool camera_ready() const { return this->ready_ && this->power_ != nullptr && this->power_->state; }
  void add_on_event_callback(std::function<void(const std::string &)> &&callback) {
    this->event_callbacks_.add(std::move(callback));
  }
  void add_on_line_callback(std::function<void(const std::string &)> &&callback) {
    this->line_callbacks_.add(std::move(callback));
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  void send_line_(const std::string &line);
  void log_line_(const char *direction, const std::string &line);
  bool send_wifi_();
  static void publish_(text_sensor::TextSensor *sensor, const std::string &value);

  switch_::Switch *power_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};
  text_sensor::TextSensor *firmware_sensor_{nullptr};
  text_sensor::TextSensor *address_sensor_{nullptr};
  uint32_t reset_window_ms_{30 * 60 * 1000};
  bool reset_window_closed_{false};  // latched in loop(): the window never reopens when millis() wraps
  uint32_t pair_until_ms_{0};        // millis() when the Pair window closes; 0 = closed (cleared in loop())
  bool log_lines_{false};
  bool passive_{false};
  bool ready_{false};

  std::unique_ptr<InstallMachine> machine_;
  CallbackManager<void(const std::string &)> line_callbacks_;
  CallbackManager<void(const std::string &)> event_callbacks_;
  std::string rx_;
  bool overflow_{false};
};

class LineTrigger : public Trigger<std::string> {
 public:
  explicit LineTrigger(CameraLink *parent) {
    parent->add_on_line_callback([this](const std::string &line) { this->trigger(line); });
  }
};

class EventTrigger : public Trigger<std::string> {
 public:
  explicit EventTrigger(CameraLink *parent) {
    parent->add_on_event_callback([this](const std::string &message) { this->trigger(message); });
  }
};

}  // namespace camera_link
}  // namespace esphome
