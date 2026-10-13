#include "flash_dumper.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include <driver/uart.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include "esphome/components/json/json_util.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// ---------------------------------------------------------------------------------------------------
// READ-ONLY GUARANTEE. Every flash-writing, erasing or boot-switching call in ESP-IDF is poisoned from
// here on: using one anywhere below is a compile error ("attempt to use poisoned ..."). This component
// exists to copy the ONLY surviving copy of a stock image; it must never be able to damage it.
// esphome/tests/test_flash_dumper.cpp also scans this directory for these names.
// ---------------------------------------------------------------------------------------------------
#pragma GCC poison esp_partition_write esp_partition_write_raw esp_partition_erase_range
#pragma GCC poison esp_ota_begin esp_ota_write esp_ota_write_with_offset esp_ota_end esp_ota_abort
#pragma GCC poison esp_ota_set_boot_partition esp_ota_erase_last_boot_app_partition
#pragma GCC poison esp_ota_mark_app_valid_cancel_rollback esp_ota_mark_app_invalid_rollback_and_reboot
#pragma GCC poison esp_flash_write esp_flash_write_encrypted esp_flash_erase_region esp_flash_erase_chip
#pragma GCC poison spi_flash_write spi_flash_write_encrypted spi_flash_erase_range spi_flash_erase_sector

namespace esphome {
namespace flash_dumper {

static const char *const TAG = "flash_dumper";
static const size_t LINK_MAX_LINE = 320;

static std::string random_hex(size_t bytes) {
  std::vector<uint8_t> rnd(bytes);
  random_bytes(rnd.data(), rnd.size());
  return to_hex(rnd.data(), rnd.size());
}

static std::string hex32(uint32_t v) {
  char buf[12];
  snprintf(buf, sizeof(buf), "0x%06" PRIx32, v);
  return buf;
}

static std::string jstr(const std::string &s) { return "\"" + desc_field(s.c_str(), s.size()) + "\""; }

void FlashDumper::set_credentials(const std::string &username, const std::string &password) {
  this->username_ = username;
  this->password_ = password;
  memset(this->expected_auth_, 0, sizeof(this->expected_auth_));
  if (username.empty() || password.size() < MIN_PASSWORD_LEN)
    return;  // stays empty: login_configured() fails and nothing is served
  const std::string raw = username + ":" + password;
  size_t out_len = 0;
  memcpy(this->expected_auth_, "Basic ", 6);
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(this->expected_auth_ + 6), AUTH_HEADER_MAX - 7, &out_len,
                            reinterpret_cast<const unsigned char *>(raw.data()), raw.size()) != 0)
    memset(this->expected_auth_, 0, sizeof(this->expected_auth_));
}

void FlashDumper::lock_ota_() {
#ifdef USE_OTA_PASSWORD
  if (this->ota_ != nullptr && !this->ota_unlocked_.load())
    this->ota_->set_auth_password(random_hex(16));  // nobody knows it: OTA is effectively off
#endif
}

void FlashDumper::setup() {
  // First, before anything can fail: while the stock image has not been verified, an OTA would erase
  // it (OTA writes the inactive slot, which is exactly the slot this dumper reads).
  this->lock_ota_();

  this->login_ok_ = login_configured(this->password_, this->expected_auth_);
  if (!this->login_ok_) {
    // No (or a too-short) password: never serve anything. The dump server is not started at all.
    ESP_LOGE(TAG, "No usable login configured (password under %u characters); dump server disabled",
             (unsigned) MIN_PASSWORD_LEN);
    this->mark_failed();
    return;
  }

  this->scan_partitions_();

  if (!this->link_version_.empty()) {
    this->link_uart_ = logger::global_logger->get_uart_num();
    this->link_send_("{\"status\":\"ready\",\"version\":\"automyco-camera/" + this->link_version_ + "\"}");
  }
}

