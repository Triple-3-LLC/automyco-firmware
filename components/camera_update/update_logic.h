#pragma once
// The AutoMyco camera's self-update rules, with no ESPHome or ESP-IDF dependencies so they run in host
// tests (esphome/tests/test_camera_update.cpp). CameraUpdate (camera_update.cpp) does the I/O.
//
// A request names a published camera release by version and SHA-256, from the main board over the camera
// link ({"ota":{"version":"automyco-camera/<v>","hash":"<sha256>"}}, the stock camera's own command) or
// from the iOS app over the camera's authenticated HTTP (POST :8080/update). The camera never takes a URL
// from either: it builds https://<host>/fw/<sha256>.bin itself, where <host> is compiled in
// (api.automyco.com, the vendor API, which only serves published camera releases and re-hashes them),
// follows no redirects, and boots the image only if its SHA-256 matches the request. Docs:
// docs/camera-install.md "Updating an AutoMyco camera".
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace camera_update_logic {

static constexpr const char *VERSION_PREFIX = "automyco-camera/";

// One TLS session's heap, the same numbers as the main board's common/tls_budget.h (which explains
// them): the 16,717 B mbedTLS IN record buffer must fit one block, and the whole handshake ~45 KB. On
// top of that the download keeps its own read buffer and the task stack.
static constexpr size_t TLS_BLOCK_NEEDED = 20 * 1024;
static constexpr size_t TLS_FREE_NEEDED = 48 * 1024;
static constexpr size_t READ_BUFFER = 4096;   // esp_http_client rx buffer and our read chunk
static constexpr size_t TASK_STACK = 8192;    // the download task (TLS handshake runs in it)
// esp_http_client builds the whole request line and headers in its TX buffer. 512 B (the default) is
// what broke the main board's self-update on 2026-10-11 (docs/fleet-updates.md); this request needs
// ~170 B, so 1 KB leaves room for a longer host.
static constexpr size_t TX_BUFFER = 1024;

inline bool heap_ok(size_t largest_internal_block, size_t free_internal) {
  return largest_internal_block >= TLS_BLOCK_NEEDED &&
         free_internal >= TLS_FREE_NEEDED + READ_BUFFER + TASK_STACK;
}

inline bool valid_version(const std::string &v) {
  if (v.empty() || v.size() > 24 || !isdigit(static_cast<unsigned char>(v[0])))
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

// "automyco-camera/1.1.0" (the link) or "1.1.0" (HTTP) -> "1.1.0"; "" if it's anything else, including a
// stock version ("camera/0.1.7.09").
inline std::string bare_version(const std::string &in) {
  std::string v = in;
  const std::string prefix = VERSION_PREFIX;
  if (v.compare(0, prefix.size(), prefix) == 0)
    v = v.substr(prefix.size());
  return valid_version(v) ? v : "";
}

// SemVer-style precedence, the same rules as the app's FirmwareVersion (apple/GrowKit
// FirmwareCatalog.swift), which picks the release the camera is asked for: numeric major.minor.patch
// (missing parts are 0), then a pre-release ("-rc1") sorts below the release, pre-release identifiers
// compare numerically when both are numbers, numbers below words, the same word with a trailing number
// by that number ("rc10" > "rc9"), other words as ASCII, and a longer list wins when the shared part is
// equal. "+build" is ignored.
inline std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t at = s.find(sep, start);
    out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
    if (at == std::string::npos)
      return out;
    start = at + 1;
  }
}

inline bool all_digits(const std::string &s) {
  if (s.empty())
    return false;
  for (char c : s)
    if (!isdigit(static_cast<unsigned char>(c)))
      return false;
  return true;
}

inline int cmp_number_strings(const std::string &a, const std::string &b) {
  const size_t za = a.find_first_not_of('0'), zb = b.find_first_not_of('0');
  const std::string x = za == std::string::npos ? "" : a.substr(za);
  const std::string y = zb == std::string::npos ? "" : b.substr(zb);
  if (x.size() != y.size())
    return x.size() < y.size() ? -1 : 1;
  return x == y ? 0 : (x < y ? -1 : 1);
}

