#pragma once
// Remote access (common/remote-access.yaml): the chamber pushes its state to automyco.com so its owner can
// see it from outside the home network. The telemetry reply may carry commands from the paired account
// (parse_reply below); remote-access.yaml applies only a fixed list of them (never UV-C on, the heater, the
// atomizer, the water lockout, restart or firmware) and the chamber's interlocks still decide. Server side:
// site/lambda/remote.mjs and remote-core.mjs.
//
// What lives here:
//   - Sample + Ring: one compact reading per minute, kept in RAM until a POST is accepted (oldest dropped
//     when full, so an outage costs history, never memory).
//   - classify() / retry_delay_s(): what an HTTP status means and when to try again. The server says
//     "no" with a status code only (403 cloud off or not enabled, 404 not paired, 401 wrong token), so the
//     chamber backs off without parsing a body.
//   - append_events(): the event-log lines (`evlog`) newer than the last line the server accepted, found
//     by a 32-bit hash of that line, so the chamber keeps no second copy of the log.
//   - post_async(): the HTTPS POST in its own FreeRTOS task on the shared cloud_http client, so the
//     climate controller and the heater's over-temperature cut-off never wait on the network.
//
// Host-testable: esphome/test/remote_access_host_test.cpp builds this with -DTS_REMOTE_HOST_TEST
// (site/test/remote_test.py runs it).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef TS_REMOTE_HOST_TEST
#include <atomic>
#include <vector>
#include "esphome/components/json/json_util.h"
#include "cloud_post.h"   // esphome/cloud_post.h; ESPHome copies every included header into src/
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#endif

namespace ts_remote {

static constexpr int SCHEMA = 1;
static constexpr size_t RING = 30;          // 30 min of samples at one a minute; ~4.5 KB of RAM
static constexpr size_t MAX_PER_POST = 15;  // keeps a body under ~3 KB
static constexpr size_t STATE_LEN = 24;
static constexpr size_t EV_MAX = 10;        // event-log lines per POST (MAX_EVENTS in remote-core.mjs)
static constexpr size_t EV_LEN = 80;        // characters per line (EVENT_LEN in remote-core.mjs)

// Alarm bits, in the same order as ALARMS in site/lambda/remote-core.mjs.
enum : uint16_t {
  A_LOW_WATER = 1 << 0,
  A_TANK_EMPTY = 1 << 1,
  A_HARD_STOP = 1 << 2,
  A_SENSOR = 1 << 3,      // HDC1080 reading missing or stale
  A_OVERTEMP = 1 << 4,    // above max_safe_temp_f (the heater is already cut off on the chamber)
  A_FAN1 = 1 << 5,        // "Not Detected" load checks
  A_FAN2 = 1 << 6,
  A_BLOWER = 1 << 7,
  A_ATOMIZER = 1 << 8,
  A_HEATER = 1 << 9,
};

struct Sample {
  uint32_t t = 0;      // UTC epoch seconds, 0 if SNTP hasn't synced
  uint32_t up = 0;     // uptime seconds when taken (the server dates samples with it when t is 0)
  float temp_f = NAN, rh = NAN, sp_rh = NAN, sp_t = NAN;
  int8_t fan1 = 0, fan2 = 0, blower = 0;  // percent, 0 = off
  uint8_t climate = 0, atomizer = 0, heater = 0, top = 0, bottom = 0, uv = 0;
  uint16_t alarms = 0;
  char state[STATE_LEN] = {0};  // Grow State text
};

class Ring {
 public:
  void push(const Sample &s) {
    if (n_ == RING) {  // full: drop the oldest
      head_ = (head_ + 1) % RING;
      n_--;
      dropped_++;
    }
    buf_[(head_ + n_) % RING] = s;
    n_++;
  }
  size_t size() const { return n_; }
  const Sample &at(size_t i) const { return buf_[(head_ + i) % RING]; }
  // Removes the oldest `k` samples (the ones a POST just delivered).
  void drop_front(size_t k) {
    if (k > n_) k = n_;
    head_ = (head_ + k) % RING;
    n_ -= k;
  }
  void clear() { head_ = n_ = 0; }
  uint32_t dropped() const { return dropped_; }