// Called from loop() once Wi-Fi is up: the network stack has to exist before httpd can open a socket,
// and this component sets up before Wi-Fi (its camera-link loop is what delivers the Wi-Fi network).
void FlashDumper::start_server_() {
  this->server_started_ = true;
  if (!this->login_ok_)
    return;  // belt and braces: setup() already refused
  this->lock_ota_();  // again, now that the radio is on and the RNG is a real entropy source
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->port_;
  config.ctrl_port = this->port_ + 1;
  config.max_open_sockets = 3;
  config.stack_size = 6144;
  config.lru_purge_enable = true;
  config.send_wait_timeout = 30;
  if (httpd_start(&this->server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Could not start the dump server on port %u", this->port_);
    this->mark_failed();
    return;
  }
  const httpd_uri_t info = {.uri = "/dump/info", .method = HTTP_GET, .handler = info_handler_, .user_ctx = this};
  const httpd_uri_t app = {.uri = "/dump/app", .method = HTTP_GET, .handler = app_handler_, .user_ctx = this};
  const httpd_uri_t unlock = {
      .uri = "/dump/ota-unlock", .method = HTTP_POST, .handler = unlock_handler_, .user_ctx = this};
  httpd_register_uri_handler(this->server_, &info);
  httpd_register_uri_handler(this->server_, &app);
  httpd_register_uri_handler(this->server_, &unlock);
}

void FlashDumper::scan_partitions_() {
  this->apps_.clear();
  this->app_parts_.clear();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr) {
    const esp_partition_t *p = esp_partition_get(it);
    this->apps_.push_back(AppSlot{p->label, p->address, p->size});
    this->app_parts_.push_back(p);
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);

  const esp_partition_t *running = esp_ota_get_running_partition();
  this->choice_ = pick_dump_slot(this->apps_, running != nullptr ? running->address : 0xFFFFFFFF);
  if (this->choice_.result != Pick::OK) {
    ESP_LOGE(TAG, "No slot to dump: %s", pick_str(this->choice_.result));
    return;
  }
  this->dump_part_ = this->app_parts_[this->choice_.index];
  const esp_partition_t *part = this->dump_part_;
  this->image_ = parse_image(
      [part](uint32_t offset, uint8_t *buf, size_t len) { return esp_partition_read(part, offset, buf, len) == ESP_OK; },
      part->size);
  if (this->image_.ok) {
    ESP_LOGI(TAG, "Dump slot %s @0x%06" PRIx32 ": %" PRIu32 " B app image in a %" PRIu32 " B slot", part->label,
             part->address, this->image_.length, part->size);
  } else {
    ESP_LOGW(TAG, "Dump slot %s @0x%06" PRIx32 ": %s", part->label, part->address, this->image_.error);
  }
}

// Two SHA-256 passes in one read: the whole trimmed image (what the known stock hash covers), and the
// bytes before the appended digest (to check the image's own SHA-256 trailer).
bool FlashDumper::hash_image_() {
  if (this->hashed_)
    return true;
  if (this->dump_part_ == nullptr || !this->image_.ok)
    return false;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[CHUNK]);
  if (!buf)
    return false;
  mbedtls_sha256_context whole, body;
  mbedtls_sha256_init(&whole);
  mbedtls_sha256_init(&body);
  mbedtls_sha256_starts(&whole, 0);
  mbedtls_sha256_starts(&body, 0);
  const uint32_t len = this->image_.length;
  const uint32_t body_len = this->image_.hash_appended ? this->image_.digest_offset : len;
  bool ok = true;
  for (uint32_t off = 0; off < len; off += CHUNK) {
    const size_t n = std::min<uint32_t>(CHUNK, len - off);
    if (esp_partition_read(this->dump_part_, off, buf.get(), n) != ESP_OK) {
      ok = false;
      break;
    }
    mbedtls_sha256_update(&whole, buf.get(), n);
    if (off < body_len)
      mbedtls_sha256_update(&body, buf.get(), std::min<uint32_t>(n, body_len - off));
  }
  uint8_t d_whole[32], d_body[32], stored[32];
  mbedtls_sha256_finish(&whole, d_whole);
  mbedtls_sha256_finish(&body, d_body);
  mbedtls_sha256_free(&whole);
  mbedtls_sha256_free(&body);
  if (!ok)
    return false;
  this->image_sha256_ = to_hex(d_whole, 32);
  if (this->image_.hash_appended &&
      esp_partition_read(this->dump_part_, this->image_.digest_offset, stored, sizeof(stored)) == ESP_OK) {
    this->appended_sha256_ = to_hex(stored, 32);
    this->appended_ok_ = memcmp(stored, d_body, 32) == 0;
  }
  this->hashed_ = true;
  ESP_LOGI(TAG, "Dump slot %s: %" PRIu32 " B image, SHA-256 %s (%s)", this->dump_part_->label, len,
           this->image_sha256_.c_str(),
           this->image_sha256_ == this->expected_sha256_ ? "matches the expected stock image" : "NOT the expected hash");
  return true;
}