// "rc10" vs "rc9": the same word, so the trailing numbers decide. Otherwise ASCII.
inline int cmp_words(const std::string &a, const std::string &b) {
  auto word_end = [](const std::string &s) {
    size_t e = s.size();
    while (e > 0 && isdigit(static_cast<unsigned char>(s[e - 1])))
      e--;
    return e;
  };
  const size_t ea = word_end(a), eb = word_end(b);
  if (ea < a.size() && eb < b.size() && a.compare(0, ea, b, 0, eb) == 0) {
    const int d = cmp_number_strings(a.substr(ea), b.substr(eb));
    if (d != 0)
      return d;
  }
  return a == b ? 0 : (a < b ? -1 : 1);
}

inline int compare_versions(const std::string &a_in, const std::string &b_in) {
  auto core_and_pre = [](const std::string &v, std::string &core, std::string &pre, bool &has_pre) {
    const std::string no_build = v.substr(0, v.find('+'));
    const size_t dash = no_build.find('-');
    core = no_build.substr(0, dash);
    has_pre = dash != std::string::npos && dash + 1 < no_build.size();
    pre = has_pre ? no_build.substr(dash + 1) : "";
  };
  std::string ca, pa, cb, pb;
  bool ha = false, hb = false;
  core_and_pre(a_in, ca, pa, ha);
  core_and_pre(b_in, cb, pb, hb);
  std::vector<std::string> na = split(ca, '.'), nb = split(cb, '.');
  while (na.size() < 3)
    na.push_back("0");
  while (nb.size() < 3)
    nb.push_back("0");
  for (size_t i = 0; i < na.size() || i < nb.size(); i++) {
    const std::string x = i < na.size() && all_digits(na[i]) ? na[i] : "0";
    const std::string y = i < nb.size() && all_digits(nb[i]) ? nb[i] : "0";
    const int d = cmp_number_strings(x, y);
    if (d != 0)
      return d;
  }
  if (!ha || !hb)
    return ha == hb ? 0 : (ha ? -1 : 1);
  const std::vector<std::string> ia = split(pa, '.'), ib = split(pb, '.');
  for (size_t i = 0; i < ia.size() && i < ib.size(); i++) {
    const bool da = all_digits(ia[i]), db = all_digits(ib[i]);
    int d;
    if (da && db)
      d = cmp_number_strings(ia[i], ib[i]);
    else if (da != db)
      d = da ? -1 : 1;
    else
      d = cmp_words(ia[i], ib[i]);
    if (d != 0)
      return d;
  }
  return ia.size() == ib.size() ? 0 : (ia.size() < ib.size() ? -1 : 1);
}

enum class Verdict { OK, INVALID, SAME, DOWNGRADE, BUSY };

// Whether to take a request for `requested` (bare version) with `hash`, running `running`. A downgrade
// (or the same version again) needs force, which only comes over POST :8080/update, i.e. from whoever
// holds the claimed camera login (the owner's app; the Update button never sets it). The camera link
// ignores "force" (chamber_link), so the main board can't. This is only the early answer: the request's
// version is just a claim, and image_verdict() below decides on the downloaded image's own version.
inline Verdict check(const std::string &running, const std::string &requested, const std::string &hash, bool force,
                     bool busy) {
  if (!valid_version(requested) || !valid_hash(hash))
    return Verdict::INVALID;
  if (busy)
    return Verdict::BUSY;
  if (force)
    return Verdict::OK;
  const int d = compare_versions(requested, running);
  if (d == 0)
    return Verdict::SAME;
  return d < 0 ? Verdict::DOWNGRADE : Verdict::OK;
}

inline std::string verdict_reason(Verdict v, const std::string &running) {
  switch (v) {
    case Verdict::OK:
      return "";
    case Verdict::INVALID:
      return "invalid request";
    case Verdict::SAME:
      return "already on " + running;
    case Verdict::DOWNGRADE:
      return "older than " + running;
    case Verdict::BUSY:
      return "an update is already running";
  }
  return "";
}