 private:
  Sample buf_[RING];
  size_t head_ = 0, n_ = 0;
  uint32_t dropped_ = 0;
};

// Copies `src` into `dst` (size n, always terminated), keeping only plain printable characters, so the
// result is JSON-safe without escaping. Used for every text that goes into a post (state, preset names,
// which include cloud recipe names).
inline void json_safe(char *dst, size_t n, const char *src) {
  size_t j = 0;
  for (size_t i = 0; src != nullptr && src[i] != '\0' && j + 1 < n; i++) {
    char c = src[i];
    dst[j++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '?';
  }
  dst[j] = '\0';
}

inline void set_state(Sample &s, const char *src) { json_safe(s.state, STATE_LEN, src); }

inline void put_num(std::string &out, const char *key, float v, int decimals) {
  char buf[32];
  if (std::isnan(v) || std::isinf(v))
    snprintf(buf, sizeof(buf), "\"%s\":null", key);
  else
    snprintf(buf, sizeof(buf), "\"%s\":%.*f", key, decimals, v);
  out += buf;
}

inline std::string sample_json(const Sample &s) {
  std::string o = "{";
  char buf[160];
  if (s.t) {
    snprintf(buf, sizeof(buf), "\"t\":%lu,", (unsigned long) s.t);
    o += buf;
  }
  snprintf(buf, sizeof(buf), "\"up\":%lu,", (unsigned long) s.up);
  o += buf;
  put_num(o, "tf", s.temp_f, 1);
  o += ",";
  put_num(o, "rh", s.rh, 1);
  o += ",";
  put_num(o, "srh", s.sp_rh, 0);
  o += ",";
  put_num(o, "stf", s.sp_t, 0);
  snprintf(buf, sizeof(buf),
           ",\"cc\":%u,\"at\":%u,\"ht\":%u,\"f1\":%d,\"f2\":%d,\"bl\":%d,\"lt\":%u,\"lb\":%u,\"uv\":%u,\"a\":%u,\"st\":\"%s\"}",
           s.climate, s.atomizer, s.heater, s.fan1, s.fan2, s.blower, s.top, s.bottom, s.uv, (unsigned) s.alarms,
           s.state);
  o += buf;
  return o;
}

// The telemetry body: up to MAX_PER_POST of the oldest samples, plus `info` (a JSON object's members,
// already formatted). `taken` is how many samples went in, to drop once the server accepts them.
inline std::string telemetry_json(const Ring &r, uint32_t up_now, const std::string &info, size_t &taken) {
  taken = r.size() < MAX_PER_POST ? r.size() : MAX_PER_POST;
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"schema\":%d,\"up\":%lu,\"dropped\":%lu,", SCHEMA, (unsigned long) up_now,
           (unsigned long) r.dropped());
  std::string o = buf;
  o += info;
  o += ",\"s\":[";
  for (size_t i = 0; i < taken; i++) {
    if (i) o += ",";
    o += sample_json(r.at(i));
  }
  o += "]}";
  return o;
}

// FNV-1a of one event-log line; never 0, which means "nothing sent yet".
inline uint32_t line_hash(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t) s[i]) * 16777619u;
  return h ? h : 1;
}