std::string FlashDumper::info_json_() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  std::string j = "{\"dumper\":1";

  j += ",\"partitions\":[";
  bool first = true;
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr) {
    const esp_partition_t *p = esp_partition_get(it);
    char row[160];
    snprintf(row, sizeof(row), "%s{\"label\":%s,\"type\":%d,\"subtype\":%d,\"offset\":\"%s\",\"size\":%" PRIu32 "}",
             first ? "" : ",", jstr(p->label).c_str(), (int) p->type, (int) p->subtype, hex32(p->address).c_str(),
             p->size);
    j += row;
    first = false;
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  j += "]";

  if (running != nullptr) {
    j += ",\"running\":{\"label\":" + jstr(running->label) + ",\"offset\":\"" + hex32(running->address) +
         "\",\"size\":" + std::to_string(running->size);
    const esp_app_desc_t *self = esp_app_get_description();
    j += ",\"version\":" + jstr(desc_field(self->version, sizeof(self->version))) +
         ",\"project\":" + jstr(desc_field(self->project_name, sizeof(self->project_name))) + "}";
  }

  j += ",\"pick\":" + jstr(pick_str(this->choice_.result));
  if (this->dump_part_ != nullptr) {
    const esp_partition_t *d = this->dump_part_;
    j += ",\"dump\":{\"label\":" + jstr(d->label) + ",\"offset\":\"" + hex32(d->address) +
         "\",\"size\":" + std::to_string(d->size);
    j += std::string(",\"image_ok\":") + (this->image_.ok ? "true" : "false");
    if (!this->image_.ok) {
      j += ",\"image_error\":" + jstr(this->image_.error);
    } else {
      j += ",\"image_length\":" + std::to_string(this->image_.length) +
           ",\"segments\":" + std::to_string(this->image_.segments) +
           ",\"chip_id\":" + std::to_string(this->image_.chip_id) +
           ",\"entry\":\"" + hex32(this->image_.entry) + "\"" +
           std::string(",\"hash_appended\":") + (this->image_.hash_appended ? "true" : "false");
      esp_app_desc_t desc{};
      if (esp_ota_get_partition_description(d, &desc) == ESP_OK) {
        j += ",\"app\":{\"version\":" + jstr(desc_field(desc.version, sizeof(desc.version))) +
             ",\"project\":" + jstr(desc_field(desc.project_name, sizeof(desc.project_name))) +
             ",\"date\":" + jstr(desc_field(desc.date, sizeof(desc.date))) +
             ",\"time\":" + jstr(desc_field(desc.time, sizeof(desc.time))) +
             ",\"idf\":" + jstr(desc_field(desc.idf_ver, sizeof(desc.idf_ver))) + "}";
      }
      if (this->hash_image_()) {
        j += ",\"image_sha256\":\"" + this->image_sha256_ + "\"";
        if (this->image_.hash_appended)
          j += ",\"appended_sha256\":\"" + this->appended_sha256_ + "\",\"appended_sha256_ok\":" +
               (this->appended_ok_ ? "true" : "false");
        j += ",\"expected_sha256\":\"" + this->expected_sha256_ + "\",\"matches_expected\":" +
             (this->image_sha256_ == this->expected_sha256_ ? "true" : "false");
      } else {
        j += ",\"image_sha256\":null";
      }
    }
    j += "}";
  }
#ifdef USE_OTA_PASSWORD
  j += std::string(",\"ota\":\"") + (this->ota_ == nullptr ? "absent" : (this->ota_unlocked_.load() ? "unlocked" : "locked")) + "\"";
#else
  j += ",\"ota\":\"absent\"";
#endif
  j += "}";
  return j;
}