// Hostname only: letters, digits, dots and dashes. The host is compiled in (config), never requested.
inline bool valid_host(const std::string &h) {
  if (h.empty() || h.size() > 64 || h.front() == '.' || h.back() == '.' || h.find("..") != std::string::npos)
    return false;
  for (char c : h)
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-'))
      return false;
  return true;
}

inline std::string download_url(const std::string &host, const std::string &hash) {
  return "https://" + host + "/fw/" + hash + ".bin";
}

// What esp_http_client puts in its TX buffer for our GET: the request line plus the Host and User-Agent
// headers it always sends, and the blank line. Must stay well under TX_BUFFER.
inline size_t request_bytes(const std::string &host, const std::string &hash) {
  const std::string path = "/fw/" + hash + ".bin";
  return std::string("GET " + path + " HTTP/1.1\r\n").size() + std::string("Host: " + host + "\r\n").size() +
         std::string("User-Agent: ESP32 HTTP Client/1.0\r\n").size() + 2;
}

enum class State { IDLE, WAITING, DOWNLOADING, VERIFYING, DONE, FAILED };

inline const char *state_name(State s) {
  switch (s) {
    case State::IDLE:
      return "idle";
    case State::WAITING:
      return "waiting";
    case State::DOWNLOADING:
      return "downloading";
    case State::VERIFYING:
      return "verifying";
    case State::DONE:
      return "installed";
    case State::FAILED:
      return "failed";
  }
  return "idle";
}

inline int percent(size_t got, size_t total) {
  if (total == 0)
    return 0;
  const unsigned long long p = static_cast<unsigned long long>(got) * 100ULL / total;
  return p > 100 ? 100 : static_cast<int>(p);
}

// Text that goes into a JSON string and the chamber's status/Event Log: letters, digits, simple
// punctuation, at most 60 characters (the main board applies the same filter again).
inline std::string safe_text(const std::string &in) {
  std::string out;
  for (char c : in) {
    if (out.size() >= 60)
      break;
    if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '.' || c == ',' || c == ':' || c == '-' ||
        c == '(' || c == ')' || c == '/')
      out += c;
  }
  return out;
}

// Progress line for the camera link (main board's camera_link; ignored by 1.2.2 and older, which only
// read the stock updater's plain-text lines, also sent).
inline std::string link_line(State s, int progress, const std::string &version, const std::string &error) {
  std::string out = std::string("{\"status\":\"ota\",\"state\":\"") + state_name(s) + "\"";
  if (s == State::DOWNLOADING || s == State::VERIFYING)
    out += ",\"progress\":" + std::to_string(progress);
  if (!version.empty())
    out += ",\"version\":\"" + safe_text(version) + "\"";
  if (s == State::FAILED)
    out += ",\"error\":\"" + safe_text(error) + "\"";
  return out + "}";
}

// GET :8080/update for the app.
inline std::string status_json(State s, int progress, const std::string &running, const std::string &target,
                               const std::string &error) {
  return std::string("{\"state\":\"") + state_name(s) + "\",\"progress\":" + std::to_string(progress) +
         ",\"running\":\"" + safe_text(running) + "\",\"version\":\"" + safe_text(target) + "\",\"error\":\"" +
         safe_text(s == State::FAILED ? error : "") + "\"}";
}

// Report progress on the link at most every 10 %, so a 1.2 MB download is ~10 lines, not thousands.
inline bool progress_step(int last_reported, int now) {
  return last_reported < 0 || now >= 100 || now / 10 > last_reported / 10;
}

// Network failures retry; a refusal or a wrong image doesn't (the server answered, and asking again
// gets the same answer).
struct Attempt {
  static constexpr int MAX = 3;
  static constexpr uint32_t BACKOFF_MS = 5000;
  static bool retryable(int http_status, bool network_error) {
    if (network_error)
      return true;
    return http_status >= 500 || http_status == 429 || http_status == 408;
  }
};

