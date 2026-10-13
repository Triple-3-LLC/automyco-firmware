#include "camera_update.h"

#include <algorithm>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include "esphome/components/camera_http/camera_http.h"
#include "esphome/components/chamber_link/chamber_link.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace camera_update {

using namespace camera_update_logic;

static const char *const TAG = "camera_update";
static const uint32_t START_WAIT_MS = 90000;  // Wi-Fi and heap, after the camera boots or a request lands

void CameraUpdate::setup() {
  if (this->link_ != nullptr) {
    this->link_->set_ota_handler([this](const std::string &version, const std::string &hash, bool force) {
      // The link never forces: a reinstall or downgrade needs the camera login (POST /update).
      (void) force;
      const Verdict v = this->request(version, hash, false);
      if (v != Verdict::OK) {
        ESP_LOGW(TAG, "Chamber's update request refused: %s", verdict_reason(v, this->running_).c_str());
        this->link_->send_line(link_line(State::FAILED, 0, bare_version(version), verdict_reason(v, this->running_)));
      }
    });
  }
  if (this->http_ != nullptr) {
    this->http_->set_update_handlers(
        [this](const std::string &body, std::string &reply) -> const char * {
          std::string version, hash;
          bool force = false;
          json::parse_json(reinterpret_cast<const uint8_t *>(body.data()), body.size(), [&](JsonObject root) -> bool {
            version = root["version"] | "";
            hash = root["hash"] | "";
            force = root["force"] | false;
            return true;
          });
          const Verdict v = this->request(version, hash, force);
          if (v == Verdict::OK) {
            reply = this->status_json_();
            return "202 Accepted";
          }
          reply = "{\"error\":\"" + safe_text(verdict_reason(v, this->running_)) + "\"}";
          return v == Verdict::INVALID ? "400 Bad Request" : "409 Conflict";
        },
        [this]() { return this->status_json_(); });
  }
}

Verdict CameraUpdate::request(const std::string &version, const std::string &hash, bool force) {
  std::lock_guard<std::mutex> guard(this->lock_);
  const std::string bare = bare_version(version);
  const Verdict v = check(this->running_, bare, hash, force, this->busy_.load());
  if (v != Verdict::OK)
    return v;
  this->target_ = bare;
  this->hash_ = hash;
  this->force_ = force;
  this->error_.clear();
  this->pending_ = true;
  this->progress_ = 0;
  this->state_ = State::WAITING;
  this->busy_ = true;
  return v;
}

std::string CameraUpdate::status_json_() {
  std::lock_guard<std::mutex> guard(this->lock_);
  return status_json(this->state_.load(), this->progress_.load(), this->running_, this->target_, this->error_);
}

void CameraUpdate::set_state_(State s, const std::string &error) {
  if (!error.empty()) {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->error_ = error;
  }
  this->state_ = s;
}

void CameraUpdate::loop() {
  bool starting = false;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    if (this->pending_) {
      this->pending_ = false;
      starting = true;
    }
  }
  if (starting) {
    ESP_LOGI(TAG, "Updating to automyco-camera/%s (from %s)", this->target_.c_str(), this->running_.c_str());
    this->wait_started_ = millis();
    this->task_started_ = false;
    this->reported_state_ = State::IDLE;
    this->reported_progress_ = -1;
    if (this->http_ != nullptr)
      this->http_->set_paused(true);
    if (this->link_ != nullptr)
      this->link_->set_updating(true);
    this->begin_callbacks_.call();  // frame uploads pause (camera.yaml, common/camera-remote.yaml)
  }

  if (this->busy_ && !this->task_started_ && this->state_ == State::WAITING) {
    const bool wifi = wifi::global_wifi_component->is_connected();
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (wifi && heap_ok(block, free)) {
      this->task_started_ = true;
      if (xTaskCreate(&CameraUpdate::download_task_, "camera_update", TASK_STACK, this, 1, nullptr) != pdPASS)
        this->set_state_(State::FAILED, "could not start the download");
    } else if (millis() - this->wait_started_ > START_WAIT_MS) {
      ESP_LOGW(TAG, "Not starting: Wi-Fi %s, heap %u free, largest block %u", wifi ? "up" : "down", (unsigned) free,
               (unsigned) block);
      this->set_state_(State::FAILED, wifi ? "not enough memory" : "camera not on Wi-Fi");
    }
  }

  this->report_();

  if (this->busy_ && this->state_ == State::FAILED)
    this->finish_failed_();
}

