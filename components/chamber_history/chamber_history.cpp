#include "chamber_history.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::chamber_history {

static const char *const TAG = "chamber_history";

// The partitions the history may use, matched exactly (label, subtype, address, size). Anything else, and
// the component stays off: it never writes outside one of these.
//   stock    the Terrashroom stock table's 'storage' SPIFFS (chambers installed over the stock firmware keep
//            that table: an OTA update can't change it). The stock firmware is gone on these chambers, so
//            its old contents are dead; each sector is erased only when the ring first reaches it.
//   esphome  ESPHome's own 4 MB table (chambers flashed over USB): the 60 KiB 'spiffs' partition, which
//            nothing in this firmware mounts or uses.
// Never: nvs, otadata, phy_init, the app slots, eeprom, or coredump (0x3F0000 on the stock table).
struct KnownLayout {
  const char *name;
  const char *label;
  uint32_t address;  // 0 = any (ESPHome places it after the app slots)
  uint32_t size;
};
static const KnownLayout LAYOUTS[] = {
    {"stock", "storage", 0x370000, 0x80000},
    {"esphome", "spiffs", 0, 0xF000},
};

bool PartitionFlash::read(uint32_t offset, void *dst, size_t len) {
  return esp_partition_read(this->part_, offset, dst, len) == ESP_OK;
}
bool PartitionFlash::write(uint32_t offset, const void *src, size_t len) {
  return esp_partition_write(this->part_, offset, src, len) == ESP_OK;
}
bool PartitionFlash::erase_sector(uint32_t offset) {
  return esp_partition_erase_range(this->part_, offset, ::history_log::SECTOR_SIZE) == ESP_OK;
}

void ChamberHistory::setup() {
  for (const auto &layout : LAYOUTS) {
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, layout.label);
    if (p == nullptr || p->size != layout.size || (layout.address != 0 && p->address != layout.address) ||
        p->encrypted)
      continue;
    this->part_ = p;
    this->layout_ = layout.name;
    break;
  }
  // GET /history goes through web_server's own auth when a build sets one (the chambers don't: their LAN
  // API is open by design), and web_guard sees it first like every request. Registered even when history
  // is off, so the app gets a clear 503 instead of a 404 (which means "older firmware").
  this->base_->add_handler(this);
  if (this->part_ == nullptr) {
    ESP_LOGW(TAG, "No known history partition in this partition table: history off");
    this->status_set_warning();
    return;
  }
  this->flash_ = new PartitionFlash(this->part_);  // NOLINT(cppcoreguidelines-owning-memory)
  this->ring_ = new Ring(this->flash_, this->part_->size);  // NOLINT(cppcoreguidelines-owning-memory)
  if (!this->ring_->begin()) {
    ESP_LOGW(TAG, "Couldn't read the history partition: history off");
    this->status_set_warning();
    return;
  }
  this->ready_ = true;

  // Interval means: every reading the sensors publish. NaN (no reading, sensor fault) doesn't count.
  if (this->temperature_ != nullptr) {
    this->temperature_->add_on_state_callback([this](float x) {
      if (std::isfinite(x)) {
        this->acc_.temp_sum += x;
        this->acc_.temp_n++;
      }
    });
  }
  if (this->humidity_ != nullptr) {
    this->humidity_->add_on_state_callback([this](float x) {
      if (std::isfinite(x)) {
        this->acc_.rh_sum += x;
        this->acc_.rh_n++;
      }
    });
  }
  if (this->co2_ != nullptr) {
    this->co2_->add_on_state_callback([this](float x) {
      if (std::isfinite(x)) {
        this->acc_.co2_sum += x;
        this->acc_.co2_n++;
      }
    });
  }

  this->begin_interval_(millis(), this->now_epoch_());
}

uint32_t ChamberHistory::now_epoch_() const {
  if (this->clock_ == nullptr)
    return 0;
  const auto now = this->clock_->now();
  if (!now.is_valid() || now.timestamp < static_cast<time_t>(::history_log::MIN_VALID_TIME))
    return 0;
  return static_cast<uint32_t>(now.timestamp);
}

void ChamberHistory::begin_interval_(uint32_t now_ms, uint32_t epoch) {
  this->acc_ = Accumulator{};
  this->acc_.start_ms = now_ms;
  this->acc_.last_ms = now_ms;
  this->acc_.start_epoch = epoch;
}

