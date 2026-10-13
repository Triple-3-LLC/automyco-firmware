#pragma once
// Who may POST :8080/claim (camera_http.cpp), on top of "unclaimed and inside the claim window". The claim
// sets the camera's only login, so a web page must not be able to do it for someone:
//   - Content-Type must be application/json. A browser can only send that cross-site after a CORS
//     preflight, and this server answers no OPTIONS, so no web page can claim (a form post or a no-cors
//     fetch is text/plain, multipart or urlencoded, and is refused).
//   - No Origin header at all. Browsers add one to every cross-origin POST; the iOS app never sends one.
//   - Host must be an IP literal or the camera's own name (<name> / <name>.local, any port): a DNS-rebinding
//     page reaches the camera under the attacker's host name, and is refused.
// The same rules as the chamber's web_guard, for one endpoint. Pure C++17: esphome/tests/test_claim_guard.cpp.
#include <cctype>
#include <cstddef>
#include <string>

namespace esphome::camera_http::claim_guard {

inline std::string lower(std::string s) {
  for (auto &c : s)
    c = (char) std::tolower((unsigned char) c);
  return s;
}

inline bool is_ipv4(const std::string &s) {
  int parts = 0;
  size_t i = 0;
  while (true) {
    const size_t start = i;
    int value = 0;
    while (i < s.size() && std::isdigit((unsigned char) s[i])) {
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

// The Host header names this camera: an IPv4 or [IPv6] literal, or `node_name` / `node_name`.local, with an
// optional :port, any case, one trailing dot allowed.
inline bool host_ok(const std::string &host_header, const std::string &node_name) {
  std::string h = lower(host_header);
  if (h.empty())
    return false;
  if (h[0] == '[') {
    const size_t close = h.find(']');
    if (close == std::string::npos || close < 3)
      return false;
    const std::string rest = h.substr(close + 1);
    if (rest.empty())
      return true;
    if (rest[0] != ':' || rest.size() == 1)
      return false;
    for (size_t i = 1; i < rest.size(); i++)
      if (!std::isdigit((unsigned char) rest[i]))
        return false;
    return true;
  }
  const size_t colon = h.rfind(':');
  if (colon != std::string::npos) {
    if (colon + 1 == h.size())
      return false;
    for (size_t i = colon + 1; i < h.size(); i++)
      if (!std::isdigit((unsigned char) h[i]))
        return false;
    h = h.substr(0, colon);
  }
  if (!h.empty() && h.back() == '.')
    h.pop_back();
  if (h.empty())
    return false;
  const std::string name = lower(node_name);
  return is_ipv4(h) || (!name.empty() && (h == name || h == name + ".local"));
}

// application/json, optionally followed by parameters ("; charset=utf-8").
inline bool json_content_type(const std::string &content_type) {
  const std::string ct = lower(content_type);
  static const std::string want = "application/json";
  if (ct.compare(0, want.size(), want) != 0)
    return false;
  return ct.size() == want.size() || ct[want.size()] == ';' || ct[want.size()] == ' ';
}

// nullptr if the request may claim; otherwise why not (for the 403 body).
inline const char *refusal(const std::string &host, bool has_origin, const std::string &content_type,
                           const std::string &node_name) {
  if (!host_ok(host, node_name))
    return "Host is not this camera";
  if (has_origin)
    return "cross-origin request";
  if (!json_content_type(content_type))
    return "Content-Type must be application/json";
  return nullptr;
}

}  // namespace esphome::camera_http::claim_guard
