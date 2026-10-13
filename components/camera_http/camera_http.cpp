#include "camera_http.h"

#include <cstring>

#include <mbedtls/base64.h>

#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_WEBSERVER_AUTH
#include "esphome/components/web_server_base/web_server_base.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "claim_guard.h"

namespace esphome {
namespace camera_http {

static const char *const TAG = "camera_http";
static const char *const BOUNDARY = "frame";
static const char *const STREAM_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *const PART_HEADER = "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void CameraHttpServer::set_credentials(const std::string &username, const std::string &password) {
  this->apply_login_(username, password);
}

// Writes the login into the inactive buffer, computes its stream/snapshot Basic header, then switches
// to it and (claim mode) points the REST/SSE web_server at the new buffer. The old buffer stays intact
// for any request that is still comparing against it.
void CameraHttpServer::apply_login_(const std::string &username, const std::string &password) {
  const uint8_t next = this->active_login_.load() ^ 1;
  SavedLogin &login = this->logins_[next];
  memset(&login, 0, sizeof(login));
  strncpy(login.username, username.c_str(), sizeof(login.username) - 1);
  strncpy(login.password, password.c_str(), sizeof(login.password) - 1);
  const std::string raw = std::string(login.username) + ":" + login.password;
  size_t out_len = 0;
  char *auth = this->expected_auth_[next];
  memset(auth, 0, AUTH_HEADER_MAX);
  memcpy(auth, "Basic ", 6);
  mbedtls_base64_encode(reinterpret_cast<unsigned char *>(auth + 6), AUTH_HEADER_MAX - 7, &out_len,
                        reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
  this->active_login_ = next;
#ifdef USE_WEBSERVER_AUTH
  if (this->claim_window_ms_ > 0 && web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->set_auth_username(login.username);
    web_server_base::global_web_server_base->set_auth_password(login.password);
  }
#endif
}

// A random login nobody knows: the camera is locked until it's claimed.
static std::string random_secret() {
  uint8_t rnd[16];
  random_bytes(rnd, sizeof(rnd));
  char out[33];
  for (size_t i = 0; i < sizeof(rnd); i++)
    snprintf(out + 2 * i, 3, "%02x", rnd[i]);
  return std::string(out, 32);
}

bool CameraHttpServer::claim_open_() const {
  return this->claim_window_ms_ > 0 && !this->claimed_.load() && !this->claim_closed_.load();
}

void CameraHttpServer::reset_claim() {
  if (this->claim_window_ms_ == 0)
    return;
  SavedLogin empty{};
  this->pref_.save(&empty);
  global_preferences->sync();
  this->claimed_ = false;
  this->apply_login_(random_secret(), random_secret());
  ESP_LOGW(TAG, "Login cleared; it can be claimed again after the next boot");
}

void CameraHttpServer::load_login() {
  if (this->claim_window_ms_ == 0)
    return;
  if (!this->login_loaded_) {
    this->pref_ = global_preferences->make_preference<SavedLogin>(fnv1_hash("camera_http_login"));
    this->login_loaded_ = true;
  }
  SavedLogin saved{};
  if (this->pref_.load(&saved) && saved.username[0] != '\0' && saved.password[0] != '\0') {
    saved.username[sizeof(saved.username) - 1] = '\0';
    saved.password[sizeof(saved.password) - 1] = '\0';
    this->claimed_ = true;
    this->apply_login_(saved.username, saved.password);
  } else {
    this->claimed_ = false;
    this->apply_login_(random_secret(), random_secret());  // also replaces the compiled-in placeholder
  }
}

void CameraHttpServer::setup() {
  this->load_login();  // again, now that the radio is on (a real RNG source for the lock)

  this->lock_ = xSemaphoreCreateMutex();
  this->ready_ = xSemaphoreCreateBinary();
  this->camera_->add_listener(this);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->port_;
  config.ctrl_port = this->port_ + 1;  // must not clash with web_server's httpd instance
  config.max_open_sockets = 3;
  config.stack_size = 6144;
  config.lru_purge_enable = true;
  if (httpd_start(&this->server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Could not start camera HTTP server on port %u", this->port_);
    this->mark_failed();
    return;
  }
  const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler_, .user_ctx = this};
  const httpd_uri_t snapshot = {.uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_handler_, .user_ctx = this};
  httpd_register_uri_handler(this->server_, &stream);
  httpd_register_uri_handler(this->server_, &snapshot);
  if (this->claim_window_ms_ > 0) {
    const httpd_uri_t claim_get = {.uri = "/claim", .method = HTTP_GET, .handler = claim_get_handler_, .user_ctx = this};
    const httpd_uri_t claim_post = {.uri = "/claim", .method = HTTP_POST, .handler = claim_post_handler_, .user_ctx = this};
    httpd_register_uri_handler(this->server_, &claim_get);
    httpd_register_uri_handler(this->server_, &claim_post);
  }
  if (this->update_post_ && this->update_get_) {
    const httpd_uri_t update_get = {.uri = "/update", .method = HTTP_GET, .handler = update_get_handler_, .user_ctx = this};
    const httpd_uri_t update_post = {.uri = "/update", .method = HTTP_POST, .handler = update_post_handler_, .user_ctx = this};
    httpd_register_uri_handler(this->server_, &update_get);
    httpd_register_uri_handler(this->server_, &update_post);
  }
}

bool CameraHttpServer::light_wanted_() const {
  if (this->light_ == nullptr)
    return false;
  return this->light_hold_.load() || (this->light_enable_->state && this->active_.load() > 0);
}

void CameraHttpServer::hold_light(bool on, uint32_t ramp_ms) {
  this->hold_ramp_ms_ = ramp_ms;
  this->light_hold_ = on;
  this->update_light_();
}

// On the main loop (light calls aren't thread-safe): the LED is on exactly while it's wanted. Anything
// else that turned it on (it's internal, but a leftover state after boot) is switched off here.
void CameraHttpServer::update_light_() {
  if (this->light_ == nullptr)
    return;
  const bool want = this->light_wanted_();
  if (this->light_->remote_values.is_on() == want)
    return;
  auto call = this->light_->make_call();
  call.set_state(want);
  // A held LED (lit timelapse frame) fades up; everything else, including every switch-off, is instant.
  call.set_transition_length(want && this->light_hold_.load() ? this->hold_ramp_ms_ : 0);
  call.perform();
}

void CameraHttpServer::wake_loop_() { App.wake_loop_threadsafe(); }

void CameraHttpServer::loop() {
  // The claim window closes once, for good: a plain `millis() < window` would open it again 49.7 days
  // after boot when millis() wraps.
  if (!this->claim_closed_.load() && this->claim_window_ms_ > 0 && millis() >= this->claim_window_ms_)
    this->claim_closed_ = true;
#ifdef USE_WIFI
  if (this->placeholder_ssid_ != nullptr)
    this->on_placeholder_ = strcmp(wifi::global_wifi_component->get_sta().get_ssid().c_str(), this->placeholder_ssid_) == 0;
#endif

  // Camera requests from the httpd handlers, applied here where the camera expects them.
  const bool want_stream = this->stream_want_.load() > 0 && !this->paused_.load();
  if (want_stream != this->stream_on_) {
    this->stream_on_ = want_stream;
    want_stream ? this->camera_->start_stream(camera::WEB_REQUESTER) : this->camera_->stop_stream(camera::WEB_REQUESTER);
  }
  if (this->image_wanted_.exchange(false) && !this->paused_.load())
    this->camera_->request_image(camera::WEB_REQUESTER);

  this->update_light_();
  if (this->save_pending_.exchange(false)) {
    this->pref_.save(&this->logins_[this->active_login_.load()]);
    global_preferences->sync();
    ESP_LOGI(TAG, "Camera claimed; login saved");  // never log the values
  }
}

void CameraHttpServer::on_shutdown() {
  if (this->server_ != nullptr)
    httpd_stop(this->server_);
}

void CameraHttpServer::dump_config() {
  ESP_LOGCONFIG(TAG, "Camera HTTP server (Basic auth):");
  ESP_LOGCONFIG(TAG, "  Port: %u  (/stream, /snapshot)", this->port_);  // credentials deliberately not logged
  if (this->claim_window_ms_ > 0)
    ESP_LOGCONFIG(TAG, "  Claim: %s (window %us after boot)", this->claimed_.load() ? "claimed" : "unclaimed",
                  (unsigned) (this->claim_window_ms_ / 1000));
}

// Main loop. A frame is kept only while a handler waits for one; otherwise holding it would stop the
// camera from capturing the next (it reuses the buffer once every shared_ptr is gone).
void CameraHttpServer::on_camera_image(const std::shared_ptr<camera::CameraImage> &image) {
  if (!image->was_requested_by(camera::WEB_REQUESTER) || this->waiters_.load() <= 0)
    return;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  this->image_ = image;
  xSemaphoreGive(this->lock_);
  xSemaphoreGive(this->ready_);
}

// httpd task: forget a frame delivered before now, so the next wait returns a frame taken after the
// request (and after the LED came on).
void CameraHttpServer::drain_frames_() {
  while (xSemaphoreTake(this->ready_, 0) == pdTRUE) {
  }
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  this->image_.reset();
  xSemaphoreGive(this->lock_);
}

std::shared_ptr<camera::CameraImage> CameraHttpServer::wait_for_frame_(uint32_t timeout_ms) {
  if (xSemaphoreTake(this->ready_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    // Timed out: a frame that lands later must not sit here pinning the camera's buffer.
    this->drain_frames_();
    return nullptr;
  }
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  auto image = std::move(this->image_);
  xSemaphoreGive(this->lock_);
  return image;
}

bool CameraHttpServer::authorized_(httpd_req_t *req) {
  char header[AUTH_HEADER_MAX] = {0};
  bool ok = false;
  if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
    const char *want = this->expected_auth_[this->active_login_.load()];
    const size_t want_len = strnlen(want, AUTH_HEADER_MAX);
    const size_t got_len = strlen(header);
    uint8_t diff = got_len != want_len;
    for (size_t i = 0; i < want_len; i++)  // constant time over the expected value
      diff |= static_cast<uint8_t>(want[i] ^ (i < got_len ? header[i] : 0));
    ok = diff == 0;
  }
  if (!ok) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AutoMyco Camera\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
  }
  return ok;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t CameraHttpServer::claim_get_handler_(httpd_req_t *req) {
  auto *self = static_cast<CameraHttpServer *>(req->user_ctx);
  char body[48];
  snprintf(body, sizeof(body), "{\"claimed\":%s,\"open\":%s}", self->claimed_.load() ? "true" : "false",
           self->claim_open_() ? "true" : "false");
  return send_json(req, "200 OK", body);
}

// One request header, "" if absent or longer than 127 bytes.
static std::string req_header(httpd_req_t *req, const char *name) {
  char value[128];
  const size_t len = httpd_req_get_hdr_value_len(req, name);
  if (len == 0 || len >= sizeof(value) || httpd_req_get_hdr_value_str(req, name, value, sizeof(value)) != ESP_OK)
    return "";
  return std::string(value, len);
}

static bool valid_field(const std::string &v, size_t min_len, size_t max_len) {
  if (v.size() < min_len || v.size() > max_len)
    return false;
  for (char c : v) {
    if (c < 0x21 || c > 0x7e || c == ':')  // printable ASCII, no space, no ':' (Basic auth separator)
      return false;
  }
  return true;
}

// One-time claim: {"username": "...", "password": "..."}. Only while unclaimed and inside the window
// after boot, so taking over a claimed camera needs physical access (reset_login + a power cycle).
esp_err_t CameraHttpServer::claim_post_handler_(httpd_req_t *req) {
  auto *self = static_cast<CameraHttpServer *>(req->user_ctx);
  if (self->claimed_.load())
    return send_json(req, "409 Conflict", "{\"error\":\"already claimed\"}");
  if (!self->claim_open_())
    return send_json(req, "403 Forbidden", "{\"error\":\"claim window closed; power-cycle the chamber\"}");
  // Only the app, never a web page (claim_guard.h): JSON, no Origin, Host is this camera.
  const char *refused = claim_guard::refusal(req_header(req, "Host"), httpd_req_get_hdr_value_len(req, "Origin") > 0,
                                             req_header(req, "Content-Type"), App.get_name());
  if (refused != nullptr) {
    ESP_LOGW(TAG, "Claim refused: %s", refused);
    return send_json(req, "403 Forbidden", "{\"error\":\"refused\"}");
  }
  // Still on the placeholder network (camera-public.yaml wifi): the chamber hasn't given this camera its Wi-Fi,
  // so whatever network it's on isn't the owner's. No claim there.
  if (self->on_placeholder_.load())
    return send_json(req, "403 Forbidden", "{\"error\":\"waiting for the chamber's Wi-Fi\"}");
  if (req->content_len == 0 || req->content_len > 256)
    return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
  char buf[257] = {0};
  size_t got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, buf + got, req->content_len - got);
    if (n <= 0)
      return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
    got += n;
  }
  std::string username, password;
  json::parse_json(reinterpret_cast<const uint8_t *>(buf), got, [&](JsonObject root) -> bool {
    username = root["username"] | "";
    password = root["password"] | "";
    return true;
  });
  if (!valid_field(username, 1, 32) || !valid_field(password, 12, 64))
    return send_json(req, "400 Bad Request", "{\"error\":\"username 1-32, password 12-64 printable chars, no ':'\"}");
  // Re-check: two claims could race on the httpd task.
  bool expected = false;
  if (!self->claimed_.compare_exchange_strong(expected, true))
    return send_json(req, "409 Conflict", "{\"error\":\"already claimed\"}");
  self->apply_login_(username, password);
  self->save_pending_ = true;
  return send_json(req, "200 OK", "{\"claimed\":true}");
}

