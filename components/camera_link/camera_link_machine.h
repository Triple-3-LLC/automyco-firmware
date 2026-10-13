#pragma once
// The camera install state machine, with no ESPHome dependencies so it runs in host tests
// (esphome/tests/test_camera_link.cpp). CameraLink (camera_link.cpp) feeds it lines from the camera
// link and the clock, and carries out what it asks through Io.
//
// One round: power-cycle -> ready line -> send Wi-Fi -> "Connected to WiFi" -> {"ota":...} -> download
// -> reboot -> AutoMyco ready line -> send Wi-Fi -> online. Stock camera Wi-Fi is unreliable (65-90%
// packet loss on the bench, 2026-10-09), so a failed round power-cycles and starts again, up to
// MAX_ROUNDS. Protocol and states: docs/camera-install.md.
//
// An AutoMyco camera (camera_update, 1.1.0+) takes the same {"ota":...} and fetches the image itself, so
// it skips the Wi-Fi steps: if it said ready since its last power-up, the command goes straight out with
// no power cycle; otherwise the round's ready line triggers it. It answers with the stock updater's
// "Starting OTA update" / "Update successfully completed" lines, and a refusal or failure as
// {"status":"ota","state":"failed","error":"..."} (docs/camera-install.md "Updating an AutoMyco camera").

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>

namespace camera_install {

struct Io {
  std::function<void(bool)> power;            // camera power switch
  std::function<void(const std::string &)> send;  // one line to the camera (newline added by the caller)
  std::function<bool()> wifi_connected;       // the chamber itself is on Wi-Fi
  std::function<bool()> send_wifi;            // send the chamber's network; false if there's none
  std::function<void(const std::string &)> status;
  std::function<void(const std::string &)> firmware;
  std::function<void(const std::string &)> address;
  std::function<void(const std::string &)> event;  // one Activity log entry (may be empty)
};

// The value of "key":"value" in a one-line JSON object (no escapes needed: versions, IPs, states).
inline std::string json_string(const std::string &line, const char *key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  size_t at = line.find(needle);
  if (at == std::string::npos)
    return "";
  at += needle.size();
  const size_t end = line.find('"', at);
  return end == std::string::npos ? "" : line.substr(at, end - at);
}

// Text from the camera that ends up in the Event Log: letters, digits and simple punctuation only,
// at most 60 characters, so a misbehaving camera can't write control characters or a wall of text
// into the owner's log.
inline std::string log_safe(const std::string &in) {
  std::string out;
  for (char c : in) {
    if (out.size() >= 60)
      break;
    if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '.' || c == ',' || c == ':' || c == '-' ||
        c == '(' || c == ')')
      out += c;
  }
  return out;
}

inline bool valid_version(const std::string &v) {
  if (v.empty() || v.size() > 24)
    return false;
  for (char c : v) {
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'))
      return false;
  }
  return true;
}

