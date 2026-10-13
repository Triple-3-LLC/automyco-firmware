#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "esphome/components/camera/camera.h"
#include "esphome/core/component.h"

#include "frame_light.h"

namespace esphome {
namespace camera_http {
class CameraHttpServer;
}
namespace frame_uploader {

// Periodically requests one frame from the camera, copies the JPEG into PSRAM (normal heap if PSRAM is
// short) and uploads the copy from a background FreeRTOS task. The camera's own frame buffer is handed
// back at once, so a slow TLS handshake (up to 20 s) never blocks the main loop or starves the live
// stream: the camera can't capture again while anyone still holds its frame.
//
// Lit frames (light_frames, default on): when the sensor's auto-exposure says the scene is dark, the
// camera's LED fades up through camera_http (its only driver), auto-exposure settles, two frames are
// thrown away, the next one is uploaded and the LED goes back to what it was. About 1.2-1.5 s of
// light, never more than 2.5 s. Logic and timing: frame_light.h.
class FrameUploader : public PollingComponent, public camera::CameraListener {
 public:
  void set_camera(camera::Camera *camera) { this->camera_ = camera; }
  void set_url(const std::string &url) { this->url_ = url; }
  void set_token(const std::string &token) { this->token_ = token; }
  void set_grow(const std::string &grow) { this->grow_ = grow; }
  // Remote mode (chamber_link on_remote): upload to <url>/api/device/remote/frame in the chamber's name.
  // While set it replaces the configured frame-ingest target; empty url = off. Never logged.
  void set_remote(const std::string &url, const std::string &device_id, const std::string &key);
  bool remote_on() const { return !this->remote_url_.empty(); }
  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  bool is_enabled() const { return this->enabled_; }
  // While the camera updates itself (camera_update on_begin/on_abort): no new uploads, so the download
  // has the only TLS session. Separate from enabled, which is the owner's Upload Frames switch.
  void set_paused(bool paused) { this->paused_ = paused; }
  // Lit timelapse frames ("Light Timelapse Frames" switch). Needs camera_http_id (the LED's driver).
  void set_camera_http(camera_http::CameraHttpServer *http) { this->http_ = http; }
  void set_dark_threshold(uint32_t index) { this->dark_threshold_ = index; }
  void set_light_frames(bool on);
  bool light_frames() const { return this->light_frames_; }
  // The sensor's exposure index (lines at 1x gain) read before the last upload; 0 = not read yet,
  // UINT32_MAX = the read failed. For calibrating dark_exposure on the bench.
  uint32_t last_exposure() const { return this->last_exposure_; }
  uint32_t frames_lit() const { return this->lit_count_; }
  uint32_t uploads_ok() const { return this->ok_; }
  uint32_t uploads_failed() const { return this->failed_; }

  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override;

 protected:
  static void upload_task_(void *arg);
  void start_upload_(const std::shared_ptr<camera::CameraImage> &image);
  bool read_exposure_(uint32_t *index);
  void sync_light_();  // LED held exactly while lit_.holding()
  bool post_(const uint8_t *data, size_t len);

  camera::Camera *camera_{nullptr};
  camera_http::CameraHttpServer *http_{nullptr};
  frame_light::LitCapture lit_;
  bool light_held_{false};
  bool request_next_{false};  // a lit capture discarded a frame; loop() asks for the next
  bool light_frames_{true};
  uint32_t dark_threshold_{1000};
  uint32_t last_exposure_{0};
  uint32_t lit_count_{0};
  std::string url_, token_, grow_;            // frame-ingest (configured; optional)
  std::string remote_url_, remote_id_, remote_key_;  // remote mode (runtime)
  // Remote mode pacing (update() runs every update_interval, e.g. 15 s, and uploads when due): every
  // remote_interval_ms_ normally, every LIVE_INTERVAL_MS while the server answers 202 (someone watching).
  uint32_t remote_interval_ms_{300000};
  uint32_t last_remote_start_{0};
  std::atomic<uint32_t> live_until_{0};  // millis(); set by the upload task from a 202
  static constexpr uint32_t LIVE_INTERVAL_MS = 30000;
  static constexpr uint32_t LIVE_HOLD_MS = 120000;
 public:
  void set_remote_interval(uint32_t ms) { this->remote_interval_ms_ = ms; }
  bool live() const {
    const uint32_t u = this->live_until_.load();
    return u != 0 && (int32_t) (millis() - u) < 0;
  }
 protected:
  // What the running upload uses, copied when it starts, so set_remote() can't change it mid-flight.
  std::string job_url_, job_auth_, job_device_;
  int job_ok_status_{201};
  bool enabled_{true};
  bool paused_{false};
  bool waiting_{false};
  std::atomic<bool> busy_{false};
  uint8_t *pending_{nullptr};  // our own copy of the JPEG, freed by the upload task
  size_t pending_len_{0};
  std::atomic<uint32_t> ok_{0}, failed_{0};
};

}  // namespace frame_uploader
}  // namespace esphome
