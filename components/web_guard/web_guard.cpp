#include "web_guard.h"

#include <cctype>
#include <cstring>
#include <string>

#include <esp_http_server.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

namespace esphome::web_guard {

static const char *const TAG = "web_guard";

namespace {

// DefaultHeaders keeps its list protected and only befriends ESPHome's own request classes. Naming
// the member through a derived class is the standard-conforming way to reach it without patching
// ESPHome: `&Access::headers_` is a pointer-to-member of the base, usable on the singleton.
struct DefaultHeadersAccess : web_server_idf::DefaultHeaders {
  static auto &headers(web_server_idf::DefaultHeaders &instance) {
    return instance.*(&DefaultHeadersAccess::headers_);
  }
};

std::string header(AsyncWebServerRequest *request, const char *name) {
  auto value = request->get_header(name);
  return value.has_value() ? std::move(*value) : std::string();
}

std::string to_lower(std::string s) {
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool all_digits(const std::string &s, size_t from) {
  if (from >= s.size())
    return false;
  for (size_t i = from; i < s.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(s[i])))
      return false;
  }
  return true;
}

// Dotted-quad IPv4 literal, each octet 0-255.
bool is_ipv4_literal(const std::string &s) {
  int parts = 0;
  size_t i = 0;
  while (true) {
    size_t start = i;
    int value = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      value = value * 10 + (s[i] - '0');
      if (value > 255 || i - start >= 3)
        return false;
      i++;
    }
    if (i == start)
      return false;
    parts++;
    if (i == s.size())
      break;
    if (s[i] != '.' || parts == 4)
      return false;
    i++;
  }
  return parts == 4;
}

// Number of non-empty segments in a decoded path: "/switch/Heater/turn_on" -> 3.
size_t path_segments(StringRef url) {
  size_t count = 0;
  bool in_segment = false;
  for (size_t i = 0; i < url.size(); i++) {
    if (url.c_str()[i] == '/') {
      in_segment = false;
    } else if (!in_segment) {
      in_segment = true;
      count++;
    }
  }
  return count;
}

const char *verdict_str(Verdict v) {
  switch (v) {
    case Verdict::BAD_HOST:
      return "Host is not this node";
    case Verdict::PREFLIGHT:
      return "CORS preflight refused";
    case Verdict::CROSS_ORIGIN:
      return "cross-origin request";
    case Verdict::GET_ACTION:
      return "actions need POST";
    case Verdict::MISSING_HEADER:
      return "POST without X-Requested-With";
    case Verdict::KEY_REQUIRED:
      // Shown as is by the chamber's own page (web/dashboard.js): a browser has no key.
      return "Only the paired AutoMyco app can change this (UV-C on, restart, water and update safety "
             "settings, camera login): it needs the X-AutoMyco-Key header. Use the app or Home Assistant.";
    case Verdict::UNAVAILABLE:
      return "chamber starting up: protected controls wait a moment";
    default:
      return "allowed";
  }
}

}  // namespace

void WebGuard::setup() {
  if (this->base_->get_server() != nullptr) {
    // Something initialised the server before us; our handler would not be first.
    ESP_LOGE(TAG, "web server already running: the guard may not see every request");
  }
  this->base_->add_handler_without_auth(this);
}

// 16 random bytes as 32 hex. random_bytes() is the hardware RNG, a real entropy source once the radio is on.
static std::string fresh_challenge() {
  uint8_t rnd[CHALLENGE_LEN / 2];
  random_bytes(rnd, sizeof(rnd));
  return to_hex(std::string(reinterpret_cast<const char *>(rnd), sizeof(rnd)));
}

void WebGuard::set_owner_password(const std::string &password) {
  const std::string challenge = fresh_challenge();
  {
    LockGuard guard(this->key_lock_);
    std::fill(this->gate_.key.begin(), this->gate_.key.end(), '\0');
    gate_set_password(this->gate_, password, challenge);
  }
  this->publish_challenge_ = true;
}

void WebGuard::loop() {
  // Expiry: a paired chamber's challenge never lives longer than CHALLENGE_TTL_MS (control_key.h).
  if (millis() - this->challenge_since_ms_ >= CHALLENGE_TTL_MS) {
    this->challenge_since_ms_ = millis();
    const std::string next = fresh_challenge();
    LockGuard guard(this->key_lock_);
    if (gate_expire(this->gate_, next))
      this->publish_challenge_ = true;
  }
  if (!this->publish_challenge_.exchange(false))
    return;
  this->challenge_since_ms_ = millis();   // any new challenge (accepted header, password change) restarts the clock
#ifdef USE_TEXT_SENSOR
  if (this->challenge_sensor_ == nullptr)
    return;
  std::string challenge;
  {
    LockGuard guard(this->key_lock_);
    challenge = this->gate_.challenge;
  }
  this->challenge_sensor_->publish_state(challenge);
#endif
}

