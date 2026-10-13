#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "update_logic.h"

namespace esphome {
namespace camera_http {
class CameraHttpServer;
}
namespace chamber_link {
class ChamberLink;
}
namespace camera_update {

// The AutoMyco camera updating itself to a published release, asked for by the main board over the
// camera link ({"ota":...}) or by the iOS app (POST :8080/update, behind the camera's login). Rules in
// update_logic.h; the flow in docs/camera-install.md "Updating an AutoMyco camera".
//
// Main loop: takes a request, pauses the stream and frame uploads (on_begin), waits for Wi-Fi and heap,
// then starts a FreeRTOS task that downloads https://<host>/fw/<sha256>.bin, hashes it while writing the
// inactive app slot (ESPHome's own IDF OTA backend), and only switches the boot slot when the SHA-256
// matches. The main loop reports progress on the link and reboots into the new image.
class CameraUpdate : public Component {
 public:
  void set_version(const std::string &version) { this->running_ = version; }
  void set_host(const std::string &host) { this->host_ = host; }
  void set_chamber_link(chamber_link::ChamberLink *link) { this->link_ = link; }
  void set_camera_http(camera_http::CameraHttpServer *http) { this->http_ = http; }
  void add_on_begin_callback(std::function<void()> &&cb) { this->begin_callbacks_.add(std::move(cb)); }
  void add_on_abort_callback(std::function<void()> &&cb) { this->abort_callbacks_.add(std::move(cb)); }

  // Thread-safe. `version` may carry the automyco-camera/ prefix. Returns the verdict; OK = queued.
  camera_update_logic::Verdict request(const std::string &version, const std::string &hash, bool force);
  bool is_updating() const { return this->busy_.load(); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Before camera_http (LATE), which registers our routes in its setup.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  using State = camera_update_logic::State;

  static void download_task_(void *arg);
  bool download_once_(int *http_status, bool *network_error);
  void set_state_(State s, const std::string &error = "");
  void report_();
  void finish_failed_();
  std::string status_json_();

  std::string running_;
  std::string host_{"api.automyco.com"};
  chamber_link::ChamberLink *link_{nullptr};
  camera_http::CameraHttpServer *http_{nullptr};
  CallbackManager<void()> begin_callbacks_;
  CallbackManager<void()> abort_callbacks_;

  std::mutex lock_;  // guards target_/hash_/error_/pending_ (httpd task, download task, main loop)
  std::string target_;
  std::string hash_;
  bool force_{false};  // only ever true from POST /update (the camera login)
  std::string error_;
  bool pending_{false};
  std::atomic<bool> busy_{false};
  std::atomic<State> state_{State::IDLE};
  std::atomic<int> progress_{0};

  // Main loop only.
  uint32_t wait_started_{0};
  bool task_started_{false};
  State reported_state_{State::IDLE};
  int reported_progress_{-1};
};

class BeginTrigger : public Trigger<> {
 public:
  explicit BeginTrigger(CameraUpdate *parent) {
    parent->add_on_begin_callback([this]() { this->trigger(); });
  }
};

class AbortTrigger : public Trigger<> {
 public:
  explicit AbortTrigger(CameraUpdate *parent) {
    parent->add_on_abort_callback([this]() { this->trigger(); });
  }
};

}  // namespace camera_update
}  // namespace esphome
