#include "chamber_link.h"

#include <algorithm>

#include "esphome/components/camera_http/camera_http.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace chamber_link {

static const char *const TAG = "chamber_link";
static const size_t MAX_LINE = 320;  // longest SSID (32) + password (64) JSON-escaped fits easily

void ChamberLink::setup() {
  this->uart_num_ = logger::global_logger->get_uart_num();
  if (this->camera_ != nullptr)
    this->camera_->add_listener(this);
  this->last_frame_ = millis();
  this->send_ready_();
}

// No valid frame for watchdog_ms_ (the camera's idle frames normally arrive every ~10 s): the OV2640 is
// wedged ("Got invalid frame from camera!") and stays that way through a soft reboot. Ask the chamber to
// cut the camera's power. At most every 5 min from here; the chamber backs off further (5, 15, 60 min).
void ChamberLink::set_updating(bool updating) {
  this->updating_ = updating;
  this->last_frame_ = millis();  // the watchdog starts afresh if the update ends without a reboot
}

void ChamberLink::check_watchdog_() {
  if (this->camera_ == nullptr || this->updating_)
    return;
  static const uint32_t REQUEST_GAP_MS = 5 * 60 * 1000;
  const uint32_t now = millis();
  const bool stalled = this->camera_->is_failed() || now - this->last_frame_ > this->watchdog_ms_;
  if (!stalled || now < this->watchdog_ms_)
    return;
  if (this->last_request_ != 0 && now - this->last_request_ < REQUEST_GAP_MS)
    return;
  this->last_request_ = now;
  const unsigned secs = (now - this->last_frame_) / 1000;
  const std::string reason = this->camera_->is_failed() ? std::string("camera sensor failed to start")
                                                        : "no valid camera frame for " + std::to_string(secs) + " s";
  ESP_LOGW(TAG, "Asking the chamber for a power cycle: %s", reason.c_str());
  this->send_line_("{\"status\":\"power_cycle\",\"reason\":\"" + reason + "\"}");
}

void ChamberLink::dump_config() {
  ESP_LOGCONFIG(TAG, "Chamber link (UART%d): automyco-camera/%s", (int) this->uart_num_, this->version_.c_str());
}

void ChamberLink::send_line_(const std::string &line) {
  // A leading newline ends any log line the logger was in the middle of, so the main board sees the
  // JSON at the start of a line.
  const std::string out = "\n" + line + "\n";
  uart_write_bytes(this->uart_num_, out.data(), out.size());
}

void ChamberLink::send_ready_() {
  this->send_line_("{\"status\":\"ready\",\"version\":\"automyco-camera/" + this->version_ + "\"}");
}

void ChamberLink::loop() {
  size_t available = 0;
  uart_get_buffered_data_len(this->uart_num_, &available);
  uint8_t buf[64];
  while (available > 0) {
    int n = uart_read_bytes(this->uart_num_, buf, std::min(available, sizeof(buf)), 0);
    if (n <= 0)
      break;
    available -= n;
    for (int i = 0; i < n; i++) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n') {
        if (!this->overflow_)
          this->handle_line_();
        this->rx_.clear();
        this->overflow_ = false;
      } else if (c != '\r') {
        if (this->rx_.size() < MAX_LINE) {
          this->rx_.push_back(c);
        } else {
          this->overflow_ = true;
        }
      }
    }
  }

  this->check_watchdog_();

  const bool connected = wifi::global_wifi_component->is_connected();
  if (connected && !this->was_connected_) {
    for (auto &ip : wifi::global_wifi_component->get_ip_addresses()) {
      if (ip.is_ip4()) {
        char addr[network::IP_ADDRESS_BUFFER_SIZE];
        ip.str_to(addr);
        this->send_line_(std::string("{\"status\":\"online\",\"ip\":\"") + addr + "\"}");
        break;
      }
    }
  }
  this->was_connected_ = connected;
}