bool WebGuard::has_control_key() const {
  LockGuard guard(this->key_lock_);
  return !this->gate_.key.empty();
}

// Could this request change a keyed entity? Decoded exactly as web_server decodes it (url_to), then matched
// wider than web_server routes (control_key.h keyed_request). Reads (GET/HEAD) never change state.
bool WebGuard::is_keyed_(AsyncWebServerRequest *request) const {
  if (this->keyed_.empty())
    return false;
  const auto method = request->method();
  if (method == HTTP_GET || method == HTTP_HEAD || method == HTTP_OPTIONS)
    return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(url_buf);
  return keyed_request(std::string(url.c_str(), url.size()), this->keyed_);
}

// The gate's decision for a keyed request (control_key.h gate_check): refused until the password and a
// challenge have loaded, open while nobody has paired, otherwise a header signed over the current challenge,
// which then rotates (published from loop(), since this runs on the httpd task).
KeyCheck WebGuard::key_check_(AsyncWebServerRequest *request) const {
  httpd_req_t *req = *request;
  const std::string presented = header(request, CONTROL_KEY_HEADER);
  const std::string next = fresh_challenge();
  LockGuard guard(this->key_lock_);
  const std::string before = this->gate_.challenge;
  const KeyCheck result = gate_check(this->gate_, http_method_str(request->method()), req->uri, presented, next);
  if (this->gate_.challenge != before)
    this->publish_challenge_ = true;
  return result;
}

void WebGuard::dump_config() {
  ESP_LOGCONFIG(TAG, "Web API guard:");
  ESP_LOGCONFIG(TAG, "  Hosts: IP literals, %s, %s.local", App.get_name().c_str(), App.get_name().c_str());
  for (const char *host : this->allowed_hosts_)
    ESP_LOGCONFIG(TAG, "  Extra host: %s", host);
  ESP_LOGCONFIG(TAG, "  Cross-origin requests, CORS preflights and GET actions are refused");
  for (const auto &e : this->keyed_) {
    ESP_LOGCONFIG(TAG, "  '%s' needs %s once a phone has paired (open: %s)", e.name.c_str(), CONTROL_KEY_HEADER,
                  e.open_action.empty() ? "nothing" : e.open_action.c_str());
  }
  if (!this->keyed_.empty()) {
    ESP_LOGCONFIG(TAG, "  Control key: %s", this->has_control_key() ? "set" : "none yet (keyed entities open)");
  }
}

void WebGuard::scrub_default_headers_() {
  // web_server_base::init() adds `Access-Control-Allow-Origin: *` to every response. Swap that slot
  // for a harmless header (the list is fixed-size, so we replace rather than remove). Runs on the
  // httpd task before any handler builds a response, so no response ever carries the wildcard.
  for (auto &h : DefaultHeadersAccess::headers(web_server_idf::DefaultHeaders::Instance())) {
    if (h.name != nullptr && strcasecmp(h.name, "Access-Control-Allow-Origin") == 0) {
      h.name = "X-Content-Type-Options";
      h.value = "nosniff";
    }
  }
}

bool WebGuard::host_allowed_(const std::string &raw) const {
  if (raw.empty())
    return false;
  std::string host = to_lower(raw);
  if (host[0] == '[') {
    // IPv6 literal, optionally with :port. A DNS-rebinding attack always carries a DNS name.
    size_t close = host.find(']');
    if (close == std::string::npos)
      return false;
    return close + 1 == host.size() || (host[close + 1] == ':' && all_digits(host, close + 2));
  }
  size_t colon = host.find(':');
  if (colon != std::string::npos) {
    if (!all_digits(host, colon + 1))
      return false;
    host.resize(colon);
  }
  while (!host.empty() && host.back() == '.')
    host.pop_back();
  if (host.empty())
    return false;
  if (is_ipv4_literal(host))
    return true;
  const std::string node = to_lower(App.get_name().str());
  if (host == node || host == node + ".local")
    return true;
  for (const char *allowed : this->allowed_hosts_) {
    if (host == allowed)  // lowercased at codegen
      return true;
  }
  return false;
}

