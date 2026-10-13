#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include <esp_partition.h>

#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

#include "history_log.h"

namespace esphome::chamber_history {

using ::history_log::Ring;
using ::history_log::Sample;

// The flash the ring lives in: one data partition, raw (no filesystem), accessed through esp_partition_*.
class PartitionFlash : public ::history_log::Flash {
 public:
  explicit PartitionFlash(const esp_partition_t *part) : part_(part) {}
  bool read(uint32_t offset, void *dst, size_t len) override;
  bool write(uint32_t offset, const void *src, size_t len) override;
  bool erase_sector(uint32_t offset) override;

 protected:
  const esp_partition_t *part_;
};

// On-chamber climate history (docs/device-api.md "Climate history"): one record per 5 minutes in an unused
// data partition, served over the LAN as CSV at GET /history?since=<epoch>.
//
// Sampling: temperature, humidity and CO2 are averaged over the interval (every reading the sensors
// publish), setpoints and phase are taken at its end, heater and mist duty are integrated in loop(). An
// interval closes at each wall-clock 5-minute boundary (12:00, 12:05, ...) once the clock is set. Until it
// is, intervals run 5 minutes by uptime and are held in RAM (up to PENDING_MAX) to be written with their
// real times when the clock arrives; a power cut before then loses them.
class ChamberHistory : public Component, public AsyncWebHandler {
 public:
  static constexpr uint32_t INTERVAL_S = 300;
  static constexpr size_t PENDING_MAX = 72;  // 6 h of intervals without a clock

  explicit ChamberHistory(web_server_base::WebServerBase *base) : base_(base) {}

  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
  void set_temperature(sensor::Sensor *s) { this->temperature_ = s; }
  void set_humidity(sensor::Sensor *s) { this->humidity_ = s; }
  void set_co2(sensor::Sensor *s) { this->co2_ = s; }
  void set_temperature_target(number::Number *n) { this->temperature_target_ = n; }
  void set_humidity_target(number::Number *n) { this->humidity_target_ = n; }
  void set_heater_duty(std::function<float()> &&f) { this->heater_duty_ = std::move(f); }
  void set_mist_duty(std::function<float()> &&f) { this->mist_duty_ = std::move(f); }
  void set_phase(std::function<uint8_t()> &&f) { this->phase_ = std::move(f); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After web_guard (WIFI - 0.5), so the guard stays the first request handler.
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

 protected:
  struct Accumulator {
    uint32_t start_ms{0};
    uint32_t last_ms{0};
    uint32_t start_epoch{0};  // 0 = the clock wasn't set when the interval began
    float temp_sum{0}, rh_sum{0}, co2_sum{0};
    uint16_t temp_n{0}, rh_n{0}, co2_n{0};
    float heat_ms{0}, mist_ms{0};  // duty-weighted on-time
  };
  struct Pending {
    uint32_t start_ms;
    Sample sample;  // time unset
  };

  uint32_t now_epoch_() const;
  void begin_interval_(uint32_t now_ms, uint32_t epoch);
  void close_interval_(uint32_t now_ms, uint32_t epoch);
  void flush_pending_(uint32_t now_ms, uint32_t epoch);
  void write_(const Sample &s);
  void serve_(AsyncWebServerRequest *request);  // the CSV stream itself, once the gate let it through

  web_server_base::WebServerBase *base_;
  time::RealTimeClock *clock_{nullptr};
  sensor::Sensor *temperature_{nullptr};
  sensor::Sensor *humidity_{nullptr};
  sensor::Sensor *co2_{nullptr};
  number::Number *temperature_target_{nullptr};
  number::Number *humidity_target_{nullptr};
  std::function<float()> heater_duty_;
  std::function<float()> mist_duty_;
  std::function<uint8_t()> phase_;

  const esp_partition_t *part_{nullptr};
  const char *layout_{"none"};
  PartitionFlash *flash_{nullptr};
  Ring *ring_{nullptr};
  std::atomic<bool> ready_{false};
  Accumulator acc_;
  std::vector<Pending> pending_;
  ::history_log::StreamGate stream_gate_;  // one /history stream at a time, then a cooldown (history_log.h)
  uint32_t write_errors_{0};
  uint32_t last_check_ms_{0};
};

}  // namespace esphome::chamber_history
