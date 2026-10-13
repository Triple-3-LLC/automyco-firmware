#include "camera_link.h"

#include <algorithm>
#include <cctype>

#include "esphome/components/json/json_util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_system.h>

namespace esphome {
namespace camera_link {

static const char *const TAG = "camera_link";
static const size_t MAX_LINE = 512;

void CameraLink::publish_(text_sensor::TextSensor *sensor, const std::string &value) {
  if (sensor != nullptr && sensor->get_state() != value)
    sensor->publish_state(value);
}

void CameraLink::setup() {
  ::camera_install::Io io;
  io.power = [this](bool on) {
    if (!on)
      this->ready_ = false;  // it has to boot and say ready again
    on ? this->power_->turn_on() : this->power_->turn_off();
  };
  io.send = [this](const std::string &line) { this->send_line_(line); };
  io.wifi_connected = []() { return wifi::global_wifi_component->is_connected(); };
  io.send_wifi = [this]() { return this->send_wifi_(); };
  io.status = [this](const std::string &s) {
    ESP_LOGI(TAG, "Camera install: %s", s.c_str());
    publish_(this->status_sensor_, s);
  };
  io.firmware = [this](const std::string &v) {
    this->ready_ = true;  // only a ready line carries a version
    publish_(this->firmware_sensor_, v);
  };
  io.address = [this](const std::string &a) { publish_(this->address_sensor_, a); };
  io.event = [this](const std::string &e) {
    ESP_LOGW(TAG, "%s", e.c_str());
    this->event_callbacks_.call(e);
  };
  this->machine_ = std::make_unique<InstallMachine>(io);

  publish_(this->status_sensor_, "idle");
  if (this->passive_)
    return;
  // An AutoMyco camera answers with its ready line (and address); a stock camera ignores it.
  this->set_timeout("camera-hello", 10000, [this]() {
    if (this->power_->state)
      this->send_line_("{\"cmd\":\"status\"}");
  });
}

void CameraLink::dump_config() {
  ESP_LOGCONFIG(TAG, "Camera link (stock camera protocol)%s", this->passive_ ? ": passive" : "");
  this->check_uart_settings(115200);
}

void CameraLink::log_line_(const char *direction, const std::string &line) {
  if (!this->log_lines_)
    return;
  std::string lower(line);
  for (auto &c : lower)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (lower.find("ssid") != std::string::npos || lower.find("pass") != std::string::npos) {
    ESP_LOGD(TAG, "%s <line with Wi-Fi credentials redacted, %u bytes>", direction, (unsigned) line.size());
  } else {
    ESP_LOGD(TAG, "%s %s", direction, line.c_str());
  }
  std::fill(lower.begin(), lower.end(), '\0');
}

void CameraLink::send_line_(const std::string &line) {
  this->log_line_(">>>", line);
  this->write_str(line.c_str());
  this->write_byte('\n');
}

// The chamber's own network, in the stock camera's format. Never logged.
bool CameraLink::send_wifi_() {
  if (!wifi::global_wifi_component->is_connected())
    return false;
  const auto sta = wifi::global_wifi_component->get_sta();
  if (sta.get_ssid().empty())
    return false;
  std::string line = json::build_json([&](JsonObject root) {
    root["ssid"] = sta.get_ssid().c_str();
    root["password"] = sta.get_password().c_str();
  });
  this->send_line_(line);
  std::fill(line.begin(), line.end(), '\0');
  return true;
}

void CameraLink::start_install(const std::string &request) {
  if (this->passive_)
    return;
  this->machine_->start(request, millis());
}

// Works in passive mode too: it only switches power (the bridge's IO0 backup procedure uses it).
void CameraLink::power_cycle(const std::string &reason) {
  if (!this->machine_->power_cycle(reason, millis(), true))
    ESP_LOGW(TAG, "Camera power cycle skipped: an install or another power cycle is running");
}

void CameraLink::reset_login() {
  if (this->passive_)
    return;
  // Physical presence: the chamber was powered on (not restarted from the API or web UI, which anyone
  // on the LAN can do) within the window, or someone held the rear button 2-4 s (the Pair window) in the
  // last 2 minutes. Otherwise a LAN client could restart the chamber, reset the camera's login and claim
  // the camera. The camera reboots after the reset and opens its own claim window (camera_http).
  const bool after_power_on = esp_reset_reason() == ESP_RST_POWERON && !this->reset_window_closed_;
  const bool pair_open = this->pair_until_ms_ != 0 && (int32_t) (this->pair_until_ms_ - millis()) > 0;
  if (!after_power_on && !pair_open) {
    // Keep the "failed: power-cycle" prefix: the iOS app recognises the refusal by it (CameraPairing.swift).
    publish_(this->status_sensor_,
             "failed: power-cycle the chamber or hold the rear button 2-4 s, then reset the camera login");
    return;
  }
  ESP_LOGW(TAG, "Asking the camera to clear its login (%s)", pair_open ? "Pair window" : "after power-on");
  this->send_line_("{\"cmd\":\"reset_login\"}");
}

bool CameraLink::open_camera_api() {
  if (this->passive_ || this->installing() || !this->camera_ready() || this->firmware_sensor_ == nullptr ||
      this->firmware_sensor_->get_state().rfind("automyco-camera/", 0) != 0)
    return false;
  ESP_LOGW(TAG, "Asking the camera to restart with its adoption window open");
  this->send_line_("{\"cmd\":\"open_api\"}");
  return true;
}

void CameraLink::loop() {
  // Closed once, for good: `millis() > window` alone would reopen it 49.7 days after power-on.
  if (!this->reset_window_closed_ && millis() > this->reset_window_ms_)
    this->reset_window_closed_ = true;
  if (this->pair_until_ms_ != 0 && (int32_t) (this->pair_until_ms_ - millis()) <= 0)
    this->pair_until_ms_ = 0;  // closed for good, so it can't come back when millis() wraps
  if (this->power_ != nullptr && !this->power_->state && this->ready_) {
    this->ready_ = false;  // switched off by hand (Camera Power): it boots afresh when it comes back
    this->machine_->camera_off();
  }
  if (this->passive_) {
    this->machine_->tick(millis());  // a power cycle may be running; the UART is the bridge's
    return;
  }
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c))
      break;
    if (c == '\n') {
      if (!this->overflow_) {
        this->log_line_("<<<", this->rx_);
        this->machine_->line(this->rx_, millis());
        this->line_callbacks_.call(this->rx_);
      }
      std::fill(this->rx_.begin(), this->rx_.end(), '\0');  // the stock camera echoes the Wi-Fi line
      this->rx_.clear();
      this->overflow_ = false;
    } else if (c != '\r') {
      if (this->rx_.size() < MAX_LINE) {
        this->rx_.push_back(static_cast<char>(c));
      } else {
        this->overflow_ = true;
      }
    }
  }
  this->machine_->tick(millis());
}

}  // namespace camera_link
}  // namespace esphome