Verdict WebGuard::check(AsyncWebServerRequest *request) const {
  const std::string host = header(request, "Host");
  bool portal = false;
#ifdef USE_CAPTIVE_PORTAL
  // While the setup hotspot is up, phones probe with foreign Host names (captive.apple.com, ...).
  // Only someone who joined the password-protected setup AP can reach the node then.
  portal = captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active();
#endif
  if (!portal && !this->host_allowed_(host))
    return Verdict::BAD_HOST;

  const auto method = request->method();
  if (method == HTTP_OPTIONS)
    return Verdict::PREFLIGHT;

  // Browsers send Origin on every cross-origin request except no-cors GETs (img/script/navigation),
  // and on every POST. Same-origin means exactly http://<Host> (the node only speaks plain HTTP).
  const std::string origin = header(request, "Origin");
  const bool has_origin = !origin.empty();
  if (has_origin && origin != "http://" + host)
    return Verdict::CROSS_ORIGIN;

  // ESPHome runs /<domain>/<name>/<action> for GET too, which an <img> tag on any page could
  // trigger without an Origin header. Actions are POST-only here.
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  if (method == HTTP_GET && path_segments(request->url_to(url_buf)) >= 3)
    return Verdict::GET_ACTION;

  // A POST must prove it is not a plain HTML form: either a same-origin Origin (the built-in page)
  // or a custom header (the app, curl, the hub), which a cross-origin page cannot add without a
  // preflight, and preflights are refused above.
  if (method == HTTP_POST && !has_origin && !request->hasHeader("X-Requested-With"))
    return Verdict::MISSING_HEADER;

  // The hazardous and credential-resetting actions (keyed_entities: UV-C on, Camera Reset Login) also need
  // proof that the caller is a phone that paired at the chamber: an HMAC over the single-use challenge
  // (control_key.h). Until one has paired there is no key and they stay open as before, so an unpaired owner
  // isn't locked out. Before the password has loaded at boot: refused (503), never open.
  // A header is used up by the first evaluation that accepts it (the challenge rotates). ESPHome's IDF server
  // asks canHandle() once per request before dispatch; its raw-body fallback (no handler claimed the request)
  // asks again, and that second look sees a new challenge and refuses: it fails closed, and an accepted
  // header never opens a second request.
  if (this->is_keyed_(request)) {
    this->last_check_ = this->key_check_(request);
    if (this->last_check_ == KeyCheck::NOT_LOADED)
      return Verdict::UNAVAILABLE;
    if (this->last_check_ != KeyCheck::OK)
      return Verdict::KEY_REQUIRED;
  }

  return Verdict::ALLOW;
}

bool WebGuard::canHandle(AsyncWebServerRequest *request) const {
  scrub_default_headers_();
  if (this->check(request) != Verdict::ALLOW)
    return true;
  // Allowed only because nothing has paired yet: say so (once per request; handleRequest isn't called).
  if (this->is_keyed_(request) && !this->has_control_key())
    ESP_LOGI(TAG, "allowed POST %s without %s: no phone has paired yet", static_cast<httpd_req_t *>(*request)->uri,
             CONTROL_KEY_HEADER);
  return false;
}

void WebGuard::handleRequest(AsyncWebServerRequest *request) {
  const Verdict verdict = this->check(request);
  const bool keyed = verdict == Verdict::KEY_REQUIRED || verdict == Verdict::UNAVAILABLE;
  ESP_LOGW(TAG, "refused %s %s: %s%s%s", http_method_str(request->method()), static_cast<httpd_req_t *>(*request)->uri,
           keyed ? "X-AutoMyco-Key " : "", keyed ? key_check_str(this->last_check_) : "",
           keyed ? "" : verdict_str(verdict));
  // Answer with the raw httpd API: ESPHome's send() maps unknown codes to 500 and would add the
  // default headers. No CORS headers at all, so the browser hides the answer from the caller.
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, verdict == Verdict::GET_ACTION ? "405 Method Not Allowed"
                             : verdict == Verdict::UNAVAILABLE ? "503 Service Unavailable"
                                                            : "403 Forbidden");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (verdict == Verdict::GET_ACTION)
    httpd_resp_set_hdr(req, "Allow", "POST");
  httpd_resp_sendstr(req, verdict_str(verdict));
}

}  // namespace esphome::web_guard