// Progress on the camera link: the stock updater's plain-text markers (which every AutoMyco main board
// already reads), plus {"status":"ota",...} with a percentage and any error.
void CameraUpdate::report_() {
  const State s = this->state_.load();
  const int p = this->progress_.load();
  if (s == this->reported_state_ && !(s == State::DOWNLOADING && progress_step(this->reported_progress_, p)))
    return;
  const bool entered = s != this->reported_state_;
  this->reported_state_ = s;
  this->reported_progress_ = p;
  if (s == State::IDLE)
    return;
  std::string target, error;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    target = this->target_;
    error = this->error_;
  }
  if (entered)
    ESP_LOGI(TAG, "Update %s%s%s", state_name(s), error.empty() ? "" : ": ", error.c_str());
  if (entered && s == State::DONE)
    this->set_timeout("reboot", 1500, []() { App.safe_reboot(); });  // after the lines below go out
  if (this->link_ == nullptr)
    return;
  if (entered && s == State::DOWNLOADING)
    this->link_->send_line("Starting OTA update");
  this->link_->send_line(link_line(s, p, target, error));
  if (s == State::DONE)
    this->link_->send_line("Update successfully completed. Rebooting...");
}

void CameraUpdate::finish_failed_() {
  this->busy_ = false;
  this->task_started_ = false;
  if (this->http_ != nullptr)
    this->http_->set_paused(false);
  if (this->link_ != nullptr)
    this->link_->set_updating(false);
  this->abort_callbacks_.call();  // frame uploads resume
}

void CameraUpdate::download_task_(void *arg) {
  auto *self = static_cast<CameraUpdate *>(arg);
  for (int attempt = 1; attempt <= Attempt::MAX; attempt++) {
    int status = 0;
    bool network_error = false;
    if (self->download_once_(&status, &network_error)) {
      self->set_state_(State::DONE);
      vTaskDelete(nullptr);
      return;
    }
    if (attempt == Attempt::MAX || !Attempt::retryable(status, network_error))
      break;
    ESP_LOGW(TAG, "Download attempt %d of %d failed; retrying", attempt, Attempt::MAX);
    self->state_ = State::WAITING;  // the app shows "asking" again, not a stale percentage
    self->progress_ = 0;
    vTaskDelay(pdMS_TO_TICKS(Attempt::BACKOFF_MS));
  }
  self->set_state_(State::FAILED);  // error_ was set by the last attempt
  vTaskDelete(nullptr);
}

