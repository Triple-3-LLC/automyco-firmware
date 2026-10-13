#include "frame_uploader.h"

#include <algorithm>
#include <cstring>

#include <esp_camera.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/components/camera_http/camera_http.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace frame_uploader {

static const char *const TAG = "frame_uploader";

void FrameUploader::setup() { this->camera_->add_listener(this); }

void FrameUploader::set_remote(const std::string &url, const std::string &device_id, const std::string &key) {
  this->remote_url_ = url;
  this->remote_id_ = device_id;
  std::fill(this->remote_key_.begin(), this->remote_key_.end(), '\0');
  this->remote_key_ = key;
}

void FrameUploader::set_light_frames(bool on) {
  this->light_frames_ = on;
  if (!on && this->lit_.active()) {
    this->lit_.cancel();
    this->waiting_ = false;
    this->sync_light_();
  }
}

void FrameUploader::sync_light_() {
  const bool want = this->lit_.holding();
  if (this->http_ == nullptr || want == this->light_held_)
    return;
  this->light_held_ = want;
  this->http_->hold_light(want, this->lit_.timing().ramp_ms);
}

// The OV2640's exposure state, from its sensor register bank (bank bit 0x100 in get_reg). Main loop only,
// like every other SCCB access after setup (esp32_camera only writes sensor settings at setup).
bool FrameUploader::read_exposure_(uint32_t *index) {
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr || s->get_reg == nullptr || s->id.PID != OV2640_PID)
    return false;
  const int r04 = s->get_reg(s, 0x100 | 0x04, 0xFF);
  const int r10 = s->get_reg(s, 0x100 | 0x10, 0xFF);
  const int r45 = s->get_reg(s, 0x100 | 0x45, 0xFF);
  const int gain = s->get_reg(s, 0x100 | 0x00, 0xFF);
  if (r04 < 0 || r10 < 0 || r45 < 0 || gain < 0)
    return false;
  const uint32_t aec = frame_light::aec_lines(r04, r10, r45);
  *index = frame_light::exposure_index(aec, static_cast<uint8_t>(gain));
  ESP_LOGD(TAG, "Exposure: AEC %u lines, gain 0x%02X -> index %u (dark from %u)", (unsigned) aec, gain,
           (unsigned) *index, (unsigned) this->dark_threshold_);
  return true;
}

void FrameUploader::update() {
  if (!this->enabled_ || this->paused_ || this->busy_ || this->lit_.active() ||
      !wifi::global_wifi_component->is_connected())
    return;
  if (this->remote_url_.empty() && this->url_.empty())
    return;  // nowhere to send: no frame-ingest configured and the chamber hasn't turned remote on
  if (!this->remote_url_.empty()) {
    // Remote mode: due every remote_interval_ms_, or every 30 s while someone is watching.
    const uint32_t due = this->live() ? LIVE_INTERVAL_MS : this->remote_interval_ms_;
    if (this->last_remote_start_ != 0 && millis() - this->last_remote_start_ < due)
      return;
    this->last_remote_start_ = millis() | 1;
  }
  if (this->light_frames_ && this->http_ != nullptr && this->http_->has_light()) {
    uint32_t index = 0;
    const bool read_ok = this->read_exposure_(&index);
    this->last_exposure_ = read_ok ? index : UINT32_MAX;
    if (frame_light::decide(true, this->http_->light_is_on(), read_ok, index, this->dark_threshold_) ==
        frame_light::Decision::LIGHT) {
      // Dark: light, settle, discard, capture. loop() and on_camera_image() drive it from here.
      this->waiting_ = false;
      this->lit_.begin(millis());
      this->sync_light_();
      return;
    }
  }
  // IDLE frames are also what the camera captures on its own at idle_framerate (and what the native API
  // forwards), so the next IDLE frame of any origin is the one we take: `waiting_` is the gate.
  this->waiting_ = true;
  this->camera_->request_image(camera::IDLE);
}

void FrameUploader::loop() {
  if (!this->lit_.active()) {
    this->request_next_ = false;
    return;
  }
  if (!this->enabled_ || !wifi::global_wifi_component->is_connected()) {
    this->lit_.cancel();
    this->waiting_ = false;
  } else {
    if (this->request_next_) {
      this->request_next_ = false;
      this->camera_->request_image(camera::IDLE);
    }
    switch (this->lit_.tick(millis())) {
      case frame_light::LitCapture::Tick::REQUEST:
        this->waiting_ = true;
        this->camera_->request_image(camera::IDLE);
        break;
      case frame_light::LitCapture::Tick::TIMEOUT:
        ESP_LOGW(TAG, "No frame within %u ms of lighting; LED released, frame skipped",
                 (unsigned) this->lit_.timing().max_on_ms);
        this->waiting_ = false;
        this->failed_++;
        break;
      case frame_light::LitCapture::Tick::NONE:
        break;
    }
  }
  this->sync_light_();
}