// Appends ,"ev":["...",...] to `o`: the lines of `log` (the newest-first event log, `cap` bytes, NUL- or
// cap-terminated) above the one whose hash is `sent`, at most EV_MAX, newest first, each cut to EV_LEN and
// made JSON-safe. "grow started: <name> (...)" and "grow ended: <name>" lose everything after the colon,
// so the grow name is never sent. Returns the hash of the newest line sent, to keep once the server
// accepts the POST, or `sent` when nothing is new (and then appends nothing).
inline uint32_t append_events(std::string &o, const char *log, size_t cap, uint32_t sent) {
  const size_t len = log ? strnlen(log, cap) : 0;
  uint32_t newest = sent;
  size_t n = 0;
  for (size_t at = 0; at < len && n < EV_MAX;) {
    const char *nl = (const char *) memchr(log + at, '\n', len - at);
    const size_t end = nl ? (size_t) (nl - log) : len;
    const char *line = log + at;
    size_t ll = end - at;
    at = end + 1;
    if (ll == 0) continue;
    const uint32_t h = line_hash(line, ll);
    if (h == sent) break;
    if (n == 0) newest = h;
    // The message starts after "MM-DD HH:MM " or "+123s ".
    const char *msg = (ll > 12 && line[2] == '-' && line[5] == ' ') ? line + 12 : (const char *) memchr(line, ' ', ll);
    if (msg && *msg == ' ') msg++;
    if (msg && msg < line + ll) {
      const size_t rest = line + ll - msg;
      for (const char *p : {"grow started:", "grow ended:"}) {
        const size_t pl = strlen(p);
        if (rest >= pl && memcmp(msg, p, pl) == 0) ll = (size_t) (msg - line) + pl - 1;
      }
    }
    if (ll > EV_LEN) ll = EV_LEN;
    char buf[EV_LEN + 1];
    memcpy(buf, line, ll);
    buf[ll] = '\0';
    char safe[EV_LEN + 1];
    json_safe(safe, sizeof(safe), buf);
    o += n++ ? ",\"" : ",\"ev\":[\"";
    o += safe;
    o += '"';
  }
  if (n) o += ']';
  return newest;
}

// What the server's answer means. -1 = no answer at all (DNS, TLS, timeout).
enum Outcome { OK, OFF, UNPAIRED, AUTH, REJECTED, RETRY };

inline Outcome classify(int status) {
  if (status == 200 || status == 204) return OK;
  if (status == 403) return OFF;       // cloud switched off, account not enabled, or chamber disabled
  if (status == 404 || status == 410) return UNPAIRED;
  if (status == 401) return AUTH;      // token doesn't match this Cloud Device ID
  if (status == 400 || status == 413) return REJECTED;  // the body itself was refused: don't resend it
  return RETRY;                        // 429, 5xx, no answer
}

// Seconds until the next telemetry POST. `fails` counts RETRY outcomes in a row (including this one).
// 0 = don't post again until something changes (re-paired, switched off and on).
inline uint32_t retry_delay_s(Outcome o, uint32_t interval_s, uint32_t fails) {
  switch (o) {
    case OK:
    case REJECTED:
      return interval_s;
    case OFF:
      return 3600;
    case AUTH:
      return 6 * 3600;
    case UNPAIRED:
      return 0;
    case RETRY:
    default: {
      uint32_t d = 60;
      for (uint32_t i = 1; i < fails && d < 3600; i++) d *= 2;
      return d < 3600 ? d : 3600;
    }
  }
}

inline const char *status_text(Outcome o) {
  switch (o) {
    case OK: return "Connected";
    case OFF: return "Paired; remote access is off for this account or chamber";
    case UNPAIRED: return "Not paired: enter a pairing code from automyco.com/remote";
    case AUTH: return "Refused: this chamber's cloud identity doesn't match";
    case REJECTED: return "Connected (last report refused)";
    default: return "Can't reach automyco.com; retrying";
  }
}

// Pairing codes as automyco.com shows them: 8 characters from an alphabet without 0/O/1/I/L, any case,
// spaces and dashes ignored. Returns "" if `in` can't be one.
inline std::string normalise_code(const std::string &in) {
  static const char *const ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
  std::string out;
  for (char c : in) {
    if (c == ' ' || c == '-') continue;
    if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
    if (strchr(ALPHABET, c) == nullptr || c == '\0') return "";
    out += c;
  }
  return out.size() == 8 ? out : "";
}