void ChamberHistory::loop() {
  if (!this->ready_)
    return;
  const uint32_t now = millis();
  // Duty: integrate what the heater and atomizer are doing, every loop (the mist pulser switches every
  // 250 ms, so a once-a-second sample would alias). A long stall (OTA write, flash erase) counts as nothing.
  const uint32_t dt = now - this->acc_.last_ms;
  this->acc_.last_ms = now;
  if (dt > 0 && dt < 10000) {
    if (this->heater_duty_) {
      const float d = this->heater_duty_();
      if (d > 0)
        this->acc_.heat_ms += dt * (d > 1 ? 1.0f : d);
    }
    if (this->mist_duty_) {
      const float d = this->mist_duty_();
      if (d > 0)
        this->acc_.mist_ms += dt * (d > 1 ? 1.0f : d);
    }
  }

  // The interval check, once a second.
  if (now - this->last_check_ms_ < 1000)
    return;
  this->last_check_ms_ = now;
  const uint32_t epoch = this->now_epoch_();
  if (epoch != 0 && !this->pending_.empty())
    this->flush_pending_(now, epoch);
  const uint32_t elapsed_s = (now - this->acc_.start_ms) / 1000;
  bool close;
  if (epoch != 0) {
    // Close at each wall-clock boundary (an interval that began before the clock was set is placed by uptime).
    const uint32_t start = this->acc_.start_epoch != 0 ? this->acc_.start_epoch : epoch - elapsed_s;
    close = epoch / INTERVAL_S != start / INTERVAL_S || elapsed_s >= INTERVAL_S;
  } else {
    close = elapsed_s >= INTERVAL_S;
  }
  if (close) {
    this->close_interval_(now, epoch);
    this->begin_interval_(now, epoch);
  }
}

void ChamberHistory::close_interval_(uint32_t now_ms, uint32_t epoch) {
  const uint32_t elapsed_ms = now_ms - this->acc_.start_ms;
  if (elapsed_ms < 30000)
    return;  // the stub of an interval right after boot or a clock step: not worth a record
  const Accumulator &a = this->acc_;
  Sample s;
  if (a.temp_n > 0)
    s.temperature = a.temp_sum / a.temp_n;
  if (a.rh_n > 0)
    s.humidity = a.rh_sum / a.rh_n;
  if (a.co2_n > 0)
    s.co2 = a.co2_sum / a.co2_n;
  if (this->temperature_target_ != nullptr && this->temperature_target_->has_state())
    s.temperature_target = this->temperature_target_->state;
  if (this->humidity_target_ != nullptr && this->humidity_target_->has_state())
    s.humidity_target = this->humidity_target_->state;
  auto pct = [elapsed_ms](float on_ms) {
    const long p = std::lround(100.0f * on_ms / elapsed_ms);
    return static_cast<uint8_t>(p > 100 ? 100 : (p < 0 ? 0 : p));
  };
  if (this->heater_duty_)
    s.heater_pct = pct(a.heat_ms);
  if (this->mist_duty_)
    s.mist_pct = pct(a.mist_ms);
  if (this->phase_)
    s.phase = this->phase_();
  if (a.start_epoch != 0) {
    s.time = a.start_epoch;
  } else if (epoch != 0) {
    s.time = epoch - elapsed_ms / 1000;
  } else {
    // No clock yet: keep it until there is one.
    if (this->pending_.size() >= PENDING_MAX)
      this->pending_.erase(this->pending_.begin());
    this->pending_.push_back(Pending{a.start_ms, s});
    return;
  }
  this->write_(s);
}

void ChamberHistory::flush_pending_(uint32_t now_ms, uint32_t epoch) {
  size_t written = 0;
  for (auto &p : this->pending_) {
    const uint32_t age_s = (now_ms - p.start_ms) / 1000;
    if (age_s >= epoch)
      continue;
    p.sample.time = epoch - age_s;
    this->write_(p.sample);
    written++;
  }
  ESP_LOGI(TAG, "Clock set: wrote %u interval(s) recorded before it", static_cast<unsigned>(written));
  this->pending_.clear();
  this->pending_.shrink_to_fit();
}

void ChamberHistory::write_(const Sample &s) {
  if (!this->ring_->append(s)) {
    this->write_errors_++;
    ESP_LOGW(TAG, "Couldn't write a history record (%" PRIu32 " so far)", this->write_errors_);
    return;
  }
  ESP_LOGD(TAG, "Recorded %" PRIu32 ": %.1f F %.1f %% heat %u%% mist %u%% (sector %" PRIu32 " slot %" PRIu32 ")",
           s.time, s.temperature, s.humidity, s.heater_pct, s.mist_pct, this->ring_->current_sector(),
           this->ring_->next_slot());
}

void ChamberHistory::dump_config() {
  ESP_LOGCONFIG(TAG, "Climate history:");
  if (this->part_ == nullptr) {
    ESP_LOGCONFIG(TAG, "  Off: no known history partition");
    return;
  }
  ESP_LOGCONFIG(TAG, "  Partition: '%s' (%s layout) at 0x%06" PRIX32 ", %" PRIu32 " KiB, %" PRIu32 " sectors",
                this->part_->label, this->layout_, this->part_->address, this->part_->size / 1024,
                this->ring_->sectors());
  ESP_LOGCONFIG(TAG, "  Capacity: %.1f days at one record per %" PRIu32 " s",
                ::history_log::capacity_days(this->part_->size, INTERVAL_S), INTERVAL_S);
  ESP_LOGCONFIG(TAG, "  Writing sector %" PRIu32 " (seq %" PRIu32 "), next slot %" PRIu32, this->ring_->current_sector(),
                this->ring_->seq(), this->ring_->next_slot());
}