// The gate every handler calls first. Nothing before it touches flash, the query string or the response.
bool FlashDumper::authorized_(httpd_req_t *req) {
  char header[AUTH_HEADER_MAX] = {0};
  HeaderState state = HeaderState::MISSING;
  const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len >= sizeof(header)) {
    state = HeaderState::TRUNCATED;  // longer than any valid login: refuse, never compare a prefix
  } else if (len > 0 && httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
    state = HeaderState::PRESENT;
  }
  const Method method = req->method == HTTP_GET ? Method::GET : (req->method == HTTP_POST ? Method::POST : Method::OTHER);
  const Plan plan = plan_request(method, req->uri, state, header, this->login_ok_, this->expected_auth_);
  memset(header, 0, sizeof(header));
  if (plan.status == 200)
    return true;
  httpd_resp_set_status(req, plan.status == 404 ? "404 Not Found" : "401 Unauthorized");
  if (plan.status == 401)
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AutoMyco camera dumper\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, plan.status == 404 ? "Not Found" : "Unauthorized", HTTPD_RESP_USE_STRLEN);
  return false;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const std::string &body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t FlashDumper::info_handler_(httpd_req_t *req) {
  auto *self = static_cast<FlashDumper *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
  return send_json(req, "200 OK", self->info_json_());
}

static bool send_all(httpd_req_t *req, const char *data, size_t len) {
  while (len > 0) {
    const int n = httpd_send(req, data, len);
    if (n <= 0)
      return false;
    data += n;
    len -= n;
  }
  return true;
}

// The response is written by hand (status line, headers, body through httpd_send) so it carries a real
// Content-Length instead of chunked encoding: the verifier and curl can then tell a short read apart.
esp_err_t FlashDumper::app_handler_(httpd_req_t *req) {
  auto *self = static_cast<FlashDumper *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
  if (self->dump_part_ == nullptr)
    return send_json(req, "409 Conflict", std::string("{\"error\":") + jstr(pick_str(self->choice_.result)) + "}");

  bool trim = true;
  char query[32] = {0}, val[4] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "trim", val, sizeof(val)) == ESP_OK)
    trim = strcmp(val, "0") != 0;

  const uint32_t len = serve_length(self->image_, self->dump_part_->size, trim);
  if (len == 0)
    return send_json(req, "409 Conflict",
                     std::string("{\"error\":") + jstr(self->image_.error) + ",\"hint\":\"try ?trim=0 for the raw slot\"}");
  if (trim && !self->hash_image_())
    return send_json(req, "500 Internal Server Error", "{\"error\":\"could not read the slot to hash it\"}");

  std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
  head += "Content-Length: " + std::to_string(len) + "\r\n";
  head += std::string("Content-Disposition: attachment; filename=\"camera-") + self->dump_part_->label +
          (trim ? ".bin" : "-raw.bin") + "\"\r\n";
  head += std::string("X-Partition: ") + self->dump_part_->label + "\r\n";
  head += "X-Partition-Offset: " + hex32(self->dump_part_->address) + "\r\n";
  head += "X-Partition-Size: " + std::to_string(self->dump_part_->size) + "\r\n";
  if (trim)
    head += "X-Image-SHA256: " + self->image_sha256_ + "\r\n";
  head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";

  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[CHUNK]);
  if (!buf)
    return send_json(req, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
  ESP_LOGI(TAG, "Sending %s (%" PRIu32 " B, %s)", self->dump_part_->label, len, trim ? "trimmed" : "whole slot");
  bool ok = send_all(req, head.data(), head.size());
  for (uint32_t off = 0; ok && off < len; off += CHUNK) {
    const size_t n = std::min<uint32_t>(CHUNK, len - off);
    ok = esp_partition_read(self->dump_part_, off, buf.get(), n) == ESP_OK &&
         send_all(req, reinterpret_cast<const char *>(buf.get()), n);
  }
  if (!ok)
    ESP_LOGW(TAG, "Dump transfer cut short");
  httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  return ESP_OK;
}