// The line that hands the camera its frame-upload credentials over the camera wire (chamber_link on the
// camera parses it), or tells it to stop. Every value is hex or the build's own base URL, so nothing
// needs escaping. Never logged: it carries the camera key.
inline std::string camera_line(const char *url, const char *device_id, const char *camera_key) {
  if (url == nullptr || device_id == nullptr || camera_key == nullptr) return "{\"cmd\":\"remote\",\"off\":true}";
  return std::string("{\"cmd\":\"remote\",\"url\":\"") + url + "\",\"id\":\"" + device_id + "\",\"key\":\"" + camera_key + "\"}";
}

// ---- Live mode and remote commands (server side: site/lambda/remote-core.mjs COMMANDS, liveReply). ----
// While someone has the dashboard or the app open, the telemetry reply carries {"live": seconds}: the
// chamber then posts every LIVE_POST_S instead of every remote_interval. Commands ride the same reply,
// {"cmd": [{"i": "<8 hex>", "a": "<action>", "v": <value>}]}. The chamber applies only the actions in
// remote-access.yaml's apply step, clamps numbers to each entity's own limits, and answers every command
// in its next post as "ack": [{"i", "r"}]. There is no action for UV-C, the heater, the atomizer, the hard
// stop, the water-lockout override, restart, self-test or firmware: an unknown action is refused.
static constexpr uint32_t LIVE_POST_S = 25;    // the server accepts a post every 20 s at most
static constexpr uint32_t MAX_LIVE_S = 300;    // a reply can't keep the chamber live longer than this
static constexpr size_t MAX_CMDS = 5;
static constexpr size_t MAX_ACKS = 8;

struct Cmd {
  std::string i, a, s;   // id, action, text value (selects, buttons)
  float n = NAN;         // number value (setpoints, speeds, on/off as 0/1)
};

inline bool is_cmd_id(const std::string &i) {
  if (i.size() != 8) return false;
  for (char c : i)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// A number clamped into [lo, hi]; `clamped` says whether it had to be.
inline float clamp_to(float v, float lo, float hi, bool &clamped) {
  clamped = v < lo || v > hi;
  return v < lo ? lo : (v > hi ? hi : v);
}

// One "ack" element. `result` is "ok", "clamped to <x>" or "refused: <why>", made JSON-safe here.
inline std::string ack_json(const std::string &i, const char *result) {
  char r[49];
  json_safe(r, sizeof(r), result);
  return std::string("{\"i\":\"") + (is_cmd_id(i) ? i : std::string("00000000")) + "\",\"r\":\"" + r + "\"}";
}

// The acks waiting to go out, as JSON array members ("{...},{...}"), at most MAX_ACKS (oldest dropped).
class Acks {
 public:
  void add(const std::string &i, const char *result) {
    if (n_ == MAX_ACKS) {
      list_.erase(0, list_.find("},{") + 2);
      n_--;
    }
    if (!list_.empty()) list_ += ",";
    list_ += ack_json(i, result);
    n_++;
  }
  // What goes into the next post (empty: nothing), and how many entries that was.
  std::string take_for_post(size_t &count) {
    count = n_;
    return n_ ? std::string(",\"ack\":[") + list_ + "]" : std::string();
  }
  // The post carrying the first `count` entries was accepted: forget them.
  void delivered(size_t count) {
    while (count-- > 0 && n_ > 0) {
      const size_t cut = list_.find("},{");
      list_ = cut == std::string::npos ? std::string() : list_.substr(cut + 2);
      n_--;
    }
  }
  size_t size() const { return n_; }

 private:
  std::string list_;
  size_t n_ = 0;
};

#ifndef TS_REMOTE_HOST_TEST
// Camera key: what the chamber gives its camera so the camera can upload frames in the chamber's name
// (POST /api/device/remote/frame). HMAC-SHA256(key = the device token, "automyco-camera-frames-v1:" +
// pairing salt), hex. It can't be turned back into the token, so the camera can post frames and nothing
// else (no readings, no pairing). `ck`, its SHA-256, goes to the server with each telemetry post, so the
// server never needs the token to check it.
// The salt is random, made on every successful pairing and erased on unpairing (remote-access.yaml), so
// the key belongs to one pairing: after the chamber moves to another account, a copy of the old key
// (someone who read the camera wire while they had the chamber) can't post frames to the new owner.
// out, hash_out: 65 bytes. false (both empty) without a salt, or if mbedtls refused.
inline bool camera_key(const char *token, const char *salt, char *out, char *hash_out) {
  static const char PREFIX[] = "automyco-camera-frames-v1:";
  out[0] = hash_out[0] = '\0';
  if (token == nullptr || salt == nullptr || strlen(salt) != 32) return false;
  char msg[sizeof(PREFIX) + 32];
  const int n = snprintf(msg, sizeof(msg), "%s%s", PREFIX, salt);
  uint8_t mac[32];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr || n <= 0 ||
      mbedtls_md_hmac(md, reinterpret_cast<const unsigned char *>(token), strlen(token),
                      reinterpret_cast<const unsigned char *>(msg), n, mac) != 0) {
    out[0] = hash_out[0] = '\0';
    return false;
  }
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", mac[i]);
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(out), 64, digest, 0);
  for (int i = 0; i < 32; i++) snprintf(hash_out + 2 * i, 3, "%02x", digest[i]);
  return true;
}

