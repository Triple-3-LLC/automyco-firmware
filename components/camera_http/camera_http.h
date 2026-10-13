#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esphome/components/camera/camera.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace camera_http {

// Authenticated MJPEG stream and snapshot server. Frames arrive on the main loop (CameraListener)
// and are handed to the httpd task through a mutex-protected shared_ptr plus a "frame ready"
// semaphore, the same pattern as ESPHome's own camera web server. A frame is only kept while a
// handler is waiting for one (waiters_), so an unclaimed frame never pins the camera's buffer.
//
// The camera's request_image/start_stream/stop_stream are not thread-safe against its own loop (it
// clears single_requesters_ there), so handlers only set flags and loop() applies them.
//
// Credentials come from the config (personal build) or, in claim mode (public build), from a one-time
// POST /claim inside a window after boot. They are saved in flash and also applied to the REST/SSE
// web_server, so one login covers the whole camera.
class CameraHttpServer : public Component, public camera::CameraListener {
 public:
  void set_camera(camera::Camera *camera) { this->camera_ = camera; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_credentials(const std::string &username, const std::string &password);
  void set_claim_window(uint32_t ms) { this->claim_window_ms_ = ms; }
  // Public build: no claim while the camera's Wi-Fi is still this placeholder network (it hasn't had the
  // chamber's network yet, so whatever it joined isn't the owner's). A string literal; nullptr = no check.
  void set_placeholder_ssid(const char *ssid) {
    this->placeholder_ssid_ = ssid;
    this->on_placeholder_ = ssid != nullptr;   // until loop() has looked
  }
  void set_light(light::LightState *light, switch_::Switch *enable) {
    this->light_ = light;
    this->light_enable_ = enable;
  }

  // Main loop only. Another component (frame_uploader's lit timelapse frames) holds the LED on for a
  // moment, fading up over ramp_ms. camera_http stays the LED's only driver: on release the LED goes
  // straight back to what camera_http wants on its own (on while a lit stream/snapshot runs, else off).
  void hold_light(bool on, uint32_t ramp_ms = 0);
  // The LED is on right now (for any reason).
  bool light_is_on() const { return this->light_ != nullptr && this->light_->remote_values.is_on(); }
  bool has_light() const { return this->light_ != nullptr; }

  // Public build: forget the claimed login (the next boot opens a fresh claim window).
  void reset_claim();
  // Public build: apply the saved login, or a random lock while unclaimed. Runs from on_boot at
  // HARDWARE priority, before web_server or the network start, so the placeholder login compiled
  // into the public image is never live; setup() runs it again once the RNG is a real source.
  void load_login();
  bool is_claimed() const { return this->claimed_.load(); }

  // camera_update's routes, behind the same login: POST /update (body: JSON request; returns the HTTP
  // status line and fills the reply body) and GET /update (status JSON). Both run on the httpd task, so
  // the handlers must be thread-safe. Set before setup() (camera_update's setup runs earlier).
  using UpdatePost = std::function<const char *(const std::string &body, std::string &reply)>;
  using UpdateGet = std::function<std::string()>;
  void set_update_handlers(UpdatePost post, UpdateGet get) {
    this->update_post_ = std::move(post);
    this->update_get_ = std::move(get);
  }
  // While the camera updates itself: /stream ends, /snapshot answers 503, and no frames are requested,
  // so the TLS download has the heap and the CPU. Any thread.
  void set_paused(bool paused) {
    this->paused_ = paused;
    this->wake_loop_();
  }
  bool is_paused() const { return this->paused_.load(); }

  void setup() override;
  void loop() override;
  void on_shutdown() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override;

 protected:
  struct SavedLogin {
    char username[33];
    char password[65];
  };
  static constexpr size_t AUTH_HEADER_MAX = 160;  // "Basic " + base64(32 + 1 + 64 bytes) = 138, plus NUL

  static esp_err_t stream_handler_(httpd_req_t *req);
  static esp_err_t snapshot_handler_(httpd_req_t *req);
  static esp_err_t claim_get_handler_(httpd_req_t *req);
  static esp_err_t claim_post_handler_(httpd_req_t *req);
  static esp_err_t update_get_handler_(httpd_req_t *req);
  static esp_err_t update_post_handler_(httpd_req_t *req);
  bool authorized_(httpd_req_t *req);
  bool claim_open_() const;
  bool login_loaded_{false};
  bool light_wanted_() const;
  void update_light_();
  void apply_login_(const std::string &username, const std::string &password);
  // httpd task side of the frame hand-off. Callers hold a waiters_ count around request + wait.
  void drain_frames_();  // forget any frame delivered before this point
  std::shared_ptr<camera::CameraImage> wait_for_frame_(uint32_t timeout_ms);
  void request_image_() { this->image_wanted_ = true; this->wake_loop_(); }
  void wake_loop_();

  camera::Camera *camera_{nullptr};
  uint16_t port_{8080};
  uint32_t claim_window_ms_{0};  // 0 = credentials from the config, no claiming
  std::atomic<bool> claimed_{false};
  std::atomic<bool> claim_closed_{false};   // latched by loop() once the window has passed (millis() wrap-proof)
  std::atomic<bool> save_pending_{false};  // a claim arrived on the httpd task; saved from loop()
  const char *placeholder_ssid_{nullptr};
  std::atomic<bool> on_placeholder_{false};  // loop(): the Wi-Fi in use is still placeholder_ssid_
  // Open streams plus snapshots in progress (httpd task); the LED follows it from loop().
  std::atomic<int> active_{0};
  // Camera requests made on the httpd task, applied from loop() (see the class comment).
  std::atomic<int> stream_want_{0};          // open /stream handlers
  std::atomic<bool> image_wanted_{false};    // a snapshot asked for one frame
  bool stream_on_{false};                    // what loop() last told the camera
  light::LightState *light_{nullptr};
  switch_::Switch *light_enable_{nullptr};
  std::atomic<bool> light_hold_{false};  // hold_light(); read by light_wanted_() on the httpd task too
  uint32_t hold_ramp_ms_{0};
  ESPPreferenceObject pref_;
  // Two fixed login buffers, never reallocated: web_server_base keeps pointers to the active one, and
  // its httpd task may read it at any time. A new login is written into the inactive buffer and then
  // made active, so no reader ever sees a half-written (or momentarily empty) username.
  SavedLogin logins_[2]{};
  char expected_auth_[2][AUTH_HEADER_MAX]{};  // "Basic <base64(user:pass)>", one per login buffer
  std::atomic<uint8_t> active_login_{0};
  httpd_handle_t server_{nullptr};
  SemaphoreHandle_t lock_{nullptr};
  SemaphoreHandle_t ready_{nullptr};
  std::atomic<int> waiters_{0};  // handlers waiting for a frame; frames are dropped when 0
  std::shared_ptr<camera::CameraImage> image_;
  std::atomic<bool> paused_{false};
  UpdatePost update_post_;
  UpdateGet update_get_;
};

}  // namespace camera_http
}  // namespace esphome