esp_err_t FlashDumper::unlock_handler_(httpd_req_t *req) {
  auto *self = static_cast<FlashDumper *>(req->user_ctx);
  if (!self->authorized_(req))
    return ESP_OK;
#ifdef USE_OTA_PASSWORD
  if (self->ota_ == nullptr)
    return send_json(req, "404 Not Found", "{\"error\":\"this build has no ota_id\"}");
  bool force = false;
  char query[32] = {0}, val[4] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "force", val, sizeof(val)) == ESP_OK)
    force = strcmp(val, "1") == 0;
  const bool verified = self->hash_image_() && self->image_sha256_ == self->expected_sha256_;
  if (!verified && !force)
    return send_json(req, "409 Conflict",
                     "{\"error\":\"the dump slot does not hash to expected_sha256; OTA stays locked\","
                     "\"hint\":\"only use ?force=1 after the downloaded image passed tools/verify_camera_dump.py\"}");
  self->ota_unlock_pending_ = true;  // applied on the main loop
  App.wake_loop_threadsafe();
  ESP_LOGW(TAG, "OTA unlock requested (%s). The next OTA OVERWRITES slot %s.", verified ? "verified" : "FORCED",
           self->dump_part_ != nullptr ? self->dump_part_->label : "?");
  return send_json(req, "200 OK",
                   std::string("{\"ota\":\"unlocked\",\"verified\":") + (verified ? "true" : "false") +
                       ",\"warning\":\"the next OTA erases the dump slot\"}");
#else
  return send_json(req, "404 Not Found", "{\"error\":\"this build has no OTA\"}");
#endif
}

void FlashDumper::loop() {
#ifdef USE_OTA_PASSWORD
  if (this->ota_unlock_pending_.exchange(false) && this->ota_ != nullptr) {
    this->ota_->set_auth_password(this->password_);
    this->ota_unlocked_ = true;
    ESP_LOGW(TAG, "OTA unlocked with the dumper password");
  }
#endif
  if (!this->link_version_.empty())
    this->link_loop_();
  if (!this->server_started_ && wifi::global_wifi_component->is_connected())
    this->start_server_();
}

void FlashDumper::on_shutdown() {
  if (this->server_ != nullptr)
    httpd_stop(this->server_);
}