// The chamber's samples waiting to be sent (not an ESPHome global: those are declared before includes).
inline Ring g_ring;
inline Acks g_acks;
inline std::string g_reply;   // the last telemetry reply body (2xx), written by the post task before g_status

// The telemetry reply: {"live": seconds} and/or {"cmd": [...]}. Empty or not JSON = nothing to do.
// Keeps at most MAX_CMDS well-formed commands; anything else in them is ignored here and judged by the
// apply step (which refuses actions it doesn't know).
inline void parse_reply(const std::string &body, uint32_t &live_s, std::vector<Cmd> &cmds) {
  live_s = 0;
  cmds.clear();
  if (body.empty() || body[0] != '{') return;
  esphome::json::parse_json(body, [&](JsonObject root) -> bool {
    if (root["live"].is<int>()) {
      const int l = root["live"].as<int>();
      live_s = l <= 0 ? 0 : (l > (int) MAX_LIVE_S ? MAX_LIVE_S : (uint32_t) l);
    }
    if (!root["cmd"].is<JsonArray>()) return true;
    for (JsonObject c : root["cmd"].as<JsonArray>()) {
      if (cmds.size() == MAX_CMDS) break;
      Cmd x;
      x.i = c["i"] | "";
      x.a = c["a"] | "";
      if (!is_cmd_id(x.i) || x.a.empty() || x.a.size() > 8) continue;
      if (c["v"].is<float>()) x.n = c["v"].as<float>();
      else if (c["v"].is<const char *>()) {
        char t[33];
        json_safe(t, sizeof(t), c["v"].as<const char *>());
        x.s = t;
      }
      cmds.push_back(std::move(x));
    }
    return true;
  });
}

// ---- Background POST (the task is cloud_post.h: one cloud request at a time, shared heap check). ----
// g_status: 0 idle, 1 running, otherwise finished with that HTTP status (-1 = no response) until the
// main loop takes it with take_status().
inline std::atomic<int> g_status{0};

// Starts the POST; false if one is running or its result is still waiting, another cloud request is in
// flight, or there isn't contiguous heap for a TLS session.
inline bool post_async(esphome::http_request::HttpRequestComponent *http, const std::string &url, std::string body,
                       const char *device_id, const char *token) {
  if (g_status != 0) return false;
  return ts_cloud::post_async(http, url, std::move(body), device_id, token, g_status, "ts01_remote", &g_reply);
}

inline bool busy() { return g_status != 0; }

// The finished request's HTTP status (and back to idle), or 0 while nothing has finished.
inline int take_status() {
  const int s = g_status.load();
  if (s == 0 || s == 1) return 0;
  g_status = 0;
  return s;
}
#endif  // TS_REMOTE_HOST_TEST

}  // namespace ts_remote
