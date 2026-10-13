#pragma once
// Proof that a request comes from a phone that paired at the chamber: the `X-AutoMyco-Key` header (public
// build, docs/device-api.md "Security"), and which requests need it.
//
//   key       = SHA-256("automyco-control:" + owner OTA password)                          (32 raw bytes)
//   challenge = the chamber's current `Control Challenge` text sensor: 32 hex, 16 random bytes
//   header    = lowercase hex of HMAC-SHA256(key, "<METHOD> <request-target> <challenge>")  (64 characters)
//
// <request-target> is the path exactly as sent on the request line: percent-encoding and query string
// included ("/switch/UV-C%20Lamp/turn_on"). The challenge is single-use: the chamber replaces it with a
// fresh random one the moment a header is accepted, at every boot, and whenever the owner password
// changes. So a captured header is dead once used, can't be replayed after a reboot, and works only for
// its own method and path. No clock is involved (SNTP is unauthenticated). The owner OTA password never
// crosses the LAN again after pairing. Keep in step with apple/AutoMyco/ShroomChamber/Transport/ControlKey.swift and
// apple/AutoMyco/mock_server/esphome_mock.py.
//
// Pure C++17 with no ESPHome or mbedtls dependency (mbedtls has HMAC too, but this keeps one implementation
// that esphome/tests/test_control_key.cpp checks on the host against FIPS 180-2 / RFC 4231 vectors).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace esphome::web_guard {

inline constexpr const char *CONTROL_KEY_HEADER = "X-AutoMyco-Key";
inline constexpr const char *CONTROL_KEY_PREFIX = "automyco-control:";
inline constexpr size_t CHALLENGE_LEN = 32;   // hex characters

namespace sha256_detail {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void block(uint32_t h[8], const uint8_t *p) {
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

}  // namespace sha256_detail

// Raw SHA-256 of `data`.
inline std::string sha256_raw(const std::string &data) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto *p = reinterpret_cast<const uint8_t *>(data.data());
  const size_t n = data.size();
  size_t off = 0;
  for (; n - off >= 64; off += 64)
    sha256_detail::block(h, p + off);
  uint8_t tail[128] = {0};
  const size_t rest = n - off;
  std::memcpy(tail, p + off, rest);
  tail[rest] = 0x80;
  const size_t tail_len = rest + 9 <= 64 ? 64 : 128;
  const uint64_t bits = (uint64_t) n * 8;
  for (int i = 0; i < 8; i++)
    tail[tail_len - 1 - i] = (uint8_t) (bits >> (8 * i));
  for (size_t i = 0; i < tail_len; i += 64)
    sha256_detail::block(h, tail + i);
  std::string out(32, '\0');
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 4; j++)
      out[4 * i + j] = (char) (uint8_t) (h[i] >> (24 - 8 * j));
  return out;
}

inline std::string to_hex(const std::string &raw) {
  static const char kHexDigits[] = "0123456789abcdef";
  std::string out(raw.size() * 2, '0');
  for (size_t i = 0; i < raw.size(); i++) {
    out[2 * i] = kHexDigits[(uint8_t) raw[i] >> 4];
    out[2 * i + 1] = kHexDigits[(uint8_t) raw[i] & 0xf];
  }
  return out;
}

inline std::string sha256_hex(const std::string &data) { return to_hex(sha256_raw(data)); }

// RFC 2104 HMAC-SHA256, raw 32 bytes.
inline std::string hmac_sha256_raw(const std::string &key, const std::string &msg) {
  std::string k = key.size() > 64 ? sha256_raw(key) : key;
  k.resize(64, '\0');
  std::string ipad(64, '\0'), opad(64, '\0');
  for (int i = 0; i < 64; i++) {
    ipad[i] = (char) ((uint8_t) k[i] ^ 0x36);
    opad[i] = (char) ((uint8_t) k[i] ^ 0x5c);
  }
  return sha256_raw(opad + sha256_raw(ipad + msg));
}

// The 32-byte control key for an owner password; "" (no owner password) means no key: nothing is gated.
inline std::string control_key(const std::string &owner_password) {
  if (owner_password.empty())
    return "";
  return sha256_raw(std::string(CONTROL_KEY_PREFIX) + owner_password);
}

// The header value a client sends for one request, signed over the chamber's current challenge.
inline std::string control_header(const std::string &key, const std::string &method, const std::string &target,
                                  const std::string &challenge) {
  return to_hex(hmac_sha256_raw(key, method + " " + target + " " + challenge));
}

// Equal-length compare whose time doesn't depend on where the strings differ. Hex letters A-F fold to a-f.
inline bool ct_equal_hex(const std::string &expected, const std::string &presented) {
  if (expected.empty())
    return false;
  uint8_t diff = presented.size() == expected.size() ? 0 : 1;
  for (size_t i = 0; i < expected.size(); i++) {
    const uint8_t c = i < presented.size() ? (uint8_t) presented[i] : 0;
    const uint8_t lower = (c >= 'A' && c <= 'F') ? (uint8_t) (c | 0x20) : c;
    diff |= (uint8_t) (lower ^ (uint8_t) expected[i]);
  }
  return diff == 0;
}