void FrameUploader::on_camera_image(const std::shared_ptr<camera::CameraImage> &image) {
  if (!this->waiting_ || !image->was_requested_by(camera::IDLE) || this->busy_)
    return;
  if (this->lit_.active()) {
    const auto verdict = this->lit_.on_frame(millis());
    this->sync_light_();  // released on USE / EXPIRED, before the upload starts
    switch (verdict) {
      case frame_light::LitCapture::Frame::DISCARD:
        // Taken before the light, or before AE caught up: ask for the next one (we never keep this one).
        // From loop(), not here: esp32_camera clears its requests right after calling its listeners.
        this->request_next_ = true;
        return;
      case frame_light::LitCapture::Frame::EXPIRED:
        this->waiting_ = false;
        this->failed_++;
        return;
      case frame_light::LitCapture::Frame::IGNORE:
        return;
      case frame_light::LitCapture::Frame::USE:
        this->lit_count_++;
        ESP_LOGI(TAG, "Lit frame (exposure index %u)", (unsigned) this->last_exposure_);
        break;
    }
  }
  this->waiting_ = false;
  this->start_upload_(image);
}

void FrameUploader::start_upload_(const std::shared_ptr<camera::CameraImage> &image) {
  // Copy the JPEG out (PSRAM first: a 1600x1200 frame is 100-200 KB) and let the camera have its buffer
  // back before the upload starts; the task may hold it for 20 s otherwise.
  const size_t len = image->get_data_length();
  uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (copy == nullptr)
    copy = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
  if (copy == nullptr) {
    ESP_LOGW(TAG, "No memory for a %u B frame copy", (unsigned) len);
    this->failed_++;
    return;
  }
  memcpy(copy, image->get_data_buffer(), len);
  if (!this->remote_url_.empty()) {
    this->job_url_ = this->remote_url_ + "/api/device/remote/frame";
    this->job_auth_ = "Bearer " + this->remote_key_;
    this->job_device_ = this->remote_id_;
    this->job_ok_status_ = 204;
  } else if (!this->url_.empty()) {
    this->job_url_ = this->url_ + "/frame?grow=" + this->grow_;
    this->job_auth_ = "Bearer " + this->token_;
    this->job_device_.clear();
    this->job_ok_status_ = 201;
  } else {
    heap_caps_free(copy);
    return;
  }
  this->busy_ = true;
  this->pending_ = copy;
  this->pending_len_ = len;
  if (xTaskCreate(&FrameUploader::upload_task_, "frame_upload", 8192, this, 1, nullptr) != pdPASS) {
    ESP_LOGW(TAG, "Could not start upload task");
    heap_caps_free(copy);
    this->pending_ = nullptr;
    this->pending_len_ = 0;
    this->failed_++;
    this->busy_ = false;
  }
}

void FrameUploader::upload_task_(void *arg) {
  auto *self = static_cast<FrameUploader *>(arg);
  uint8_t *data = self->pending_;
  const size_t len = self->pending_len_;
  self->pending_ = nullptr;
  self->pending_len_ = 0;
  bool ok = self->post_(data, len);
  heap_caps_free(data);
  ok ? self->ok_++ : self->failed_++;
  self->busy_ = false;
  vTaskDelete(nullptr);
}

bool FrameUploader::post_(const uint8_t *data, size_t len) {
  esp_http_client_config_t cfg = {};
  cfg.url = this->job_url_.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 20000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;  // verify the Lambda URL's TLS cert
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr)
    return false;
  esp_http_client_set_header(client, "Authorization", this->job_auth_.c_str());
  if (!this->job_device_.empty())
    esp_http_client_set_header(client, "X-Device-Id", this->job_device_.c_str());
  esp_http_client_set_header(client, "Content-Type", "image/jpeg");
  esp_http_client_set_post_field(client, reinterpret_cast<const char *>(data), len);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  // Remote mode: 202 = accepted, and someone is watching the chamber remotely: upload every 30 s for a while.
  if (err == ESP_OK && this->job_ok_status_ == 204 && (status == 204 || status == 202)) {
    this->live_until_ = status == 202 ? ((millis() + LIVE_HOLD_MS) | 1) : 0;
    status = 204;
  }
  if (err != ESP_OK || status != this->job_ok_status_) {
    ESP_LOGW(TAG, "Upload of %u B failed: %s, HTTP %d", (unsigned) len, esp_err_to_name(err), status);
    return false;
  }
  ESP_LOGI(TAG, "Uploaded %u B frame (%s)", (unsigned) len, this->job_device_.empty() ? this->grow_.c_str() : "remote");
  return true;
}

void FrameUploader::dump_config() {
  ESP_LOGCONFIG(TAG, "Frame uploader:");
  if (!this->url_.empty()) {
    ESP_LOGCONFIG(TAG, "  Frame-ingest endpoint: %s/frame", this->url_.c_str());  // token deliberately not logged
    ESP_LOGCONFIG(TAG, "  Grow: %s", this->grow_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Remote mode: set by the chamber at runtime (%s)", this->remote_url_.empty() ? "off" : "on");
  if (this->http_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Lit frames: %s, dark from exposure index %u", this->light_frames_ ? "on" : "off",
                  (unsigned) this->dark_threshold_);
  }
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace frame_uploader
}  // namespace esphome