// --- What the downloaded image says it is ----------------------------------------------------------
//
// The request's version is only a claim: the vendor API serves every published camera hash, so "version
// 9.9.9" with an old release's hash would otherwise install the old image. The image itself decides, from
// two places inside the hashed bytes:
//   - its esp_app_desc_t (right after the 24 B image header and the first 8 B segment header): magic,
//     version (= camera_version, set by CONFIG_APP_PROJECT_VER in common/camera-core.yaml), project_name.
//   - ESPHome's "Project community.automyco-camera version <v>" string, which every AutoMyco camera build
//     carries (publish_firmware.py checks it too). It must agree with the descriptor, which rules out
//     images built before the descriptor carried camera_version (theirs read "2026.9.1", the ESPHome
//     version, which would compare as newer than anything).

static constexpr size_t APP_DESC_OFFSET = 24 + 8;
static constexpr size_t APP_DESC_MIN_BYTES = APP_DESC_OFFSET + 80;  // through project_name[32]
static constexpr uint32_t APP_DESC_MAGIC = 0xABCD5432;

struct AppDesc {
  bool ok{false};
  std::string version;
  std::string project;
};

inline std::string fixed_string(const uint8_t *p, size_t n) {
  size_t len = 0;
  while (len < n && p[len] != 0)
    len++;
  return std::string(reinterpret_cast<const char *>(p), len);
}

// From the first bytes of an app image (at least APP_DESC_MIN_BYTES).
inline AppDesc parse_app_desc(const uint8_t *data, size_t len) {
  AppDesc out;
  if (data == nullptr || len < APP_DESC_MIN_BYTES || data[0] != 0xE9)
    return out;
  const uint8_t *d = data + APP_DESC_OFFSET;
  const uint32_t magic = d[0] | (d[1] << 8) | (d[2] << 16) | (static_cast<uint32_t>(d[3]) << 24);
  if (magic != APP_DESC_MAGIC)
    return out;
  out.version = fixed_string(d + 16, 32);
  out.project = fixed_string(d + 48, 32);
  out.ok = true;
  return out;
}

// Finds ESPHome's project string in a stream of chunks (it can straddle two).
class ProjectVersionScanner {
 public:
  static constexpr const char *NEEDLE = "Project community.automyco-camera version ";
  static constexpr size_t MAX_VERSION = 24;

  void feed(const uint8_t *data, size_t len) {
    if (this->done_)
      return;
    this->window_.append(reinterpret_cast<const char *>(data), len);
    const std::string needle = NEEDLE;
    const size_t at = this->window_.find(needle);
    if (at != std::string::npos) {
      const size_t start = at + needle.size();
      const size_t end = this->window_.find('\0', start);
      if (end != std::string::npos) {
        if (end - start <= MAX_VERSION) {
          this->version_ = this->window_.substr(start, end - start);
          this->done_ = true;
        } else {
          this->window_.erase(0, start);  // not ours; keep looking after it
        }
        return;
      }
      if (this->window_.size() - start > MAX_VERSION)
        this->window_.erase(0, start);
      else
        return;  // the rest of the version is in the next chunk
    }
    const size_t keep = needle.size() + MAX_VERSION;
    if (this->window_.size() > keep)
      this->window_.erase(0, this->window_.size() - keep);
  }
  bool found() const { return this->done_; }
  const std::string &version() const { return this->version_; }

 private:
  std::string window_;
  std::string version_;
  bool done_{false};
};

// The final say, after the SHA-256 matched and before the boot slot changes. "" = install it.
inline std::string image_verdict(const AppDesc &desc, const std::string &project_string_version,
                                 const std::string &running_project, const std::string &running,
                                 const std::string &requested, bool force) {
  if (!desc.ok)
    return "not an AutoMyco camera image";
  if (!running_project.empty() && desc.project != running_project)
    return "not an AutoMyco camera image";
  if (!valid_version(desc.version) || desc.version != project_string_version)
    return "image has no camera version";
  if (!force && desc.version != requested)
    return "image is " + desc.version + ", not " + requested;
  if (!force && compare_versions(desc.version, running) <= 0)
    return compare_versions(desc.version, running) == 0 ? "already on " + running : "older than " + running;
  return "";
}

}  // namespace camera_update_logic