enum class KeyCheck : uint8_t { OK, NOT_LOADED, BAD_MAC };

inline const char *key_check_str(KeyCheck c) {
  switch (c) {
    case KeyCheck::OK:
      return "ok";
    case KeyCheck::NOT_LOADED:
      return "starting up";
    default:
      return "missing, wrong or already used";
  }
}

// The chamber's side of the gate, RAM only. A reboot starts it NOT loaded, with no challenge.
struct KeyGate {
  bool loaded{false};     // the owner password has loaded this boot (radio up, so the challenge is really
                          // random); until then keyed actions are refused
  std::string key;        // 32 raw bytes; empty once loaded = nobody has paired: keyed actions open
  std::string challenge;  // current single-use challenge (CHALLENGE_LEN hex)
};

// Loads (boot) or replaces (re-pair, set_ota_password) the owner password, with a fresh random challenge.
inline void gate_set_password(KeyGate &g, const std::string &owner_password, const std::string &fresh_challenge) {
  g.key = control_key(owner_password);
  g.challenge = fresh_challenge;
  g.loaded = fresh_challenge.size() == CHALLENGE_LEN;
}

// A challenge nobody uses still expires: loop() replaces it after this long, so a signed header that was
// captured and held back (never delivered) stops working within minutes, not whenever the next keyed
// action happens to rotate it.
inline constexpr uint32_t CHALLENGE_TTL_MS = 5UL * 60UL * 1000UL;

// Replaces the challenge of a paired gate (expiry). Unpaired or not loaded: nothing to protect, left alone.
inline bool gate_expire(KeyGate &g, const std::string &fresh_challenge) {
  if (!g.loaded || g.key.empty() || fresh_challenge.size() != CHALLENGE_LEN)
    return false;
  g.challenge = fresh_challenge;
  return true;
}

// The whole decision for a request that could change a keyed entity. `next_challenge` (fresh random,
// CHALLENGE_LEN hex) replaces the current one when a header is accepted, so the same header never works
// twice. Unpaired (loaded, no key): OK without a header, and the challenge is left alone.
inline KeyCheck gate_check(KeyGate &g, const std::string &method, const std::string &target,
                           const std::string &presented, const std::string &next_challenge) {
  if (!g.loaded || g.challenge.size() != CHALLENGE_LEN)
    return KeyCheck::NOT_LOADED;
  if (g.key.empty())
    return KeyCheck::OK;
  const std::string expected = control_header(g.key, method, target, g.challenge);
  if (!ct_equal_hex(expected, presented))
    return KeyCheck::BAD_MAC;
  g.challenge = next_challenge;
  return KeyCheck::OK;
}

// ---- Which requests need the header ----

// ESPHome's url_decode (web_server_idf/utils.cpp): %XX and '+' -> ' '. Used for a second pass, so a
// double-encoded name is still recognised.
inline std::string pct_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
      out += (char) (hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

inline std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = (char) (c | 0x20);
  return s;
}

// A protected entity: its name and object_id (lowercase), and the one action that stays open (UV-C's
// "turn_off"; "" for none).
struct KeyedEntity {
  std::string name;
  std::string object_id;
  std::string open_action;
};

// True if a state-changing request on this (already once-decoded, as web_server sees it) path could reach a
// keyed entity. Deliberately wider than web_server's own routing: any path segment that names the entity in
// any spelling (name or object_id, any case, "<domain>-<object_id>", decoded once more) counts, wherever it
// sits, so trailing slashes, "..", extra or device segments and double encoding can't slip past. Only a path
// whose last segment is exactly the entity's open action (UV-C turn_off) and that names nothing else keyed
// stays open.
inline bool keyed_request(const std::string &decoded_path, const std::vector<KeyedEntity> &entities) {
  std::vector<std::string> segs;
  std::string cur;
  for (char c : decoded_path) {
    if (c == '/') {
      if (!cur.empty())
        segs.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    segs.push_back(cur);
  const std::string last = segs.empty() ? "" : segs.back();
  for (const auto &e : entities) {
    bool named = false;
    for (const auto &s : segs) {
      for (const std::string &v : {lower(s), lower(pct_decode(s))}) {
        if (v == e.name || v == e.object_id ||
            (v.size() > e.object_id.size() && v.compare(v.size() - e.object_id.size(), std::string::npos,
                                                        e.object_id) == 0 &&
             v[v.size() - e.object_id.size() - 1] == '-'))
          named = true;
      }
    }
    if (named && (e.open_action.empty() || last != e.open_action))
      return true;
  }
  return false;
}

}  // namespace esphome::web_guard