void FlashDumper::dump_config() {
  ESP_LOGCONFIG(TAG, "Flash dumper (read-only, HTTP Basic on port %u: /dump/info, /dump/app)", this->port_);
  for (size_t i = 0; i < this->apps_.size(); i++)
    ESP_LOGCONFIG(TAG, "  %-8s 0x%06" PRIx32 " %7" PRIu32 " B%s%s", this->apps_[i].label.c_str(), this->apps_[i].address,
                  this->apps_[i].size, (int) i == this->choice_.running ? "  (running)" : "",
                  (int) i == this->choice_.index ? "  (dump slot)" : "");
  if (this->choice_.result != Pick::OK) {
    ESP_LOGCONFIG(TAG, "  Nothing to dump: %s", pick_str(this->choice_.result));
  } else if (!this->image_.ok) {
    ESP_LOGCONFIG(TAG, "  Dump slot image: %s", this->image_.error);
  } else {
    ESP_LOGCONFIG(TAG, "  Dump slot image: %" PRIu32 " B, %u segments%s", this->image_.length, this->image_.segments,
                  this->image_.hash_appended ? ", SHA-256 appended" : "");
  }
  if (!this->link_version_.empty()) {
    ESP_LOGCONFIG(TAG, "  Chamber link: automyco-camera/%s", this->link_version_.c_str());
  }
}

// ---- Chamber link: the stock {"ssid","password"} handover on UART0 (same protocol as chamber_link) ----

void FlashDumper::link_send_(const std::string &line) {
  const std::string out = "\n" + line + "\n";  // leading newline ends any half-written log line
  uart_write_bytes(static_cast<uart_port_t>(this->link_uart_), out.data(), out.size());
}

void FlashDumper::link_loop_() {
  size_t available = 0;
  uart_get_buffered_data_len(static_cast<uart_port_t>(this->link_uart_), &available);
  uint8_t buf[64];
  while (available > 0) {
    const int n = uart_read_bytes(static_cast<uart_port_t>(this->link_uart_), buf, std::min(available, sizeof(buf)), 0);
    if (n <= 0)
      break;
    available -= n;
    for (int i = 0; i < n; i++) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n') {
        if (!this->link_overflow_)
          this->link_line_();
        std::fill(this->link_rx_.begin(), this->link_rx_.end(), '\0');
        this->link_rx_.clear();
        this->link_overflow_ = false;
      } else if (c != '\r') {
        if (this->link_rx_.size() < LINK_MAX_LINE)
          this->link_rx_.push_back(c);
        else
          this->link_overflow_ = true;
      }
    }
  }

  const bool connected = wifi::global_wifi_component->is_connected();
  if (connected && !this->link_was_connected_) {
    for (auto &ip : wifi::global_wifi_component->get_ip_addresses()) {
      if (ip.is_ip4()) {
        char addr[network::IP_ADDRESS_BUFFER_SIZE];
        ip.str_to(addr);
        this->link_send_(std::string("{\"status\":\"online\",\"ip\":\"") + addr + "\"}");
        break;
      }
    }
  }
  this->link_was_connected_ = connected;
}

void FlashDumper::link_line_() {
  const size_t start = this->link_rx_.find('{');
  if (start == std::string::npos)
    return;
  std::string ssid, password, cmd;
  bool has_wifi = false;
  json::parse_json(this->link_rx_.substr(start), [&](JsonObject root) -> bool {
    if (root["ssid"].is<const char *>()) {
      has_wifi = true;
      ssid = root["ssid"].as<const char *>();
      password = root["password"] | "";
    }
    cmd = root["cmd"] | "";
    return true;
  });
  if (has_wifi) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
      ESP_LOGW(TAG, "Ignored an invalid Wi-Fi network from the chamber");
    } else {
      const auto current = wifi::global_wifi_component->get_sta();
      if (ssid != current.get_ssid().c_str() || password != current.get_password().c_str()) {
        ESP_LOGI(TAG, "Wi-Fi network received from the chamber; connecting");  // values never logged
        wifi::global_wifi_component->save_wifi_sta(ssid, password);
      }
    }
    std::fill(password.begin(), password.end(), '\0');
    return;
  }
  if (cmd == "status") {
    this->link_send_("{\"status\":\"ready\",\"version\":\"automyco-camera/" + this->link_version_ + "\"}");
    if (wifi::global_wifi_component->is_connected())
      this->link_was_connected_ = false;  // re-announce the address on the next loop
  }
}

}  // namespace flash_dumper
}  // namespace esphome
