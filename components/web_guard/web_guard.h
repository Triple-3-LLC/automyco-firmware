#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "control_key.h"

namespace esphome::web_guard {

// Why a request was refused (or ALLOW). Order is the order the checks run in.
enum class Verdict : uint8_t {
  ALLOW,
  BAD_HOST,         // Host is not this node (DNS rebinding) -> 403
  PREFLIGHT,        // OPTIONS: never grant a CORS preflight -> 403
  CROSS_ORIGIN,     // Origin present and not this node -> 403
  GET_ACTION,       // GET /<domain>/<name>/<action>: actions are POST-only -> 405
  MISSING_HEADER,   // POST with neither a same-origin Origin nor X-Requested-With -> 403
  KEY_REQUIRED,     // change to a keyed entity without a valid X-AutoMyco-Key, once a key is set -> 403
  UNAVAILABLE,      // the same, but the owner password and challenge haven't loaded yet (boot) -> 503
};

// Request filter that sits in front of ESPHome's web_server (and its Basic/Digest auth). It is
// registered as the FIRST handler on the shared web_server_base, so every request reaches
// canHandle() here before any other handler: canHandle() returns true only for requests it refuses,
// which it then answers itself; everything else falls through to web_server untouched.
//
// It also replaces the `Access-Control-Allow-Origin: *` header that web_server_base adds to every
// response, so no cross-origin page can read anything the node sends back.
//
// See docs/device-api.md "Security" for the threat model.
class WebGuard : public Component, public AsyncWebHandler {
 public:
  explicit WebGuard(web_server_base::WebServerBase *base) : base_(base) {}

  void add_allowed_host(const char *host) { this->allowed_hosts_.push_back(host); }
  // An entity whose state changes need the control key once one is set (keyed_entities:). `name` and
  // `object_id` lowercase; `open_action` is the one action that stays open ("turn_off" for UV-C, "" none).
  void add_keyed_entity(const char *name, const char *object_id, const char *open_action) {
    this->keyed_.push_back({name, object_id, open_action});
  }
  // Loads (boot) or replaces the owner OTA password (control_key.h), with a fresh random challenge. Call it
  // once the radio is up (real entropy). Until the first call keyed entities are refused (503 starting up);
  // "" means nobody has paired, and they stay open. Called from the main loop, read on the httpd task, hence
  // the lock.
  void set_owner_password(const std::string &password);
  bool has_control_key() const;
#ifdef USE_TEXT_SENSOR
  // The Control Challenge text sensor: the app signs keyed requests over its current value.
  void set_challenge_sensor(text_sensor::TextSensor *sensor) { this->challenge_sensor_ = sensor; }
#endif
  // Publishes a rotated challenge on the main loop (it rotates on the httpd task).
  void loop() override;

  void setup() override;
  void dump_config() override;
  // After wifi (250) and before web_server (WIFI - 1), so this handler is registered first.
  float get_setup_priority() const override { return setup_priority::WIFI - 0.5f; }

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  // The verdict for a request. Not pure: an accepted X-AutoMyco-Key uses up the challenge.
  Verdict check(AsyncWebServerRequest *request) const;

 protected:
  bool host_allowed_(const std::string &host) const;
  static void scrub_default_headers_();

  bool is_keyed_(AsyncWebServerRequest *request) const;
  KeyCheck key_check_(AsyncWebServerRequest *request) const;

  web_server_base::WebServerBase *base_;
  std::vector<const char *> allowed_hosts_;
  std::vector<KeyedEntity> keyed_;
  mutable Mutex key_lock_;
  mutable KeyGate gate_;          // key, load state and single-use challenge (RAM only; guarded by key_lock_)
  mutable std::atomic<bool> publish_challenge_{false};   // set on the httpd task, published in loop()
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *challenge_sensor_{nullptr};
#endif
  uint32_t challenge_since_ms_{0};   // millis() the current challenge was published (main loop only)
  mutable KeyCheck last_check_{KeyCheck::OK};  // for the refusal log (httpd task only)
};

}  // namespace esphome::web_guard