// One download. Writes error_ on failure and says whether it's worth another attempt.
bool CameraUpdate::download_once_(int *http_status, bool *network_error) {
  std::string target, hash;
  bool force;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    target = this->target_;
    hash = this->hash_;
    force = this->force_;
  }
  auto fail = [this](const std::string &why) {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->error_ = why;
    return false;
  };

  const std::string url = download_url(this->host_, hash);
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 20000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;  // api.automyco.com: ACM certificate, Amazon root
  cfg.buffer_size = READ_BUFFER;
  cfg.buffer_size_tx = TX_BUFFER;
  cfg.disable_auto_redirect = true;  // only our host, never wherever a Location points
  cfg.keep_alive_enable = false;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    *network_error = true;
    return fail("could not start the download");
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    *network_error = true;
    return fail(std::string("could not reach ") + this->host_);
  }
  const int64_t length = esp_http_client_fetch_headers(client);
  *http_status = esp_http_client_get_status_code(client);
  if (length < 0) {
    esp_http_client_cleanup(client);
    *network_error = true;
    return fail("no answer from the server");
  }
  if (*http_status != 200) {
    esp_http_client_cleanup(client);
    return fail(*http_status == 404 ? std::string("release not found on the server")
                                    : "server answered HTTP " + std::to_string(*http_status));
  }

  auto backend = ota::make_ota_backend();
  if (backend->begin(length > 0 ? static_cast<size_t>(length) : 0) != ota::OTA_RESPONSE_OK) {
    esp_http_client_cleanup(client);
    return fail("image does not fit the camera");
  }
  uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(READ_BUFFER, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    backend->abort();
    esp_http_client_cleanup(client);
    *network_error = true;  // transient: try again once the heap has recovered
    return fail("not enough memory");
  }
  this->progress_ = 0;
  this->state_ = State::DOWNLOADING;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t got = 0;
  bool ok = true;
  std::string why;
  uint8_t head[APP_DESC_MIN_BYTES];  // the image's first bytes, for its app descriptor
  size_t head_len = 0;
  ProjectVersionScanner project_version;
  while (true) {
    const int n = esp_http_client_read(client, reinterpret_cast<char *>(buf), READ_BUFFER);
    if (n < 0) {
      ok = false;
      *network_error = true;
      why = "download cut off";
      break;
    }
    if (n == 0) {
      if (!esp_http_client_is_complete_data_received(client)) {
        ok = false;
        *network_error = true;
        why = "download cut off";
      }
      break;
    }
    mbedtls_sha256_update(&sha, buf, n);
    if (head_len < sizeof(head)) {
      const size_t take = std::min(sizeof(head) - head_len, static_cast<size_t>(n));
      memcpy(head + head_len, buf, take);
      head_len += take;
    }
    project_version.feed(buf, n);
    if (backend->write(buf, n) != ota::OTA_RESPONSE_OK) {
      ok = false;
      why = "could not write the image";
      break;
    }
    got += n;
    if (length > 0) {
      const int p = percent(got, static_cast<size_t>(length));
      this->progress_ = p >= 100 ? 99 : p;  // 100 once verified
    }
    vTaskDelay(1);  // let the idle task (and its watchdog) run between chunks
  }
  heap_caps_free(buf);
  esp_http_client_cleanup(client);
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (ok && length > 0 && got != static_cast<size_t>(length)) {
    ok = false;
    *network_error = true;
    why = "download cut off";
  }
  if (!ok) {
    backend->abort();
    return fail(why);
  }

  this->state_ = State::VERIFYING;
  char hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  if (hash != hex) {
    // The server re-hashes before sending and TLS protects the transfer, so this is a wrong file, not
    // a flaky network: don't ask again. The boot slot is untouched.
    backend->abort();
    ESP_LOGE(TAG, "Image SHA-256 %s does not match the requested %s; not installed", hex, hash.c_str());
    return fail("image hash did not match");
  }
  // The hash proves it's the published file the request named, not that it's newer: the request's version
  // is only a claim (update_logic.h image_verdict). Decide on the image's own version, before end()
  // switches the boot slot.
  const AppDesc desc = parse_app_desc(head, head_len);
  const std::string verdict = image_verdict(desc, project_version.version(), esp_app_get_description()->project_name,
                                            this->running_, target, force);
  if (!verdict.empty()) {
    backend->abort();
    ESP_LOGE(TAG, "Image refused (descriptor %s, project string %s): %s", desc.version.c_str(),
             project_version.version().c_str(), verdict.c_str());
    return fail(verdict);
  }
  // end() checks the image (esp_ota_end) and only then points the bootloader at it.
  if (backend->end() != ota::OTA_RESPONSE_OK)
    return fail("camera rejected the image");
  this->progress_ = 100;
  ESP_LOGI(TAG, "automyco-camera/%s written and verified (%u B); rebooting", target.c_str(), (unsigned) got);
  return true;
}

void CameraUpdate::dump_config() {
  ESP_LOGCONFIG(TAG, "Camera self-update:");
  ESP_LOGCONFIG(TAG, "  Running: automyco-camera/%s", this->running_.c_str());
  ESP_LOGCONFIG(TAG, "  Images from: https://%s/fw/<sha256>.bin (SHA-256 checked before booting)", this->host_.c_str());
  ESP_LOGCONFIG(TAG, "  Requests: camera link%s", this->http_ != nullptr ? ", POST :8080/update (camera login)" : "");
}

}  // namespace camera_update
}  // namespace esphome
