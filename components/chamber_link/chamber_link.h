#pragma once

#include <string>

#include <driver/uart.h>

#include "esphome/core/hal.h"

#include "esphome/components/camera/camera.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace camera_http {
class CameraHttpServer;
}
namespace chamber_link {

class ChamberLink : public Component, public camera::CameraListener {
 public:
  void set_camera(camera::Camera *camera) { this->camera_ = camera; }
  void set_watchdog_timeout(uint32_t ms) { this->watchdog_ms_ = ms; }
  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override { this->last_frame_ = millis(); }
  void set_version(const std::string &version) { this->version_ = version; }
  void set_camera_http(camera_http::CameraHttpServer *http) { this->camera_http_ = http; }
  // {"cmd":"remote",...} from the chamber: frame-upload credentials (url, chamber device ID, camera key),
  // or all three empty for "stop". RAM only; the chamber sends them again whenever the camera says ready.
  void add_on_remote_callback(std::function<void(const std::string &, const std::string &, const std::string &)> &&cb) {
    this->remote_callbacks_.add(std::move(cb));
  }
  // {"cmd":"<other>"} from the chamber that this component doesn't handle itself (open_api: camera-public.yaml
  // on_command). Only the chamber is on this wire.
  void add_on_command_callback(std::function<void(const std::string &)> &&cb) {
    this->command_callbacks_.add(std::move(cb));
  }
  // {"ota":{"version","hash"[,"force"]}} from the chamber (the stock camera's update command): handled by
  // camera_update, which validates it and ignores force from the link. version is as sent
  // ("automyco-camera/<v>").
  void set_ota_handler(std::function<void(const std::string &, const std::string &, bool)> &&handler) {
    this->ota_handler_ = std::move(handler);
  }
  // While the camera updates itself: no frame-watchdog power-cycle requests (the chamber cutting power
  // mid-write would only waste the download).
  void set_updating(bool updating);
  // One line to the chamber (camera_update's progress lines).
  void send_line(const std::string &line) { this->send_line_(line); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the logger (which installs the UART0 driver) and Wi-Fi.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  void handle_line_();
  void send_line_(const std::string &line);
  void send_ready_();
  void check_watchdog_();

  std::string version_;
  camera_http::CameraHttpServer *camera_http_{nullptr};
  uart_port_t uart_num_{UART_NUM_0};
  std::string rx_;
  bool overflow_{false};
  bool was_connected_{false};
  camera::Camera *camera_{nullptr};
  uint32_t watchdog_ms_{60000};
  uint32_t last_frame_{0};
  uint32_t last_request_{0};
  CallbackManager<void(const std::string &, const std::string &, const std::string &)> remote_callbacks_;
  CallbackManager<void(const std::string &)> command_callbacks_;
  std::function<void(const std::string &, const std::string &, bool)> ota_handler_;
  bool updating_{false};
};

class CommandTrigger : public Trigger<std::string> {
 public:
  explicit CommandTrigger(ChamberLink *parent) {
    parent->add_on_command_callback([this](const std::string &cmd) { this->trigger(cmd); });
  }
};

class RemoteTrigger : public Trigger<std::string, std::string, std::string> {
 public:
  explicit RemoteTrigger(ChamberLink *parent) {
    parent->add_on_remote_callback(
        [this](const std::string &url, const std::string &id, const std::string &key) { this->trigger(url, id, key); });
  }
};

}  // namespace chamber_link
}  // namespace esphome