inline bool valid_hash(const std::string &h) {
  if (h.size() != 64)
    return false;
  for (char c : h) {
    if (!(isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

class InstallMachine {
 public:
  static constexpr const char *AUTOMYCO_PREFIX = "automyco-camera/";
  static constexpr uint8_t MAX_ROUNDS = 4;
  static constexpr uint32_t INSTALL_TIMEOUT_MS = 15 * 60 * 1000;
  // A wedged OV2640 only recovers with its power removed (a soft reboot keeps it powered), seen on
  // the bench 2026-10-09. 10 s off, like the fix that worked by hand.
  static constexpr uint32_t POWER_CYCLE_OFF_MS = 10000;
  // Backoff between automatic power cycles, so a camera that never recovers isn't cycled forever.
  static constexpr uint32_t AUTO_CYCLE_DELAYS_MS[4] = {0, 5 * 60 * 1000, 15 * 60 * 1000, 60 * 60 * 1000};
  static constexpr uint32_t AUTO_CYCLE_RESET_MS = 2 * 60 * 60 * 1000;  // quiet this long = start over
  static constexpr uint32_t MANUAL_CYCLE_GAP_MS = 2 * 60 * 1000;  // the button: at most once per 2 min

  enum class Step {
    IDLE,
    POWER_OFF,      // camera power off, waiting before switching it back on
    WAIT_READY,     // powered on, waiting for the camera's ready line
    SEND_WIFI,      // stock camera is up; its command reader starts a moment after the ready line
    WAIT_WIFI,      // Wi-Fi sent, waiting for "Connected to WiFi"
    WAIT_OTA_SEND,  // stock camera on Wi-Fi; short settle before the ota command
    REQUESTED,      // ota command sent, waiting for the download to start
    DOWNLOADING,
    RESTARTING,     // "Update successfully completed", waiting for the AutoMyco ready line
    WAIT_ONLINE,    // AutoMyco camera has Wi-Fi, waiting for its address
  };

  explicit InstallMachine(Io io) : io_(std::move(io)) {}

  Step step() const { return this->step_; }
  uint8_t round() const { return this->round_; }
  bool power_cycling() const { return this->cycling_; }
  bool installing() const { return this->step_ != Step::IDLE; }
  bool power_cycles_deferred() const { return this->deferred_; }

  // Hold off power cycles while the chamber's self-test measures the 24 V rail: a camera switching
  // off or booting moves the rail more than the blower's whole +6 mA signal. A request that arrives
  // meanwhile is dropped without touching the backoff; the camera's watchdog asks again later.
  void defer_power_cycles(bool defer) { this->deferred_ = defer; }

  // Hard power cycle: camera power off for POWER_CYCLE_OFF_MS, then on. `manual` (the Camera Power
  // Cycle button) runs at most once per MANUAL_CYCLE_GAP_MS; automatic requests (the camera's frame
  // watchdog) are backed off further. Never during an install, which power-cycles the camera itself,
  // and never while deferred (the self-test).
  bool power_cycle(const std::string &reason, uint32_t now, bool manual) {
    if (this->step_ != Step::IDLE || this->cycling_ || this->deferred_)
      return false;
    if (manual) {
      if (this->manual_cycled_ && now - this->last_manual_cycle_ < MANUAL_CYCLE_GAP_MS)
        return false;
      this->manual_cycled_ = true;
      this->last_manual_cycle_ = now;
    } else {
      if (this->auto_cycles_ > 0 && now - this->last_auto_cycle_ > AUTO_CYCLE_RESET_MS)
        this->auto_cycles_ = 0;
      const uint8_t idx = this->auto_cycles_ < 4 ? this->auto_cycles_ : 3;
      if (this->auto_cycles_ > 0 && now - this->last_auto_cycle_ < AUTO_CYCLE_DELAYS_MS[idx])
        return false;  // backed off; the camera asks again later
      this->last_auto_cycle_ = now;
      if (this->auto_cycles_ < 255)
        this->auto_cycles_++;
    }
    this->cycling_ = true;
    this->cycle_started_ = now;
    this->camera_version_.clear();
    this->io_.power(false);
    if (this->io_.event)
      this->io_.event("camera power-cycled: " + reason);
    return true;
  }

  // "<version> <sha256>" from the Camera Install text entity.
  void start(const std::string &request, uint32_t now) {
    const size_t space = request.find(' ');
    const std::string version = space == std::string::npos ? "" : request.substr(0, space);
    const std::string hash = space == std::string::npos ? "" : request.substr(space + 1);
    if (!valid_version(version) || !valid_hash(hash)) {
      this->io_.status("failed: invalid request");
      return;
    }
    if (this->step_ != Step::IDLE)
      return;  // already installing
    if (this->cycling_) {
      // A power cycle has the camera off right now; the install's own first round cuts the power too, so
      // starting now would double up the cuts and confuse the tick's "switch it back on" bookkeeping.
      this->io_.status("failed: camera power cycle running, try again in 10 s");
      return;
    }
    this->target_version_ = AUTOMYCO_PREFIX + version;
    this->target_hash_ = hash;
    this->round_ = 0;
    this->install_started_ = now;
    if (this->camera_version_ == this->target_version_) {
      this->finish_("already installed");
      return;
    }
    if (this->camera_version_.rfind(AUTOMYCO_PREFIX, 0) == 0) {
      // An AutoMyco camera that's up: it updates itself, no power cycle or Wi-Fi needed.
      this->round_ = 1;
      this->request_ota_(now);
      return;
    }
    this->begin_round_(now);
  }

  // The camera lost power (Camera Power off): what it said in its last ready line no longer holds.
  void camera_off() { this->camera_version_.clear(); }

  void tick(uint32_t now) {
    if (this->cycling_ && now - this->cycle_started_ >= POWER_CYCLE_OFF_MS) {
      this->cycling_ = false;
      this->io_.power(true);
    }
    if (this->step_ == Step::IDLE)
      return;
    if (now - this->install_started_ > INSTALL_TIMEOUT_MS) {
      this->finish_("failed: timed out");
      return;
    }
    if (now - this->step_started_ < this->step_timeout_)
      return;
    switch (this->step_) {  // the current step's time is up
      case Step::POWER_OFF:
        this->io_.power(true);
        this->set_step_(Step::WAIT_READY, 20000, now);
        break;
      case Step::WAIT_READY:
        this->round_failed_("camera not responding", now);
        break;
      case Step::SEND_WIFI:
        if (this->io_.send_wifi()) {
          this->set_step_(Step::WAIT_WIFI, 45000, now);
        } else {
          this->finish_("failed: no Wi-Fi to give the camera");
        }
        break;
      case Step::WAIT_WIFI:
        this->round_failed_("camera could not join Wi-Fi", now);
        break;
      case Step::WAIT_OTA_SEND:
        this->request_ota_(now);
        break;
      case Step::REQUESTED:
      case Step::DOWNLOADING:
        this->round_failed_("download kept failing", now);
        break;
      case Step::RESTARTING:
        this->finish_("failed: camera did not restart");
        break;
      case Step::WAIT_ONLINE:
        this->finish_("failed: camera could not join Wi-Fi");
        break;
      case Step::IDLE:
        break;
    }
  }

  void line(const std::string &line, uint32_t now) {
    // JSON status lines from either firmware. The stock camera also echoes our own lines back as
    // "=> {...}", which carry no "status" key.
    if (line.find('{') != std::string::npos && line.find("\"status\"") != std::string::npos) {
      const std::string status = json_string(line, "status");
      if (status == "ready") {
        const std::string version = json_string(line, "version");
        if (!version.empty())
          this->on_ready_(version, now);
      } else if (status == "power_cycle") {
        // The AutoMyco camera's frame watchdog: no valid frame for a while.
        const std::string reason = log_safe(json_string(line, "reason"));
        this->power_cycle(reason.empty() ? "camera asked" : "camera: " + reason, now, false);
      } else if (status == "ota") {
        // An AutoMyco camera updating itself (camera_update). Progress also comes as the stock lines below.
        if (json_string(line, "state") == "failed" &&
            (this->step_ == Step::REQUESTED || this->step_ == Step::DOWNLOADING)) {
          const std::string why = log_safe(json_string(line, "error"));
          this->finish_("failed: " + (why.empty() ? std::string("camera refused the update") : why));
        }
      } else if (status == "online") {
        const std::string ip = json_string(line, "ip");
        if (!ip.empty()) {
          this->io_.address(ip);
          if (this->step_ == Step::WAIT_ONLINE)
            this->finish_("installed");
        }
      }
      return;
    }

    // Plain-text log lines from the stock camera's updater (seen on the bench 2026-10-09).
    if (this->step_ == Step::WAIT_WIFI && line.find("Connected to WiFi") != std::string::npos) {
      this->set_step_(Step::WAIT_OTA_SEND, 2000, now);
    } else if (line.find("beginInternal()") != std::string::npos && line.find("/firmware_url") != std::string::npos) {
      this->last_http_code_ = 0;  // a new attempt: forget the previous attempt's status code
    } else if (line.find("handleHeaderResponse(): code: ") != std::string::npos) {
      this->last_http_code_ = atoi(line.c_str() + line.find("code: ") + 6);
    } else if (this->step_ == Step::REQUESTED && line.find("Got signed URL:") != std::string::npos) {
      const size_t after = line.find("Got signed URL:") + 15;
      const bool empty = line.find_first_not_of(" \t\r", after) == std::string::npos;
      // A 200 with no URL is the server saying no; anything else is the network, which retries.
      if (empty && this->last_http_code_ == 200)
        this->finish_("failed: firmware not offered");
    } else if (line.find("Starting OTA update") != std::string::npos) {
      if (this->step_ == Step::REQUESTED) {
        this->io_.status("downloading");
        this->set_step_(Step::DOWNLOADING, 5 * 60 * 1000, now);
      }
    } else if (line.find("Update successfully completed") != std::string::npos) {
      if (this->step_ == Step::DOWNLOADING || this->step_ == Step::REQUESTED) {
        this->io_.status("restarting camera");
        this->set_step_(Step::RESTARTING, 90000, now);
      }
    } else if (line.find("Maximum update attempts reached") != std::string::npos) {
      if (this->step_ == Step::REQUESTED || this->step_ == Step::DOWNLOADING)
        this->round_failed_("download kept failing", now);
    }
  }

 protected:
  void request_ota_(uint32_t now) {
    this->io_.send("{\"ota\":{\"version\":\"" + this->target_version_ + "\",\"hash\":\"" + this->target_hash_ +
                   "\"}}");
    this->io_.status("requesting firmware");
    // The stock camera tries 3 times, ~5 s apart, each with up to ~7 s of DNS timeout. An AutoMyco camera
    // waits up to 90 s for Wi-Fi and heap before it starts.
    this->set_step_(Step::REQUESTED, 120000, now);
  }

  void on_ready_(const std::string &version, uint32_t now) {
    this->camera_version_ = version;
    this->io_.firmware(version);
    this->io_.address("");  // until the camera reports it again
    const bool automyco = version.rfind(AUTOMYCO_PREFIX, 0) == 0;
    switch (this->step_) {
      case Step::WAIT_READY:
        if (version == this->target_version_) {
          this->io_.send_wifi();
          this->finish_("already installed");
        } else if (automyco) {
          // AutoMyco camera firmware updates itself (camera_update); it still needs the chamber's Wi-Fi.
          this->io_.send_wifi();
          this->request_ota_(now);
        } else if (!this->io_.wifi_connected()) {
          this->finish_("failed: no Wi-Fi to give the camera");
        } else {
          this->io_.status("sending Wi-Fi");
          this->set_step_(Step::SEND_WIFI, 2000, now);
        }
        break;
      case Step::RESTARTING:
      case Step::DOWNLOADING:  // a fast reboot can beat the "completed" line
        if (version == this->target_version_) {
          this->io_.send_wifi();
          this->set_step_(Step::WAIT_ONLINE, 60000, now);
        } else {
          this->round_failed_("download kept failing", now);  // back on stock: the image was rejected
        }
        break;
      default:
        if (automyco)
          this->io_.send_wifi();  // outside an install: keep an AutoMyco camera on the chamber's network
        break;
    }
  }

  void begin_round_(uint32_t now) {
    this->round_++;
    this->camera_version_.clear();
    this->last_http_code_ = 0;
    this->io_.status("starting camera");
    this->io_.power(false);
    if (this->io_.event)
      this->io_.event("camera power-cycled: install round " + std::to_string(this->round_));
    // A full 10 s off: a short cut (1.5 s on 2026-10-09) can leave the OV2640 wedged on the next boot.
    this->set_step_(Step::POWER_OFF, POWER_CYCLE_OFF_MS, now);
  }

  void round_failed_(const char *why, uint32_t now) {
    if (this->round_ >= MAX_ROUNDS) {
      this->finish_(std::string("failed: ") + why);
      return;
    }
    this->begin_round_(now);
  }

  void set_step_(Step step, uint32_t timeout_ms, uint32_t now) {
    this->step_ = step;
    this->step_started_ = now;
    this->step_timeout_ = timeout_ms;
  }

  void finish_(const std::string &status) {
    // An install that ends (the 15-min timeout, say) while a round has the camera's power off must not
    // leave it off: nothing else would switch it back on.
    if (this->step_ == Step::POWER_OFF)
      this->io_.power(true);
    this->step_ = Step::IDLE;
    this->io_.status(status);
  }

  Io io_;
  Step step_{Step::IDLE};
  uint32_t step_started_{0};
  uint32_t step_timeout_{0};
  uint32_t install_started_{0};
  uint8_t round_{0};
  int last_http_code_{0};
  bool cycling_{false};
  bool deferred_{false};
  uint32_t cycle_started_{0};
  uint8_t auto_cycles_{0};
  uint32_t last_auto_cycle_{0};
  bool manual_cycled_{false};
  uint32_t last_manual_cycle_{0};
  std::string target_version_;
  std::string target_hash_;
  std::string camera_version_;  // from the camera's last ready line since it was powered on; "" = unknown
};

}  // namespace camera_install