esp_err_t CameraHttpServer::update_get_handler_(httpd_req_t *req) {
  auto *self = static_cast<CameraHttpServer *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
  const std::string body = self->update_get_();
  return send_json(req, "200 OK", body.c_str());
}

// {"version": "1.1.0", "hash": "<sha256>", "force": false}: camera_update checks it and answers.
esp_err_t CameraHttpServer::update_post_handler_(httpd_req_t *req) {
  auto *self = static_cast<CameraHttpServer *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
  if (req->content_len == 0 || req->content_len > 256)
    return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
  char buf[257] = {0};
  size_t got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, buf + got, req->content_len - got);
    if (n <= 0)
      return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
    got += n;
  }
  std::string reply;
  const char *status = self->update_post_(std::string(buf, got), reply);
  return send_json(req, status, reply.c_str());
}

esp_err_t CameraHttpServer::snapshot_handler_(httpd_req_t *req) {
  auto *self = static_cast<CameraHttpServer *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
  if (self->paused_.load())
    return send_json(req, "503 Service Unavailable", "{\"error\":\"camera is updating\"}");
  self->active_++;
  self->waiters_++;
  if (self->light_wanted_()) {
    vTaskDelay(pdMS_TO_TICKS(400));  // the main loop lights the LED; give the exposure time to settle
    self->wait_for_frame_(0);        // drop a frame that may have been taken before the light was on
    // The camera's next frame can still be one it captured (and queued) before the LED came on, so
    // take one more and throw it away; the frame after that postdates the light.
    self->drain_frames_();
    self->request_image_();
    self->wait_for_frame_(3000);
  }
  self->drain_frames_();
  self->request_image_();
  auto image = self->wait_for_frame_(5000);
  self->waiters_--;
  self->active_--;
  if (!image) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
  return httpd_resp_send(req, reinterpret_cast<const char *>(image->get_data_buffer()), image->get_data_length());
}

esp_err_t CameraHttpServer::stream_handler_(httpd_req_t *req) {
  auto *self = static_cast<CameraHttpServer *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
  if (self->paused_.load())
    return send_json(req, "503 Service Unavailable", "{\"error\":\"camera is updating\"}");
  httpd_resp_set_type(req, STREAM_TYPE);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  self->active_++;
  self->waiters_++;
  self->drain_frames_();
  self->stream_want_++;
  self->wake_loop_();
  esp_err_t res = ESP_OK;
  char part[96];
  while (res == ESP_OK && !self->paused_.load()) {
    auto image = self->wait_for_frame_(5000);
    if (!image)
      break;  // camera stalled; client will reconnect
    int n = snprintf(part, sizeof(part), PART_HEADER, (unsigned) image->get_data_length());
    res = httpd_resp_send_chunk(req, part, n);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(image->get_data_buffer()),
                                  image->get_data_length());
  }
  self->stream_want_--;
  self->wake_loop_();
  self->waiters_--;
  self->active_--;
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

}  // namespace camera_http
}  // namespace esphome
