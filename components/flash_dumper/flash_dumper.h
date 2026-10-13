#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <esp_http_server.h>
#include <esp_partition.h>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#include "dump_logic.h"

#ifdef USE_OTA_PASSWORD
#include "esphome/components/esphome/ota/ota_esphome.h"
#endif

namespace esphome {
namespace flash_dumper {

using namespace ::dump_logic;  // NOLINT: the pure helpers (dump_logic.h)

// Read-only stock-firmware dumper for the TS-01 camera board (docs/camera-donor-dump.md).
//
// Serves, behind HTTP Basic auth, the app partition the chip is NOT running: on a donor camera that
// took this firmware through the stock updater, that slot still holds the stock camera image.
//   GET  /dump/info        JSON: partition table, running slot, the dump slot's image length, its
//                          SHA-256 (computed here), the appended SHA-256 and whether it checks out
//   GET  /dump/app         raw bytes of the dump slot, trimmed to the image length (?trim=0: whole slot)
//   POST /dump/ota-unlock  only with an ota_id: swaps the random OTA lock for the configured password,
//                          once the dump slot's SHA-256 matches expected_sha256 (?force=1 skips that)
//
// THIS COMPONENT NEVER WRITES FLASH. No OTA calls, no erase, no partition writes, no boot-partition
// change: flash_dumper.cpp poisons those identifiers, so adding one is a compile error, and the host
// test (esphome/tests/test_flash_dumper.cpp) scans the sources as well.
//
// Optional chamber_link_version: the camera end of the chamber's camera link (UART0, the stock
// {"ssid","password"} handover), so the dumper gets Wi-Fi from the chamber exactly like the AutoMyco
// camera does, without pulling in the camera stack. It reports automyco-camera/<version>.
class FlashDumper : public Component {
 public:
  void set_port(uint16_t port) { this->port_ = port; }
  void set_credentials(const std::string &username, const std::string &password);
  void set_expected_sha256(const std::string &hex) { this->expected_sha256_ = hex; }
  void set_link_version(const std::string &version) { this->link_version_ = version; }
#ifdef USE_OTA_PASSWORD
  void set_ota(::esphome::ESPHomeOTAComponent *ota) { this->ota_ = ota; }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  // Before Wi-Fi and OTA: the OTA lock must be in place before the OTA server starts, and the camera-link
  // loop has to run while Wi-Fi waits, since it is what receives the Wi-Fi network from the chamber.
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  static constexpr size_t AUTH_HEADER_MAX = 160;
  static constexpr size_t CHUNK = 4096;

  static esp_err_t info_handler_(httpd_req_t *req);
  static esp_err_t app_handler_(httpd_req_t *req);
  static esp_err_t unlock_handler_(httpd_req_t *req);
  bool authorized_(httpd_req_t *req);
  void scan_partitions_();
  bool hash_image_();  // fills image_sha256_ / appended_*; httpd task only
  std::string info_json_();
  void lock_ota_();
  void start_server_();

  // Chamber link (Wi-Fi handover).
  void link_loop_();
  void link_line_();
  void link_send_(const std::string &line);

  uint16_t port_{80};
  std::string username_;
  std::string password_;
  char expected_auth_[AUTH_HEADER_MAX]{};
  std::string expected_sha256_;
  httpd_handle_t server_{nullptr};
  bool server_started_{false};
  bool login_ok_{false};  // a >= 16-char password is configured; nothing is served otherwise

  std::vector<AppSlot> apps_;
  std::vector<const esp_partition_t *> app_parts_;
  Choice choice_;
  const esp_partition_t *dump_part_{nullptr};
  ImageInfo image_;

  bool hashed_{false};
  bool appended_ok_{false};
  std::string image_sha256_;
  std::string appended_sha256_;

  std::atomic<bool> ota_unlock_pending_{false};
  std::atomic<bool> ota_unlocked_{false};
#ifdef USE_OTA_PASSWORD
  ::esphome::ESPHomeOTAComponent *ota_{nullptr};
#endif

  std::string link_version_;
  int link_uart_{0};
  std::string link_rx_;
  bool link_overflow_{false};
  bool link_was_connected_{false};
};

}  // namespace flash_dumper
}  // namespace esphome