bool ChamberHistory::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(url_buf);
  return url == "/history";
}

namespace {

// Appends one CSV row. Unknown values are empty fields.
size_t format_row(char *out, size_t cap, const Sample &s) {
  char t[8], h[8], ts[6], hs[6], he[4], mi[4], co[6];
  auto num1 = [](char *b, size_t n, float v) {
    if (std::isnan(v))
      b[0] = '\0';
    else
      snprintf(b, n, "%.1f", v);
  };
  auto num0 = [](char *b, size_t n, float v) {
    if (std::isnan(v))
      b[0] = '\0';
    else
      snprintf(b, n, "%d", static_cast<int>(std::lround(v)));
  };
  auto pct = [](char *b, size_t n, uint8_t v) {
    if (v == Sample::UNKNOWN_PCT)
      b[0] = '\0';
    else
      snprintf(b, n, "%u", v);
  };
  num1(t, sizeof(t), s.temperature);
  num1(h, sizeof(h), s.humidity);
  num0(ts, sizeof(ts), s.temperature_target);
  num0(hs, sizeof(hs), s.humidity_target);
  pct(he, sizeof(he), s.heater_pct);
  pct(mi, sizeof(mi), s.mist_pct);
  num0(co, sizeof(co), s.co2);
  const int n = snprintf(out, cap, "%" PRIu32 ",%s,%s,%s,%s,%s,%s,%s,%u\n", s.time, t, h, ts, hs, he, mi, co, s.phase);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

}  // namespace

// Streams the history as CSV in chunks, oldest first, from flash: the response is never held in RAM (a
// 1 KiB line buffer and a 340-byte read buffer). Runs on the web server's task; reading flash while the
// main loop appends is safe (history_log.h, Ring::for_each).
void ChamberHistory::handleRequest(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!this->ready_) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, this->part_ == nullptr ? "history unavailable: no history partition"
                                                   : "history unavailable: partition unreadable");
    return;
  }
  uint32_t retry_after_s = 0;
  if (!this->stream_gate_.admit(millis(), retry_after_s)) {
    char retry[12];
    snprintf(retry, sizeof(retry), "%" PRIu32, retry_after_s);
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_hdr(req, "Retry-After", retry);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "history busy: try again shortly");
    return;
  }
  this->serve_(request);
  this->stream_gate_.finish(millis());
}

void ChamberHistory::serve_(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  uint32_t since = 0;
  const std::string arg = request->arg("since");
  if (!arg.empty())
    since = static_cast<uint32_t>(strtoul(arg.c_str(), nullptr, 10));

  constexpr size_t OUT_CAP = 1024;
  constexpr size_t READ_CAP = ::history_log::RECORD_SIZE * 34;
  std::unique_ptr<char[]> out(new (std::nothrow) char[OUT_CAP]);
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[READ_CAP]);
  if (!out || !buf) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "history unavailable: low memory");
    return;
  }
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "text/csv");
  int len = snprintf(out.get(), OUT_CAP,
                     "# automyco-history 1 layout=%s interval=%" PRIu32 " capacity_days=%.1f now=%" PRIu32 "\n"
                     "time,temperature_f,humidity_pct,temperature_target_f,humidity_target_pct,heater_pct,"
                     "mist_pct,co2_ppm,phase\n",
                     this->layout_, INTERVAL_S, ::history_log::capacity_days(this->part_->size, INTERVAL_S),
                     this->now_epoch_());
  size_t used = len > 0 ? static_cast<size_t>(len) : 0;
  bool sock_ok = true;
  uint32_t chunks = 0;
  const bool read_ok = this->ring_->for_each(since, buf.get(), READ_CAP, [&](const Sample &s) {
    if (!sock_ok)
      return;
    if (OUT_CAP - used < 80) {
      sock_ok = httpd_resp_send_chunk(req, out.get(), used) == ESP_OK;
      used = 0;
      if (++chunks % 16 == 0)
        vTaskDelay(1);  // let the other tasks (and the main loop's flash access) in
    }
    used += format_row(out.get() + used, OUT_CAP - used, s);
  });
  if (sock_ok && used > 0)
    sock_ok = httpd_resp_send_chunk(req, out.get(), used) == ESP_OK;
  if (sock_ok)
    httpd_resp_send_chunk(req, nullptr, 0);
  if (!read_ok)
    ESP_LOGW(TAG, "History read error while serving /history");
}

}  // namespace esphome::chamber_history