void ChamberLink::handle_line_() {
  const size_t start = this->rx_.find('{');
  if (start == std::string::npos)
    return;
  std::string ssid, password, cmd, r_url, r_id, r_key, ota_version, ota_hash;
  bool has_wifi = false, r_off = false, has_ota = false, ota_force = false;
  json::parse_json(this->rx_.substr(start), [&](JsonObject root) -> bool {
    if (root["ssid"].is<const char *>()) {
      has_wifi = true;
      ssid = root["ssid"].as<const char *>();
      password = root["password"] | "";
    }
    if (root["ota"].is<JsonObject>()) {
      has_ota = true;
      ota_version = root["ota"]["version"] | "";
      ota_hash = root["ota"]["hash"] | "";
      ota_force = root["ota"]["force"] | false;
    }
    cmd = root["cmd"] | "";
    if (cmd == "remote") {
      r_off = root["off"] | false;
      r_url = root["url"] | "";
      r_id = root["id"] | "";
      r_key = root["key"] | "";
    }
    return true;
  });
  // The line held the Wi-Fi password: don't leave it in RAM longer than needed.
  std::fill(this->rx_.begin(), this->rx_.end(), '\0');

  if (has_wifi) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
      ESP_LOGW(TAG, "Ignored an invalid Wi-Fi network from the chamber");
      return;
    }
    const auto current = wifi::global_wifi_component->get_sta();
    if (ssid == current.get_ssid().c_str() && password == current.get_password().c_str()) {
      ESP_LOGD(TAG, "Chamber sent the Wi-Fi network already in use");
      return;
    }
    ESP_LOGI(TAG, "Wi-Fi network received from the chamber; connecting");  // values never logged
    wifi::global_wifi_component->save_wifi_sta(ssid, password);
    // Connected to something else (the placeholder network, say, broadcast by someone nearby, or the old
    // router): the chamber's network wins now, not at the next disconnect. save_wifi_sta() alone only acts
    // when Wi-Fi is idle. Same call Improv serial makes for a device that is already online.
    if (wifi::global_wifi_component->is_connected())
      wifi::global_wifi_component->start_connecting(wifi::global_wifi_component->get_sta());
    return;
  }
  if (has_ota) {
    if (this->ota_handler_) {
      this->ota_handler_(ota_version, ota_hash, ota_force);
    } else {
      ESP_LOGW(TAG, "Chamber asked for an update, but this build can't update itself over the link");
    }
    return;
  }
  if (cmd == "status") {
    this->send_ready_();
    if (wifi::global_wifi_component->is_connected())
      this->was_connected_ = false;  // re-announce the address on the next loop
  } else if (cmd == "remote") {
    // Frame uploads in the chamber's name (docs/remote-access.md "Camera"). The key is never logged.
    auto hex = [](const std::string &s, size_t n) {
      return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    const bool url_ok = r_url.rfind("https://", 0) == 0 && r_url.size() <= 96 && r_url.find_first_of(" \"\\?#") == std::string::npos;
    if (r_off) {
      ESP_LOGI(TAG, "Chamber: remote frame uploads off");
      this->remote_callbacks_.call("", "", "");
    } else if (url_ok && hex(r_id, 32) && hex(r_key, 64)) {
      ESP_LOGI(TAG, "Chamber: remote frame uploads on (%s)", r_url.c_str());
      this->remote_callbacks_.call(r_url, r_id, r_key);
    } else {
      ESP_LOGW(TAG, "Ignored invalid remote frame settings from the chamber");
    }
    std::fill(r_key.begin(), r_key.end(), '\0');
  } else if (cmd == "reset_login") {
    if (this->camera_http_ != nullptr) {
      this->camera_http_->reset_claim();
      ESP_LOGW(TAG, "Login reset by the chamber; restarting");
      App.safe_reboot();
    }
  } else if (!cmd.empty() && cmd.size() <= 24) {
    this->command_callbacks_.call(cmd);   // e.g. open_api (camera-public.yaml on_command)
  }
}

}  // namespace chamber_link
}  // namespace esphome
